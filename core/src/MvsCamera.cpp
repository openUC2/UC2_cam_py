#include "MvsCamera.h"
#include "hikLog.h"
#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "MvsError.h"

namespace hik {

namespace {

// Directory of the libMvCameraControl this process actually loaded
// (<root>/lib/<arch>), or "" if it cannot be told. Taken from the library, not
// from MVCAM_SDK_PATH: an MVS installer exports that system-wide, and it may
// name another SDK than the one this program was linked against.
std::string sdkLibDir()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&MV_CC_Initialize), &info) == 0 || info.dli_fname == nullptr)
    {
        return std::string();
    }
    const std::string path = info.dli_fname;
    const size_t slash = path.rfind('/');
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

// Point the SDK's own variables at the SDK this process loaded, overwriting
// what an MVS installation exported (/opt/MVS): its parts must not be mixed in.
// The plug-ins are found next to libMvCameraControl anyway; these only matter
// for the SDK's remaining look-ups. ALLUSERSPROFILE (the GenICam XML cache) is
// not set here - the SDK reads it while the library loads, before any of our
// code runs, so the Python package sets it before it imports _hik.
void setupSdkEnv()
{
    const std::string lib = sdkLibDir();
    const size_t cut = lib.rfind("/lib/");
    if (cut == std::string::npos)
    {
        return;
    }
    const std::string root = lib.substr(0, cut);
    setenv("MVCAM_SDK_PATH", root.c_str(), 1);
    setenv("MVCAM_COMMON_RUNENV", (root + "/lib").c_str(), 1);
    setenv("MVCAM_GENICAM_CLPROTOCOL", (root + "/lib/CLProtocol").c_str(), 1);
    DBG("Using the MVS SDK in " + root);
}

// Preload the SDK renderer so the live display works without LD_LIBRARY_PATH.
// libMvCameraControl loads its renderer via dlopen("libMVRender.so") (bare
// soname), which only resolves when the MVS lib dir is on the loader path.
// Loading it here by absolute path with RTLD_GLOBAL makes the SDK's later
// dlopen reuse the already-loaded object (matched by SONAME). Best effort:
// failure just means the preview window is unavailable.
void preloadRenderLib()
{
    std::string path = sdkLibDir() + "/libMVRender.so";
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h != nullptr) { DBG("Loaded SDK renderer from: " + path); }
    else              { DBG("SDK renderer unavailable: " + std::string(dlerror()));}
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

// Reference held by initSdk() and released by finalizeSdk(), so callers that
// use the explicit API keep the SDK alive without owning an MvsCamera.
std::shared_ptr<MvsSdk> g_explicitSdk;

} // namespace

// ---------------------------------------------------------------------------
// Process-wide SDK lifetime
// ---------------------------------------------------------------------------

std::shared_ptr<MvsSdk> MvsSdk::acquire()
{
    static std::mutex            mtx;
    static std::weak_ptr<MvsSdk> weak;

    std::lock_guard<std::mutex> lock(mtx);
    if (std::shared_ptr<MvsSdk> alive = weak.lock())
    {
        return alive;
    }

    DBG("Initializing SDK...");
    setupSdkEnv();
    int ret = MV_CC_Initialize();
    if (ret != MV_OK)
    {
        ERR("MV_CC_Initialize failed: " + mvsErrorLabel(ret));
        return nullptr;
    }
    preloadRenderLib();
    DBG("SDK initialized successfully.");

    // Not make_shared: the constructor is private.
    std::shared_ptr<MvsSdk> sdk(new MvsSdk());
    weak = sdk;
    return sdk;
}

MvsSdk::~MvsSdk()
{
    DBG("Finalizing SDK.");
    MV_CC_Finalize();
}

// ---------------------------------------------------------------------------
// MvsCamera
// ---------------------------------------------------------------------------

MvsCamera::MvsCamera()
    : m_sdk(MvsSdk::acquire())
{
    if (!m_sdk)
    {
        ERR("MvsCamera: SDK initialisation failed; this camera is unusable");
    }
}

// Member order in the class is m_handle, m_sdk, m_grabbing, m_info, m_devices;
// the init list must follow it (-Wreorder).
MvsCamera::MvsCamera(MvsCamera&& other) noexcept
    : m_handle(other.m_handle)          // Handle übernehmen
    , m_sdk(std::move(other.m_sdk))
    , m_grabbing(other.m_grabbing)
    , m_info(std::move(other.m_info))
    , m_devices(std::move(other.m_devices))
{
    other.m_handle   = nullptr;         // Quelle entwaffnen
    other.m_grabbing = false;
}

