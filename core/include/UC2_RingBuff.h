#ifndef UC2HIK_RINGBUFF_H
#define UC2HIK_RINGBUFF_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// @file UC2_RingBuff.h
/// @brief Single-producer / multi-consumer ring buffer for image frames.
///
/// ## Design contract
///
/// The buffer keeps its entire state in one contiguous memory region so that the
/// very same layout can later be mapped into several processes (camera process
/// writes, streaming process saves, analysis process reads). Three rules make
/// that possible and are honoured throughout, even though the only allocator
/// shipped today is a plain heap one:
///
///  1. **No pointers inside the region.** Everything is a byte offset from the
///     region base; a pointer stored by one process is meaningless in another,
///     which maps the same segment at a different address.
///  2. **No C++ objects with virtual functions inside the region.** A vtable
///     pointer is a per-process code address. This is precisely why the
///     extension points below are C structs of function pointers held *outside*
///     the region (UC2_MemoryOps, UC2_SinkOps) rather than abstract base
///     classes -- and why a PyCapsule (a `void*` plus a destructor) fits them
///     without an adapter.
///  3. **No std::string / std::vector inside the region.** Metadata uses
///     fixed-size character arrays.
///
/// ## Copy budget
///
/// Exactly one copy happens on the way in: source buffer -> ring slot, performed
/// by the caller straight into RingBuffer::WriteHandle::data(). Reading copies
/// nothing at all; consumers receive a `const uint8_t*` that aliases the slot.
/// The on-disk streamer writes the slot with `writev()`, so no staging copy
/// happens on the way out either.
///
/// ## Synchronisation
///
/// Correctness rests entirely on lock-free atomics, so a stalled or crashed
/// consumer can never corrupt the buffer. Each slot carries a seqlock counter
/// (even = stable, odd = a write is in progress); a reader samples it before and
/// after use and rejects the frame if the producer recycled the slot underneath
/// it (RingBuffer::ReadHandle::stillValid()). The mutex and condition variable
/// live *outside* the region and serve only to avoid spinning -- replacing them
/// with process-shared pthread primitives later does not touch the protocol.

