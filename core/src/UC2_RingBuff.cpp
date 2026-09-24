#include "UC2_RingBuff.h"

#include "hikLog.h"

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <new>
#include <sstream>

namespace hik {

// ---------------------------------------------------------------------------
// Layout invariants
//
// These asserts are what keeps the region mappable into a second process later:
// every struct has a fixed, architecture-independent size, and every field the
// two sides touch concurrently is lock-free on all three target ISAs.
// ---------------------------------------------------------------------------

static_assert(sizeof(UC2_SessionHeader) == 8448, "session header must stay 8448 bytes");
static_assert(sizeof(UC2_FrameHeader)   == 64,  "frame header must stay 64 bytes");
static_assert(sizeof(UC2_SlotRecord)    == 64,  "slot record must stay one cache line");
static_assert(sizeof(UC2_Consumer)      == 64,  "consumer record must stay one cache line");
static_assert(std::is_standard_layout<UC2_SessionHeader>::value, "header must be standard layout");
static_assert(std::is_standard_layout<UC2_SlotRecord>::value,    "slot record must be standard layout");
static_assert(std::is_standard_layout<UC2_Control>::value,       "control block must be standard layout");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "32-bit atomics must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");

namespace {

/// Largest region we are willing to allocate, as a guard against a bogus
/// geometry turning into a multi-terabyte request.
const uint64_t kMaxRegionBytes = 16ull << 30;   // 16 GiB

const uint32_t kMaxDimension = 65536;
const uint32_t kMaxChannels  = 4096;
const uint32_t kMaxSlots     = 65536;

/// Round @p value up to the next multiple of @p alignment (a power of two).
uint64_t alignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/// Multiply with overflow detection.
/// @return false if a * b does not fit into a uint64_t.
bool mulChecked(uint64_t a, uint64_t b, uint64_t& out)
{
    if (a != 0 && b > (UINT64_MAX / a))
    {
        return false;
    }
    out = a * b;
    return true;
}

/// Copy @p src into the fixed-size field @p dst, always NUL-terminating.
void copyFixed(char* dst, size_t cap, const std::string& src)
{
    const size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    std::memset(dst + n, 0, cap - n);
}

/// Reduce @p in to characters that are safe in a file name.
std::string sanitize(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                     || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.';
        out.push_back(ok ? c : '_');
    }
    return out;
}

/// Escape @p in for embedding in a JSON string literal.
std::string jsonEscape(const std::string& in)
{
    std::ostringstream os;
    for (unsigned char c : in)
    {
        switch (c)
        {
        case '"':  os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\n': os << "\\n";  break;
        case '\r': os << "\\r";  break;
        case '\t': os << "\\t";  break;
        default:
            if (c < 0x20)
            {
                os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                   << static_cast<int>(c) << std::dec;
            }
            else
            {
                os << static_cast<char>(c);
            }
            break;
        }
    }
    return os.str();
}

/// Write an entire iovec array, tolerating short writes and EINTR.
bool writevAll(int fd, struct iovec* iov, int count)
{
    while (count > 0)
    {
        const ssize_t n = ::writev(fd, iov, count);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;   // no progress possible
        }
        size_t left = static_cast<size_t>(n);
        while (count > 0 && left >= iov->iov_len)
        {
            left -= iov->iov_len;
            ++iov;
            --count;
        }
        if (count > 0 && left > 0)
        {
            iov->iov_base = static_cast<uint8_t*>(iov->iov_base) + left;
            iov->iov_len -= left;
        }
    }
    return true;
}

extern "C" {

void* heapAcquire(void* /*ctx*/, size_t bytes, size_t alignment)
{
    if (alignment < sizeof(void*))
    {
        alignment = sizeof(void*);
    }
    void* p = nullptr;
    if (::posix_memalign(&p, alignment, bytes) != 0)
    {
        return nullptr;
    }
    return p;
}

void heapRelease(void* /*ctx*/, void* base, size_t /*bytes*/)
{
    ::free(base);
}

} // extern "C"

} // namespace

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

const char* depthName(PixelDepth d)
{
    switch (d)
    {
    case PixelDepth::UInt8:   return "uint8";
    case PixelDepth::UInt16:  return "uint16";
    case PixelDepth::Float64: return "float64";
    }
    return "unknown";
}

UC2_MemoryOps heapMemoryOps()
{
    UC2_MemoryOps ops;
    ops.abiVersion = kAbiVersion;
    ops.ctx        = nullptr;
    ops.acquire    = &heapAcquire;
    ops.release    = &heapRelease;
    return ops;
}

int64_t monotonicNs()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t wallClockNs()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

std::string formatWallClock(int64_t wallNs)
{
    const std::time_t secs = static_cast<std::time_t>(wallNs / 1000000000ll);
    const int         ms   = static_cast<int>((wallNs % 1000000000ll) / 1000000ll);

    std::tm tmv{};
#if defined(_WIN32)
    ::gmtime_s(&tmv, &secs);
#else
    ::gmtime_r(&secs, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);

    std::ostringstream os;
    os << buf << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return os.str();
}

// ---------------------------------------------------------------------------
// RingBuffer::WriteHandle
// ---------------------------------------------------------------------------

RingBuffer::WriteHandle::~WriteHandle()
{
    if (m_owner != nullptr)
    {
        m_owner->abortWrite(*this);
    }
}

RingBuffer::WriteHandle::WriteHandle(WriteHandle&& other) noexcept
    : m_owner(other.m_owner)
    , m_data(other.m_data)
    , m_capacity(other.m_capacity)
    , m_sequence(other.m_sequence)
    , m_slot(other.m_slot)
{
    other.m_owner = nullptr;
    other.m_data  = nullptr;
}