MvsCamera& MvsCamera::operator=(MvsCamera&& other) noexcept
{
    if (this != &other)                 // Selbstzuweisung abfangen
    {
        close();                        // eigene Ressourcen erst freigeben

        m_sdk      = std::move(other.m_sdk);
        m_handle   = other.m_handle;
        m_info     = std::move(other.m_info);
        m_grabbing = other.m_grabbing;
        m_devices  = std::move(other.m_devices);

        other.m_handle   = nullptr;
        other.m_grabbing = false;
    }
    return *this;
}

MvsCamera::~MvsCamera()
{
    close();
}

bool MvsCamera::initSdk()
{
    g_explicitSdk = MvsSdk::acquire();
    return g_explicitSdk != nullptr;
}

std::string MvsCamera::sdkPath()
{
    const std::string lib = sdkLibDir();
    const size_t cut = lib.rfind("/lib/");
    return (cut == std::string::npos) ? lib : lib.substr(0, cut);
}

void MvsCamera::finalizeSdk()
{
    // Only drops the explicit reference: MV_CC_Finalize() runs once the last
    // MvsCamera holding the SDK is gone too.
    g_explicitSdk.reset();
}

int MvsCamera::enumerateDevices(unsigned int layers)
{
    if (!m_sdk)
    {
        ERR("enumerateDevices: SDK not initialised");
        return -1;
    }

    m_devices.clear();

    MV_CC_DEVICE_INFO_LIST deviceList;
    std::memset(&deviceList, 0, sizeof(deviceList));

    int ret = MV_CC_EnumDevices(layers, &deviceList);
    if (ret != MV_OK)
    {
        ERR("MV_CC_EnumDevices failed: "+ mvsErrorLabel(ret));
        return -1;
    }
    DBG("Found " + std::to_string(deviceList.nDeviceNum) + " devices.");
    for (unsigned int i = 0; i < deviceList.nDeviceNum; ++i)
    {
        MV_CC_DEVICE_INFO* p = deviceList.pDeviceInfo[i];
        if (p == nullptr)
        {
            continue;
        }

        DeviceInfo info;
        // Own copy of the descriptor: deviceList.pDeviceInfo[] points into SDK
        // memory that the next MV_CC_EnumDevices() call invalidates.
        info.raw = *p;
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
        ERR("open: a device is already open");
        return false;
    }
    if (index >= m_devices.size())
    {
        ERR("open: index " + std::to_string(index) + " out of range (" +
            std::to_string(m_devices.size()) + " devices)");
        return false;
    }

    // MV_CC_CreateHandle() copies the descriptor into its own resources, so the
    // address only has to stay valid for the duration of the call.
    MV_CC_DEVICE_INFO* selected = &m_devices[index].raw;

    int ret = MV_CC_CreateHandle(&m_handle, selected);
    if (ret != MV_OK)
    {
        ERR("MV_CC_CreateHandle failed: " + mvsErrorLabel(ret));
        m_handle = nullptr;
        return false;
    }

    ret = MV_CC_OpenDevice(m_handle);
    if (ret != MV_OK)
    {
        ERR("MV_CC_OpenDevice failed: " + mvsErrorLabel(ret));
        MV_CC_DestroyHandle(m_handle);
        m_handle = nullptr;
        return false;
    }

    m_info = m_devices[index];

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
                WARN("set packet size failed: " + mvsErrorLabel(ret));
            }
        }
        else
        {
            WARN("get optimal packet size failed: " + mvsErrorLabel(packetSize));
        }
    }

    // Continuous (free-run) acquisition: turn the hardware trigger off.
    ret = MV_CC_SetEnumValue(m_handle, "TriggerMode", MV_TRIGGER_MODE_OFF);
    if (ret != MV_OK)
    {
        WARN("set TriggerMode=off failed: " + mvsErrorLabel(ret));
    }

    DBG("Opened device [" + std::to_string(index) + "]: " + m_info.modelName +
        " (SN " + m_info.serial + ")");
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
    m_info   = DeviceInfo{};
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
        ERR("MV_CC_StartGrabbing failed: " + mvsErrorLabel(ret));
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
        ERR("MV_CC_StopGrabbing failed: " + mvsErrorLabel(ret));
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
        ERR("MV_CC_DisplayOneFrameEx2 failed: " + mvsErrorLabel(ret));
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
        ERR("MV_CC_SaveImageToFileEx failed: " + mvsErrorLabel(ret));
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

