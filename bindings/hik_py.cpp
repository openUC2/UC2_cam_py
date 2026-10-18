// pybind11 bindings for the uc2_cam_py core library (module `_hik`).
//
// Low-level layer only: the public, vendor-neutral Python API lives in
// python/uc2_cam_py (camera.py). This file marshals arguments, builds
// zero-copy numpy views onto ring-buffer slots, releases the GIL around every
// call that can block, and hands C++ log messages to Python through a queue.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "CameraBase.h"
#include "CaptureApp.h"
#include "HikCamera.h"
#include "ImageWindow.h"
#include "MvsCamera.h"
#include "SimCamera.h"
#include "hikLog.h"

namespace py = pybind11;

namespace {

// ---------------------------------------------------------------------------
// Log forwarding
//
// C++ threads (camera callback, disk writer, ...) must never wait for the GIL:
// the sink only appends to this queue. A Python daemon thread drains it with
// _pop_log(), which blocks WITHOUT the GIL, and calls the Python logger.
// ---------------------------------------------------------------------------

struct LogQueue
{
    std::mutex                              mtx;
    std::condition_variable                 cv;
    std::deque<std::pair<int, std::string>> items;
    uint64_t                                dropped = 0;
    bool                                    wake    = false;   ///< release a waiting _pop_log()
};

const size_t kLogQueueMax = 10000;

/// Never destroyed on purpose: C++ threads may still log while the interpreter
/// and static destructors are already shutting down.
LogQueue& logQueue()
{
    static LogQueue* q = new LogQueue();
    return *q;
}

void queueSink(hik::LogLevel level, const char* message, void* ctx)
{
    auto* q = static_cast<LogQueue*>(ctx);
    {
        std::lock_guard<std::mutex> lk(q->mtx);
        if (q->items.size() >= kLogQueueMax)
        {
            ++q->dropped;
            return;
        }
        q->items.emplace_back(static_cast<int>(level), message);
    }
    q->cv.notify_one();
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

/// A pinned ring slot owned by Python. Keeps the ring alive (shared_ptr inside
/// CameraBase::Image) for as long as the handle or any numpy view of it lives.
struct ImageRef
{
    hik::CameraBase::Image img;
};

py::dtype dtypeOf(uint8_t depth)
{
    switch (static_cast<hik::PixelDepth>(depth))
    {
    case hik::PixelDepth::UInt16:  return py::dtype::of<uint16_t>();
    case hik::PixelDepth::Float64: return py::dtype::of<double>();
    case hik::PixelDepth::UInt8:   break;
    }
    return py::dtype::of<uint8_t>();
}

/// Read-only numpy view onto the slot of @p self (an ImageHandle object), which
/// becomes the array's base and so stays alive as long as the array does.
py::array imageArray(py::object self)
{
    const ImageRef& r = self.cast<const ImageRef&>();
    const hik::UC2_SessionHeader& h = r.img.header;
    const py::ssize_t item = static_cast<py::ssize_t>(hik::itemSize(static_cast<hik::PixelDepth>(h.depth)));
    std::vector<py::ssize_t> shape;
    std::vector<py::ssize_t> strides;
    if (h.channels == 1)
    {
        shape   = { static_cast<py::ssize_t>(h.height), static_cast<py::ssize_t>(h.width) };
        strides = { static_cast<py::ssize_t>(h.width) * item, item };
    }
    else
    {
        const py::ssize_t c = static_cast<py::ssize_t>(h.channels);
        shape   = { static_cast<py::ssize_t>(h.height), static_cast<py::ssize_t>(h.width), c };
        strides = { static_cast<py::ssize_t>(h.width) * c * item, c * item, item };
    }
    py::array arr(dtypeOf(h.depth), shape, strides, r.img.handle.data(), self);
    // The slot belongs to the ring (and to the .uc2rb stream): no writing into it.
    arr.attr("flags").attr("writeable") = false;
    return arr;
}

// ---------------------------------------------------------------------------
// Small converters
// ---------------------------------------------------------------------------

py::object rangeOrNone(const hik::ValueRange& r)
{
    if (!r.valid)
    {
        return py::none();
    }
    return py::make_tuple(r.min, r.max);
}

template <typename T, typename F>
py::object valueOrNone(F read)
{
    T v{};
    if (!read(v))
    {
        return py::none();
    }
    return py::cast(v);
}

py::dict headerDict(const hik::UC2_SessionHeader& h)
{
    py::dict d;
    d["magic"]          = std::string(h.magic, strnlen(h.magic, sizeof(h.magic)));
    d["format_version"] = h.formatVersion;
    d["header_bytes"]   = h.headerBytes;
    d["width"]          = h.width;
    d["height"]         = h.height;
    d["channels"]       = h.channels;
    d["depth"]          = h.depth;
    d["frame_bytes"]    = h.frameBytes;
    d["slot_count"]     = h.slotCount;
    d["layout_version"] = h.layoutVersion;
    d["start_wall_ns"]  = h.startWallNs;
    d["start_mono_ns"]  = h.startMonoNs;
    d["slot_stride"]    = h.slotStride;
    d["camera_id"]      = std::string(h.cameraId);
    d["comment"]        = std::string(h.comment);
    return d;
}

int64_t simPixelFormat(const std::string& name)
{
    if (name == "Mono8")  { return hik::pfnc::Mono8; }
    if (name == "Mono16") { return hik::pfnc::Mono16; }
    throw std::invalid_argument("SimCamera: pixel_format must be 'Mono8' or 'Mono16'");
}

py::object readImage(hik::CameraBase& c, bool latest, uint32_t slot)
{
    std::unique_ptr<ImageRef> ref(new ImageRef());
    const bool ok = latest ? c.readLatest(ref->img) : c.readSlot(slot, ref->img);
    if (!ok)
    {
        return py::none();
    }
    return py::cast(std::move(ref));
}

} // namespace

PYBIND11_MODULE(_hik, m)
{
    m.doc() = "Low-level bindings of the uc2_cam_py camera core (use uc2_cam_py.create_camera).";

    // --- Devices -------------------------------------------------------------
    py::class_<hik::DeviceInfo>(m, "DeviceInfo",
        "Description of one enumerated HikRobot camera (read-only).")
        .def_readonly("transport", &hik::DeviceInfo::transport,
                      "str: Transport layer, 'GigE' or 'USB3'.")
        .def_readonly("model_name", &hik::DeviceInfo::modelName, "str: Camera model name.")
        .def_readonly("serial", &hik::DeviceInfo::serial, "str: Serial number.")
        .def_readonly("user_name", &hik::DeviceInfo::userName,
                      "str: User-defined name (may be empty).")
        .def_readonly("ip", &hik::DeviceInfo::ip, "str: Current IP (GigE only, else empty).")
        .def("__repr__", [](const hik::DeviceInfo& d) {
            std::string r = "<DeviceInfo " + d.transport + " " + d.modelName + " SN=" + d.serial;
            if (!d.ip.empty())
            {
                r += " IP=" + d.ip;
            }
            return r + ">";
        });

    m.def("list_devices",
          [](const std::string& connection_type) {
              std::vector<hik::DeviceInfo> all;
              {
                  py::gil_scoped_release release;
                  all = hik::listDevices();
              }
              if (connection_type.empty())
              {
                  return all;
              }
              std::vector<hik::DeviceInfo> out;
              for (const hik::DeviceInfo& d : all)
              {
                  if (d.transport == connection_type)
                  {
                      out.push_back(d);
                  }
              }
              return out;
          },
          py::arg("connection_type") = std::string(),
          "Enumerate the connected HikRobot cameras.\n\n"
          "Args:\n"
          "    connection_type (str): 'USB3', 'GigE' or '' for both (default).\n\n"
          "Returns:\n"
          "    list[DeviceInfo]: One entry per camera, in enumeration order.");

    m.def("initialize", &hik::MvsCamera::initSdk,
          "Initialise the MVS SDK (done automatically on import).\n\n"
          "Returns:\n"
          "    bool: True on success.");
    m.def("finalize", &hik::MvsCamera::finalizeSdk,
          "Release the explicit SDK reference taken by initialize().");

    m.def("run_connection_test",
          [](int index, long frames, const std::string& snapshot, bool display) {
              hik::CaptureOptions opts;
              opts.index    = index;
              opts.frames   = frames;
              opts.snapshot = snapshot;
              opts.display  = display;
              py::gil_scoped_release release;
              return hik::runCapture(opts);
          },
          py::arg("index") = -1, py::arg("frames") = 0, py::arg("snapshot") = std::string(),
          py::arg("display") = true,
          "Quick connection test: open a HikRobot camera, grab a few frames, optionally\n"
          "show a preview and save a snapshot. A diagnostic tool, not the acquisition API.\n\n"
          "Args:\n"
          "    index (int): Device index, or -1 to prompt on the terminal (default -1).\n"
          "    frames (int): Stop after N frames, 0 = until Enter/window close (default 0).\n"
          "    snapshot (str): Path for the first-frame snapshot; empty to skip.\n"
          "    display (bool): Open a preview window when available (default True).\n\n"
          "Returns:\n"
          "    int: 0 on success, 2 if no camera was found, 1 on other errors.");

    // --- Logging -------------------------------------------------------------
    m.def("_set_log_forwarding",
          [](bool on) {
              LogQueue& q = logQueue();
              if (on)
              {
                  hik::setLogSink(&queueSink, &q);
                  return;
              }
              hik::setLogSink(nullptr, nullptr);
              {
                  std::lock_guard<std::mutex> lk(q.mtx);
                  q.wake = true;
              }
              q.cv.notify_all();
          },
          py::arg("on"),
          "Route C++ log messages into the queue read by _pop_log() (True) or back to stderr.");

    m.def("_pop_log",
          [](unsigned int timeout_ms, size_t max_items) {
              std::vector<std::pair<int, std::string>> out;
              {
                  py::gil_scoped_release release;
                  LogQueue& q = logQueue();
                  std::unique_lock<std::mutex> lk(q.mtx);
                  q.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [&q] { return !q.items.empty() || q.wake; });
                  q.wake = false;
                  while (!q.items.empty() && out.size() < max_items)
                  {
                      out.push_back(std::move(q.items.front()));
                      q.items.pop_front();
                  }
              }
              return out;
          },
          py::arg("timeout_ms") = 200, py::arg("max_items") = 256,
          "Wait up to timeout_ms (without the GIL) for queued C++ log messages.\n\n"
          "Returns:\n"
          "    list[tuple[int, str]]: (level, message); levels are the logging values.");