namespace hik {

// ---------------------------------------------------------------------------
// Scalar types and constants
// ---------------------------------------------------------------------------

/// Sample type of one channel value. The enumerator value is the size in bytes.
enum class PixelDepth : uint8_t
{
    UInt8   = 1,   ///< 8-bit unsigned integer
    UInt16  = 2,   ///< 16-bit unsigned integer
    Float64 = 8    ///< IEEE-754 double
};

/// @return size in bytes of one sample of @p d.
inline uint32_t itemSize(PixelDepth d) { return static_cast<uint32_t>(d); }

/// @return short human-readable name of @p d ("uint8", "uint16", "float64").
const char* depthName(PixelDepth d);

/// What the producer does when the slot it is about to reuse still holds a
/// frame that the registered consumers have not acknowledged yet.
enum class FullPolicy : uint8_t
{
    Overwrite = 0,  ///< never block; recycle the slot and count an overwrite
    Wait      = 1   ///< block until every registered consumer acknowledged it,
                    ///< then fall back to overwriting after Config::waitTimeoutMs
};

/// Per-frame status bits, stored in UC2_SlotRecord::flags.
enum FrameFlag : uint32_t
{
    kFlagWriting = 1u << 0,   ///< a write into this slot is in progress
    kFlagWritten = 1u << 1,   ///< the write completed; the frame is readable
    kFlagRead    = 1u << 2,   ///< at least one consumer has read the frame
    kFlagSaved   = 1u << 3    ///< the frame has been written to disk
};

/// Maximum number of consumers that can hold the producer back in Wait mode.
static const uint32_t kMaxConsumers = 8;

/// Consumer handle that does not participate in the Wait-mode gate. Reads made
/// under this id never stall the producer, which is what casual readers
/// (preview, probing) want.
static const uint32_t kAnonymousConsumer = 0xFFFFFFFFu;

/// Cache-line size assumed for padding and slot alignment. 64 bytes on both
/// Cortex-A76 (Raspberry Pi 5) and every x86-64 part of interest.
static const size_t kCacheLine = 64;

/// Version of the shared-memory layout. Bumped on any incompatible change to
/// UC2_SessionHeader, UC2_Control or UC2_SlotRecord.
static const uint32_t kLayoutVersion = 1;

/// Version of the on-disk container format.
static const uint32_t kFormatVersion = 1;

/// Version tag carried by the C ABI vtables below.
static const uint32_t kAbiVersion = 1;

// ---------------------------------------------------------------------------
// POD types stored inside the shared region (no pointers, no vtables)
// ---------------------------------------------------------------------------

#pragma pack(push, 8)

/// Immutable description of one recording session. Written once at configure()
/// time, then copied verbatim to the head of the container file.
///
/// The pairing of @ref startWallNs with @ref startMonoNs is deliberate:
/// per-frame timestamps use the monotonic clock (immune to NTP steps and
/// daylight-saving jumps), and this pair is the anchor that converts them back
/// to absolute time.
struct UC2_SessionHeader
{
    char     magic[8];        ///< "UC2RB\0\0\0"
    uint32_t formatVersion;   ///< kFormatVersion
    uint32_t headerBytes;     ///< sizeof(UC2_SessionHeader)
    uint32_t width;           ///< image width in pixels
    uint32_t height;          ///< image height in pixels
    uint32_t channels;        ///< channels per pixel (>= 1, not limited to 3)
    uint8_t  depth;           ///< PixelDepth value
    uint8_t  reserved0[3];
    uint64_t frameBytes;      ///< width * height * channels * itemSize(depth)
    uint32_t slotCount;       ///< number of slots in the ring
    uint32_t layoutVersion;   ///< kLayoutVersion
    int64_t  startWallNs;     ///< UTC epoch nanoseconds at startRecording()
    int64_t  startMonoNs;     ///< monotonic nanoseconds at the same instant
    uint64_t slotStride;      ///< payload stride in bytes (frameBytes, cache-aligned)
    char     cameraId[64];    ///< NUL-terminated camera identifier
    char     comment[8192];   ///< NUL-terminated free-text comment, max. 8191 characters
    uint8_t  reserved2[120];
};

/// Fixed-size record prefixed to every frame in the container file.
struct UC2_FrameHeader
{
    uint64_t sequence;      ///< global frame index, 0-based and gap-free
    uint64_t nbytes;        ///< valid payload bytes following this header
    int64_t  tsBeforeNs;    ///< monotonic ns sampled before the frame was read
    int64_t  tsAfterNs;     ///< monotonic ns sampled after the frame was read
    uint32_t slot;          ///< ring slot the frame occupied
    uint32_t flags;         ///< FrameFlag bits at the time of saving
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint8_t  depth;         ///< PixelDepth value
    uint8_t  reserved[11];
};

/// Bookkeeping for one slot. Lives in the shared region, so every mutable field
/// is atomic and no field is a pointer.
struct UC2_SlotRecord
{
    std::atomic<uint32_t> seq;         ///< seqlock: even = stable, odd = writing
    std::atomic<uint32_t> flags;       ///< FrameFlag bits
    std::atomic<uint32_t> pins;        ///< consumers currently holding this slot
    uint32_t              reserved0;
    std::atomic<uint64_t> sequence;    ///< global frame index held by this slot
    std::atomic<uint64_t> nbytes;      ///< valid payload bytes
    std::atomic<int64_t>  tsBeforeNs;  ///< monotonic ns before the read
    std::atomic<int64_t>  tsAfterNs;   ///< monotonic ns after the read
    uint64_t              payloadOffset;  ///< byte offset of the payload from the region base
    uint8_t               reserved1[8];
};

/// Ack cursor of one registered consumer, padded to its own cache line so that
/// consumers do not ping-pong the producer's cursor between cores.
struct UC2_Consumer
{
    std::atomic<uint32_t> active;       ///< non-zero while the slot is claimed
    uint32_t              reserved0;
    std::atomic<uint64_t> ackSequence;  ///< first sequence NOT yet acknowledged
    uint8_t               pad[kCacheLine - 16];
};

/// Producer cursor, counters and the consumer table.
struct UC2_Control
{
    std::atomic<uint64_t> writeSequence;   ///< next sequence the producer will claim
    std::atomic<uint64_t> committedCount;  ///< number of frames fully committed
    std::atomic<uint64_t> overwritten;     ///< frames recycled before being read
    std::atomic<uint64_t> waitTimeouts;    ///< Wait-mode gates that expired
    std::atomic<uint64_t> aborted;         ///< writes abandoned via abortWrite()
    std::atomic<uint64_t> rejectedReads;   ///< reads rejected by the seqlock
    std::atomic<uint32_t> recording;       ///< non-zero between start/stopRecording()
    uint32_t              reserved0;
    uint8_t               pad0[kCacheLine - 56];
    UC2_Consumer          consumers[kMaxConsumers];
};

#pragma pack(pop)

/// Description of one committed frame handed to a UC2_SinkOps callback. Holds a
/// raw pointer and therefore never lives inside the shared region.
struct UC2_FrameRef
{
    const void* data;        ///< first payload byte (aliases the ring slot)
    uint64_t    nbytes;      ///< valid payload bytes at @ref data
    uint64_t    sequence;    ///< global frame index
    int64_t     tsBeforeNs;  ///< monotonic ns before the frame was read
    int64_t     tsAfterNs;   ///< monotonic ns after the frame was read
    uint32_t    slot;        ///< ring slot index
    uint32_t    width;
    uint32_t    height;
    uint32_t    channels;
    uint8_t     depth;       ///< PixelDepth value
    uint8_t     reserved[7];
};

// ---------------------------------------------------------------------------
// C ABI extension points ("vtables")
// ---------------------------------------------------------------------------
//
// Deliberately C structs of function pointers, not C++ interfaces: they survive
// the C++/Python boundary unchanged, they can be handed around inside a
// PyCapsule, and they contain no per-process code addresses that would have to
// be patched when the region is mapped somewhere else.

extern "C" {

/// Allocate @p bytes aligned to @p alignment. Returns nullptr on failure.
typedef void* (*UC2_AcquireFn)(void* ctx, size_t bytes, size_t alignment);
/// Release a block previously returned by UC2_AcquireFn.
typedef void  (*UC2_ReleaseFn)(void* ctx, void* base, size_t bytes);

/// Called once when a recording session opens. Return 0 on success.
typedef int   (*UC2_SessionStartFn)(void* ctx, const UC2_SessionHeader* header);
/// Called for every committed frame, on the producer thread. Return 0 on
/// success. Must not block: the camera thread is waiting on it.
typedef int   (*UC2_FrameFn)(void* ctx, const UC2_FrameRef* frame);
/// Called once when the recording session closes.
typedef void  (*UC2_SessionEndFn)(void* ctx);
/// Called when the ring buffer drops its reference to the sink. This is where a
/// PyCapsule destructor belongs.
typedef void  (*UC2_DestroyFn)(void* ctx);

/// Where the ring buffer's memory region comes from.
///
/// The shipped implementation is heapMemoryOps(); an `shm_open` + `mmap`
/// variant slots in here unchanged when the buffer moves across processes.
struct UC2_MemoryOps
{
    uint32_t      abiVersion;  ///< must be kAbiVersion
    void*         ctx;         ///< opaque, passed back to every callback
    UC2_AcquireFn acquire;     ///< required
    UC2_ReleaseFn release;     ///< required
};

/// A consumer of committed frames: the built-in disk streamer today, a Python
/// hook tomorrow.
///
/// @warning @ref onFrame runs on the producer thread with the slot pinned.
///          Copy what you need and return promptly; do not perform I/O there.
///          The built-in streamer therefore only records the sequence number
///          and pulls the payload from its own thread.
struct UC2_SinkOps
{
    uint32_t           abiVersion;     ///< must be kAbiVersion
    void*              ctx;            ///< opaque, passed back to every callback
    UC2_SessionStartFn onSessionStart; ///< optional (may be nullptr)
    UC2_FrameFn        onFrame;        ///< optional (may be nullptr)
    UC2_SessionEndFn   onSessionEnd;   ///< optional (may be nullptr)
    UC2_DestroyFn      destroy;        ///< optional (may be nullptr)
};

} // extern "C"

/// @return a UC2_MemoryOps backed by posix_memalign()/free().
UC2_MemoryOps heapMemoryOps();

/// @return monotonic nanoseconds (steady_clock); safe for durations.
int64_t monotonicNs();

/// @return UTC nanoseconds since the Unix epoch (system_clock).
int64_t wallClockNs();

/// Format @p wallNs as "YYYY-mm-ddTHH:MM:SS.mmmZ" (UTC).
std::string formatWallClock(int64_t wallNs);

class FileStreamSink;

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------

/// Single-producer / multi-consumer ring buffer of fixed-size image frames.
///
/// Lifecycle:
/// @code
///   RingBuffer::Config cfg;
///   cfg.storagePath = "/data/recordings";   // empty -> keep in memory only
///   cfg.slotCount   = 64;
///   cfg.comment     = "bead tracking run 7";
///   cfg.policy      = FullPolicy::Overwrite;
///
///   RingBuffer rb(cfg);
///   rb.setImageSize(1920, 1200);
///   rb.setChannels(1);
///   rb.setDepth(PixelDepth::UInt8);
///   rb.setCameraId("MV-CA023-10UM:01234");
///   rb.configure();          // validates and allocates
///   rb.startRecording();     // stamps the start time, opens the sinks
///
///   RingBuffer::WriteHandle w;
///   if (rb.beginWrite(w))
///   {
///       std::memcpy(w.data(), frame.data, frame.nbytes);   // the one copy
///       rb.commitWrite(w, frame.nbytes);
///   }
///
///   RingBuffer::ReadHandle r;
///   if (rb.readLatest(r)) { use(r.data(), r.nbytes()); }   // no copy
/// @endcode
///
/// The class is neither copyable nor movable: it owns a memory region that
/// consumer handles point into.
class RingBuffer
{
public:
    /// Construction-time parameters. Geometry is *not* here on purpose -- it is
    /// normally drawn from the camera after the buffer already exists.
    struct Config
    {
        std::string storagePath;                      ///< directory for the container file; empty: RAM only (see startStreaming())
        uint32_t    slotCount     = 16;               ///< number of images the ring holds (>= 2)
        std::string comment;                          ///< free text stored in the session header
        FullPolicy  policy        = FullPolicy::Overwrite;  ///< behaviour when the next slot is unread
        uint32_t    waitTimeoutMs = 1000;             ///< Wait-mode gate timeout before overwriting anyway
    };