MV_XML_AccessMode MvsCamera::accessMode(const char* key)
{
    if (m_handle == nullptr)
    {
        return AM_NI;
    }
    MV_XML_AccessMode mode = AM_NI;
    if (MV_XML_GetNodeAccessMode(m_handle, key, &mode) != MV_OK)
    {
        return AM_NI;
    }
    return mode;
}

bool MvsCamera::isWritable(const char* key)
{
    const MV_XML_AccessMode mode = accessMode(key);
    return mode == AM_RW || mode == AM_WO;
}

MV_XML_InterfaceType MvsCamera::interfaceType(const char* key)
{
    MV_XML_InterfaceType type = IFT_IValue;
    if (m_handle == nullptr || MV_XML_GetNodeInterfaceType(m_handle, key, &type) != MV_OK)
    {
        return IFT_IValue;
    }
    return type;
}

bool MvsCamera::tryGetFloat(const char* key, double* cur, double* min, double* max)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    MVCC_FLOATVALUE value;
    std::memset(&value, 0, sizeof(value));
    if (MV_CC_GetFloatValue(m_handle, key, &value) != MV_OK)
    {
        return false;
    }
    if (cur != nullptr) { *cur = value.fCurValue; }
    if (min != nullptr) { *min = value.fMin; }
    if (max != nullptr) { *max = value.fMax; }
    return true;
}

bool MvsCamera::tryGetInt(const char* key, int64_t* cur, int64_t* min, int64_t* max, int64_t* inc)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    MVCC_INTVALUE_EX value;
    std::memset(&value, 0, sizeof(value));
    if (MV_CC_GetIntValueEx(m_handle, key, &value) != MV_OK)
    {
        return false;
    }
    if (cur != nullptr) { *cur = value.nCurValue; }
    if (min != nullptr) { *min = value.nMin; }
    if (max != nullptr) { *max = value.nMax; }
    if (inc != nullptr) { *inc = value.nInc; }
    return true;
}

bool MvsCamera::tryGetEnum(const char* key, unsigned int* cur, std::vector<unsigned int>* supported)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    MVCC_ENUMVALUE value;
    std::memset(&value, 0, sizeof(value));
    if (MV_CC_GetEnumValue(m_handle, key, &value) != MV_OK)
    {
        return false;
    }
    if (cur != nullptr) { *cur = value.nCurValue; }
    if (supported != nullptr)
    {
        supported->assign(value.nSupportValue,
                          value.nSupportValue + std::min<unsigned int>(value.nSupportedNum,
                                                                       MV_MAX_XML_SYMBOLIC_NUM));
    }
    return true;
}

bool MvsCamera::tryGetBool(const char* key, bool* cur)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    bool value = false;
    if (MV_CC_GetBoolValue(m_handle, key, &value) != MV_OK)
    {
        return false;
    }
    if (cur != nullptr) { *cur = value; }
    return true;
}

bool MvsCamera::setFloatNode(const char* key, float value)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    int ret = MV_CC_SetFloatValue(m_handle, key, value);
    if (ret != MV_OK)
    {
        ERR(std::string("MV_CC_SetFloatValue(") + key + ") failed: " + mvsErrorLabel(ret));
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
        ERR(std::string("MV_CC_GetFloatValue(") + key + ") failed: " + mvsErrorLabel(ret));
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
        ERR(std::string("MV_CC_SetIntValueEx(") + key + ") failed: " + mvsErrorLabel(ret));
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
        ERR(std::string("MV_CC_GetIntValueEx(") + key + ") failed: " + mvsErrorLabel(ret));
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
        ERR(std::string("MV_CC_SetEnumValue(") + key + ") failed: " + mvsErrorLabel(ret));
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
        ERR(std::string("MV_CC_GetEnumValue(") + key + ") failed: " + mvsErrorLabel(ret));
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
        ERR(std::string("MV_CC_SetBoolValue(") + key + ") failed: " + mvsErrorLabel(ret));
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
