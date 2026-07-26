// pybind11 bindings for the UC2_HIKpy core library.
//
// Exposes just enough to drive the tool from Python: initialise/finalise the
// SDK, list cameras, run a one-shot capture session, and grab frames
// continuously with a zero-copy view of the latest image. All image handling
// stays in C++ (core/); this file only marshals arguments, builds numpy views
// and releases the GIL around blocking calls.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "CaptureApp.h"
#include "FrameGrabber.h"
#include "MvsCamera.h"

namespace py = pybind11;

namespace {

/// Build a numpy array for a frame.
///
/// When @p copy is false the array is a zero-copy view aliasing the SDK-owned
/// buffer (kept alive by a no-op capsule base); it is valid only until the next
/// FrameGrabber.wait_for_frame() call. When @p copy is true an independent,
/// owning array is returned. Mono8 -> (H, W); RGB8/BGR8 -> (H, W, 3); any other
/// pixel format -> a flat (nbytes,) byte array plus the pixel_format tag.
py::array frame_to_array(const hik::FrameView& f, bool copy)
{
    int channels = 0;
    int itemsize = 1;
    bool simple = false;
    hik::pixelInfo(f.pixelType, channels, itemsize, simple);

    std::vector<py::ssize_t> shape;
    std::vector<py::ssize_t> strides;

    const uint64_t dense =
        static_cast<uint64_t>(f.width) * f.height * static_cast<uint64_t>(channels) * itemsize;

    if (simple && channels > 0 && dense == f.nbytes)
    {
        if (channels == 1)
        {
            shape   = { static_cast<py::ssize_t>(f.height), static_cast<py::ssize_t>(f.width) };
            strides = { static_cast<py::ssize_t>(f.width * itemsize),
                        static_cast<py::ssize_t>(itemsize) };
        }
        else
        {
            shape   = { static_cast<py::ssize_t>(f.height),
                        static_cast<py::ssize_t>(f.width),
                        static_cast<py::ssize_t>(channels) };
            strides = { static_cast<py::ssize_t>(f.width * channels * itemsize),
                        static_cast<py::ssize_t>(channels * itemsize),
                        static_cast<py::ssize_t>(itemsize) };
        }
    }
    else
    {
        shape   = { static_cast<py::ssize_t>(f.nbytes) };
        strides = { static_cast<py::ssize_t>(1) };
    }

    const auto dtype = py::dtype::of<uint8_t>();
    if (copy)
    {
        // Non-null pointer with no base: pybind11 allocates and copies.
        return py::array(dtype, shape, strides, f.data);
    }
    // Non-null base: pybind11 does not copy; the grabber owns the memory.
    py::capsule base(static_cast<const void*>(f.data), [](void*) {});
    return py::array(dtype, shape, strides, f.data, base);
}

} // namespace

