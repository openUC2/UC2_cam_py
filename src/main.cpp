// HikCamtest - a small terminal test tool for HikRobot / MVS cameras.
//
// It enumerates the connected GigE/USB3 cameras, opens one, streams frames in
// free-run mode, prints per-frame information to the terminal, shows the live
// image in a simple window (when built with X11 support) and can save a
// snapshot to disk.

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "ImageWindow.h"
#include "MvsCamera.h"

namespace {

struct Options
{
    int         index    = -1;      ///< device index; -1 => ask interactively
    long        frames   = 0;       ///< stop after N frames; 0 => until Enter/close
    std::string snapshot;           ///< path for the first-frame snapshot (may be empty)
    bool        noDisplay = false;  ///< force headless even if X11 is available
};

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

// Map a file extension to an SDK save format. Returns false for unknown types.
bool formatFromPath(const std::string& path, hik::SaveFormat& out)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
    {
        return false;
    }
    std::string ext = path.substr(dot + 1);
    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == "bmp")                    { out = hik::SaveFormat::Bmp;  return true; }
    if (ext == "jpg" || ext == "jpeg")   { out = hik::SaveFormat::Jpeg; return true; }
    if (ext == "png")                    { out = hik::SaveFormat::Png;  return true; }
    if (ext == "tif" || ext == "tiff")   { out = hik::SaveFormat::Tiff; return true; }
    return false;
}

bool parseArgs(int argc, char** argv, Options& opt)
{
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
            if (!v) return false;
            opt.index = std::atoi(v);
        }
        else if (a == "-n" || a == "--frames")
        {
            const char* v = needValue(a.c_str());
            if (!v) return false;
            opt.frames = std::atol(v);
        }
        else if (a == "-s" || a == "--snapshot")
        {
            const char* v = needValue(a.c_str());
            if (!v) return false;
            opt.snapshot = v;
        }
        else if (a == "--no-display")
        {
            opt.noDisplay = true;
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", a.c_str());
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

// Print the enumerated devices as a compact table.
void printDeviceList(const std::vector<hik::DeviceInfo>& devices)
{
    std::printf("\nFound %zu device(s):\n", devices.size());
    for (size_t i = 0; i < devices.size(); ++i)
    {
        const hik::DeviceInfo& d = devices[i];
        std::printf("  [%zu] %-6s  %-20s  SN:%-16s",
                    i, d.transport.c_str(), d.modelName.c_str(), d.serial.c_str());
        if (!d.ip.empty())
        {
            std::printf("  IP:%s", d.ip.c_str());
        }
        if (!d.userName.empty())
        {
            std::printf("  name:%s", d.userName.c_str());
        }
        std::printf("\n");
    }
}

// Ask the user to pick a device index on the terminal.
int promptForIndex(int deviceCount)
{
    std::printf("\nEnter camera index [0-%d]: ", deviceCount - 1);
    std::fflush(stdout);

    int index = -1;
    if (std::scanf("%d", &index) != 1)
    {
        std::fprintf(stderr, "No valid index entered.\n");
        return -1;
    }
    if (index < 0 || index >= deviceCount)
    {
        std::fprintf(stderr, "Index out of range.\n");
        return -1;
    }
    return index;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
    {
        return 0;
    }

    if (!hik::MvsCamera::initSdk())
    {
        return 1;
    }

    int rc = 0;
    do
    {
        hik::MvsCamera camera;

        int count = camera.enumerateDevices();
        if (count <= 0)
        {
            std::printf("No cameras found.\n"
                        "  - Check the cable / power.\n"
                        "  - For GigE, make sure the camera and NIC are in the same subnet\n"
                        "    (see /opt/MVS/bin Ip_Configurator).\n");
            rc = 2;
            break;
        }
        printDeviceList(camera.devices());

        int index = opt.index;
        if (index < 0)
        {
            index = promptForIndex(count);
            if (index < 0)
            {
                rc = 1;
                break;
            }
        }
        else if (index >= count)
        {
            std::fprintf(stderr, "Requested index %d but only %d device(s) found.\n", index, count);
            rc = 1;
            break;
        }

        if (!camera.open(static_cast<unsigned int>(index)))
        {
            rc = 1;
            break;
        }
        std::printf("Opened device [%d]: %s (SN %s)\n",
                    index,
                    camera.devices()[index].modelName.c_str(),
                    camera.devices()[index].serial.c_str());

        // Decide whether we get a live preview window.
        bool wantDisplay = !opt.noDisplay && hik::ImageWindow::displaySupported();
        hik::ImageWindow window(752, 480, "HikCamtest - live preview");
        bool haveWindow = wantDisplay && window.isValid();

        // Ensure the user always gets something to look at: if there is no live
        // window and no snapshot was requested, default to snapshot.png.
        std::string snapshotPath = opt.snapshot;
        if (snapshotPath.empty() && !haveWindow)
        {
            snapshotPath = "snapshot.png";
        }
        hik::SaveFormat snapshotFormat = hik::SaveFormat::Png;
        if (!snapshotPath.empty() && !formatFromPath(snapshotPath, snapshotFormat))
        {
            std::fprintf(stderr, "Unknown snapshot extension in '%s', using PNG.\n",
                         snapshotPath.c_str());
            snapshotFormat = hik::SaveFormat::Png;
            snapshotPath += ".png";
        }

        if (!camera.startGrabbing())
        {
            rc = 1;
            break;
        }

        // When streaming without a frame limit, let the user stop with Enter.
        std::atomic<bool> stop(false);
        std::thread inputThread;
        const bool waitForEnter = (opt.frames <= 0);
        if (waitForEnter)
        {
            std::printf("\nStreaming... press Enter to stop.\n\n");
            inputThread = std::thread([&stop]() {
                std::getchar();
                stop.store(true);
            });
        }
        else
        {
            std::printf("\nStreaming %ld frame(s)...\n\n", opt.frames);
        }

        long grabbed = 0;
        bool snapshotSaved = snapshotPath.empty();
        while (!stop.load())
        {
            if (haveWindow && !window.pumpEvents())
            {
                break;  // window closed by user
            }

            MV_FRAME_OUT frame;
            if (!camera.getFrame(frame, 1000))
            {
                // Timeout or transient error; keep trying while streaming.
                continue;
            }

            ++grabbed;
            std::printf("frame #%u  %ux%u  len=%llu bytes  pixel=0x%08x\n",
                        frame.stFrameInfo.nFrameNum,
                        frame.stFrameInfo.nExtendWidth,
                        frame.stFrameInfo.nExtendHeight,
                        static_cast<unsigned long long>(frame.stFrameInfo.nFrameLenEx),
                        static_cast<unsigned int>(frame.stFrameInfo.enPixelType));

            if (haveWindow)
            {
                camera.displayFrame(window.nativeHandle(), frame);
            }

            if (!snapshotSaved)
            {
                if (camera.saveFrame(frame, snapshotPath, snapshotFormat))
                {
                    std::printf("Saved snapshot to '%s'\n", snapshotPath.c_str());
                }
                snapshotSaved = true;
            }

            camera.freeFrame(frame);

            if (opt.frames > 0 && grabbed >= opt.frames)
            {
                break;
            }
        }

        stop.store(true);
        if (inputThread.joinable())
        {
            std::printf("\nStopping... (press Enter if the prompt is still waiting)\n");
            inputThread.join();
        }

        std::printf("Grabbed %ld frame(s) in total.\n", grabbed);
        camera.stopGrabbing();
        camera.close();
    } while (false);

    hik::MvsCamera::finalizeSdk();
    return rc;
}