    m.def("_wake_log",
          [] {
              LogQueue& q = logQueue();
              {
                  std::lock_guard<std::mutex> lk(q.mtx);
                  q.wake = true;
              }
              q.cv.notify_all();
          },
          "Release a thread blocked in _pop_log() (used at shutdown).");

    m.def("_log_dropped",
          [] {
              LogQueue& q = logQueue();
              std::lock_guard<std::mutex> lk(q.mtx);
              const uint64_t n = q.dropped;
              q.dropped = 0;
              return n;
          },
          "Number of messages dropped because the queue was full (and reset it).");

    m.def("_set_log_level",
          [](int level) { hik::setLogLevel(static_cast<hik::LogLevel>(level)); },
          py::arg("level"), "Drop C++ messages below this logging level before they are queued.");

    m.def("_emit_log",
          [](int level, const std::string& msg) {
              hik::logMessage(static_cast<hik::LogLevel>(level), msg);
          },
          py::arg("level"), py::arg("message"),
          "Emit a message through the C++ logging path (for tests).");

    // --- Images --------------------------------------------------------------
    py::class_<ImageRef>(m, "ImageHandle",
        "One pinned ring-buffer slot. Its `array` is a zero-copy, read-only view;\n"
        "handle and array keep the ring memory alive (even after the camera closed).")
        .def_property_readonly("array", &imageArray,
            "numpy.ndarray: Read-only view of the pixels, (H, W) or (H, W, 3).")
        .def_property_readonly("sequence", [](const ImageRef& r) { return r.img.handle.sequence(); },
            "int: Global frame index within the session.")
        .def_property_readonly("slot", [](const ImageRef& r) { return r.img.handle.slot(); },
            "int: Ring slot the frame occupies.")
        .def_property_readonly("ts_before_ns", [](const ImageRef& r) { return r.img.handle.tsBeforeNs(); },
            "int: Monotonic ns when the frame reached the host.")
        .def_property_readonly("ts_after_ns", [](const ImageRef& r) { return r.img.handle.tsAfterNs(); },
            "int: Monotonic ns when the copy into the slot was complete.")
        .def_property_readonly("nbytes", [](const ImageRef& r) { return r.img.handle.nbytes(); },
            "int: Valid payload bytes.")
        .def_property_readonly("address",
            [](const ImageRef& r) { return reinterpret_cast<std::uintptr_t>(r.img.handle.data()); },
            "int: Address of the slot payload (identical to the array's data pointer).")
        .def("still_valid", [](const ImageRef& r) { return r.img.handle.stillValid(); },
            "bool: True if the producer has not recycled the slot since it was read.");

