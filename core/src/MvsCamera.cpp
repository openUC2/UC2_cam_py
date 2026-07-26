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

// ---------------------------------------------------------------------------
// Camera parameters
// ---------------------------------------------------------------------------

int64_t alignRoiValue(int64_t value, int64_t min, int64_t max, int64_t inc)
{
    if (inc < 1)
    {
        inc = 1;
    }
    if (value < min)
    {
        value = min;
    }
    if (value > max)
    {
        value = max;
    }
    // Values must lie on the grid min + k*inc; round the request down onto it.
    return min + ((value - min) / inc) * inc;
}

// --- Generic GenICam node accessors ----------------------------------------

bool MvsCamera::setFloatNode(const char* key, float value)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    int ret = MV_CC_SetFloatValue(m_handle, key, value);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_SetFloatValue(%s) failed: %s\n", key, mvsErrorLabel(ret).c_str());
        return false;
    }
    return true;
}

bool MvsCamera::getFloatNode(const char* key, float* cur, float* min, float* max)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    MVCC_FLOATVALUE value;
    std::memset(&value, 0, sizeof(value));
    int ret = MV_CC_GetFloatValue(m_handle, key, &value);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_GetFloatValue(%s) failed: %s\n", key, mvsErrorLabel(ret).c_str());
        return false;
    }
    if (cur != nullptr) { *cur = value.fCurValue; }
    if (min != nullptr) { *min = value.fMin; }
    if (max != nullptr) { *max = value.fMax; }
    return true;
}

bool MvsCamera::setIntNode(const char* key, int64_t value)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    int ret = MV_CC_SetIntValueEx(m_handle, key, value);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_SetIntValueEx(%s) failed: %s\n", key, mvsErrorLabel(ret).c_str());
        return false;
    }
    return true;
}

bool MvsCamera::getIntNode(const char* key, int64_t* cur, int64_t* min, int64_t* max, int64_t* inc)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    MVCC_INTVALUE_EX value;
    std::memset(&value, 0, sizeof(value));
    int ret = MV_CC_GetIntValueEx(m_handle, key, &value);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_GetIntValueEx(%s) failed: %s\n", key, mvsErrorLabel(ret).c_str());
        return false;
    }
    if (cur != nullptr) { *cur = value.nCurValue; }
    if (min != nullptr) { *min = value.nMin; }
    if (max != nullptr) { *max = value.nMax; }
    if (inc != nullptr) { *inc = value.nInc; }
    return true;
}

bool MvsCamera::setEnumNode(const char* key, unsigned int value)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    int ret = MV_CC_SetEnumValue(m_handle, key, value);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_SetEnumValue(%s) failed: %s\n", key, mvsErrorLabel(ret).c_str());
        return false;
    }
    return true;
}

bool MvsCamera::getEnumNode(const char* key, unsigned int* cur)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    MVCC_ENUMVALUE value;
    std::memset(&value, 0, sizeof(value));
    int ret = MV_CC_GetEnumValue(m_handle, key, &value);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_GetEnumValue(%s) failed: %s\n", key, mvsErrorLabel(ret).c_str());
        return false;
    }
    if (cur != nullptr) { *cur = value.nCurValue; }
    return true;
}

bool MvsCamera::setBoolNode(const char* key, bool value)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    int ret = MV_CC_SetBoolValue(m_handle, key, value);
    if (ret != MV_OK)
    {
        std::fprintf(stderr, "MV_CC_SetBoolValue(%s) failed: %s\n", key, mvsErrorLabel(ret).c_str());
        return false;
    }
    return true;
}

// --- Exposure --------------------------------------------------------------

bool MvsCamera::setExposureTime(double microseconds)
{
    setExposureAuto(AutoMode::Off);   // best-effort: manual value only sticks with auto off
    return setFloatNode("ExposureTime", static_cast<float>(microseconds));
}

bool MvsCamera::getExposureTime(double& microseconds)
{
    float cur = 0.0f;
    if (!getFloatNode("ExposureTime", &cur, nullptr, nullptr))
    {
        return false;
    }
    microseconds = cur;
    return true;
}

bool MvsCamera::getExposureTimeRange(double& minUs, double& maxUs)
{
    float mn = 0.0f, mx = 0.0f;
    if (!getFloatNode("ExposureTime", nullptr, &mn, &mx))
    {
        return false;
    }
    minUs = mn;
    maxUs = mx;
    return true;
}

// --- Gain ------------------------------------------------------------------

bool MvsCamera::setGain(double db)
{
    setGainAuto(AutoMode::Off);   // best-effort
    return setFloatNode("Gain", static_cast<float>(db));
}

