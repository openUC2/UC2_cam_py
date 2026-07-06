// pybind11 bindings for the HikCamtest core library.
//
// Exposes just enough to embed the tool in Python: initialise/finalise the SDK,
// list cameras and run a capture session. The heavy lifting stays in C++
// (core/), this file only marshals arguments and releases the GIL during the
// blocking capture loop.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <string>

#include "CaptureApp.h"
#include "MvsCamera.h"

namespace py = pybind11;

PYBIND11_MODULE(_hikcamtest, m)
{
    m.doc() = "Python bindings for the HikCamtest MVS/HikRobot camera test tool";

    py::class_<hik::DeviceInfo>(m, "DeviceInfo",
        "Description of one enumerated camera.")
        .def_readonly("transport", &hik::DeviceInfo::transport)
        .def_readonly("model_name", &hik::DeviceInfo::modelName)
        .def_readonly("serial", &hik::DeviceInfo::serial)
        .def_readonly("user_name", &hik::DeviceInfo::userName)
        .def_readonly("ip", &hik::DeviceInfo::ip)
        .def("__repr__", [](const hik::DeviceInfo& d) {
            std::string r = "<DeviceInfo " + d.transport + " " + d.modelName +
                            " SN=" + d.serial;
            if (!d.ip.empty())
            {
                r += " IP=" + d.ip;
            }
            return r + ">";
        });

    m.def("initialize", &hik::MvsCamera::initSdk,
          "Initialise the MVS SDK. Returns True on success. Call once before "
          "list_devices()/run().");

    m.def("finalize", &hik::MvsCamera::finalizeSdk,
          "Finalise the MVS SDK.");

    m.def("list_devices", &hik::listDevices,
          "Enumerate connected GigE/USB3 cameras and return a list of DeviceInfo.");

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
          "Run a capture session. Returns 0 on success, 2 if no camera was "
          "found, 1 on other errors.\n\n"
          "index:    device index (default -1 = prompt on the terminal)\n"
          "frames:   stop after N frames (default 0 = until Enter / window close)\n"
          "snapshot: path for the first-frame snapshot (.bmp/.jpg/.png/.tif)\n"
          "display:  open a preview window when X11 support is available");
}