    // --- Cameras -------------------------------------------------------------
    py::class_<hik::CameraBase, std::shared_ptr<hik::CameraBase>>(m, "CameraBase",
        "Vendor-neutral camera core (abstract). Units: ms, fps, dB, unbinned sensor pixels.")
        .def_property_readonly("vendor", &hik::CameraBase::vendor)
        .def_property_readonly("model", &hik::CameraBase::model)
        .def_property_readonly("serial", &hik::CameraBase::serial)
        .def_property_readonly("connection_type", &hik::CameraBase::connectionType)
        .def_property_readonly("index", &hik::CameraBase::index)

        .def("can_set_exposure", &hik::CameraBase::canSetExposure)
        .def("can_set_gain", &hik::CameraBase::canSetGain)
        .def("can_set_frame_rate", &hik::CameraBase::canSetFrameRate)
        .def("can_set_binning", &hik::CameraBase::canSetBinning)
        .def("can_set_trigger", &hik::CameraBase::canSetTrigger)
        .def("can_set_roi", &hik::CameraBase::canSetRoi)

        .def("exposure_range_ms", [](hik::CameraBase& c) { return rangeOrNone(c.exposureRangeMs()); })
        .def("frame_rate_range", [](hik::CameraBase& c) { return rangeOrNone(c.frameRateRange()); })
        .def("gain_range_db", [](hik::CameraBase& c) { return rangeOrNone(c.gainRangeDb()); })
        .def("binning_values", &hik::CameraBase::binningValues)

