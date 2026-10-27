#include "UC2_RingBuff.h"

#include "hikLog.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

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
static_assert(sizeof(UC2_Control) == kCacheLine * (1 + kMaxConsumers), "control block must stay 576 bytes");
static_assert(std::is_standard_layout<UC2_Control>::value,       "control block must be standard layout");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "32-bit atomics must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");
// The futex syscall operates on a plain 32-bit word at the atomic's address.
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "futex word must be a bare uint32_t");

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

/// Byte layout of a region, derived from the geometry alone: the producer's
/// configure() and a reader's attach() run the very same arithmetic.
struct RegionLayout
{
    uint64_t frameBytes    = 0;
    uint64_t slotStride    = 0;
    uint64_t controlOffset = 0;
    uint64_t slotsOffset   = 0;
    uint64_t payloadOffset = 0;
    uint64_t regionBytes   = 0;
};

/// Lay out a region for @p slotCount frames of the given geometry, with every
/// multiplication checked: a bad ROI must fail loudly rather than wrap around
/// into a tiny allocation.
/// @return false, with the reason in @p why, on overflow or above kMaxRegionBytes.
bool computeLayout(uint32_t width, uint32_t height, uint32_t channels, PixelDepth depth,
                   uint32_t slotCount, RegionLayout& out, std::string& why)
{
    uint64_t frameBytes = 0;
    if (!mulChecked(width, height, frameBytes)
        || !mulChecked(frameBytes, channels, frameBytes)
        || !mulChecked(frameBytes, itemSize(depth), frameBytes))
    {
        why = "frame size overflows 64 bits";
        return false;
    }

    const uint64_t slotStride    = alignUp(frameBytes, kCacheLine);
    const uint64_t headerBytes   = alignUp(sizeof(UC2_SessionHeader), kCacheLine);
    const uint64_t controlBytes  = alignUp(sizeof(UC2_Control), kCacheLine);
    const uint64_t slotTableSize = alignUp(
        static_cast<uint64_t>(slotCount) * sizeof(UC2_SlotRecord), kCacheLine);

    uint64_t payloadBytes = 0;
    if (!mulChecked(slotStride, slotCount, payloadBytes))
    {
        why = "payload area overflows 64 bits";
        return false;
    }

    out.frameBytes    = frameBytes;
    out.slotStride    = slotStride;
    out.controlOffset = headerBytes;
    out.slotsOffset   = out.controlOffset + controlBytes;
    out.payloadOffset = out.slotsOffset + slotTableSize;
    out.regionBytes   = out.payloadOffset + payloadBytes;

    if (out.regionBytes > kMaxRegionBytes)
    {
        why = "region of " + std::to_string(out.regionBytes / (1024 * 1024)) + " MiB exceeds the "
            + std::to_string(kMaxRegionBytes / (1024 * 1024)) + " MiB limit";
        return false;
    }
    return true;
}

/// @return true if @p d is one of the PixelDepth enumerators.
bool validDepth(uint8_t d)
{
    return d == static_cast<uint8_t>(PixelDepth::UInt8) || d == static_cast<uint8_t>(PixelDepth::UInt16)
        || d == static_cast<uint8_t>(PixelDepth::Float64);
}

/// Sleep until @p word no longer holds @p expected, a wake-up arrives or
/// @p timeoutNs pass. Spurious returns are fine: the caller re-checks.
void futexWait(std::atomic<uint32_t>& word, uint32_t expected, int64_t timeoutNs)
{
#if defined(__linux__)
    struct timespec ts;
    ts.tv_sec  = static_cast<time_t>(timeoutNs / 1000000000ll);
    ts.tv_nsec = static_cast<long>(timeoutNs % 1000000000ll);
    // Not FUTEX_PRIVATE_FLAG: the word may live in a region shared with another process.
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT, expected, &ts, nullptr, 0);
#else
    (void)word;
    (void)expected;
    std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(timeoutNs, 1000000)));
#endif
}

/// Wake every thread sleeping in futexWait() on @p word, in any process.
void futexWakeAll(std::atomic<uint32_t>& word)
{
#if defined(__linux__)
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE, INT32_MAX, nullptr, nullptr, 0);
#else
    (void)word;   // waiters poll
#endif
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

