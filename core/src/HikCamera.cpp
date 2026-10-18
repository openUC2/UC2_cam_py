#include "HikCamera.h"

#include "MvsError.h"
#include "hikLog.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace hik {

namespace {

const unsigned int kAutoOff        = 0;
const unsigned int kAutoContinuous = 2;
const unsigned int kSdkNodes       = 4;   ///< SDK buffer nodes: frames are copied out at once

bool exists(MvsCamera& cam, const char* key)
{
    const MV_XML_AccessMode mode = cam.accessMode(key);
    return mode != AM_NI && mode != AM_NA;
}

} // namespace

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

HikCamera::Opened HikCamera::openDevice(const std::string& connectionType,
                                        const std::string& serial, int index)
{
    unsigned int layers = 0;
    if (connectionType.empty())        { layers = MV_GIGE_DEVICE | MV_USB_DEVICE; }
    else if (connectionType == "USB3") { layers = MV_USB_DEVICE; }
    else if (connectionType == "GigE") { layers = MV_GIGE_DEVICE; }
    else
    {
        throw std::invalid_argument("HikCamera: unknown connection type '" + connectionType
                                    + "' (expected 'USB3' or 'GigE')");
    }

    Opened o;
    o.camera.reset(new MvsCamera());
    const std::string where = connectionType.empty() ? std::string() : " (" + connectionType + ")";
    const int n = o.camera->enumerateDevices(layers);
    if (n <= 0)
    {
        throw std::runtime_error("HikCamera: no HikRobot camera found" + where);
    }

    const std::vector<DeviceInfo>& devs = o.camera->devices();
    if (!serial.empty())
    {
        auto it = std::find_if(devs.begin(), devs.end(),
                               [&serial](const DeviceInfo& d) { return d.serial == serial; });
        if (it == devs.end())
        {
            std::string found;
            for (const DeviceInfo& d : devs)
            {
                found += (found.empty() ? "" : ", ") + d.serial;
            }
            throw std::runtime_error("HikCamera: no camera with serial '" + serial + "'" + where
                                     + " (found: " + found + ")");
        }
        o.index = static_cast<int>(it - devs.begin());
    }
    else if (index >= 0)
    {
        if (index >= n)
        {
            throw std::out_of_range("HikCamera: index " + std::to_string(index) + " out of range ("
                                    + std::to_string(n) + " camera(s)" + where + ")");
        }
        o.index = index;
    }
    else
    {
        o.index = 0;
    }

    if (!o.camera->open(static_cast<unsigned int>(o.index)))
    {
        throw std::runtime_error("HikCamera: cannot open camera " + devs[static_cast<size_t>(o.index)].serial
                                 + " - in use by another program?");
    }
    return o;
}

HikCamera::HikCamera(const std::string& connectionType, const std::string& serial, int index)
    : HikCamera(openDevice(connectionType, serial, index))
{
}

HikCamera::HikCamera(Opened&& o)
    : CameraBase("HikRobot", o.camera->info().modelName, o.camera->info().serial,
                 o.camera->info().transport, o.index)
    , m_cam(std::move(o.camera))
{
    ensureStorablePixelFormat();
    // Binning, ROI and trigger nodes turn read-only while grabbing; their
    // capability is therefore taken now and refreshed whenever stopped.
    m_canBinning = m_cam->isWritable("BinningHorizontal");
    m_canRoi     = m_cam->isWritable("Width");
}

HikCamera::~HikCamera()
{
    close();
}

void HikCamera::ensureStorablePixelFormat()
{
    const int64_t pf = pixelFormat();
    if (pixelLayout(pf).supported)
    {
        return;
    }
    std::vector<unsigned int> supported;
    m_cam->tryGetEnum("PixelFormat", nullptr, &supported);
    for (int64_t alt : unpackedAlternatives(pf))
    {
        if (std::find(supported.begin(), supported.end(), static_cast<unsigned int>(alt)) != supported.end()
            && m_cam->setEnumNode("PixelFormat", static_cast<unsigned int>(alt)))
        {
            INFO(std::string("HikCamera: pixel format ") + pixelLayout(pf).name
                 + " cannot be stored, switched to " + pixelLayout(alt).name);
            return;
        }
    }
    std::ostringstream os;
    os << "HikCamera: pixel format " << pixelLayout(pf).name << " (0x" << std::hex << pf
       << ") cannot be stored and the camera offers no unpacked alternative";
    throw std::runtime_error(os.str());
}

// ---------------------------------------------------------------------------
// Capabilities and ranges
// ---------------------------------------------------------------------------