        .def("get_exposure_ms", [](hik::CameraBase& c) {
            return valueOrNone<double>([&c](double& v) { return c.getExposureMs(v); }); })
        .def("get_frame_rate", [](hik::CameraBase& c) {
            return valueOrNone<double>([&c](double& v) { return c.getFrameRate(v); }); })
        .def("get_gain_db", [](hik::CameraBase& c) {
            return valueOrNone<double>([&c](double& v) { return c.getGainDb(v); }); })
        .def("get_binning", [](hik::CameraBase& c) {
            return valueOrNone<uint32_t>([&c](uint32_t& v) { return c.getBinning(v); }); })
        .def("get_roi", [](hik::CameraBase& c) -> py::object {
                hik::SensorRoi r;
                if (!c.getRoi(r)) { return py::none(); }
                return py::make_tuple(r.x, r.y, r.width, r.height); },
            "tuple[int, int, int, int] | None: (x, y, width, height) in unbinned sensor pixels.")
        .def("sensor_size", [](hik::CameraBase& c) -> py::object {
                int64_t w = 0, h = 0;
                if (!c.getSensorSize(w, h)) { return py::none(); }
                return py::make_tuple(w, h); })
        .def("image_size", [](hik::CameraBase& c) -> py::object {
                int64_t w = 0, h = 0;
                if (!c.getImageSize(w, h)) { return py::none(); }
                return py::make_tuple(w, h); })
        .def("pixel_format", &hik::CameraBase::pixelFormat, "int: PFNC code of the pixel format.")
        .def("pixel_layout", [](hik::CameraBase& c) {
                const hik::PixelLayout l = hik::pixelLayout(c.pixelFormat());
                return py::make_tuple(l.supported, l.channels, hik::itemSize(l.depth),
                                      std::string(l.name)); },
            "tuple[bool, int, int, str]: (storable, channels, bytes per sample, format name).")