RingBuffer::WriteHandle& RingBuffer::WriteHandle::operator=(WriteHandle&& other) noexcept
{
    if (this != &other)
    {
        if (m_owner != nullptr)
        {
            m_owner->abortWrite(*this);
        }
        m_owner    = other.m_owner;
        m_data     = other.m_data;
        m_capacity = other.m_capacity;
        m_sequence = other.m_sequence;
        m_slot     = other.m_slot;
        other.m_owner = nullptr;
        other.m_data  = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// RingBuffer::ReadHandle
// ---------------------------------------------------------------------------

RingBuffer::ReadHandle::~ReadHandle()
{
    if (m_owner != nullptr)
    {
        m_owner->releaseSlot(*this, true, false);
    }
}

RingBuffer::ReadHandle::ReadHandle(ReadHandle&& other) noexcept
    : m_owner(other.m_owner)
    , m_data(other.m_data)
    , m_nbytes(other.m_nbytes)
    , m_sequence(other.m_sequence)
    , m_tsBeforeNs(other.m_tsBeforeNs)
    , m_tsAfterNs(other.m_tsAfterNs)
    , m_slot(other.m_slot)
    , m_flags(other.m_flags)
    , m_seqStamp(other.m_seqStamp)
    , m_consumer(other.m_consumer)
{
    other.m_owner = nullptr;
    other.m_data  = nullptr;
}

RingBuffer::ReadHandle& RingBuffer::ReadHandle::operator=(ReadHandle&& other) noexcept
{
    if (this != &other)
    {
        if (m_owner != nullptr)
        {
            m_owner->releaseSlot(*this, true, false);
        }
        m_owner      = other.m_owner;
        m_data       = other.m_data;
        m_nbytes     = other.m_nbytes;
        m_sequence   = other.m_sequence;
        m_tsBeforeNs = other.m_tsBeforeNs;
        m_tsAfterNs  = other.m_tsAfterNs;
        m_slot       = other.m_slot;
        m_flags      = other.m_flags;
        m_seqStamp   = other.m_seqStamp;
        m_consumer   = other.m_consumer;
        other.m_owner = nullptr;
        other.m_data  = nullptr;
    }
    return *this;
}

bool RingBuffer::ReadHandle::stillValid() const
{
    if (m_owner == nullptr)
    {
        return false;
    }
    const UC2_SlotRecord* rec = m_owner->slotAt(m_slot);
    return rec != nullptr && rec->seq.load(std::memory_order_acquire) == m_seqStamp;
}

void RingBuffer::ReadHandle::release(bool markRead, bool markSaved)
{
    if (m_owner != nullptr)
    {
        m_owner->releaseSlot(*this, markRead, markSaved);
    }
}

// ---------------------------------------------------------------------------
// RingBuffer -- construction and configuration
// ---------------------------------------------------------------------------

RingBuffer::RingBuffer(const Config& cfg)
    : m_cfg(cfg)
    , m_mem(heapMemoryOps())
    , m_comment(cfg.comment)
    , m_writeBusy(false)
{
}

RingBuffer::~RingBuffer()
{
    stopRecording();
    m_stream.reset();     // joins the writer thread before the region goes away
    destroySinks();
    releaseRegion();
}

bool RingBuffer::setImageSize(uint32_t width, uint32_t height)
{
    if (m_configured)
    {
        ERR("RingBuffer::setImageSize: buffer is already configured");
        return false;
    }
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension)
    {
        ERR("RingBuffer::setImageSize: implausible size "
            + std::to_string(width) + "x" + std::to_string(height));
        return false;
    }
    m_width  = width;
    m_height = height;
    return true;
}

bool RingBuffer::setChannels(uint32_t channels)
{
    if (m_configured)
    {
        ERR("RingBuffer::setChannels: buffer is already configured");
        return false;
    }
    if (channels == 0 || channels > kMaxChannels)
    {
        ERR("RingBuffer::setChannels: channel count out of range: " + std::to_string(channels));
        return false;
    }
    m_channels = channels;
    return true;
}

bool RingBuffer::setDepth(PixelDepth depth)
{
    if (m_configured)
    {
        ERR("RingBuffer::setDepth: buffer is already configured");
        return false;
    }
    switch (depth)
    {
    case PixelDepth::UInt8:
    case PixelDepth::UInt16:
    case PixelDepth::Float64:
        m_depth = depth;
        return true;
    }
    ERR("RingBuffer::setDepth: unsupported depth");
    return false;
}

bool RingBuffer::setCameraId(const std::string& id)
{
    if (m_configured)
    {
        ERR("RingBuffer::setCameraId: buffer is already configured");
        return false;
    }
    if (id.size() >= sizeof(UC2_SessionHeader::cameraId))
    {
        WARN("RingBuffer::setCameraId: id truncated to 63 characters");
    }
    m_cameraId = id;
    return true;
}

bool RingBuffer::setComment(const std::string& comment)
{
    if (m_configured)
    {
        ERR("RingBuffer::setComment: buffer is already configured");
        return false;
    }
    if (comment.size() >= sizeof(UC2_SessionHeader::comment))
    {
        WARN("RingBuffer::setComment: comment truncated to "
             + std::to_string(sizeof(UC2_SessionHeader::comment) - 1) + " characters");
    }
    m_comment = comment;
    return true;
}

bool RingBuffer::setMemoryOps(const UC2_MemoryOps& ops)
{
    if (m_configured)
    {
        ERR("RingBuffer::setMemoryOps: buffer is already configured");
        return false;
    }
    if (ops.abiVersion != kAbiVersion)
    {
        ERR("RingBuffer::setMemoryOps: ABI version mismatch (got "
            + std::to_string(ops.abiVersion) + ", expected " + std::to_string(kAbiVersion) + ")");
        return false;
    }
    if (ops.acquire == nullptr || ops.release == nullptr)
    {
        ERR("RingBuffer::setMemoryOps: acquire and release are mandatory");
        return false;
    }
    m_mem = ops;
    return true;
}

bool RingBuffer::configure()
{
    if (m_configured)
    {
        return true;
    }
    if (m_width == 0 || m_height == 0)
    {
        ERR("RingBuffer::configure: image size has not been set");
        return false;
    }
    if (m_cfg.slotCount < 2 || m_cfg.slotCount > kMaxSlots)
    {
        ERR("RingBuffer::configure: slot count must be in [2, " + std::to_string(kMaxSlots)
            + "], got " + std::to_string(m_cfg.slotCount));
        return false;
    }

    // Frame geometry, with every multiplication checked: a bad ROI must fail
    // loudly here rather than wrap around into a tiny allocation.
    uint64_t frameBytes = 0;
    if (!mulChecked(m_width, m_height, frameBytes)
        || !mulChecked(frameBytes, m_channels, frameBytes)
        || !mulChecked(frameBytes, itemSize(m_depth), frameBytes))
    {
        ERR("RingBuffer::configure: frame size overflows 64 bits");
        return false;
    }

    const uint64_t slotStride    = alignUp(frameBytes, kCacheLine);
    const uint64_t headerBytes   = alignUp(sizeof(UC2_SessionHeader), kCacheLine);
    const uint64_t controlBytes  = alignUp(sizeof(UC2_Control), kCacheLine);
    const uint64_t slotTableSize = alignUp(
        static_cast<uint64_t>(m_cfg.slotCount) * sizeof(UC2_SlotRecord), kCacheLine);

    uint64_t payloadBytes = 0;
    if (!mulChecked(slotStride, m_cfg.slotCount, payloadBytes))
    {
        ERR("RingBuffer::configure: payload area overflows 64 bits");
        return false;
    }

    const uint64_t controlOffset = headerBytes;
    const uint64_t slotsOffset   = controlOffset + controlBytes;
    const uint64_t payloadOffset = slotsOffset + slotTableSize;
    const uint64_t regionBytes   = payloadOffset + payloadBytes;

    if (regionBytes > kMaxRegionBytes)
    {
        ERR("RingBuffer::configure: region of " + std::to_string(regionBytes / (1024 * 1024))
            + " MiB exceeds the " + std::to_string(kMaxRegionBytes / (1024 * 1024))
            + " MiB limit");
        return false;
    }

    uint8_t* region = static_cast<uint8_t*>(
        m_mem.acquire(m_mem.ctx, static_cast<size_t>(regionBytes), kCacheLine));
    if (region == nullptr)
    {
        ERR("RingBuffer::configure: allocation of " + std::to_string(regionBytes / (1024 * 1024))
            + " MiB failed");
        return false;
    }
    std::memset(region, 0, static_cast<size_t>(regionBytes));

    m_region        = region;
    m_regionBytes   = regionBytes;
    m_controlOffset = controlOffset;
    m_slotsOffset   = slotsOffset;
    m_payloadOffset = payloadOffset;
    m_slotStride    = slotStride;

    // Placement-new every object that lives in the region. All of them are
    // trivially destructible, so tearing the region down is just a free().
    m_header = new (region) UC2_SessionHeader();
    std::memcpy(m_header->magic, "UC2RB\0\0\0", 8);
    m_header->formatVersion = kFormatVersion;
    m_header->headerBytes   = static_cast<uint32_t>(sizeof(UC2_SessionHeader));
    m_header->width         = m_width;
    m_header->height        = m_height;
    m_header->channels      = m_channels;
    m_header->depth         = static_cast<uint8_t>(m_depth);
    m_header->frameBytes    = frameBytes;
    m_header->slotCount     = m_cfg.slotCount;
    m_header->layoutVersion = kLayoutVersion;
    m_header->startWallNs   = 0;
    m_header->startMonoNs   = 0;
    m_header->slotStride    = slotStride;
    copyFixed(m_header->cameraId, sizeof(m_header->cameraId), m_cameraId);
    copyFixed(m_header->comment,  sizeof(m_header->comment),  m_comment);

    m_control = new (region + controlOffset) UC2_Control();

    for (uint32_t i = 0; i < m_cfg.slotCount; ++i)
    {
        UC2_SlotRecord* rec = new (region + slotsOffset + i * sizeof(UC2_SlotRecord)) UC2_SlotRecord();
        rec->payloadOffset = payloadOffset + static_cast<uint64_t>(i) * slotStride;
    }

    m_configured = true;

    if (!m_cfg.storagePath.empty())
    {
        m_stream.reset(new FileStreamSink(*this, m_cfg.storagePath));
        if (!addSink(m_stream->ops()))
        {
            ERR("RingBuffer::configure: could not register the disk streamer");
            m_stream.reset();
        }
    }

    INFO("RingBuffer: " + std::to_string(m_cfg.slotCount) + " slots x "
         + std::to_string(m_width) + "x" + std::to_string(m_height) + "x"
         + std::to_string(m_channels) + " " + depthName(m_depth) + " ("
         + std::to_string(frameBytes) + " B/frame, " + std::to_string(regionBytes / (1024 * 1024))
         + " MiB total)");
    return true;
}

void RingBuffer::releaseRegion()
{
    if (m_region != nullptr)
    {
        m_mem.release(m_mem.ctx, m_region, static_cast<size_t>(m_regionBytes));
        m_region  = nullptr;
        m_header  = nullptr;
        m_control = nullptr;
    }
    m_configured = false;
}

// ---------------------------------------------------------------------------
// RingBuffer -- region accessors
// ---------------------------------------------------------------------------

UC2_SlotRecord* RingBuffer::slotAt(uint32_t index)
{
    if (!m_configured || index >= m_cfg.slotCount)
    {
        return nullptr;
    }
    return reinterpret_cast<UC2_SlotRecord*>(
        m_region + m_slotsOffset + static_cast<uint64_t>(index) * sizeof(UC2_SlotRecord));
}

const UC2_SlotRecord* RingBuffer::slotAt(uint32_t index) const
{
    return const_cast<RingBuffer*>(this)->slotAt(index);
}

uint8_t* RingBuffer::payloadAt(const UC2_SlotRecord& rec)
{
    return m_region + rec.payloadOffset;
}

const uint8_t* RingBuffer::payloadAt(const UC2_SlotRecord& rec) const
{
    return m_region + rec.payloadOffset;
}

// ---------------------------------------------------------------------------
// RingBuffer -- recording and sinks
// ---------------------------------------------------------------------------

bool RingBuffer::addSink(const UC2_SinkOps& ops)
{
    if (ops.abiVersion != kAbiVersion)
    {
        ERR("RingBuffer::addSink: ABI version mismatch (got " + std::to_string(ops.abiVersion)
            + ", expected " + std::to_string(kAbiVersion) + ")");
        return false;
    }
    if (isRecording())
    {
        ERR("RingBuffer::addSink: cannot add a sink while a session is running");
        return false;
    }
    m_sinks.push_back(ops);
    return true;
}

void RingBuffer::destroySinks()
{
    for (UC2_SinkOps& s : m_sinks)
    {
        if (s.destroy != nullptr)
        {
            s.destroy(s.ctx);
        }
    }
    m_sinks.clear();
}

bool RingBuffer::startRecording()
{
    if (!m_configured)
    {
        ERR("RingBuffer::startRecording: buffer is not configured");
        return false;
    }
    if (isRecording())
    {
        return true;
    }

    m_header->startWallNs = wallClockNs();
    m_header->startMonoNs = monotonicNs();

    size_t started = 0;
    for (UC2_SinkOps& s : m_sinks)
    {
        if (s.onSessionStart != nullptr && s.onSessionStart(s.ctx, m_header) != 0)
        {
            ERR("RingBuffer::startRecording: a sink refused to open the session");
            // Unwind the sinks that did open, so none is left half-started.
            for (size_t i = 0; i < started; ++i)
            {
                if (m_sinks[i].onSessionEnd != nullptr)
                {
                    m_sinks[i].onSessionEnd(m_sinks[i].ctx);
                }
            }
            return false;
        }
        ++started;
    }

    m_control->recording.store(1, std::memory_order_release);
    INFO("RingBuffer: recording started at " + formatWallClock(m_header->startWallNs));
    return true;
}

void RingBuffer::stopRecording()
{
    if (!m_configured || !isRecording())
    {
        return;
    }
    m_control->recording.store(0, std::memory_order_release);
    for (UC2_SinkOps& s : m_sinks)
    {
        if (s.onSessionEnd != nullptr)
        {
            s.onSessionEnd(s.ctx);
        }
    }
    INFO("RingBuffer: recording stopped after " + std::to_string(framesWritten()) + " frames");
}

bool RingBuffer::isRecording() const
{
    return m_configured && m_control->recording.load(std::memory_order_acquire) != 0;
}

void RingBuffer::dispatchFrame(const UC2_SlotRecord& rec, const uint8_t* payload, uint64_t nbytes)
{
    if (m_sinks.empty() || !isRecording())
    {
        return;
    }

    UC2_FrameRef ref;
    std::memset(&ref, 0, sizeof(ref));
    ref.data       = payload;
    ref.nbytes     = nbytes;
    ref.sequence   = rec.sequence.load(std::memory_order_relaxed);
    ref.tsBeforeNs = rec.tsBeforeNs.load(std::memory_order_relaxed);
    ref.tsAfterNs  = rec.tsAfterNs.load(std::memory_order_relaxed);
    ref.slot       = static_cast<uint32_t>(ref.sequence % m_cfg.slotCount);
    ref.width      = m_header->width;
    ref.height     = m_header->height;
    ref.channels   = m_header->channels;
    ref.depth      = m_header->depth;

    for (UC2_SinkOps& s : m_sinks)
    {
        if (s.onFrame != nullptr && s.onFrame(s.ctx, &ref) != 0)
        {
            DBG("RingBuffer: sink rejected frame " + std::to_string(ref.sequence));
        }
    }
}

// ---------------------------------------------------------------------------
// RingBuffer -- producer side
// ---------------------------------------------------------------------------

uint64_t RingBuffer::minAckSequence() const
{
    uint64_t lowest = UINT64_MAX;
    for (uint32_t i = 0; i < kMaxConsumers; ++i)
    {
        const UC2_Consumer& c = m_control->consumers[i];
        if (c.active.load(std::memory_order_acquire) != 0)
        {
            lowest = std::min(lowest, c.ackSequence.load(std::memory_order_acquire));
        }
    }
    return lowest;
}

void RingBuffer::applyFullPolicy(uint32_t slot, uint64_t sequence)
{
    UC2_SlotRecord* rec = slotAt(slot);
    if ((rec->flags.load(std::memory_order_acquire) & kFlagWritten) == 0)
    {
        return;   // virgin slot, nothing can be lost
    }
    const uint64_t outgoing = rec->sequence.load(std::memory_order_relaxed);

    if (m_cfg.policy == FullPolicy::Wait)
    {
        // Gate on the ack cursors rather than on kFlagRead: with several
        // consumers the flag only tells us that *someone* looked, while the
        // cursors tell us that *everyone* is done. The timeout is what keeps a
        // crashed consumer from stalling the camera for good.
        std::unique_lock<std::mutex> lk(m_mtx);
        const bool freed = m_slotFreed.wait_for(
            lk, std::chrono::milliseconds(m_cfg.waitTimeoutMs),
            [this, rec, outgoing]
            {
                return minAckSequence() > outgoing
                       && rec->pins.load(std::memory_order_acquire) == 0;
            });
        if (!freed)
        {
            m_control->waitTimeouts.fetch_add(1, std::memory_order_relaxed);
            lk.unlock();
            WARN("RingBuffer: slot " + std::to_string(slot) + " still holds unacknowledged frame "
                 + std::to_string(outgoing) + " after " + std::to_string(m_cfg.waitTimeoutMs)
                 + " ms - overwriting it for frame " + std::to_string(sequence));
        }
    }

    if ((rec->flags.load(std::memory_order_relaxed) & kFlagRead) == 0)
    {
        m_control->overwritten.fetch_add(1, std::memory_order_relaxed);
    }
}

bool RingBuffer::beginWrite(WriteHandle& out)
{
    return beginWrite(out, monotonicNs());
}

bool RingBuffer::beginWrite(WriteHandle& out, int64_t tsBeforeNs)
{
    if (!m_configured)
    {
        ERR("RingBuffer::beginWrite: buffer is not configured");
        return false;
    }
    if (out.held())
    {
        ERR("RingBuffer::beginWrite: the handle already holds a slot");
        return false;
    }

    bool expected = false;
    if (!m_writeBusy.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire))
    {
        ERR("RingBuffer::beginWrite: a write is already in progress "
            "(the ring buffer accepts exactly one producer)");
        return false;
    }

    const uint64_t sequence = m_control->writeSequence.load(std::memory_order_relaxed);
    const uint32_t slot     = static_cast<uint32_t>(sequence % m_cfg.slotCount);

    applyFullPolicy(slot, sequence);

    UC2_SlotRecord* rec = slotAt(slot);
    // Odd seqlock value: any reader that samples the slot from here on knows a
    // write is in flight and backs off.
    rec->seq.fetch_add(1, std::memory_order_acq_rel);
    rec->flags.store(kFlagWriting, std::memory_order_relaxed);
    rec->sequence.store(sequence, std::memory_order_relaxed);
    rec->nbytes.store(0, std::memory_order_relaxed);
    rec->tsBeforeNs.store(tsBeforeNs, std::memory_order_relaxed);
    rec->tsAfterNs.store(0, std::memory_order_relaxed);

    out.m_owner    = this;
    out.m_data     = payloadAt(*rec);
    out.m_capacity = m_header->frameBytes;
    out.m_sequence = sequence;
    out.m_slot     = slot;
    return true;
}

