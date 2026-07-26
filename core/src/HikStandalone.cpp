// HikCamtest - a small terminal test tool for HikRobot / MVS cameras.
//
// It enumerates the connected GigE/USB3 cameras, opens one, streams frames in
// free-run mode, prints per-frame information to the terminal, shows the live
// image in a simple window (when built with X11 support) and can save a
// snapshot to disk.
//
// All the actual work lives in the reusable core library (CaptureApp / MvsCamera);
// this file only turns command-line arguments into a CaptureOptions and runs it.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "CaptureApp.h"
#include "MvsCamera.h"

namespace {

void printUsage(const char* argv0)
{
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -i, --index N       Open device N (skip the interactive prompt)\n"
        "  -n, --frames N      Stop after N frames (default: 0 = run until Enter / window close)\n"
        "  -s, --snapshot PATH Save the first grabbed frame to PATH (.bmp/.jpg/.png/.tif)\n"
        "      --no-display    Do not open a preview window (headless)\n"
        "  -h, --help          Show this help\n"
        "\n"
        "With no camera window available, the first frame is saved to snapshot.png\n"
        "so you can still inspect the captured image.\n",
        argv0);
}

// Returns true to continue running, false to exit (help / parse error).
bool parseArgs(int argc, char** argv, hik::CaptureOptions& opt, int& exitCode)
{
    exitCode = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "Option %s requires a value.\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help")
        {
            printUsage(argv[0]);
            return false;
        }
        else if (a == "-i" || a == "--index")
        {
            const char* v = needValue(a.c_str());
            if (!v) { exitCode = 1; return false; }
            opt.index = std::atoi(v);
        }
        else if (a == "-n" || a == "--frames")
        {
            const char* v = needValue(a.c_str());
            if (!v) { exitCode = 1; return false; }
            opt.frames = std::atol(v);
        }
        else if (a == "-s" || a == "--snapshot")
        {
            const char* v = needValue(a.c_str());
            if (!v) { exitCode = 1; return false; }
            opt.snapshot = v;
        }
        else if (a == "--no-display")
        {
            opt.display = false;
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", a.c_str());
            printUsage(argv[0]);
            exitCode = 1;
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    hik::CaptureOptions opt;
    int exitCode = 0;
    if (!parseArgs(argc, argv, opt, exitCode))
    {
        return exitCode;
    }

    if (!hik::MvsCamera::initSdk())
    {
        return 1;
    }

    int rc = hik::runCapture(opt);

    hik::MvsCamera::finalizeSdk();
    return rc;
}
