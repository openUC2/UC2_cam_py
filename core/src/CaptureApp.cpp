#include "CaptureApp.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <string>
#include <thread>

#include "ImageWindow.h"

namespace hik {

namespace {

// Map a file extension to an SDK save format. Returns false for unknown types.
bool formatFromPath(const std::string& path, SaveFormat& out)
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
    if (ext == "bmp")                    { out = SaveFormat::Bmp;  return true; }
    if (ext == "jpg" || ext == "jpeg")   { out = SaveFormat::Jpeg; return true; }
    if (ext == "png")                    { out = SaveFormat::Png;  return true; }
    if (ext == "tif" || ext == "tiff")   { out = SaveFormat::Tiff; return true; }
    return false;
}

// Print the enumerated devices as a compact table.
void printDeviceList(const std::vector<DeviceInfo>& devices)
{
    std::printf("\nFound %zu device(s):\n", devices.size());
    for (size_t i = 0; i < devices.size(); ++i)
    {
        const DeviceInfo& d = devices[i];
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

std::vector<DeviceInfo> listDevices()
{
    MvsCamera camera;
    if (camera.enumerateDevices() < 0)
    {
        return {};
    }
    return camera.devices();
}

int runCapture(const CaptureOptions& opts)
{
    MvsCamera camera;

    int count = camera.enumerateDevices();
    if (count <= 0)
    {
        std::printf("No cameras found.\n"
                    "  - Check the cable / power.\n"
                    "  - For GigE, make sure the camera and NIC are in the same subnet\n"
                    "    (see /opt/MVS/bin Ip_Configurator).\n");
        return 2;
    }
    printDeviceList(camera.devices());

    int index = opts.index;
    if (index < 0)
    {
        index = promptForIndex(count);
        if (index < 0)
        {
            return 1;
        }
    }
    else if (index >= count)
    {
        std::fprintf(stderr, "Requested index %d but only %d device(s) found.\n", index, count);
        return 1;
    }

    if (!camera.open(static_cast<unsigned int>(index)))
    {
        return 1;
    }
    std::printf("Opened device [%d]: %s (SN %s)\n",
                index,
                camera.devices()[index].modelName.c_str(),
                camera.devices()[index].serial.c_str());

    // Decide whether we get a live preview window.
    bool wantDisplay = opts.display && ImageWindow::displaySupported();
    ImageWindow window(752, 480, "HikCamtest - live preview");
    bool haveWindow = wantDisplay && window.isValid();

    // Ensure the user always gets something to look at: if there is no live
    // window and no snapshot was requested, default to snapshot.png.
    std::string snapshotPath = opts.snapshot;
    if (snapshotPath.empty() && !haveWindow)
    {
        snapshotPath = "snapshot.png";
    }
    SaveFormat snapshotFormat = SaveFormat::Png;
    if (!snapshotPath.empty() && !formatFromPath(snapshotPath, snapshotFormat))
    {
        std::fprintf(stderr, "Unknown snapshot extension in '%s', using PNG.\n",
                     snapshotPath.c_str());
        snapshotFormat = SaveFormat::Png;
        snapshotPath += ".png";
    }

    if (!camera.startGrabbing())
    {
        return 1;
    }

    // When streaming without a frame limit, let the user stop with Enter.
    std::atomic<bool> stop(false);
    std::thread inputThread;
    const bool waitForEnter = (opts.frames <= 0);
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
        std::printf("\nStreaming %ld frame(s)...\n\n", opts.frames);
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

        if (opts.frames > 0 && grabbed >= opts.frames)
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
    return 0;
}

} // namespace hik