bool RingBuffer::commitWrite(WriteHandle& handle, uint64_t nbytes)
{
    return commitWrite(handle, nbytes, monotonicNs());
}

bool RingBuffer::commitWrite(WriteHandle& handle, uint64_t nbytes, int64_t tsAfterNs)
{
    if (!handle.held() || handle.m_owner != this)
    {
        ERR("RingBuffer::commitWrite: the handle does not hold a slot of this buffer");
        return false;
    }

    uint64_t n = nbytes;
    if (n > m_header->frameBytes)
    {
        WARN("RingBuffer::commitWrite: " + std::to_string(nbytes) + " bytes exceed the "
             + std::to_string(m_header->frameBytes) + " byte frame size - truncating");
        n = m_header->frameBytes;
    }

    UC2_SlotRecord* rec = slotAt(handle.m_slot);
    rec->nbytes.store(n, std::memory_order_relaxed);
    rec->tsAfterNs.store(tsAfterNs, std::memory_order_relaxed);
    rec->flags.store(kFlagWritten, std::memory_order_release);
    // Back to an even seqlock value: the slot is stable and readable again.
    rec->seq.fetch_add(1, std::memory_order_acq_rel);

    m_control->writeSequence.store(handle.m_sequence + 1, std::memory_order_release);
    m_control->committedCount.fetch_add(1, std::memory_order_release);

    const uint8_t* payload = payloadAt(*rec);

    handle.m_owner = nullptr;
    handle.m_data  = nullptr;
    m_writeBusy.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(m_mtx);
    }
    m_frameReady.notify_all();

    dispatchFrame(*rec, payload, n);
    return true;
}