// Shared-memory provider. ctx points at RingBuffer::m_shmFd, which receives the
// memfd so the ring can hand it to a reader process.
void* shmAcquire(void* ctx, size_t bytes, size_t /*alignment: mmap is page-aligned*/)
{
#if defined(__linux__)
    int* fdOut = static_cast<int*>(ctx);
    const int fd = ::memfd_create("uc2_ring", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
    {
        ERR("RingBuffer: memfd_create failed: " + std::string(std::strerror(errno)));
        return nullptr;
    }
    // fallocate, not just ftruncate: the pages are reserved now, so running out
    // of memory fails here instead of as SIGBUS on a later page fault.
    int rc = 0;
    do
    {
        rc = ::fallocate(fd, 0, 0, static_cast<off_t>(bytes));
    } while (rc != 0 && errno == EINTR);
    if (rc != 0)
    {
        ERR("RingBuffer: cannot reserve " + std::to_string(bytes / (1024 * 1024))
            + " MiB of shared memory: " + std::strerror(errno));
        ::close(fd);
        return nullptr;
    }
    // Sealed against resizing: no process can shrink the region under a reader
    // that has it mapped (an access beyond the end of a file raises SIGBUS).
    if (::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
    {
        ERR("RingBuffer: cannot seal the shared memory: " + std::string(std::strerror(errno)));
        ::close(fd);
        return nullptr;
    }
    void* base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        ERR("RingBuffer: cannot map the shared memory: " + std::string(std::strerror(errno)));
        ::close(fd);
        return nullptr;
    }
    *fdOut = fd;
    return base;
#else
    (void)ctx;
    (void)bytes;
    ERR("RingBuffer: shared memory is only available on Linux");
    return nullptr;
#endif
}

void shmRelease(void* ctx, void* base, size_t bytes)
{
    ::munmap(base, bytes);
    int* fd = static_cast<int*>(ctx);
    if (fd != nullptr && *fd >= 0)
    {
        ::close(*fd);
        *fd = -1;
    }
}

} // extern "C"

UC2_MemoryOps sharedMemoryOps(int* fdOut)
{
    UC2_MemoryOps ops;
    ops.abiVersion = kAbiVersion;
    ops.ctx        = fdOut;
    ops.acquire    = &shmAcquire;
    ops.release    = &shmRelease;
    return ops;
}

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
    , m_mem(cfg.sharedMemory ? sharedMemoryOps(&m_shmFd) : heapMemoryOps())
    , m_comment(cfg.comment)
    , m_writeBusy(false)
    , m_streamDir(cfg.storagePath)
{
}