    /// Producer-side claim on one slot.
    ///
    /// Obtained from beginWrite(), finished with RingBuffer::commitWrite() or
    /// RingBuffer::abortWrite(). If the handle goes out of scope while still
    /// held, the write is aborted -- a partially filled slot is never published.
    class WriteHandle
    {
    public:
        WriteHandle() = default;
        ~WriteHandle();

        WriteHandle(WriteHandle&& other) noexcept;
        WriteHandle& operator=(WriteHandle&& other) noexcept;
        WriteHandle(const WriteHandle&)            = delete;
        WriteHandle& operator=(const WriteHandle&) = delete;

        /// @return true while the handle holds a claimed slot.
        bool held() const { return m_owner != nullptr; }

        /// @return writable payload memory of the claimed slot (nullptr if not held).
        uint8_t* data() const { return m_data; }

        /// @return payload capacity in bytes, i.e. the configured frame size.
        uint64_t capacity() const { return m_capacity; }

        /// @return global frame index this write will publish.
        uint64_t sequence() const { return m_sequence; }

        /// @return ring slot index being written.
        uint32_t slot() const { return m_slot; }

    private:
        friend class RingBuffer;
        RingBuffer* m_owner    = nullptr;
        uint8_t*    m_data     = nullptr;
        uint64_t    m_capacity = 0;
        uint64_t    m_sequence = 0;
        uint32_t    m_slot     = 0;
    };