void RingBuffer::abortWrite(WriteHandle& handle)
{
    if (!handle.held() || handle.m_owner != this)
    {
        return;
    }
    UC2_SlotRecord* rec = slotAt(handle.m_slot);
    rec->flags.store(0, std::memory_order_relaxed);
    rec->seq.fetch_add(1, std::memory_order_acq_rel);   // even again, but unpublished

    // writeSequence is deliberately left alone, so the abandoned index is
    // handed out again and committed frame numbers stay gap-free.
    m_control->aborted.fetch_add(1, std::memory_order_relaxed);

    handle.m_owner = nullptr;
    handle.m_data  = nullptr;
    m_writeBusy.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(m_mtx);
    }
    m_slotFreed.notify_all();
}

bool RingBuffer::writeFrame(const void* src, uint64_t nbytes)
{
    if (src == nullptr)
    {
        ERR("RingBuffer::writeFrame: source pointer is null");
        return false;
    }
    WriteHandle h;
    if (!beginWrite(h))
    {
        return false;
    }
    uint64_t n = nbytes;
    if (n > h.capacity())
    {
        WARN("RingBuffer::writeFrame: " + std::to_string(nbytes) + " bytes exceed the "
             + std::to_string(h.capacity()) + " byte frame size - truncating");
        n = h.capacity();
    }
    std::memcpy(h.data(), src, static_cast<size_t>(n));   // the one and only copy
    return commitWrite(h, n);
}