bool MvsCamera::getGain(double& db)
{
    float cur = 0.0f;
    if (!getFloatNode("Gain", &cur, nullptr, nullptr))
    {
        return false;
    }
    db = cur;
    return true;
}

bool MvsCamera::getGainRange(double& minDb, double& maxDb)
{
    float mn = 0.0f, mx = 0.0f;
    if (!getFloatNode("Gain", nullptr, &mn, &mx))
    {
        return false;
    }
    minDb = mn;
    maxDb = mx;
    return true;
}

// --- Frame rate ------------------------------------------------------------

bool MvsCamera::setFrameRate(double fps)
{
    setBoolNode("AcquisitionFrameRateEnable", true);   // best-effort
    return setFloatNode("AcquisitionFrameRate", static_cast<float>(fps));
}

bool MvsCamera::getFrameRate(double& fps)
{
    float cur = 0.0f;
    if (!getFloatNode("AcquisitionFrameRate", &cur, nullptr, nullptr))
    {
        return false;
    }
    fps = cur;
    return true;
}

bool MvsCamera::getResultingFrameRate(double& fps)
{
    float cur = 0.0f;
    if (!getFloatNode("ResultingFrameRate", &cur, nullptr, nullptr))
    {
        return false;
    }
    fps = cur;
    return true;
}

bool MvsCamera::getFrameRateRange(double& minFps, double& maxFps)
{
    float mn = 0.0f, mx = 0.0f;
    if (!getFloatNode("AcquisitionFrameRate", nullptr, &mn, &mx))
    {
        return false;
    }
    minFps = mn;
    maxFps = mx;
    return true;
}

// --- Auto modes ------------------------------------------------------------

bool MvsCamera::setExposureAuto(AutoMode mode)
{
    return setEnumNode("ExposureAuto", static_cast<unsigned int>(mode));
}

bool MvsCamera::getExposureAuto(AutoMode& mode)
{
    unsigned int cur = 0;
    if (!getEnumNode("ExposureAuto", &cur))
    {
        return false;
    }
    mode = static_cast<AutoMode>(cur);
    return true;
}

bool MvsCamera::setGainAuto(AutoMode mode)
{
    return setEnumNode("GainAuto", static_cast<unsigned int>(mode));
}

bool MvsCamera::getGainAuto(AutoMode& mode)
{
    unsigned int cur = 0;
    if (!getEnumNode("GainAuto", &cur))
    {
        return false;
    }
    mode = static_cast<AutoMode>(cur);
    return true;
}

// --- Region of interest ----------------------------------------------------

bool MvsCamera::setRoi(const Roi& roi)
{
    if (m_handle == nullptr)
    {
        return false;
    }

    // 1. Zero the offsets so the full width/height range is available.
    setIntNode("OffsetX", 0);
    setIntNode("OffsetY", 0);

    // 2. Set the (aligned) size.
    int64_t cur = 0, mn = 0, mx = 0, inc = 1;
    if (!getIntNode("Width", &cur, &mn, &mx, &inc) ||
        !setIntNode("Width", alignRoiValue(roi.width, mn, mx, inc)))
    {
        return false;
    }
    if (!getIntNode("Height", &cur, &mn, &mx, &inc) ||
        !setIntNode("Height", alignRoiValue(roi.height, mn, mx, inc)))
    {
        return false;
    }

    // 3. Apply the (aligned) offsets; their max now reflects the new size.
    if (getIntNode("OffsetX", &cur, &mn, &mx, &inc))
    {
        setIntNode("OffsetX", alignRoiValue(roi.offsetX, mn, mx, inc));
    }
    if (getIntNode("OffsetY", &cur, &mn, &mx, &inc))
    {
        setIntNode("OffsetY", alignRoiValue(roi.offsetY, mn, mx, inc));
    }
    return true;
}

bool MvsCamera::getRoi(Roi& out)
{
    int64_t value = 0;
    if (!getIntNode("Width", &value, nullptr, nullptr, nullptr)) { return false; }
    out.width = value;
    if (!getIntNode("Height", &value, nullptr, nullptr, nullptr)) { return false; }
    out.height = value;
    if (!getIntNode("OffsetX", &value, nullptr, nullptr, nullptr)) { return false; }
    out.offsetX = value;
    if (!getIntNode("OffsetY", &value, nullptr, nullptr, nullptr)) { return false; }
    out.offsetY = value;
    return true;
}

bool MvsCamera::getSensorSize(int64_t& widthMax, int64_t& heightMax)
{
    int64_t w = 0, h = 0;
    if (!getIntNode("WidthMax", &w, nullptr, nullptr, nullptr) ||
        !getIntNode("HeightMax", &h, nullptr, nullptr, nullptr))
    {
        return false;
    }
    widthMax = w;
    heightMax = h;
    return true;
}

} // namespace hik