    /// Consumer-side view of one frame. Pins the slot while held; releases it
    /// (marking the frame read) on destruction.
    ///
    /// The payload pointer aliases the ring slot, so no copy is made. In
    /// FullPolicy::Overwrite the producer may recycle the slot while the handle
    /// is held -- call stillValid() after consuming the bytes to find out
    /// whether what you saw was a consistent frame.
    class ReadHandle
    {
    public:
        ReadHandle() = default;
        ~ReadHandle();

        ReadHandle(ReadHandle&& other) noexcept;
        ReadHandle& operator=(ReadHandle&& other) noexcept;
        ReadHandle(const ReadHandle&)            = delete;
        ReadHandle& operator=(const ReadHandle&) = delete;

        /// @return true while the handle pins a slot.
        bool held() const { return m_owner != nullptr; }

        /// Re-check the slot's seqlock.
        /// @return true if the producer has not touched the slot since the
        ///         handle was acquired, i.e. the bytes read are consistent.
        bool stillValid() const;

        /// Release the slot.
        /// @param markRead  set kFlagRead and advance this consumer's ack cursor.
        /// @param markSaved set kFlagSaved.
        void release(bool markRead = true, bool markSaved = false);

        const uint8_t* data() const { return m_data; }          ///< payload bytes (aliases the slot)
        uint64_t   nbytes() const { return m_nbytes; }          ///< valid payload bytes
        uint64_t   sequence() const { return m_sequence; }      ///< global frame index
        uint32_t   slot() const { return m_slot; }              ///< ring slot index
        int64_t    tsBeforeNs() const { return m_tsBeforeNs; }  ///< monotonic ns before the read
        int64_t    tsAfterNs() const { return m_tsAfterNs; }    ///< monotonic ns after the read
        uint32_t   flags() const { return m_flags; }            ///< FrameFlag bits sampled at acquire time