// ---------------------------------------------------------------------------
// RingBuffer -- consumer side
// ---------------------------------------------------------------------------

uint32_t RingBuffer::registerConsumer()
{
    if (!m_configured)
    {
        ERR("RingBuffer::registerConsumer: buffer is not configured");
        return kAnonymousConsumer;
    }
    for (uint32_t i = 0; i < kMaxConsumers; ++i)
    {
        UC2_Consumer& c = m_control->consumers[i];
        uint32_t expected = 0;
        if (c.active.compare_exchange_strong(expected, 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire))
        {
            // Start at the current head: a consumer that joins late must not
            // gate the producer on frames it was never going to read.
            c.ackSequence.store(m_control->committedCount.load(std::memory_order_acquire),
                                std::memory_order_release);
            return i;
        }
    }
    ERR("RingBuffer::registerConsumer: all " + std::to_string(kMaxConsumers)
        + " consumer slots are taken");
    return kAnonymousConsumer;
}

void RingBuffer::unregisterConsumer(uint32_t consumer)
{
    if (!m_configured || consumer >= kMaxConsumers)
    {
        return;
    }
    m_control->consumers[consumer].active.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
    }
    m_slotFreed.notify_all();
}

void RingBuffer::acknowledge(uint32_t consumer, uint64_t sequence)
{
    if (!m_configured || consumer >= kMaxConsumers)
    {
        return;
    }
    UC2_Consumer&  c    = m_control->consumers[consumer];
    const uint64_t want = sequence + 1;
    uint64_t       cur  = c.ackSequence.load(std::memory_order_relaxed);
    while (cur < want
           && !c.ackSequence.compare_exchange_weak(cur, want,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
    {
        // cur was refreshed by the failed exchange; retry unless someone else
        // already moved the cursor past us.
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
    }
    m_slotFreed.notify_all();
}

bool RingBuffer::acquireSlot(uint32_t index, const uint64_t* wantSequence,
                             ReadHandle& out, uint32_t consumer)
{
    if (!m_configured)
    {
        ERR("RingBuffer::read: buffer is not configured");
        return false;
    }
    if (out.held())
    {
        ERR("RingBuffer::read: the handle already pins a slot");
        return false;
    }
    UC2_SlotRecord* rec = slotAt(index);
    if (rec == nullptr)
    {
        ERR("RingBuffer::read: slot index " + std::to_string(index) + " is out of range");
        return false;
    }

    // First half of the seqlock.
    const uint32_t stamp = rec->seq.load(std::memory_order_acquire);
    if ((stamp & 1u) != 0)
    {
        m_control->rejectedReads.fetch_add(1, std::memory_order_relaxed);
        return false;   // the producer is writing this slot right now
    }
    const uint32_t flags = rec->flags.load(std::memory_order_acquire);
    if ((flags & kFlagWritten) == 0)
    {
        return false;   // nothing has been committed here yet
    }
    const uint64_t sequence = rec->sequence.load(std::memory_order_relaxed);
    if (wantSequence != nullptr && *wantSequence != sequence)
    {
        return false;   // the frame asked for has already been recycled
    }
    const uint64_t nbytes     = rec->nbytes.load(std::memory_order_relaxed);
    const int64_t  tsBeforeNs = rec->tsBeforeNs.load(std::memory_order_relaxed);
    const int64_t  tsAfterNs  = rec->tsAfterNs.load(std::memory_order_relaxed);

    // Pin first, then re-read the counter: the pin is advisory (only Wait mode
    // honours it) but it must be visible before we conclude the slot is ours.
    rec->pins.fetch_add(1, std::memory_order_acq_rel);

    // Second half of the seqlock.
    if (rec->seq.load(std::memory_order_acquire) != stamp)
    {
        rec->pins.fetch_sub(1, std::memory_order_acq_rel);
        m_control->rejectedReads.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    out.m_owner      = this;
    out.m_data       = payloadAt(*rec);
    out.m_nbytes     = nbytes;
    out.m_sequence   = sequence;
    out.m_tsBeforeNs = tsBeforeNs;
    out.m_tsAfterNs  = tsAfterNs;
    out.m_slot       = index;
    out.m_flags      = flags;
    out.m_seqStamp   = stamp;
    out.m_consumer   = consumer;
    return true;
}

void RingBuffer::releaseSlot(ReadHandle& handle, bool markRead, bool markSaved)
{
    UC2_SlotRecord* rec = slotAt(handle.m_slot);
    if (rec != nullptr)
    {
        // Only stamp flags if the slot still holds the frame we looked at --
        // otherwise we would label a newer frame as read or saved.
        if (rec->seq.load(std::memory_order_acquire) == handle.m_seqStamp)
        {
            uint32_t add = 0;
            if (markRead)  { add |= kFlagRead; }
            if (markSaved) { add |= kFlagSaved; }
            if (add != 0)
            {
                rec->flags.fetch_or(add, std::memory_order_acq_rel);
            }
        }
        rec->pins.fetch_sub(1, std::memory_order_acq_rel);
    }

    const uint32_t consumer = handle.m_consumer;
    const uint64_t sequence = handle.m_sequence;

    handle.m_owner = nullptr;
    handle.m_data  = nullptr;

    if (markRead && consumer != kAnonymousConsumer)
    {
        acknowledge(consumer, sequence);   // notifies the producer for us
    }
    else
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
        }
        m_slotFreed.notify_all();
    }
}

bool RingBuffer::readLatest(ReadHandle& out, uint32_t consumer)
{
    if (!m_configured)
    {
        ERR("RingBuffer::readLatest: buffer is not configured");
        return false;
    }
    const uint64_t committed = m_control->committedCount.load(std::memory_order_acquire);
    if (committed == 0)
    {
        return false;
    }
    const uint64_t sequence = committed - 1;
    return acquireSlot(static_cast<uint32_t>(sequence % m_cfg.slotCount), &sequence, out, consumer);
}

bool RingBuffer::readSlot(uint32_t slot, ReadHandle& out, uint32_t consumer)
{
    return acquireSlot(slot, nullptr, out, consumer);
}

bool RingBuffer::readSequence(uint64_t sequence, ReadHandle& out, uint32_t consumer)
{
    if (!m_configured)
    {
        ERR("RingBuffer::readSequence: buffer is not configured");
        return false;
    }
    return acquireSlot(static_cast<uint32_t>(sequence % m_cfg.slotCount), &sequence, out, consumer);
}

// ---------------------------------------------------------------------------
// RingBuffer -- metadata and statistics
// ---------------------------------------------------------------------------

uint32_t   RingBuffer::width() const       { return m_header ? m_header->width : m_width; }
uint32_t   RingBuffer::height() const      { return m_header ? m_header->height : m_height; }
uint32_t   RingBuffer::channels() const    { return m_header ? m_header->channels : m_channels; }
PixelDepth RingBuffer::depth() const       { return m_depth; }
uint64_t   RingBuffer::frameBytes() const  { return m_header ? m_header->frameBytes : 0; }
uint32_t   RingBuffer::slotCount() const   { return m_cfg.slotCount; }
uint64_t   RingBuffer::regionBytes() const { return m_regionBytes; }

std::string RingBuffer::cameraId() const { return m_header ? std::string(m_header->cameraId) : m_cameraId; }
std::string RingBuffer::comment() const  { return m_header ? std::string(m_header->comment)  : m_comment; }

int64_t RingBuffer::startWallNs() const { return m_header ? m_header->startWallNs : 0; }
int64_t RingBuffer::startMonoNs() const { return m_header ? m_header->startMonoNs : 0; }

UC2_SessionHeader RingBuffer::sessionHeader() const
{
    UC2_SessionHeader copy;
    std::memset(&copy, 0, sizeof(copy));
    if (m_header != nullptr)
    {
        std::memcpy(&copy, m_header, sizeof(copy));
    }
    return copy;
}

uint64_t RingBuffer::framesWritten() const
{
    return m_control ? m_control->committedCount.load(std::memory_order_acquire) : 0;
}

uint64_t RingBuffer::framesOverwritten() const
{
    return m_control ? m_control->overwritten.load(std::memory_order_relaxed) : 0;
}

uint64_t RingBuffer::framesAborted() const
{
    return m_control ? m_control->aborted.load(std::memory_order_relaxed) : 0;
}

uint64_t RingBuffer::waitTimeouts() const
{
    return m_control ? m_control->waitTimeouts.load(std::memory_order_relaxed) : 0;
}

uint64_t RingBuffer::rejectedReads() const
{
    return m_control ? m_control->rejectedReads.load(std::memory_order_relaxed) : 0;
}

uint32_t RingBuffer::slotFlags(uint32_t slot) const
{
    const UC2_SlotRecord* rec = slotAt(slot);
    return rec ? rec->flags.load(std::memory_order_acquire) : 0;
}

std::string RingBuffer::streamFilePath() const
{
    return m_stream ? m_stream->filePath() : std::string();
}

uint64_t RingBuffer::streamFramesSaved() const
{
    return m_stream ? m_stream->framesSaved() : 0;
}

uint64_t RingBuffer::streamFramesDropped() const
{
    return m_stream ? m_stream->framesDropped() : 0;
}

bool RingBuffer::streamHasError() const
{
    return m_stream ? m_stream->hasError() : false;
}

// ---------------------------------------------------------------------------
// FileStreamSink
// ---------------------------------------------------------------------------

FileStreamSink::FileStreamSink(RingBuffer& owner, const std::string& dirPath)
    : m_owner(owner)
    , m_dirPath(dirPath)
    , m_saved(0)
    , m_dropped(0)
    , m_error(false)
{
}

FileStreamSink::~FileStreamSink()
{
    onSessionEnd();
}

UC2_SinkOps FileStreamSink::ops()
{
    UC2_SinkOps o;
    o.abiVersion     = kAbiVersion;
    o.ctx            = this;
    o.onSessionStart = &FileStreamSink::sessionStartThunk;
    o.onFrame        = &FileStreamSink::frameThunk;
    o.onSessionEnd   = &FileStreamSink::sessionEndThunk;
    o.destroy        = nullptr;   // the RingBuffer owns this instance
    return o;
}

int FileStreamSink::sessionStartThunk(void* ctx, const UC2_SessionHeader* header)
{
    return static_cast<FileStreamSink*>(ctx)->onSessionStart(*header);
}

int FileStreamSink::frameThunk(void* ctx, const UC2_FrameRef* /*frame*/)
{
    // Deliberately ignores the payload pointer: this runs on the producer
    // thread, so it only rings the bell. The writer thread pulls the bytes out
    // of the ring itself, which keeps file I/O off the camera's critical path.
    static_cast<FileStreamSink*>(ctx)->onFrame();
    return 0;
}

void FileStreamSink::sessionEndThunk(void* ctx)
{
    static_cast<FileStreamSink*>(ctx)->onSessionEnd();
}

int FileStreamSink::onSessionStart(const UC2_SessionHeader& header)
{
    if (m_fd >= 0)
    {
        return 0;   // already open
    }

    std::error_code ec;
    std::filesystem::create_directories(m_dirPath, ec);
    if (ec && !std::filesystem::is_directory(m_dirPath))
    {
        ERR("FileStreamSink: cannot create directory '" + m_dirPath + "': " + ec.message());
        m_error.store(true);
        return -1;
    }

    std::string camera = sanitize(header.cameraId);
    if (camera.empty())
    {
        camera = "cam";
    }

    const std::time_t secs = static_cast<std::time_t>(header.startWallNs / 1000000000ll);
    std::tm tmv{};
    ::gmtime_r(&secs, &tmv);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);

    // The stamp only resolves to seconds, so a stop/start pair inside the same
    // second would otherwise truncate the recording that just finished.
    const std::filesystem::path base =
        std::filesystem::path(m_dirPath) / ("uc2_" + camera + "_" + stamp);
    m_filePath = base.string() + ".uc2rb";
    for (int n = 1; n < 1000 && std::filesystem::exists(m_filePath); ++n)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "_%03d", n);
        m_filePath = base.string() + suffix + ".uc2rb";
    }

    m_fd = ::open(m_filePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m_fd < 0)
    {
        ERR("FileStreamSink: cannot open '" + m_filePath + "': " + std::strerror(errno));
        m_error.store(true);
        return -1;
    }

    struct iovec iov;
    iov.iov_base = const_cast<UC2_SessionHeader*>(&header);
    iov.iov_len  = sizeof(UC2_SessionHeader);
    if (!writevAll(m_fd, &iov, 1))
    {
        ERR("FileStreamSink: cannot write the session header: " + std::string(std::strerror(errno)));
        ::close(m_fd);
        m_fd = -1;
        m_error.store(true);
        return -1;
    }

    if (!writeSidecar(header))
    {
        WARN("FileStreamSink: the JSON sidecar could not be written");
    }

    m_consumer = m_owner.registerConsumer();
    if (m_consumer == kAnonymousConsumer)
    {
        WARN("FileStreamSink: no consumer slot available - the Wait policy will "
             "not hold frames back for the streamer");
    }

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cursor  = m_owner.framesWritten();   // only save what arrives from now on
        m_running = true;
    }
    m_writer = std::thread(&FileStreamSink::writerLoop, this);

    INFO("FileStreamSink: streaming to " + m_filePath);
    return 0;
}