        .def("set_exposure_ms", &hik::CameraBase::setExposureMs, py::arg("ms"))
        .def("set_exposure_auto", &hik::CameraBase::setExposureAuto)
        .def("set_frame_rate", &hik::CameraBase::setFrameRate, py::arg("fps"))
        .def("disable_frame_rate_limit", &hik::CameraBase::disableFrameRateLimit)
        .def("set_gain_db", &hik::CameraBase::setGainDb, py::arg("db"))
        .def("set_gain_auto", &hik::CameraBase::setGainAuto)
        .def("set_binning", &hik::CameraBase::setBinning, py::arg("factor"),
             py::call_guard<py::gil_scoped_release>())
        .def("set_roi",
             [](hik::CameraBase& c, int64_t x, int64_t y, int64_t w, int64_t h) {
                 hik::SensorRoi r;
                 r.x = x; r.y = y; r.width = w; r.height = h;
                 py::gil_scoped_release release;
                 return c.setRoi(r);
             },
             py::arg("x"), py::arg("y"), py::arg("width"), py::arg("height"))
        .def("reset_roi", &hik::CameraBase::resetRoi, py::call_guard<py::gil_scoped_release>())
        .def("last_change_restarted", &hik::CameraBase::lastChangeRestarted)

        .def("set_buffer_config",
             [](hik::CameraBase& c, uint32_t slot_count, const std::string& storage_path,
                const std::string& comment) {
                 hik::CameraBase::BufferConfig cfg;
                 cfg.slotCount   = slot_count;
                 cfg.storagePath = storage_path;
                 cfg.comment     = comment;
                 c.setBufferConfig(cfg);
             },
             py::arg("slot_count"), py::arg("storage_path"), py::arg("comment"))
        .def("start", &hik::CameraBase::start, py::call_guard<py::gil_scoped_release>())
        .def("stop", &hik::CameraBase::stop, py::call_guard<py::gil_scoped_release>())
        .def("close", &hik::CameraBase::close, py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("is_running", &hik::CameraBase::isRunning)

        .def("read_latest", [](hik::CameraBase& c) { return readImage(c, true, 0); },
            "ImageHandle | None: The newest complete frame, None if there is none yet.")
        .def("read_slot", [](hik::CameraBase& c, uint32_t slot) { return readImage(c, false, slot); },
            py::arg("slot"),
            "ImageHandle | None: Frame in ring slot `slot`, None if empty or out of range.")
        .def("slot_count", [](hik::CameraBase& c) -> uint32_t {
                auto r = c.ring();
                return r ? r->slotCount() : c.bufferConfig().slotCount; })
        .def("session_header", [](hik::CameraBase& c) -> py::object {
                hik::UC2_SessionHeader h{};
                if (!c.sessionHeader(h)) { return py::none(); }
                return headerDict(h); })
        .def("recording_file", &hik::CameraBase::recordingFile)
        .def("frames_written", [](hik::CameraBase& c) -> uint64_t {
                auto r = c.ring();
                return r ? r->framesWritten() : 0; })
        .def("frames_rejected", &hik::CameraBase::framesRejected)
        .def("stream_stats", [](hik::CameraBase& c) {
                auto r = c.ring();
                if (!r) { return py::make_tuple(uint64_t(0), uint64_t(0), false); }
                return py::make_tuple(r->streamFramesSaved(), r->streamFramesDropped(),
                                      r->streamHasError()); },
            "tuple[int, int, bool]: (frames saved, frames dropped, write error) of the disk stream.");

    py::class_<hik::HikCamera, hik::CameraBase, std::shared_ptr<hik::HikCamera>>(m, "HikCamera",
        "HikRobot (MVS) camera. Raises ValueError (connection type), IndexError (index)\n"
        "or RuntimeError (no such camera / cannot open).")
        .def(py::init<const std::string&, const std::string&, int>(),
             py::arg("connection_type") = std::string(), py::arg("serial") = std::string(),
             py::arg("index") = -1, py::call_guard<py::gil_scoped_release>());

    py::class_<hik::SimCamera, hik::CameraBase, std::shared_ptr<hik::SimCamera>>(m, "SimCamera",
        "Simulated camera producing a walking Gaussian spot (for tests without hardware).")
        .def(py::init([](const std::string& serial, const std::string& connection_type,
                         const std::string& pixel_format, uint32_t sensor_width,
                         uint32_t sensor_height, double frame_rate) {
                 hik::SimCamera::Options o;
                 o.serial         = serial;
                 o.connectionType = connection_type;
                 o.pixelFormat    = simPixelFormat(pixel_format);
                 o.sensorWidth    = sensor_width;
                 o.sensorHeight   = sensor_height;
                 o.frameRate      = frame_rate;
                 return std::make_shared<hik::SimCamera>(o);
             }),
             py::arg("serial") = std::string("SIM-0001"), py::arg("connection_type") = std::string("USB3"),
             py::arg("pixel_format") = std::string("Mono16"), py::arg("sensor_width") = 1920,
             py::arg("sensor_height") = 1200, py::arg("frame_rate") = 10.0)
        .def("spot_position", [](const hik::SimCamera& c, uint64_t sequence) {
                 const hik::sim::SpotPosition p = c.spotPositionFor(sequence);
                 return py::make_tuple(p.x, p.y); },
             py::arg("sequence"),
             "tuple[float, float]: Expected spot centre (sensor pixels) of frame `sequence`.")
        .def("exposure_auto", &hik::SimCamera::exposureAuto)
        .def("gain_auto", &hik::SimCamera::gainAuto);

    // --- Preview window -------------------------------------------------------
    py::class_<hik::ImageWindow>(m, "ImageWindow",
        "Minimal X11 window that shows 8-bit grey images (see uc2_cam_py.preview).")
        .def(py::init<unsigned int, unsigned int, const std::string&>(),
             py::arg("width"), py::arg("height"), py::arg("title"))
        .def_property_readonly("is_valid", &hik::ImageWindow::isValid)
        .def_property_readonly("close_requested", &hik::ImageWindow::closeRequested)
        .def("pump_events", &hik::ImageWindow::pumpEvents,
             "bool: Process window events; False once the window was closed.")
        .def("show_gray8",
             [](hik::ImageWindow& w, py::array_t<uint8_t, py::array::c_style | py::array::forcecast> img) {
                 if (img.ndim() != 2)
                 {
                     throw std::invalid_argument("show_gray8 expects a 2-D uint8 array");
                 }
                 return w.showGray8(img.data(), static_cast<unsigned int>(img.shape(1)),
                                    static_cast<unsigned int>(img.shape(0)));
             },
             py::arg("image"), "bool: Draw a (H, W) uint8 image at the top-left corner.")
        .def_static("display_supported", &hik::ImageWindow::displaySupported);
}