    private:
        friend class RingBuffer;
        RingBuffer*    m_owner      = nullptr;
        const uint8_t* m_data       = nullptr;
        uint64_t       m_nbytes     = 0;
        uint64_t       m_sequence   = 0;
        int64_t        m_tsBeforeNs = 0;
        int64_t        m_tsAfterNs  = 0;
        uint32_t       m_slot       = 0;
        uint32_t       m_flags      = 0;
        uint32_t       m_seqStamp   = 0;
        uint32_t       m_consumer   = kAnonymousConsumer;
    };

    explicit RingBuffer(const Config& cfg);
    ~RingBuffer();

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&)                 = delete;
    RingBuffer& operator=(RingBuffer&&)      = delete;

    // --- Geometry and identity ------------------------------------------
    // All of these are normally taken from the camera once it is open, hence
    // they are setters rather than constructor arguments. They must be applied
    // before configure(); afterwards they are refused (and logged) because the
    // slot size would no longer match the allocated region.

    bool setImageSize(uint32_t width, uint32_t height);  ///< @return false if already configured or zero-sized
    bool setChannels(uint32_t channels);                 ///< channels per pixel, >= 1, not limited to 3
    bool setDepth(PixelDepth depth);                     ///< sample type of one channel value
    bool setCameraId(const std::string& id);             ///< truncated to 63 characters
    bool setComment(const std::string& comment);         ///< truncated to 8191 characters

    /// Install a custom memory provider. Must be called before configure();
    /// the default is heapMemoryOps().
    /// @return false if the ops are incomplete or the buffer is already configured.
    bool setMemoryOps(const UC2_MemoryOps& ops);

    /// Validate the geometry, allocate the region and lay out the slots. Also
    /// creates the (idle) disk streamer.
    /// @return true on success; on failure the buffer stays unconfigured.
    bool configure();

    /// @return true once configure() has succeeded.
    bool isConfigured() const { return m_configured; }

    // --- Recording ------------------------------------------------------

    /// Stamp the session start time, open every sink and, if a streaming
    /// directory is set (Config::storagePath or startStreaming()), a container file.
    /// @return true on success (also true when there is nothing to stream to).
    bool startRecording();

    /// Flush and close every sink and the container file. Safe to call when not recording.
    void stopRecording();

    /// @return true between a successful startRecording() and stopRecording().
    bool isRecording() const;

    // --- Sinks ----------------------------------------------------------

    /// Register a frame sink. Ownership of @p ops.ctx passes to the buffer:
    /// UC2_SinkOps::destroy is called for it when the buffer is destroyed.
    /// @return false if the ABI version does not match or a session is running.
    bool addSink(const UC2_SinkOps& ops);

    // --- Producer -------------------------------------------------------

    /// Claim the next slot for writing. Applies Config::policy, so this call may
    /// block for up to Config::waitTimeoutMs in FullPolicy::Wait.
    ///
    /// Samples the "before" timestamp. Exactly one producer may be inside a
    /// begin/commit pair at a time; a second concurrent call is refused.
    ///
    /// @param out [out] handle describing the claimed slot.
    /// @return true if a slot was claimed.
    bool beginWrite(WriteHandle& out);