bool HikCamera::canSetExposure()
{
    // ExposureTime is read-only while ExposureAuto runs; writing a value switches
    // auto off first, so a writable auto node counts as "settable" too.
    return m_cam->isWritable("ExposureTime")
        || (exists(*m_cam, "ExposureTime") && m_cam->isWritable("ExposureAuto"));
}

bool HikCamera::canSetGain()
{
    return m_cam->isWritable("Gain") || (exists(*m_cam, "Gain") && m_cam->isWritable("GainAuto"));
}

bool HikCamera::canSetFrameRate()
{
    return m_cam->isWritable("AcquisitionFrameRate")
        || (exists(*m_cam, "AcquisitionFrameRate") && m_cam->isWritable("AcquisitionFrameRateEnable"));
}

bool HikCamera::canSetBinning()
{
    if (!isRunning())
    {
        m_canBinning = m_cam->isWritable("BinningHorizontal");
    }
    return m_canBinning;
}

bool HikCamera::canSetTrigger()
{
    return exists(*m_cam, "TriggerMode");
}

bool HikCamera::canSetRoi()
{
    if (!isRunning())
    {
        m_canRoi = m_cam->isWritable("Width");
    }
    return m_canRoi;
}

ValueRange HikCamera::exposureRangeMs()
{
    double mn = 0.0, mx = 0.0;
    if (!canSetExposure() || !m_cam->tryGetFloat("ExposureTime", nullptr, &mn, &mx))
    {
        return ValueRange{};
    }
    return ValueRange{ true, mn / 1000.0, mx / 1000.0 };   // us -> ms
}

ValueRange HikCamera::frameRateRange()
{
    double mn = 0.0, mx = 0.0;
    if (!canSetFrameRate() || !m_cam->tryGetFloat("AcquisitionFrameRate", nullptr, &mn, &mx))
    {
        return ValueRange{};
    }
    return ValueRange{ true, mn, mx };
}

ValueRange HikCamera::gainRangeDb()
{
    double mn = 0.0, mx = 0.0;
    if (!canSetGain() || !m_cam->tryGetFloat("Gain", nullptr, &mn, &mx))
    {
        return ValueRange{};
    }
    return ValueRange{ true, mn, mx };
}