RingBuffer::~RingBuffer()
{
    stopRecording();      // closes the container file, joining the writer thread
    m_stream.reset();     // ... before the region goes away
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
    if (m_cfg.sharedMemory)
    {
        ERR("RingBuffer::setMemoryOps: Config::sharedMemory already selects the memfd provider");
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

    RegionLayout lay;
    std::string  why;
    if (!computeLayout(m_width, m_height, m_channels, m_depth, m_cfg.slotCount, lay, why))
    {
        ERR("RingBuffer::configure: " + why);
        return false;
    }
    const uint64_t frameBytes    = lay.frameBytes;
    const uint64_t slotStride    = lay.slotStride;
    const uint64_t regionBytes   = lay.regionBytes;
    const uint64_t slotsOffset   = lay.slotsOffset;
    const uint64_t payloadOffset = lay.payloadOffset;

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
    m_controlOffset = lay.controlOffset;
    m_slotsOffset   = slotsOffset;
    m_payloadOffset = payloadOffset;
    m_slotStride    = slotStride;
    m_frameBytes    = frameBytes;

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

    m_control = new (region + m_controlOffset) UC2_Control();

    for (uint32_t i = 0; i < m_cfg.slotCount; ++i)
    {
        UC2_SlotRecord* rec = new (region + slotsOffset + i * sizeof(UC2_SlotRecord)) UC2_SlotRecord();
        rec->payloadOffset = payloadOffset + static_cast<uint64_t>(i) * slotStride;
    }

    m_configured = true;

    // Created even without a directory: it then idles until startStreaming().
    m_stream.reset(new FileStreamSink(*this, m_streamDir));

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

uint8_t* RingBuffer::payloadAt(uint32_t index)
{
    return m_region + m_payloadOffset + static_cast<uint64_t>(index) * m_slotStride;
}

const uint8_t* RingBuffer::payloadAt(uint32_t index) const
{
    return const_cast<RingBuffer*>(this)->payloadAt(index);
}

int RingBuffer::sharedFd() const
{
    return (m_configured && !m_attached) ? m_shmFd : -1;
}

// ---------------------------------------------------------------------------
// RingBuffer -- attaching to another process's region
// ---------------------------------------------------------------------------

namespace {

extern "C" void unmapRelease(void* /*ctx*/, void* base, size_t bytes)
{
    ::munmap(base, bytes);
}

/// Check @p h (a private copy) against the size of the mapping it came from.
/// @return empty if the header describes a ring of exactly @p mappedBytes,
///         otherwise why not.
std::string validateHeader(const UC2_SessionHeader& h, uint64_t mappedBytes, RegionLayout& lay)
{
    if (std::memcmp(h.magic, "UC2RB\0\0\0", 8) != 0)
    {
        return "no ring buffer header (bad magic)";
    }
    if (h.layoutVersion != kLayoutVersion)
    {
        return "layout version " + std::to_string(h.layoutVersion) + ", expected "
             + std::to_string(kLayoutVersion);
    }
    if (h.headerBytes != sizeof(UC2_SessionHeader))
    {
        return "header size " + std::to_string(h.headerBytes) + ", expected "
             + std::to_string(sizeof(UC2_SessionHeader));
    }
    if (h.width == 0 || h.height == 0 || h.width > kMaxDimension || h.height > kMaxDimension
        || h.channels == 0 || h.channels > kMaxChannels || !validDepth(h.depth))
    {
        return "implausible geometry " + std::to_string(h.width) + "x" + std::to_string(h.height)
             + "x" + std::to_string(h.channels) + ", depth " + std::to_string(h.depth);
    }
    if (h.slotCount < 2 || h.slotCount > kMaxSlots)
    {
        return "slot count " + std::to_string(h.slotCount) + " out of range";
    }
    std::string why;
    if (!computeLayout(h.width, h.height, h.channels, static_cast<PixelDepth>(h.depth), h.slotCount,
                       lay, why))
    {
        return why;
    }
    if (h.frameBytes != lay.frameBytes || h.slotStride != lay.slotStride)
    {
        return "frame size or stride does not match the geometry";
    }
    if (lay.regionBytes != mappedBytes)
    {
        return "the header describes " + std::to_string(lay.regionBytes) + " bytes, the region has "
             + std::to_string(mappedBytes);
    }
    return std::string();
}

} // namespace

std::shared_ptr<RingBuffer> RingBuffer::attach(int fd)
{
#if defined(__linux__)
    struct stat st;
    if (::fstat(fd, &st) != 0)
    {
        ERR("RingBuffer::attach: fstat failed: " + std::string(std::strerror(errno)));
        return nullptr;
    }
    // Only a region sealed against resizing is safe to map: otherwise the
    // producer side could shrink it and turn our reads into SIGBUS.
    const int seals = ::fcntl(fd, F_GET_SEALS);
    const int needed = F_SEAL_SHRINK | F_SEAL_GROW;
    if (seals < 0 || (seals & needed) != needed)
    {
        ERR("RingBuffer::attach: the region is not sealed against resizing - refusing to map it");
        return nullptr;
    }
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    if (size < sizeof(UC2_SessionHeader) + sizeof(UC2_Control))
    {
        ERR("RingBuffer::attach: region of " + std::to_string(size) + " bytes is too small");
        return nullptr;
    }
    void* base = ::mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        ERR("RingBuffer::attach: mmap failed: " + std::string(std::strerror(errno)));
        return nullptr;
    }

    // Validate a private copy: the producer could change the shared header
    // while we look at it, and from here on only the copy is trusted.
    UC2_SessionHeader h;
    std::memcpy(&h, base, sizeof(h));
    RegionLayout lay;
    const std::string bad = validateHeader(h, size, lay);
    if (!bad.empty())
    {
        ::munmap(base, static_cast<size_t>(size));
        ERR("RingBuffer::attach: " + bad);
        return nullptr;
    }

    Config cfg;
    cfg.slotCount = h.slotCount;
    std::shared_ptr<RingBuffer> rb(new RingBuffer(cfg));
    rb->m_mem.abiVersion = kAbiVersion;
    rb->m_mem.ctx        = nullptr;
    rb->m_mem.acquire    = nullptr;   // never called: the ring is configured already
    rb->m_mem.release    = &unmapRelease;

    rb->m_region        = static_cast<uint8_t*>(base);
    rb->m_regionBytes   = size;
    rb->m_controlOffset = lay.controlOffset;
    rb->m_slotsOffset   = lay.slotsOffset;
    rb->m_payloadOffset = lay.payloadOffset;
    rb->m_slotStride    = lay.slotStride;
    rb->m_frameBytes    = lay.frameBytes;
    rb->m_width         = h.width;
    rb->m_height        = h.height;
    rb->m_channels      = h.channels;
    rb->m_depth         = static_cast<PixelDepth>(h.depth);
    rb->m_cameraId      = std::string(h.cameraId, strnlen(h.cameraId, sizeof(h.cameraId)));
    rb->m_comment       = std::string(h.comment, strnlen(h.comment, sizeof(h.comment)));
    rb->m_header        = static_cast<UC2_SessionHeader*>(base);
    rb->m_control       = reinterpret_cast<UC2_Control*>(rb->m_region + lay.controlOffset);
    rb->m_attached      = true;
    rb->m_configured    = true;
    DBG("RingBuffer: attached " + std::to_string(h.slotCount) + " slots x " + std::to_string(h.width)
        + "x" + std::to_string(h.height) + "x" + std::to_string(h.channels) + " "
        + depthName(rb->m_depth) + " (" + std::to_string(size / (1024 * 1024)) + " MiB)");
    return rb;
#else
    (void)fd;
    ERR("RingBuffer::attach: shared memory is only available on Linux");
    return nullptr;
#endif
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
    if (m_attached)
    {
        ERR("RingBuffer::addSink: an attached ring can only be read");
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
    if (m_attached)
    {
        ERR("RingBuffer::startRecording: an attached ring can only be read");
        return false;
    }
    std::lock_guard<std::mutex> lk(m_streamMtx);
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
            endSinks(started);   // none is left half-started
            return false;
        }
        ++started;
    }
    if (!m_streamDir.empty() && !openStreamLocked())
    {
        ERR("RingBuffer::startRecording: the disk streamer refused to open the session");
        endSinks(started);
        return false;
    }

    m_control->recording.store(1, std::memory_order_release);
    INFO("RingBuffer: recording started at " + formatWallClock(m_header->startWallNs));
    return true;
}