    /// As beginWrite(), but with a caller-supplied "before" timestamp -- use it
    /// when the timestamp must be taken before the SDK call rather than before
    /// the slot claim.
    /// @param out        [out] handle describing the claimed slot.
    /// @param tsBeforeNs monotonic nanoseconds to record for this frame.
    bool beginWrite(WriteHandle& out, int64_t tsBeforeNs);

    /// Publish the frame held by @p handle: sample the "after" timestamp, set
    /// kFlagWritten, advance the write cursor and notify the sinks.
    /// @param handle a handle obtained from beginWrite(); released on return.
    /// @param nbytes valid payload bytes (clamped to the frame size).
    /// @return true if the frame was published.
    bool commitWrite(WriteHandle& handle, uint64_t nbytes);

    /// As commitWrite(), but with a caller-supplied "after" timestamp.
    bool commitWrite(WriteHandle& handle, uint64_t nbytes, int64_t tsAfterNs);

    /// Give the slot back without publishing anything. The write cursor is
    /// rolled back so frame indices stay gap-free.
    void abortWrite(WriteHandle& handle);

    /// Convenience wrapper: claim, copy @p nbytes from @p src, commit.
    /// This performs the single permitted memcpy internally.
    /// @return true if the frame was published.
    bool writeFrame(const void* src, uint64_t nbytes);

    // --- Consumers ------------------------------------------------------

    /// Claim a consumer slot in the ack table. Only registered consumers hold
    /// the producer back in FullPolicy::Wait.
    /// @return a consumer id, or kAnonymousConsumer if the table is full.
    uint32_t registerConsumer();

    /// Release a consumer id so it stops gating the producer.
    void unregisterConsumer(uint32_t consumer);

    /// Read the most recently committed frame.
    /// @param out      [out] pinned view of the frame.
    /// @param consumer consumer id from registerConsumer(), or kAnonymousConsumer.
    /// @return false if nothing has been committed yet or the frame was being rewritten.
    bool readLatest(ReadHandle& out, uint32_t consumer = kAnonymousConsumer);

    /// Block until a frame newer than @p afterSequence has been committed, so a
    /// reader can react to each new frame instead of polling readLatest().
    /// @param afterSequence sequence of the newest frame the caller has seen,
    ///                      or -1 to wait for any frame.
    /// @param timeoutMs     upper bound for the wait; 0 only checks.
    /// @return true if such a frame exists; false after the timeout, or at once
    ///         if the buffer is not configured.
    bool waitForFrame(int64_t afterSequence, uint32_t timeoutMs);

    /// Read whatever frame currently occupies ring slot @p slot.
    /// @param slot     ring slot index in [0, slotCount).
    /// @param out      [out] pinned view of the frame.
    /// @param consumer consumer id from registerConsumer(), or kAnonymousConsumer.
    /// @return false if the index is out of range or the slot holds no committed frame.
    bool readSlot(uint32_t slot, ReadHandle& out, uint32_t consumer = kAnonymousConsumer);

    /// Read the frame with global index @p sequence, if it is still resident.
    /// @param sequence global frame index as reported by WriteHandle::sequence().
    /// @param out      [out] pinned view of the frame.
    /// @param consumer consumer id from registerConsumer(), or kAnonymousConsumer.
    /// @return false if that frame has already been recycled.
    bool readSequence(uint64_t sequence, ReadHandle& out, uint32_t consumer = kAnonymousConsumer);

    /// Move a consumer's ack cursor past @p sequence without reading the frame.
    /// The disk streamer uses this to avoid stalling the producer over a frame
    /// it could not keep up with.
    void acknowledge(uint32_t consumer, uint64_t sequence);

    // --- Metadata and statistics ----------------------------------------

    uint32_t   width() const;        ///< configured image width in pixels
    uint32_t   height() const;       ///< configured image height in pixels
    uint32_t   channels() const;     ///< configured channels per pixel
    PixelDepth depth() const;        ///< configured sample type
    uint64_t   frameBytes() const;   ///< bytes of one complete image
    uint32_t   slotCount() const;    ///< number of slots in the ring
    uint64_t   regionBytes() const;  ///< total size of the allocated region
    std::string cameraId() const;    ///< configured camera identifier
    std::string comment() const;     ///< configured free-text comment
    int64_t    startWallNs() const;  ///< UTC ns at startRecording() (0 if never started)
    int64_t    startMonoNs() const;  ///< monotonic ns at the same instant