std::vector<uint32_t> HikCamera::binningValues()
{
    auto valuesOf = [this](const char* key) {
        std::vector<uint32_t> out;
        const MV_XML_InterfaceType type = m_cam->interfaceType(key);
        if (type == IFT_IEnumeration)
        {
            // HikRobot binning enumerations use the factor as the entry value.
            std::vector<unsigned int> sup;
            if (m_cam->tryGetEnum(key, nullptr, &sup))
            {
                for (unsigned int v : sup)
                {
                    if (v >= 1 && v <= 16) { out.push_back(v); }
                }
            }
        }
        else if (type == IFT_IInteger)
        {
            int64_t mn = 0, mx = 0, inc = 1;
            if (m_cam->tryGetInt(key, nullptr, &mn, &mx, &inc))
            {
                for (int64_t v = std::max<int64_t>(mn, 1); v <= std::min<int64_t>(mx, 16);
                     v += std::max<int64_t>(inc, 1))
                {
                    out.push_back(static_cast<uint32_t>(v));
                }
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    };

    std::vector<uint32_t> h = valuesOf("BinningHorizontal");
    if (h.empty())
    {
        return { 1 };
    }
    if (exists(*m_cam, "BinningVertical"))
    {
        const std::vector<uint32_t> v = valuesOf("BinningVertical");
        std::vector<uint32_t> both;
        std::set_intersection(h.begin(), h.end(), v.begin(), v.end(), std::back_inserter(both));
        h = both;
    }
    return h.empty() ? std::vector<uint32_t>{ 1 } : h;
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

bool HikCamera::getExposureMs(double& ms)
{
    double us = 0.0;
    if (!m_cam->tryGetFloat("ExposureTime", &us, nullptr, nullptr))
    {
        return false;
    }
    ms = us / 1000.0;
    return true;
}

bool HikCamera::getFrameRate(double& fps)
{
    // With the limit enabled the configured rate is what the camera aims for;
    // without it the camera runs free and only ResultingFrameRate is meaningful.
    bool enabled = true;
    m_cam->tryGetBool("AcquisitionFrameRateEnable", &enabled);
    if (!enabled && m_cam->tryGetFloat("ResultingFrameRate", &fps, nullptr, nullptr))
    {
        return true;
    }
    return m_cam->tryGetFloat("AcquisitionFrameRate", &fps, nullptr, nullptr);
}

bool HikCamera::getGainDb(double& db)
{
    return m_cam->tryGetFloat("Gain", &db, nullptr, nullptr);
}

bool HikCamera::readBinning(const char* key, uint32_t& factor)
{
    if (m_cam->interfaceType(key) == IFT_IEnumeration)
    {
        unsigned int v = 0;
        if (!m_cam->tryGetEnum(key, &v, nullptr)) { return false; }
        factor = v;
        return true;
    }
    int64_t v = 0;
    if (!m_cam->tryGetInt(key, &v, nullptr, nullptr, nullptr)) { return false; }
    factor = static_cast<uint32_t>(v);
    return true;
}

bool HikCamera::writeBinning(const char* key, uint32_t factor)
{
    if (m_cam->interfaceType(key) == IFT_IEnumeration)
    {
        return m_cam->setEnumNode(key, factor);
    }
    return m_cam->setIntNode(key, factor);
}

bool HikCamera::getBinning(uint32_t& factor)
{
    if (!exists(*m_cam, "BinningHorizontal"))
    {
        factor = 1;
        return true;
    }
    return readBinning("BinningHorizontal", factor) && factor >= 1;
}

bool HikCamera::getRoi(SensorRoi& roi)
{
    uint32_t b = 1;
    getBinning(b);
    SensorRoi d;
    if (!m_cam->tryGetInt("Width", &d.width, nullptr, nullptr, nullptr)
        || !m_cam->tryGetInt("Height", &d.height, nullptr, nullptr, nullptr))
    {
        return false;
    }
    m_cam->tryGetInt("OffsetX", &d.x, nullptr, nullptr, nullptr);
    m_cam->tryGetInt("OffsetY", &d.y, nullptr, nullptr, nullptr);
    roi = toSensor(d, b);
    return true;
}

bool HikCamera::getSensorSize(int64_t& width, int64_t& height)
{
    if (m_cam->tryGetInt("SensorWidth", &width, nullptr, nullptr, nullptr)
        && m_cam->tryGetInt("SensorHeight", &height, nullptr, nullptr, nullptr)
        && width > 0 && height > 0)
    {
        return true;
    }
    // WidthMax/HeightMax count binned pixels (SDK node table: "after binning").
    uint32_t b = 1;
    getBinning(b);
    int64_t w = 0, h = 0;
    if (!m_cam->tryGetInt("WidthMax", &w, nullptr, nullptr, nullptr)
        || !m_cam->tryGetInt("HeightMax", &h, nullptr, nullptr, nullptr) || w <= 0 || h <= 0)
    {
        return false;
    }
    width  = w * b;
    height = h * b;
    return true;
}

bool HikCamera::getImageSize(int64_t& width, int64_t& height)
{
    return m_cam->tryGetInt("Width", &width, nullptr, nullptr, nullptr)
        && m_cam->tryGetInt("Height", &height, nullptr, nullptr, nullptr);
}

int64_t HikCamera::pixelFormat()
{
    unsigned int pf = 0;
    if (!m_cam->tryGetEnum("PixelFormat", &pf, nullptr))
    {
        return 0;
    }
    return static_cast<int64_t>(pf);
}

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------

bool HikCamera::setExposureMs(double ms)
{
    if (m_cam->isWritable("ExposureAuto"))
    {
        m_cam->setEnumNode("ExposureAuto", kAutoOff);   // a manual value only sticks with auto off
    }
    const ValueRange r = exposureRangeMs();
    const double v = r.valid ? clampValue(ms, r.min, r.max) : ms;
    return m_cam->setFloatNode("ExposureTime", static_cast<float>(v * 1000.0));   // ms -> us
}

bool HikCamera::setExposureAuto()
{
    return m_cam->isWritable("ExposureAuto") && m_cam->setEnumNode("ExposureAuto", kAutoContinuous);
}

bool HikCamera::setFrameRate(double fps)
{
    if (m_cam->isWritable("AcquisitionFrameRateEnable"))
    {
        m_cam->setBoolNode("AcquisitionFrameRateEnable", true);
    }
    const ValueRange r = frameRateRange();
    const double v = r.valid ? clampValue(fps, r.min, r.max) : fps;
    return m_cam->setFloatNode("AcquisitionFrameRate", static_cast<float>(v));
}

bool HikCamera::disableFrameRateLimit()
{
    return m_cam->isWritable("AcquisitionFrameRateEnable")
        && m_cam->setBoolNode("AcquisitionFrameRateEnable", false);
}

bool HikCamera::setGainDb(double db)
{
    if (m_cam->isWritable("GainAuto"))
    {
        m_cam->setEnumNode("GainAuto", kAutoOff);
    }
    const ValueRange r = gainRangeDb();
    const double v = r.valid ? clampValue(db, r.min, r.max) : db;
    return m_cam->setFloatNode("Gain", static_cast<float>(v));
}

bool HikCamera::setGainAuto()
{
    return m_cam->isWritable("GainAuto") && m_cam->setEnumNode("GainAuto", kAutoContinuous);
}

// ---------------------------------------------------------------------------
// Geometry (only reached while stopped; CameraBase restarts around it)
// ---------------------------------------------------------------------------

bool HikCamera::doSetBinning(uint32_t factor)
{
    const uint32_t b = nearestBinning(factor, binningValues());
    if (!exists(*m_cam, "BinningHorizontal"))
    {
        return b == 1;
    }
    bool ok = writeBinning("BinningHorizontal", b);
    if (exists(*m_cam, "BinningVertical"))
    {
        ok = writeBinning("BinningVertical", b) && ok;
    }
    return ok;
}

bool HikCamera::doSetRoi(const SensorRoi& roi)
{
    uint32_t b = 1;
    getBinning(b);
    const SensorRoi d = toBinned(roi, b);

    // Offsets to zero first so the full size range is available, then the size,
    // then the offsets whose maximum now reflects the new size.
    if (m_cam->isWritable("OffsetX")) { m_cam->setIntNode("OffsetX", 0); }
    if (m_cam->isWritable("OffsetY")) { m_cam->setIntNode("OffsetY", 0); }

    int64_t cur = 0, mn = 0, mx = 0, inc = 1;
    if (!m_cam->tryGetInt("Width", &cur, &mn, &mx, &inc)
        || !m_cam->setIntNode("Width", nearestOnGrid(d.width, mn, mx, inc)))
    {
        return false;
    }
    if (!m_cam->tryGetInt("Height", &cur, &mn, &mx, &inc)
        || !m_cam->setIntNode("Height", nearestOnGrid(d.height, mn, mx, inc)))
    {
        return false;
    }
    if (m_cam->tryGetInt("OffsetX", &cur, &mn, &mx, &inc))
    {
        m_cam->setIntNode("OffsetX", nearestOnGrid(d.x, mn, mx, inc));
    }
    if (m_cam->tryGetInt("OffsetY", &cur, &mn, &mx, &inc))
    {
        m_cam->setIntNode("OffsetY", nearestOnGrid(d.y, mn, mx, inc));
    }
    return true;
}

bool HikCamera::doResetRoi()
{
    if (m_cam->isWritable("OffsetX")) { m_cam->setIntNode("OffsetX", 0); }
    if (m_cam->isWritable("OffsetY")) { m_cam->setIntNode("OffsetY", 0); }
    int64_t mx = 0;
    bool ok = true;
    if (m_cam->tryGetInt("Width", nullptr, nullptr, &mx, nullptr))  { ok = m_cam->setIntNode("Width", mx) && ok; }
    if (m_cam->tryGetInt("Height", nullptr, nullptr, &mx, nullptr)) { ok = m_cam->setIntNode("Height", mx) && ok; }
    return ok;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void HikCamera::onImage(MV_FRAME_OUT* frame, void* user, bool /*autoFree*/)
{
    if (frame == nullptr || user == nullptr)
    {
        return;
    }
    const uint64_t n = (frame->stFrameInfo.nFrameLenEx != 0) ? frame->stFrameInfo.nFrameLenEx
                                                              : frame->stFrameInfo.nFrameLen;
    static_cast<HikCamera*>(user)->deliverFrame(frame->pBufAddr, n);
}

bool HikCamera::doStartStream()
{
    void* h = m_cam->handle();
    int ret = MV_CC_SetImageNodeNum(h, kSdkNodes);
    if (ret != MV_OK)
    {
        ERR("MV_CC_SetImageNodeNum failed: " + mvsErrorLabel(ret));
        return false;
    }
    ret = MV_CC_SetGrabStrategy(h, MV_GrabStrategy_OneByOne);
    if (ret != MV_OK)
    {
        ERR("MV_CC_SetGrabStrategy failed: " + mvsErrorLabel(ret));
        return false;
    }
    // bAutoFree = true: the frame is copied into the ring inside the callback,
    // so the SDK may take its node back as soon as the callback returns.
    ret = MV_CC_RegisterImageCallBackEx2(h, &HikCamera::onImage, this, true);
    if (ret != MV_OK)
    {
        ERR("MV_CC_RegisterImageCallBackEx2 failed: " + mvsErrorLabel(ret));
        return false;
    }
    if (!m_cam->startGrabbing())
    {
        MV_CC_RegisterImageCallBackEx2(h, nullptr, nullptr, true);
        return false;
    }
    return true;
}

void HikCamera::doStopStream()
{
    if (!m_cam->isOpen())
    {
        return;
    }
    m_cam->stopGrabbing();
    MV_CC_RegisterImageCallBackEx2(m_cam->handle(), nullptr, nullptr, true);
}

void HikCamera::doClose()
{
    m_cam->close();
}

} // namespace hik
