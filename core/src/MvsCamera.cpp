#include "MvsCamera.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MvsError.h"

namespace hik {

namespace {

// Root of the MVS SDK, from the environment or the default install location.
std::string sdkRoot()
{
    const char* sdk = std::getenv("MVCAM_SDK_PATH");
    return (sdk != nullptr && *sdk != '\0') ? sdk : "/opt/MVS";
}

// Provide sane defaults for the MVS environment variables when the SDK's
// set_env_path.sh has not been sourced. setenv() with overwrite=0 respects any
// value the user already exported. Without ALLUSERSPROFILE the SDK writes its
// GenICam XML cache to a literal "$(ALLUSERSPROFILE)" directory in the CWD.
void setupSdkEnvDefaults()
{
    std::string base = sdkRoot();
    setenv("MVCAM_SDK_PATH", base.c_str(), 0);
    setenv("ALLUSERSPROFILE", (base + "/MVFG").c_str(), 0);
}

// Preload the SDK renderer so the live display works without LD_LIBRARY_PATH.
// libMvCameraControl loads its renderer via dlopen("libMVRender.so") (bare
// soname), which only resolves when the MVS lib dir is on the loader path.
// Loading it here by absolute path with RTLD_GLOBAL makes the SDK's later
// dlopen reuse the already-loaded object (matched by SONAME). Best effort:
// failure just means the preview window is unavailable.
void preloadRenderLib()
{
    std::string sub = (sizeof(void*) == 8) ? "64" : "32";
    std::string path = sdkRoot() + "/lib/" + sub + "/libMVRender.so";
    (void)dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
}

// Interpret a fixed-size unsigned char[] field as a trimmed std::string.
std::string toStr(const unsigned char* buf, size_t maxLen)
{
    // The SDK guarantees NUL-termination inside the fixed buffer.
    size_t len = 0;
    while (len < maxLen && buf[len] != '\0')
    {
        ++len;
    }
    return std::string(reinterpret_cast<const char*>(buf), len);
}

std::string formatIp(unsigned int ip)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                  (ip >> 8) & 0xff, ip & 0xff);
    return buf;
}

MV_SAVE_IAMGE_TYPE toSdkFormat(SaveFormat f)
{
    switch (f)
    {
    case SaveFormat::Bmp:  return MV_Image_Bmp;
    case SaveFormat::Jpeg: return MV_Image_Jpeg;
    case SaveFormat::Png:  return MV_Image_Png;
    case SaveFormat::Tiff: return MV_Image_Tif;
    }
    return MV_Image_Bmp;
}

} // namespace

MvsCamera::MvsCamera()
    : m_handle(nullptr)
    , m_deviceList{}
    , m_grabbing(false)
{
    std::memset(&m_deviceList, 0, sizeof(m_deviceList));
}

MvsCamera::~MvsCamera()
{
    close();
}

bool MvsCamera::initSdk()
{
    setupSdkEnvDefaults();
    int ret = MV_CC_Initialize();
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_Initialize failed: %s\n", mvsErrorLabel(ret).c_str());
        return false;
    }
    preloadRenderLib();
    return true;
}

void MvsCamera::finalizeSdk()
{
    MV_CC_Finalize();
}

int MvsCamera::enumerateDevices()
{
    m_devices.clear();
    std::memset(&m_deviceList, 0, sizeof(m_deviceList));

    const unsigned int layers = MV_GIGE_DEVICE | MV_USB_DEVICE;
    int ret = MV_CC_EnumDevices(layers, &m_deviceList);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_EnumDevices failed: %s\n", mvsErrorLabel(ret).c_str());
        return -1;
    }

    for (unsigned int i = 0; i < m_deviceList.nDeviceNum; ++i)
    {
        MV_CC_DEVICE_INFO* p = m_deviceList.pDeviceInfo[i];
        if (p == nullptr)
        {
            continue;
        }

        DeviceInfo info;
        if (p->nTLayerType == MV_GIGE_DEVICE)
        {
            const auto& g = p->SpecialInfo.stGigEInfo;
            info.transport = "GigE";
            info.modelName = toStr(g.chModelName, sizeof(g.chModelName));
            info.serial    = toStr(g.chSerialNumber, sizeof(g.chSerialNumber));
            info.userName  = toStr(g.chUserDefinedName, sizeof(g.chUserDefinedName));
            info.ip        = formatIp(g.nCurrentIp);
        }
        else if (p->nTLayerType == MV_USB_DEVICE)
        {
            const auto& u = p->SpecialInfo.stUsb3VInfo;
            info.transport = "USB3";
            info.modelName = toStr(u.chModelName, sizeof(u.chModelName));
            info.serial    = toStr(u.chSerialNumber, sizeof(u.chSerialNumber));
            info.userName  = toStr(u.chUserDefinedName, sizeof(u.chUserDefinedName));
        }
        else
        {
            info.transport = "other";
        }
        m_devices.push_back(std::move(info));
    }

    return static_cast<int>(m_devices.size());
}