    /// Copy of the session header, e.g. to compare against a container file.
    UC2_SessionHeader sessionHeader() const;

    uint64_t framesWritten() const;   ///< frames successfully committed
    uint64_t framesOverwritten() const; ///< frames recycled before being read
    uint64_t framesAborted() const;   ///< writes abandoned via abortWrite()
    uint64_t waitTimeouts() const;    ///< Wait-mode gates that expired
    uint64_t rejectedReads() const;   ///< reads the seqlock rejected

    /// @return FrameFlag bits currently set on ring slot @p slot (0 if out of range).
    uint32_t slotFlags(uint32_t slot) const;

    // --- Disk streaming --------------------------------------------------

    /// Stream to a new container file in @p dirPath. During a session the file
    /// opens at once and saves the frames committed from now on, while the
    /// producer keeps running; otherwise it opens with the next startRecording().
    /// Already streaming to @p dirPath: nothing happens. Thread-safe against the
    /// producer and against start/stopRecording().
    /// @return false if the buffer is not configured, @p dirPath is empty or
    ///         the file cannot be created (then nothing is streamed).
    bool startStreaming(const std::string& dirPath);

    /// Stop streaming: the frames committed up to now are still written, then
    /// the file is closed. The session and the producer carry on.
    void stopStreaming();

    /// @return true while a container file is being written.
    bool isStreaming() const;

    /// @return path of the current or last container file; empty if none was opened.
    std::string streamFilePath() const;

    uint64_t streamFramesSaved() const;    ///< frames written to the container
    uint64_t streamFramesDropped() const;  ///< frames recycled before they could be saved
    bool     streamHasError() const;       ///< true if a write or open failed

private:
    friend class FileStreamSink;

    UC2_SlotRecord*       slotAt(uint32_t index);
    const UC2_SlotRecord* slotAt(uint32_t index) const;
    uint8_t*              payloadAt(const UC2_SlotRecord& rec);
    const uint8_t*        payloadAt(const UC2_SlotRecord& rec) const;

    /// Fill @p out from slot @p index under the seqlock. @return true on success.
    bool acquireSlot(uint32_t index, const uint64_t* wantSequence,
                     ReadHandle& out, uint32_t consumer);

    /// Undo an acquireSlot(): update flags, drop the pin, advance the ack cursor.
    void releaseSlot(ReadHandle& handle, bool markRead, bool markSaved);

    /// Apply Config::policy before reusing @p slot for frame @p sequence.
    void applyFullPolicy(uint32_t slot, uint64_t sequence);

    /// @return the lowest ack cursor over all registered consumers, or
    ///         UINT64_MAX when nobody is registered.
    uint64_t minAckSequence() const;

    void dispatchFrame(const UC2_SlotRecord& rec, const uint8_t* payload, uint64_t nbytes);
    void destroySinks();
    void releaseRegion();

    /// Call onSessionEnd() of the first @p count sinks (undoes a partial start).
    void endSinks(size_t count);
    /// Open a container file in m_streamDir and publish the streamer to the
    /// producer. Caller holds m_streamMtx. @return false if the file cannot be opened.
    bool openStreamLocked();
    /// Unpublish the streamer, let it finish and close its file. Caller holds m_streamMtx.
    void closeStreamLocked();

    Config                   m_cfg;
    UC2_MemoryOps            m_mem;

    uint8_t*                 m_region     = nullptr;   ///< base of the contiguous region
    uint64_t                 m_regionBytes = 0;
    uint64_t                 m_controlOffset = 0;
    uint64_t                 m_slotsOffset   = 0;
    uint64_t                 m_payloadOffset = 0;
    uint64_t                 m_slotStride    = 0;

    UC2_SessionHeader*       m_header  = nullptr;      ///< inside the region
    UC2_Control*             m_control = nullptr;      ///< inside the region

    // Pending geometry, applied to m_header by configure().
    uint32_t                 m_width    = 0;
    uint32_t                 m_height   = 0;
    uint32_t                 m_channels = 1;
    PixelDepth               m_depth    = PixelDepth::UInt8;
    std::string              m_cameraId;
    std::string              m_comment;

    bool                     m_configured = false;
    std::atomic<bool>        m_writeBusy;              ///< catches a second concurrent producer