void RingBuffer::stopRecording()
{
    if (!m_configured || m_attached)   // the recording flag of an attached ring is the producer's
    {
        return;
    }
    std::lock_guard<std::mutex> lk(m_streamMtx);
    if (!isRecording())
    {
        return;
    }
    m_control->recording.store(0, std::memory_order_release);
    closeStreamLocked();
    endSinks(m_sinks.size());
    INFO("RingBuffer: recording stopped after " + std::to_string(framesWritten()) + " frames");
}

void RingBuffer::endSinks(size_t count)
{
    for (size_t i = 0; i < count && i < m_sinks.size(); ++i)
    {
        if (m_sinks[i].onSessionEnd != nullptr)
        {
            m_sinks[i].onSessionEnd(m_sinks[i].ctx);
        }
    }
}

bool RingBuffer::isRecording() const
{
    return m_configured && m_control->recording.load(std::memory_order_acquire) != 0;
}

void RingBuffer::dispatchFrame(const UC2_SlotRecord& rec, const uint8_t* payload, uint64_t nbytes)
{
    // Only rings the writer's bell; the writer thread pulls the frame itself.
    if (FileStreamSink* stream = m_liveStream.load(std::memory_order_acquire))
    {
        stream->onFrame();
    }

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
    ref.width      = m_width;
    ref.height     = m_height;
    ref.channels   = m_channels;
    ref.depth      = static_cast<uint8_t>(m_depth);

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
    if (m_attached)
    {
        ERR("RingBuffer::beginWrite: an attached ring can only be read");
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
    out.m_data     = payloadAt(slot);
    out.m_capacity = m_frameBytes;
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
    if (n > m_frameBytes)
    {
        WARN("RingBuffer::commitWrite: " + std::to_string(nbytes) + " bytes exceed the "
             + std::to_string(m_frameBytes) + " byte frame size - truncating");
        n = m_frameBytes;
    }

    UC2_SlotRecord* rec = slotAt(handle.m_slot);
    rec->nbytes.store(n, std::memory_order_relaxed);
    rec->tsAfterNs.store(tsAfterNs, std::memory_order_relaxed);
    rec->flags.store(kFlagWritten, std::memory_order_release);
    // Back to an even seqlock value: the slot is stable and readable again.
    rec->seq.fetch_add(1, std::memory_order_acq_rel);

    m_control->writeSequence.store(handle.m_sequence + 1, std::memory_order_release);
    m_control->committedCount.fetch_add(1, std::memory_order_release);

    const uint8_t* payload = payloadAt(handle.m_slot);

    handle.m_owner = nullptr;
    handle.m_data  = nullptr;
    m_writeBusy.store(false, std::memory_order_release);

    // Wake readers in waitForFrame(), in this process or another one. Bumping
    // the word after committedCount is what lets a waiter that saw the old
    // value rely on the futex comparison; the syscall is skipped while nobody
    // sleeps.
    m_control->frameSignal.fetch_add(1, std::memory_order_seq_cst);
    if (m_control->frameWaiters.load(std::memory_order_seq_cst) != 0)
    {
        futexWakeAll(m_control->frameSignal);
    }

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
    if (m_attached)
    {
        ERR("RingBuffer::registerConsumer: an attached ring reads anonymously only");
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
    // Clamped: in an attached ring the record was written by another process,
    // and nbytes decides how far a consumer such as the disk writer reads.
    const uint64_t nbytes     = std::min(rec->nbytes.load(std::memory_order_relaxed), m_frameBytes);
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
    out.m_data       = payloadAt(index);
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

bool RingBuffer::waitForFrame(int64_t afterSequence, uint32_t timeoutMs)
{
    if (!m_configured)
    {
        return false;
    }
    // committedCount - 1 is the newest sequence, as in readLatest().
    const auto newer = [this, afterSequence]
    {
        const uint64_t committed = m_control->committedCount.load(std::memory_order_acquire);
        return static_cast<int64_t>(committed) - 1 > afterSequence;
    };
    const int64_t deadline = monotonicNs() + static_cast<int64_t>(timeoutMs) * 1000000ll;
    for (;;)
    {
        // Sample the futex word BEFORE the check: a commit after the check
        // changes it, so the futex call below returns at once instead of
        // sleeping through that frame.
        const uint32_t signal = m_control->frameSignal.load(std::memory_order_seq_cst);
        if (newer())
        {
            return true;
        }
        const int64_t left = deadline - monotonicNs();
        if (left <= 0)
        {
            return false;
        }
        m_control->frameWaiters.fetch_add(1, std::memory_order_seq_cst);
        futexWait(m_control->frameSignal, signal, left);
        m_control->frameWaiters.fetch_sub(1, std::memory_order_seq_cst);
    }
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

// Geometry comes from this object's own copies, never from the shared header:
// in an attached ring another process could have changed that since attach().
uint32_t   RingBuffer::width() const       { return m_width; }
uint32_t   RingBuffer::height() const      { return m_height; }
uint32_t   RingBuffer::channels() const    { return m_channels; }
PixelDepth RingBuffer::depth() const       { return m_depth; }
uint64_t   RingBuffer::frameBytes() const  { return m_frameBytes; }
uint32_t   RingBuffer::slotCount() const   { return m_cfg.slotCount; }
uint64_t   RingBuffer::regionBytes() const { return m_regionBytes; }

std::string RingBuffer::cameraId() const { return m_cameraId.substr(0, sizeof(UC2_SessionHeader::cameraId) - 1); }
std::string RingBuffer::comment() const  { return m_comment.substr(0, sizeof(UC2_SessionHeader::comment) - 1); }

int64_t RingBuffer::startWallNs() const { return m_header ? m_header->startWallNs : 0; }
int64_t RingBuffer::startMonoNs() const { return m_header ? m_header->startMonoNs : 0; }

UC2_SessionHeader RingBuffer::sessionHeader() const
{
    UC2_SessionHeader copy;
    std::memset(&copy, 0, sizeof(copy));
    if (m_header != nullptr)
    {
        std::memcpy(&copy, m_header, sizeof(copy));
        // Callers size numpy views and file records from this copy: the
        // geometry is overwritten with the validated values, and the strings
        // are terminated, whatever the shared region holds by now.
        copy.width      = m_width;
        copy.height     = m_height;
        copy.channels   = m_channels;
        copy.depth      = static_cast<uint8_t>(m_depth);
        copy.frameBytes = m_frameBytes;
        copy.slotCount  = m_cfg.slotCount;
        copy.slotStride = m_slotStride;
        copy.cameraId[sizeof(copy.cameraId) - 1] = '\0';
        copy.comment[sizeof(copy.comment) - 1]   = '\0';
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

bool RingBuffer::startStreaming(const std::string& dirPath)
{
    if (!m_configured)
    {
        ERR("RingBuffer::startStreaming: buffer is not configured");
        return false;
    }
    if (m_attached)
    {
        ERR("RingBuffer::startStreaming: an attached ring can only be read");
        return false;
    }
    if (dirPath.empty())
    {
        ERR("RingBuffer::startStreaming: no directory given");
        return false;
    }
    std::lock_guard<std::mutex> lk(m_streamMtx);
    if (m_liveStream.load() != nullptr)
    {
        if (dirPath == m_streamDir)
        {
            return true;
        }
        closeStreamLocked();   // another directory: finish this file, open a new one there
    }
    m_streamDir = dirPath;
    if (!isRecording())
    {
        return true;           // the file opens with the next startRecording()
    }
    if (!openStreamLocked())
    {
        m_streamDir.clear();
        return false;
    }
    return true;
}

void RingBuffer::stopStreaming()
{
    std::lock_guard<std::mutex> lk(m_streamMtx);
    m_streamDir.clear();
    closeStreamLocked();
}

bool RingBuffer::isStreaming() const
{
    return m_liveStream.load(std::memory_order_acquire) != nullptr;
}

bool RingBuffer::openStreamLocked()
{
    m_stream->setDirectory(m_streamDir);
    if (m_stream->onSessionStart(*m_header) != 0)
    {
        return false;
    }
    m_liveStream.store(m_stream.get(), std::memory_order_release);
    return true;
}

void RingBuffer::closeStreamLocked()
{
    // Unpublish first, so the producer stops ringing; a ring already under
    // way only reaches the sink's own mutex and is harmless.
    if (m_liveStream.exchange(nullptr, std::memory_order_acq_rel) != nullptr)
    {
        m_stream->onSessionEnd();
    }
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
    // The counters describe the file about to be opened, not an earlier one.
    m_saved.store(0);
    m_dropped.store(0);
    m_error.store(false);

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

    // Named after the moment the file opens: the session start, or later when
    // streaming was switched on during the session.
    const std::time_t secs = static_cast<std::time_t>(wallClockNs() / 1000000000ll);
    std::tm tmv{};
    ::gmtime_r(&secs, &tmv);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);

    // The stamp only resolves to seconds, so a stop/start pair inside the same
    // second would otherwise truncate the recording that just finished.
    const std::filesystem::path base =
        std::filesystem::path(m_dirPath) / ("uc2_" + camera + "_" + stamp);
    std::string path = base.string() + ".uc2rb";
    for (int n = 1; n < 1000 && std::filesystem::exists(path); ++n)
    {
        char suffix[16];   // room for any int, keeps -Wformat-truncation quiet
        std::snprintf(suffix, sizeof(suffix), "_%03d", n);
        path = base.string() + suffix + ".uc2rb";
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);   // filePath() may be read from any thread
        m_filePath = path;
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
        // Fix the end now: when streaming stops during a session the producer
        // keeps committing, and draining up to its moving tip would never end.
        m_stopAt = m_owner.framesWritten();
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
            target = run ? m_owner.framesWritten() : std::min(m_owner.framesWritten(), m_stopAt);
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

void FileStreamSink::setDirectory(const std::string& dirPath)
{
    if (m_fd < 0)
    {
        m_dirPath = dirPath;
    }
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