bool MvsCamera::open(unsigned int index)
{
    if (m_handle != nullptr)
    {
        std::fprintf(stderr, "open: a device is already open\n");
        return false;
    }
    if (index >= m_deviceList.nDeviceNum)
    {
        std::fprintf(stderr, "open: index %u out of range (%u devices)\n",
                     index, m_deviceList.nDeviceNum);
        return false;
    }

    MV_CC_DEVICE_INFO* selected = m_deviceList.pDeviceInfo[index];

    int ret = MV_CC_CreateHandle(&m_handle, selected);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_CreateHandle failed: %s\n", mvsErrorLabel(ret).c_str());
        m_handle = nullptr;
        return false;
    }

    ret = MV_CC_OpenDevice(m_handle);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_OpenDevice failed: %s\n", mvsErrorLabel(ret).c_str());
        MV_CC_DestroyHandle(m_handle);
        m_handle = nullptr;
        return false;
    }

    // For GigE cameras, negotiate the optimal streaming packet size. This is a
    // performance tweak only, so warnings are non-fatal.
    if (selected->nTLayerType == MV_GIGE_DEVICE)
    {
        int packetSize = MV_CC_GetOptimalPacketSize(m_handle);
        if (packetSize > 0)
        {
            ret = MV_CC_SetIntValueEx(m_handle, "GevSCPSPacketSize", packetSize);
            if (ret != MV_OK)
            {
                std::fprintf(stderr, "Warning: set packet size failed: %s\n",
                             mvsErrorLabel(ret).c_str());
            }
        }
        else
        {
            std::fprintf(stderr, "Warning: get optimal packet size failed: %s\n",
                         mvsErrorLabel(packetSize).c_str());
        }
    }

    // Continuous (free-run) acquisition: turn the hardware trigger off.
    ret = MV_CC_SetEnumValue(m_handle, "TriggerMode", MV_TRIGGER_MODE_OFF);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "Warning: set TriggerMode=off failed: %s\n",
                     mvsErrorLabel(ret).c_str());
    }

    return true;
}

void MvsCamera::close()
{
    if (m_handle == nullptr)
    {
        return;
    }
    if (m_grabbing)
    {
        stopGrabbing();
    }
    MV_CC_CloseDevice(m_handle);
    MV_CC_DestroyHandle(m_handle);
    m_handle = nullptr;
}

bool MvsCamera::startGrabbing()
{
    if (m_handle == nullptr)
    {
        return false;
    }
    if (m_grabbing)
    {
        return true;
    }
    int ret = MV_CC_StartGrabbing(m_handle);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_StartGrabbing failed: %s\n", mvsErrorLabel(ret).c_str());
        return false;
    }
    m_grabbing = true;
    return true;
}

bool MvsCamera::stopGrabbing()
{
    if (m_handle == nullptr || !m_grabbing)
    {
        return true;
    }
    int ret = MV_CC_StopGrabbing(m_handle);
    m_grabbing = false;
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_StopGrabbing failed: %s\n", mvsErrorLabel(ret).c_str());
        return false;
    }
    return true;
}

bool MvsCamera::getFrame(MV_FRAME_OUT& frame, unsigned int timeoutMs)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    std::memset(&frame, 0, sizeof(frame));
    int ret = MV_CC_GetImageBuffer(m_handle, &frame, timeoutMs);
    return ret == MV_OK;
}

void MvsCamera::freeFrame(MV_FRAME_OUT& frame)
{
    if (m_handle != nullptr && frame.pBufAddr != nullptr)
    {
        MV_CC_FreeImageBuffer(m_handle, &frame);
    }
}

bool MvsCamera::displayFrame(void* windowHandle, const MV_FRAME_OUT& frame)
{
    if (m_handle == nullptr || windowHandle == nullptr || frame.pBufAddr == nullptr)
    {
        return false;
    }

    MV_CC_IMAGE image;
    std::memset(&image, 0, sizeof(image));
    image.nWidth      = frame.stFrameInfo.nExtendWidth;
    image.nHeight     = frame.stFrameInfo.nExtendHeight;
    image.enPixelType = frame.stFrameInfo.enPixelType;
    image.pImageBuf   = frame.pBufAddr;
    image.nImageLen   = frame.stFrameInfo.nFrameLenEx;

    // enRenderMode 0 => OpenGL on Linux (SDK default).
    int ret = MV_CC_DisplayOneFrameEx2(m_handle, windowHandle, &image, 0);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_DisplayOneFrameEx2 failed: %s\n", mvsErrorLabel(ret).c_str());
        return false;
    }
    return true;
}

bool MvsCamera::saveFrame(const MV_FRAME_OUT& frame, const std::string& path, SaveFormat format)
{
    if (m_handle == nullptr || frame.pBufAddr == nullptr)
    {
        return false;
    }

    MV_SAVE_IMAGE_TO_FILE_PARAM_EX param;
    std::memset(&param, 0, sizeof(param));
    param.nWidth      = frame.stFrameInfo.nExtendWidth;
    param.nHeight     = frame.stFrameInfo.nExtendHeight;
    param.enPixelType = frame.stFrameInfo.enPixelType;
    param.pData       = frame.pBufAddr;
    param.nDataLen    = static_cast<unsigned int>(frame.stFrameInfo.nFrameLenEx);
    param.enImageType = toSdkFormat(format);
    param.pcImagePath = const_cast<char*>(path.c_str());
    param.nQuality    = 90;   // used for JPEG only
    param.iMethodValue = 1;   // balanced Bayer interpolation

    int ret = MV_CC_SaveImageToFileEx(m_handle, &param);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_SaveImageToFileEx failed: %s\n", mvsErrorLabel(ret).c_str());
        return false;
    }
    return true;
}

} // namespace hik