    // Outside the region on purpose: a wake-up aid only, never the source of
    // correctness. Swapping these for process-shared pthread primitives later
    // leaves the protocol untouched.
    mutable std::mutex       m_mtx;
    std::condition_variable  m_slotFreed;   ///< a consumer released or acked a slot
    std::condition_variable  m_frameReady;  ///< a frame was committed

    std::vector<UC2_SinkOps> m_sinks;

    // The built-in streamer is kept apart from m_sinks: that list must not
    // change during a session, while streaming can be switched at any time.
    // m_stream lives from configure() to the destructor, so the pointer the
    // producer reads through m_liveStream never dangles.
    std::unique_ptr<FileStreamSink> m_stream;
    std::atomic<FileStreamSink*>    m_liveStream{nullptr};   ///< m_stream while it writes a file, else null
    std::string                     m_streamDir;             ///< directory to stream to; empty = RAM only
    std::mutex                      m_streamMtx;             ///< serialises streaming with start/stopRecording()
};

// ---------------------------------------------------------------------------
// FileStreamSink
// ---------------------------------------------------------------------------

/// Streams committed frames to a single append-only container file.
///
/// Layout of `<path>/uc2_<cameraId>_<YYYYmmdd_HHMMSS>.uc2rb` (time the file was opened):
/// @code
///   [UC2_SessionHeader  8448 B]
///   [UC2_FrameHeader 64 B][payload]   sequence n
///   [UC2_FrameHeader 64 B][payload]   sequence n + 1
///   ...
/// @endcode
/// n is the first frame committed after the file was opened: 0 when streaming
/// from the session start, larger when it was switched on during the session.
/// The header is the session's, so its start stamps stay the time base of the
/// frames' monotonic timestamps. A `.json` sidecar with the same metadata is
/// written next to it so the recording can be identified without a parser.
///
/// One instance can open and close several files in turn (RingBuffer::startStreaming()
/// / stopStreaming()); the counters below always refer to the current or last file.
///
/// The frame callback runs on the producer thread and only bumps a counter; a
/// dedicated writer thread pulls the payload out of the ring as a registered
/// consumer and hands header plus payload to `writev()` in a single syscall, so
/// the camera thread never touches the file system and no staging copy is made.
class FileStreamSink
{
public:
    /// @param owner    ring buffer to pull frames from; must outlive the sink.
    /// @param dirPath  directory to create the container file in.
    FileStreamSink(RingBuffer& owner, const std::string& dirPath);
    ~FileStreamSink();

    FileStreamSink(const FileStreamSink&)            = delete;
    FileStreamSink& operator=(const FileStreamSink&) = delete;

    /// @return a UC2_SinkOps vtable bound to this instance. The returned ops do
    ///         not own the instance (UC2_SinkOps::destroy is nullptr).
    UC2_SinkOps ops();

    std::string filePath() const;     ///< container path, empty until a file was opened
    uint64_t    framesSaved() const;  ///< frames appended to the container
    uint64_t    framesDropped() const;///< frames recycled before they could be saved
    bool        hasError() const;     ///< a write or open failed

private:
    friend class RingBuffer;   // drives the session hooks of its built-in streamer directly

    /// Directory for the next file; ignored while a file is open.
    void setDirectory(const std::string& dirPath);

    static int  sessionStartThunk(void* ctx, const UC2_SessionHeader* header);
    static int  frameThunk(void* ctx, const UC2_FrameRef* frame);
    static void sessionEndThunk(void* ctx);

    int  onSessionStart(const UC2_SessionHeader& header);
    void onFrame();
    void onSessionEnd();
    void writerLoop();
    bool writeSidecar(const UC2_SessionHeader& header) const;

    RingBuffer&           m_owner;
    std::string           m_dirPath;
    std::string           m_filePath;
    int                   m_fd = -1;

    std::thread           m_writer;
    mutable std::mutex    m_mtx;
    std::condition_variable m_cv;
    bool                  m_running = false;
    uint64_t              m_cursor  = 0;     ///< next sequence to save
    uint64_t              m_stopAt  = 0;     ///< frames to save in all, fixed by onSessionEnd()

    uint32_t              m_consumer = kAnonymousConsumer;
    std::atomic<uint64_t> m_saved;
    std::atomic<uint64_t> m_dropped;
    std::atomic<bool>     m_error;
};

} // namespace hik

#endif // UC2HIK_RINGBUFF_H