PYBIND11_MODULE(_hik, m)
{
    m.doc() = "Python bindings for the UC2_HIKpy MVS/HikRobot camera core.";

    py::class_<hik::DeviceInfo>(m, "DeviceInfo",
        "Description of one enumerated camera (read-only).")
        .def_readonly("transport", &hik::DeviceInfo::transport,
                      "str: Transport layer, e.g. 'GigE' or 'USB3'.")
        .def_readonly("model_name", &hik::DeviceInfo::modelName, "str: Camera model name.")
        .def_readonly("serial", &hik::DeviceInfo::serial, "str: Serial number.")
        .def_readonly("user_name", &hik::DeviceInfo::userName,
                      "str: User-defined name (may be empty).")
        .def_readonly("ip", &hik::DeviceInfo::ip, "str: Current IP (GigE only, else empty).")
        .def("__repr__", [](const hik::DeviceInfo& d) {
            std::string r = "<DeviceInfo " + d.transport + " " + d.modelName +
                            " SN=" + d.serial;
            if (!d.ip.empty())
            {
                r += " IP=" + d.ip;
            }
            return r + ">";
        });

    py::class_<hik::FrameView>(m, "Frame",
        "A single grabbed frame.\n\n"
        "The `array` view is zero-copy and aliases SDK-owned memory: it is only\n"
        "valid until the next FrameGrabber.wait_for_frame() call (or stop/close).\n"
        "Use `copy()` to obtain an independent array you may keep.")
        .def_property_readonly(
            "array",
            [](const hik::FrameView& f) { return frame_to_array(f, /*copy=*/false); },
            "numpy.ndarray: Zero-copy view of the pixels. Mono8 -> (H, W) uint8;\n"
            "RGB8/BGR8 -> (H, W, 3) uint8; other formats -> flat (nbytes,) uint8.\n"
            "Valid only until the next wait_for_frame() call.")
        .def("copy",
             [](const hik::FrameView& f) { return frame_to_array(f, /*copy=*/true); },
             "Return an independent, owning copy of the pixels.\n\n"
             "Returns:\n"
             "    numpy.ndarray: A newly allocated array with the same shape and\n"
             "    dtype as `array`, safe to keep beyond the next frame.")
        .def_property_readonly(
            "width", [](const hik::FrameView& f) { return f.width; },
            "int: Image width in pixels.")
        .def_property_readonly(
            "height", [](const hik::FrameView& f) { return f.height; },
            "int: Image height in pixels.")
        .def_property_readonly(
            "pixel_format", [](const hik::FrameView& f) { return f.pixelType; },
            "int: MvGvspPixelType value of the frame.")
        .def_property_readonly(
            "frame_num", [](const hik::FrameView& f) { return f.frameNum; },
            "int: Device frame/sequence number.")
        .def_property_readonly(
            "timestamp", [](const hik::FrameView& f) { return f.hostTimestamp; },
            "int: Host-generated timestamp of the frame.")
        .def_property_readonly(
            "lost_packets", [](const hik::FrameView& f) { return f.lostPacket; },
            "int: Number of packets lost while transferring this frame.")
        .def_property_readonly(
            "nbytes", [](const hik::FrameView& f) { return f.nbytes; },
            "int: Number of valid pixel bytes.")
        .def_property_readonly(
            "itemsize",
            [](const hik::FrameView& f) {
                int c = 0, i = 1; bool s = false;
                hik::pixelInfo(f.pixelType, c, i, s);
                return i;
            },
            "int: Size in bytes of one channel sample (1 for 8-bit formats).")
        .def_property_readonly(
            "address",
            [](const hik::FrameView& f) { return reinterpret_cast<std::uintptr_t>(f.data); },
            "int: Address of the pixel buffer, for handing to an external ring.\n"
            "The memory is valid only until the next wait_for_frame() call.");

    py::enum_<hik::AutoMode>(m, "AutoMode",
        "Auto-regulation mode for exposure or gain.")
        .value("Off", hik::AutoMode::Off,
               "Firmware regulation disabled; the manual value is used.")
        .value("Once", hik::AutoMode::Once, "Regulate once, then hold.")
        .value("Continuous", hik::AutoMode::Continuous, "Regulate continuously.");

    py::class_<hik::Roi>(m, "Roi",
        "Region of interest (in pixels) on the sensor.")
        .def_readonly("width", &hik::Roi::width, "int: ROI width in pixels.")
        .def_readonly("height", &hik::Roi::height, "int: ROI height in pixels.")
        .def_readonly("offset_x", &hik::Roi::offsetX,
                      "int: Horizontal offset from the left sensor edge.")
        .def_readonly("offset_y", &hik::Roi::offsetY,
                      "int: Vertical offset from the top sensor edge.")
        .def("__repr__", [](const hik::Roi& r) {
            return "<Roi " + std::to_string(r.width) + "x" + std::to_string(r.height) +
                   " +" + std::to_string(r.offsetX) + "+" + std::to_string(r.offsetY) + ">";
        });

    py::class_<hik::FrameGrabber>(m, "FrameGrabber",
        "Continuous, callback-driven grabber exposing the latest frame zero-copy.\n\n"
        "Acquisition runs on an SDK worker thread using the OneByOne strategy.\n"
        "There is no ring buffer here; only the most recent frame is exposed.\n"
        "Typical lifecycle: open() -> start() -> wait_for_frame()* -> stop()/close().")
        .def(py::init<>(), "Construct an unopened grabber. The MVS SDK must already be initialised.")
        .def("open", &hik::FrameGrabber::open, py::arg("index"),
             "Enumerate devices and open one.\n\n"
             "Args:\n"
             "    index (int): Zero-based device index into the enumeration.\n\n"
             "Returns:\n"
             "    bool: True if the device was opened (or already open).")
        .def("set_node_num", &hik::FrameGrabber::setNodeNum, py::arg("n"),
             "Set the SDK internal buffer-node count used from the next start().\n\n"
             "Args:\n"
             "    n (int): Number of nodes (clamped to at least 3).\n\n"
             "Returns:\n"
             "    None")
        .def("start", &hik::FrameGrabber::start,
             "Configure node count and grab strategy, register the image callback\n"
             "and begin grabbing. Call after open().\n\n"
             "Returns:\n"
             "    bool: True on success.")
        .def("wait_for_frame",
             [](hik::FrameGrabber& g, unsigned int timeout_ms) -> py::object {
                 hik::FrameView fv;
                 {
                     // Block off the GIL so other Python threads keep running.
                     py::gil_scoped_release release;
                     fv = g.waitForFrame(timeout_ms);
                 }
                 if (!fv.valid)
                 {
                     return py::none();
                 }
                 return py::cast(fv);
             },
             py::arg("timeout_ms") = 1000,
             "Block until the next frame is ready, a timeout elapses, or the grabber stops.\n\n"
             "Args:\n"
             "    timeout_ms (int): Maximum time to wait, in milliseconds (default 1000).\n\n"
             "Returns:\n"
             "    Frame | None: The latest frame, or None on timeout/stop. The frame's\n"
             "    zero-copy view is valid only until the next wait_for_frame() call.")
        .def("stop", &hik::FrameGrabber::stop,
             "Stop grabbing, unregister the callback and release held buffers.\n\n"
             "Returns:\n"
             "    None")
        .def("close", &hik::FrameGrabber::close,
             "Stop grabbing and close the device.\n\n"
             "Returns:\n"
             "    None")
        .def_property_readonly("is_running", &hik::FrameGrabber::isRunning,
                               "bool: True while grabbing is active.")
        .def_property_readonly("frames_received", &hik::FrameGrabber::framesReceived,
                               "int: Total frames delivered by the SDK callback.")
        .def_property_readonly("frames_dropped", &hik::FrameGrabber::framesDropped,
                               "int: Frames superseded before the consumer read them.")
        .def_property_readonly("payload_size", &hik::FrameGrabber::payloadSize,
                               "int: Camera PayloadSize in bytes (0 until open()).")
        .def_property_readonly("width", &hik::FrameGrabber::width,
                               "int: Width of the most recent frame (0 if none yet).")
        .def_property_readonly("height", &hik::FrameGrabber::height,
                               "int: Height of the most recent frame (0 if none yet).")
        .def_property_readonly("pixel_format", &hik::FrameGrabber::pixelType,
                               "int: MvGvspPixelType of the most recent frame (0 if none).")

        // --- Exposure ---------------------------------------------------------
        .def("set_exposure_time", &hik::FrameGrabber::setExposureTime,
             py::arg("microseconds"),
             "Set the exposure time (switches auto-exposure Off so the value sticks).\n\n"
             "Args:\n"
             "    microseconds (float): Exposure time in microseconds.\n\n"
             "Returns:\n"
             "    bool: True on success.")
        .def("get_exposure_time",
             [](hik::FrameGrabber& g) {
                 double v = 0.0;
                 if (!g.getExposureTime(v)) throw std::runtime_error("failed to read ExposureTime");
                 return v;
             },
             "Read the current exposure time.\n\n"
             "Returns:\n"
             "    float: Exposure time in microseconds.\n\n"
             "Raises:\n"
             "    RuntimeError: If the value cannot be read.")
        .def("get_exposure_time_range",
             [](hik::FrameGrabber& g) {
                 double mn = 0.0, mx = 0.0;
                 if (!g.getExposureTimeRange(mn, mx)) throw std::runtime_error("failed to read ExposureTime range");
                 return py::make_tuple(mn, mx);
             },
             "Read the exposure-time limits.\n\n"
             "Returns:\n"
             "    tuple[float, float]: (min, max) exposure time in microseconds.")

        // --- Gain -------------------------------------------------------------
        .def("set_gain", &hik::FrameGrabber::setGain, py::arg("db"),
             "Set the analog gain (switches auto-gain Off so the value sticks).\n\n"
             "Args:\n"
             "    db (float): Gain in decibels.\n\n"
             "Returns:\n"
             "    bool: True on success.")
        .def("get_gain",
             [](hik::FrameGrabber& g) {
                 double v = 0.0;
                 if (!g.getGain(v)) throw std::runtime_error("failed to read Gain");
                 return v;
             },
             "Read the current gain.\n\n"
             "Returns:\n"
             "    float: Gain in decibels.\n\n"
             "Raises:\n"
             "    RuntimeError: If the value cannot be read.")
        .def("get_gain_range",
             [](hik::FrameGrabber& g) {
                 double mn = 0.0, mx = 0.0;
                 if (!g.getGainRange(mn, mx)) throw std::runtime_error("failed to read Gain range");
                 return py::make_tuple(mn, mx);
             },
             "Read the gain limits.\n\n"
             "Returns:\n"
             "    tuple[float, float]: (min, max) gain in decibels.")

        // --- Frame rate -------------------------------------------------------
        .def("set_frame_rate", &hik::FrameGrabber::setFrameRate, py::arg("fps"),
             "Enable manual frame-rate control and set the acquisition frame rate.\n\n"
             "Args:\n"
             "    fps (float): Requested frame rate in frames per second.\n\n"
             "Returns:\n"
             "    bool: True on success.")
        .def("get_frame_rate",
             [](hik::FrameGrabber& g) {
                 double v = 0.0;
                 if (!g.getFrameRate(v)) throw std::runtime_error("failed to read AcquisitionFrameRate");
                 return v;
             },
             "Read the configured acquisition frame rate.\n\n"
             "Returns:\n"
             "    float: Frame rate in frames per second.\n\n"
             "Raises:\n"
             "    RuntimeError: If the value cannot be read.")
        .def("get_resulting_frame_rate",
             [](hik::FrameGrabber& g) {
                 double v = 0.0;
                 if (!g.getResultingFrameRate(v)) throw std::runtime_error("ResultingFrameRate unavailable");
                 return v;
             },
             "Read the resulting (actual) frame rate the camera can achieve.\n\n"
             "Returns:\n"
             "    float: Resulting frame rate in frames per second.\n\n"
             "Raises:\n"
             "    RuntimeError: If the node is unavailable.")
        .def("get_frame_rate_range",
             [](hik::FrameGrabber& g) {
                 double mn = 0.0, mx = 0.0;
                 if (!g.getFrameRateRange(mn, mx)) throw std::runtime_error("failed to read frame-rate range");
                 return py::make_tuple(mn, mx);
             },
             "Read the acquisition frame-rate limits.\n\n"
             "Returns:\n"
             "    tuple[float, float]: (min, max) frame rate in frames per second.")

        // --- Auto modes -------------------------------------------------------
        .def("set_exposure_auto", &hik::FrameGrabber::setExposureAuto, py::arg("mode"),
             "Set the auto-exposure mode.\n\n"
             "Args:\n"
             "    mode (AutoMode): Off, Once or Continuous.\n\n"
             "Returns:\n"
             "    bool: True on success.")
        .def("get_exposure_auto",
             [](hik::FrameGrabber& g) {
                 hik::AutoMode m = hik::AutoMode::Off;
                 if (!g.getExposureAuto(m)) throw std::runtime_error("failed to read ExposureAuto");
                 return m;
             },
             "Read the auto-exposure mode.\n\n"
             "Returns:\n"
             "    AutoMode: The current mode.")
        .def("set_gain_auto", &hik::FrameGrabber::setGainAuto, py::arg("mode"),
             "Set the auto-gain mode.\n\n"
             "Args:\n"
             "    mode (AutoMode): Off, Once or Continuous.\n\n"
             "Returns:\n"
             "    bool: True on success.")
        .def("get_gain_auto",
             [](hik::FrameGrabber& g) {
                 hik::AutoMode m = hik::AutoMode::Off;
                 if (!g.getGainAuto(m)) throw std::runtime_error("failed to read GainAuto");
                 return m;
             },
             "Read the auto-gain mode.\n\n"
             "Returns:\n"
             "    AutoMode: The current mode.")

        // --- ROI --------------------------------------------------------------
        .def("set_roi",
             [](hik::FrameGrabber& g, int64_t width, int64_t height,
                int64_t offset_x, int64_t offset_y) {
                 hik::Roi roi;
                 roi.width = width;
                 roi.height = height;
                 roi.offsetX = offset_x;
                 roi.offsetY = offset_y;
                 return g.setRoi(roi);
             },
             py::arg("width"), py::arg("height"), py::arg("offset_x") = 0, py::arg("offset_y") = 0,
             "Set the region of interest (values are clamped/aligned to the camera grid).\n\n"
             "If grabbing is active this briefly stops and restarts the stream, because\n"
             "the SDK only accepts ROI changes while stopped.\n\n"
             "Args:\n"
             "    width (int): ROI width in pixels.\n"
             "    height (int): ROI height in pixels.\n"
             "    offset_x (int): Horizontal offset from the left edge (default 0).\n"
             "    offset_y (int): Vertical offset from the top edge (default 0).\n\n"
             "Returns:\n"
             "    bool: True on success.")
        .def("get_roi",
             [](hik::FrameGrabber& g) {
                 hik::Roi r;
                 if (!g.getRoi(r)) throw std::runtime_error("failed to read ROI");
                 return r;
             },
             "Read the current region of interest.\n\n"
             "Returns:\n"
             "    Roi: The current width/height/offset_x/offset_y.")
        .def("get_sensor_size",
             [](hik::FrameGrabber& g) {
                 int64_t w = 0, h = 0;
                 if (!g.getSensorSize(w, h)) throw std::runtime_error("failed to read sensor size");
                 return py::make_tuple(w, h);
             },
             "Read the full sensor size (WidthMax, HeightMax).\n\n"
             "Returns:\n"
             "    tuple[int, int]: (width_max, height_max) in pixels.");

    m.def("initialize", &hik::MvsCamera::initSdk,
          "Initialise the MVS SDK. Call once before list_devices()/run()/FrameGrabber.\n\n"
          "Returns:\n"
          "    bool: True on success.");

    m.def("finalize", &hik::MvsCamera::finalizeSdk,
          "Finalise the MVS SDK and release its resources.\n\n"
          "Returns:\n"
          "    None");

    m.def("list_devices", &hik::listDevices,
          "Enumerate the connected GigE/USB3 cameras.\n\n"
          "Returns:\n"
          "    list[DeviceInfo]: One entry per detected camera (empty if none).");

    m.def("run",
          [](int index, long frames, const std::string& snapshot, bool display) {
              hik::CaptureOptions opts;
              opts.index    = index;
              opts.frames   = frames;
              opts.snapshot = snapshot;
              opts.display  = display;
              // The capture loop blocks (grabbing, waiting for Enter); let other
              // Python threads run meanwhile.
              py::gil_scoped_release release;
              return hik::runCapture(opts);
          },
          py::arg("index") = -1,
          py::arg("frames") = 0,
          py::arg("snapshot") = std::string(),
          py::arg("display") = true,
          "Run a one-shot interactive capture session.\n\n"
          "Args:\n"
          "    index (int): Device index, or -1 to prompt on the terminal (default -1).\n"
          "    frames (int): Stop after N frames, or 0 to run until Enter/window close (default 0).\n"
          "    snapshot (str): Path for the first-frame snapshot (.bmp/.jpg/.png/.tif); empty to skip.\n"
          "    display (bool): Open a preview window when X11 support is available (default True).\n\n"
          "Returns:\n"
          "    int: 0 on success, 2 if no camera was found, 1 on other errors.");
}