void FileStreamSink::onFrame()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
    }
    m_cv.notify_one();
}

void FileStreamSink::onSessionEnd()
{
    bool wasRunning = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        wasRunning = m_running;
        m_running  = false;
    }
    if (wasRunning)
    {
        m_cv.notify_all();
        if (m_writer.joinable())
        {
            m_writer.join();
        }
    }

    if (m_consumer != kAnonymousConsumer)
    {
        m_owner.unregisterConsumer(m_consumer);
        m_consumer = kAnonymousConsumer;
    }

    if (m_fd >= 0)
    {
        ::fsync(m_fd);
        ::close(m_fd);
        m_fd = -1;
        INFO("FileStreamSink: closed " + m_filePath + " after "
             + std::to_string(m_saved.load()) + " frames ("
             + std::to_string(m_dropped.load()) + " dropped)");
    }
}

void FileStreamSink::writerLoop()
{
    for (;;)
    {
        uint64_t target = 0;
        bool     run    = true;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait_for(lk, std::chrono::milliseconds(50),
                          [this] { return !m_running || m_owner.framesWritten() > m_cursor; });
            run    = m_running;
            target = m_owner.framesWritten();
            if (!run && target <= m_cursor)
            {
                break;   // stopped and fully drained
            }
        }

        while (m_cursor < target)
        {
            RingBuffer::ReadHandle h;
            if (m_owner.readSequence(m_cursor, h, m_consumer))
            {
                UC2_FrameHeader fh;
                std::memset(&fh, 0, sizeof(fh));
                fh.sequence   = h.sequence();
                fh.nbytes     = h.nbytes();
                fh.tsBeforeNs = h.tsBeforeNs();
                fh.tsAfterNs  = h.tsAfterNs();
                fh.slot       = h.slot();
                fh.flags      = h.flags() | kFlagSaved;
                fh.width      = m_owner.width();
                fh.height     = m_owner.height();
                fh.channels   = m_owner.channels();
                fh.depth      = static_cast<uint8_t>(m_owner.depth());

                struct iovec iov[2];
                iov[0].iov_base = &fh;
                iov[0].iov_len  = sizeof(fh);
                iov[1].iov_base = const_cast<uint8_t*>(h.data());
                iov[1].iov_len  = static_cast<size_t>(h.nbytes());

                // One syscall for header plus payload, straight out of the ring
                // slot: no staging copy anywhere on the way to the file.
                bool ok = writevAll(m_fd, iov, 2);
                if (!ok)
                {
                    ERR("FileStreamSink: write failed for frame " + std::to_string(m_cursor)
                        + ": " + std::strerror(errno));
                    m_error.store(true);
                }
                else if (!h.stillValid())
                {
                    // The producer recycled the slot while we were writing it,
                    // so the record on disk may be a mix of two frames. Only
                    // reachable under FullPolicy::Overwrite.
                    WARN("FileStreamSink: frame " + std::to_string(m_cursor)
                         + " was recycled while being written - record may be torn");
                    m_dropped.fetch_add(1, std::memory_order_relaxed);
                    ok = false;
                }
                else
                {
                    m_saved.fetch_add(1, std::memory_order_relaxed);
                }
                h.release(true, ok);
            }
            else
            {
                // The frame was gone before we got to it. Acknowledge it anyway,
                // otherwise a Wait-mode producer would block on it for ever.
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                m_owner.acknowledge(m_consumer, m_cursor);
            }
            ++m_cursor;
        }

        if (!run)
        {
            break;
        }
    }
}

bool FileStreamSink::writeSidecar(const UC2_SessionHeader& header) const
{
    const std::string path = std::filesystem::path(m_filePath).replace_extension(".json").string();
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        return false;
    }

    std::ostringstream os;
    os << "{\n"
       << "  \"format\": \"uc2rb\",\n"
       << "  \"formatVersion\": " << header.formatVersion << ",\n"
       << "  \"layoutVersion\": " << header.layoutVersion << ",\n"
       << "  \"container\": \"" << jsonEscape(std::filesystem::path(m_filePath).filename().string()) << "\",\n"
       << "  \"headerBytes\": " << header.headerBytes << ",\n"
       << "  \"frameHeaderBytes\": " << sizeof(UC2_FrameHeader) << ",\n"
       << "  \"width\": " << header.width << ",\n"
       << "  \"height\": " << header.height << ",\n"
       << "  \"channels\": " << header.channels << ",\n"
       << "  \"dtype\": \"" << depthName(static_cast<PixelDepth>(header.depth)) << "\",\n"
       << "  \"frameBytes\": " << header.frameBytes << ",\n"
       << "  \"slotCount\": " << header.slotCount << ",\n"
       << "  \"cameraId\": \"" << jsonEscape(header.cameraId) << "\",\n"
       << "  \"comment\": \"" << jsonEscape(header.comment) << "\",\n"
       << "  \"startTimeUtc\": \"" << formatWallClock(header.startWallNs) << "\",\n"
       << "  \"startWallNs\": " << header.startWallNs << ",\n"
       << "  \"startMonoNs\": " << header.startMonoNs << "\n"
       << "}\n";

    const std::string text = os.str();
    struct iovec iov;
    iov.iov_base = const_cast<char*>(text.data());
    iov.iov_len  = text.size();
    const bool ok = writevAll(fd, &iov, 1);
    ::close(fd);
    return ok;
}

std::string FileStreamSink::filePath() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_filePath;
}

uint64_t FileStreamSink::framesSaved() const   { return m_saved.load(std::memory_order_relaxed); }
uint64_t FileStreamSink::framesDropped() const { return m_dropped.load(std::memory_order_relaxed); }
bool     FileStreamSink::hasError() const      { return m_error.load(std::memory_order_relaxed); }

} // namespace hik
