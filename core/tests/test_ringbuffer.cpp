// Unit tests for the ring buffer core: configuration, geometry, the flag
// lifecycle, both full-slot policies and the seqlock that protects readers.
// No camera and no file system access -- the disk streamer is covered by
// test_ringbuffer_stream.cpp.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "UC2_RingBuff.h"

using hik::PixelDepth;
using hik::RingBuffer;

namespace {

/// A buffer that keeps everything in memory (no storage path).
RingBuffer::Config memConfig(uint32_t slots = 4)
{
    RingBuffer::Config c;
    c.slotCount = slots;
    c.comment   = "unit test";
    return c;
}

/// Configure @p rb as a small mono uint8 image.
bool configureSmall(RingBuffer& rb, uint32_t w = 8, uint32_t h = 4, uint32_t ch = 1,
                    PixelDepth d = PixelDepth::UInt8)
{
    return rb.setImageSize(w, h) && rb.setChannels(ch) && rb.setDepth(d) && rb.configure();
}

/// Fill @p buf with a pattern that identifies @p seq unambiguously.
std::vector<uint8_t> pattern(uint64_t seq, uint64_t nbytes)
{
    std::vector<uint8_t> v(static_cast<size_t>(nbytes));
    for (size_t i = 0; i < v.size(); ++i)
    {
        v[i] = static_cast<uint8_t>((seq * 31u + i) & 0xFF);
    }
    return v;
}

/// Counting allocator used to prove the memory vtable is actually consulted.
struct AllocStats
{
    int    acquires  = 0;
    int    releases  = 0;
    size_t bytes     = 0;
    size_t alignment = 0;
};

extern "C" {

void* testAcquire(void* ctx, size_t bytes, size_t alignment)
{
    AllocStats* s = static_cast<AllocStats*>(ctx);
    ++s->acquires;
    s->bytes     = bytes;
    s->alignment = alignment;
    void* p = nullptr;
    if (::posix_memalign(&p, alignment, bytes) != 0)
    {
        return nullptr;
    }
    return p;
}

void testRelease(void* ctx, void* base, size_t /*bytes*/)
{
    ++static_cast<AllocStats*>(ctx)->releases;
    ::free(base);
}

} // extern "C"

} // namespace

// ---------------------------------------------------------------------------
// Configuration and validation
// ---------------------------------------------------------------------------

TEST_CASE("a fresh RingBuffer is unconfigured and empty", "[ring]")
{
    RingBuffer rb(memConfig());
    CHECK_FALSE(rb.isConfigured());
    CHECK(rb.framesWritten() == 0);
    CHECK(rb.frameBytes() == 0);
    CHECK(rb.regionBytes() == 0);
    CHECK_FALSE(rb.isRecording());

    RingBuffer::ReadHandle h;
    CHECK_FALSE(rb.readLatest(h));
}

TEST_CASE("configure() refuses an unset image size", "[ring]")
{
    RingBuffer rb(memConfig());
    rb.setChannels(1);
    CHECK_FALSE(rb.configure());
    CHECK_FALSE(rb.isConfigured());
}

TEST_CASE("configure() refuses a degenerate slot count", "[ring]")
{
    for (uint32_t slots : {0u, 1u})
    {
        RingBuffer rb(memConfig(slots));
        rb.setImageSize(8, 8);
        CHECK_FALSE(rb.configure());
    }
}

TEST_CASE("setImageSize rejects zero dimensions", "[ring]")
{
    RingBuffer rb(memConfig());
    CHECK_FALSE(rb.setImageSize(0, 4));
    CHECK_FALSE(rb.setImageSize(4, 0));
    CHECK(rb.setImageSize(4, 4));
}

TEST_CASE("any positive channel count is accepted, not just 1 or 3", "[ring]")
{
    for (uint32_t ch : {1u, 2u, 3u, 5u, 17u})
    {
        RingBuffer rb(memConfig());
        REQUIRE(configureSmall(rb, 8, 4, ch));
        CHECK(rb.channels() == ch);
        CHECK(rb.frameBytes() == 8ull * 4ull * ch);
    }
}

TEST_CASE("the frame size follows the pixel depth", "[ring]")
{
    struct Case { PixelDepth d; uint64_t itemsize; const char* name; };
    const Case cases[] = {
        { PixelDepth::UInt8,   1, "uint8"   },
        { PixelDepth::UInt16,  2, "uint16"  },
        { PixelDepth::Float64, 8, "float64" },
    };

    for (const Case& c : cases)
    {
        RingBuffer rb(memConfig());
        REQUIRE(configureSmall(rb, 10, 6, 3, c.d));
        CHECK(rb.frameBytes() == 10ull * 6ull * 3ull * c.itemsize);
        CHECK(hik::itemSize(c.d) == c.itemsize);
        CHECK(std::string(hik::depthName(c.d)) == c.name);
    }
}

TEST_CASE("channel count zero is rejected", "[ring]")
{
    RingBuffer rb(memConfig());
    CHECK_FALSE(rb.setChannels(0));
}

TEST_CASE("geometry is frozen once the buffer is configured", "[ring]")
{
    RingBuffer rb(memConfig());
    REQUIRE(configureSmall(rb, 8, 4, 1));

    CHECK_FALSE(rb.setImageSize(16, 16));
    CHECK_FALSE(rb.setChannels(3));
    CHECK_FALSE(rb.setDepth(PixelDepth::UInt16));
    CHECK_FALSE(rb.setCameraId("other"));

    // The refusals must not have half-applied anything.
    CHECK(rb.width() == 8);
    CHECK(rb.height() == 4);
    CHECK(rb.channels() == 1);
    CHECK(rb.frameBytes() == 32);
}

TEST_CASE("identity metadata survives configure()", "[ring]")
{
    RingBuffer::Config c = memConfig(4);
    c.comment = "bead tracking run 7";

    RingBuffer rb(c);
    REQUIRE(rb.setCameraId("MV-CA023-10UM:01234"));
    REQUIRE(configureSmall(rb));

    CHECK(rb.cameraId() == "MV-CA023-10UM:01234");
    CHECK(rb.comment()  == "bead tracking run 7");
}

TEST_CASE("over-long metadata is truncated rather than rejected", "[ring]")
{
    RingBuffer rb(memConfig());
    REQUIRE(rb.setCameraId(std::string(200, 'x')));
    REQUIRE(rb.setComment(std::string(10000, 'y')));
    REQUIRE(configureSmall(rb));

    CHECK(rb.cameraId().size() == 63);
    CHECK(rb.comment().size()  == 8191);
}

TEST_CASE("the session header is self-describing", "[ring]")
{
    RingBuffer rb(memConfig(8));
    REQUIRE(rb.setCameraId("cam-a"));
    REQUIRE(configureSmall(rb, 12, 5, 2, PixelDepth::UInt16));

    const hik::UC2_SessionHeader hdr = rb.sessionHeader();
    CHECK(std::memcmp(hdr.magic, "UC2RB", 5) == 0);
    CHECK(hdr.formatVersion == hik::kFormatVersion);
    CHECK(hdr.layoutVersion == hik::kLayoutVersion);
    CHECK(hdr.headerBytes == sizeof(hik::UC2_SessionHeader));
    CHECK(hdr.width == 12);
    CHECK(hdr.height == 5);
    CHECK(hdr.channels == 2);
    CHECK(hdr.depth == static_cast<uint8_t>(PixelDepth::UInt16));
    CHECK(hdr.frameBytes == 12ull * 5ull * 2ull * 2ull);
    CHECK(hdr.slotCount == 8);
    CHECK(hdr.slotStride >= hdr.frameBytes);
    CHECK(std::string(hdr.cameraId) == "cam-a");
}

TEST_CASE("the region is large enough to hold every slot", "[ring]")
{
    RingBuffer rb(memConfig(16));
    REQUIRE(configureSmall(rb, 64, 64, 1));
    CHECK(rb.regionBytes() >= 16ull * rb.frameBytes());
}

// ---------------------------------------------------------------------------
// Write / read round trip
// ---------------------------------------------------------------------------

TEST_CASE("a written frame can be read back verbatim", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb, 8, 4, 1));

    const std::vector<uint8_t> src = pattern(0, rb.frameBytes());
    REQUIRE(rb.writeFrame(src.data(), src.size()));
    CHECK(rb.framesWritten() == 1);

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readLatest(h));
    CHECK(h.sequence() == 0);
    CHECK(h.nbytes() == rb.frameBytes());
    CHECK(std::memcmp(h.data(), src.data(), src.size()) == 0);
    CHECK(h.stillValid());
}

TEST_CASE("multi-byte pixel data round-trips bit-exactly", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb, 4, 2, 3, PixelDepth::Float64));

    std::vector<double> src(4 * 2 * 3);
    for (size_t i = 0; i < src.size(); ++i)
    {
        src[i] = 1.0 / static_cast<double>(i + 1);
    }
    REQUIRE(rb.writeFrame(src.data(), src.size() * sizeof(double)));

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readLatest(h));
    const double* got = reinterpret_cast<const double*>(h.data());
    for (size_t i = 0; i < src.size(); ++i)
    {
        CHECK(got[i] == src[i]);
    }
}

TEST_CASE("the two timestamps bracket the write", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    const int64_t before = hik::monotonicNs();
    const std::vector<uint8_t> src = pattern(1, rb.frameBytes());
    REQUIRE(rb.writeFrame(src.data(), src.size()));
    const int64_t after = hik::monotonicNs();

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readLatest(h));
    CHECK(h.tsBeforeNs() >= before);
    CHECK(h.tsAfterNs()  >= h.tsBeforeNs());
    CHECK(h.tsAfterNs()  <= after);
}

TEST_CASE("caller-supplied timestamps are stored unchanged", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    RingBuffer::WriteHandle w;
    REQUIRE(rb.beginWrite(w, 1111));
    std::memset(w.data(), 0, static_cast<size_t>(w.capacity()));
    REQUIRE(rb.commitWrite(w, rb.frameBytes(), 2222));

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readLatest(h));
    CHECK(h.tsBeforeNs() == 1111);
    CHECK(h.tsAfterNs()  == 2222);
}

TEST_CASE("the flag lifecycle runs writing -> written -> read -> saved", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    RingBuffer::WriteHandle w;
    REQUIRE(rb.beginWrite(w));
    CHECK((rb.slotFlags(w.slot()) & hik::kFlagWriting) != 0);
    CHECK((rb.slotFlags(w.slot()) & hik::kFlagWritten) == 0);

    const uint32_t slot = w.slot();
    std::memset(w.data(), 7, static_cast<size_t>(w.capacity()));
    REQUIRE(rb.commitWrite(w, rb.frameBytes()));

    CHECK((rb.slotFlags(slot) & hik::kFlagWritten) != 0);
    CHECK((rb.slotFlags(slot) & hik::kFlagWriting) == 0);
    CHECK((rb.slotFlags(slot) & hik::kFlagRead) == 0);

    {
        RingBuffer::ReadHandle h;
        REQUIRE(rb.readLatest(h));
        CHECK(h.flags() == hik::kFlagWritten);
        h.release(true, true);
    }
    CHECK((rb.slotFlags(slot) & hik::kFlagRead) != 0);
    CHECK((rb.slotFlags(slot) & hik::kFlagSaved) != 0);
}

TEST_CASE("a ReadHandle marks the frame read when it goes out of scope", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));
    const std::vector<uint8_t> src = pattern(0, rb.frameBytes());
    REQUIRE(rb.writeFrame(src.data(), src.size()));

    {
        RingBuffer::ReadHandle h;
        REQUIRE(rb.readLatest(h));
        CHECK(h.held());
    }
    CHECK((rb.slotFlags(0) & hik::kFlagRead) != 0);
}

// ---------------------------------------------------------------------------
// Addressing: by slot, by sequence, wrap-around
// ---------------------------------------------------------------------------

TEST_CASE("readSlot returns whatever occupies that ring slot", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    for (uint64_t i = 0; i < 3; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readSlot(1, h));
    CHECK(h.sequence() == 1);
    const std::vector<uint8_t> expect = pattern(1, rb.frameBytes());
    CHECK(std::memcmp(h.data(), expect.data(), expect.size()) == 0);
}

TEST_CASE("an empty slot and an out-of-range slot both fail cleanly", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));
    const std::vector<uint8_t> src = pattern(0, rb.frameBytes());
    REQUIRE(rb.writeFrame(src.data(), src.size()));

    RingBuffer::ReadHandle h;
    CHECK_FALSE(rb.readSlot(3, h));    // never written
    CHECK_FALSE(rb.readSlot(99, h));   // out of range
    CHECK_FALSE(h.held());
}

TEST_CASE("waitForFrame reports only frames newer than the given sequence", "[ring]")
{
    RingBuffer rb(memConfig(4));
    CHECK_FALSE(rb.waitForFrame(-1, 0));   // not configured: false at once
    REQUIRE(configureSmall(rb));

    CHECK_FALSE(rb.waitForFrame(-1, 20));   // nothing committed: runs into the timeout
    for (uint64_t i = 0; i < 2; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    CHECK(rb.waitForFrame(-1, 0));
    CHECK(rb.waitForFrame(0, 0));           // frame 1 is newer than 0
    CHECK_FALSE(rb.waitForFrame(1, 20));    // 1 is the newest
}

TEST_CASE("sequences wrap around the ring and old ones become unreachable", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    for (uint64_t i = 0; i < 10; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    CHECK(rb.framesWritten() == 10);

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readLatest(h));
    CHECK(h.sequence() == 9);
    h.release();

    // The four most recent frames are still resident...
    for (uint64_t i = 6; i <= 9; ++i)
    {
        RingBuffer::ReadHandle r;
        REQUIRE(rb.readSequence(i, r));
        CHECK(r.sequence() == i);
        const std::vector<uint8_t> expect = pattern(i, rb.frameBytes());
        CHECK(std::memcmp(r.data(), expect.data(), expect.size()) == 0);
    }
    // ...everything older has been recycled.
    for (uint64_t i = 0; i <= 5; ++i)
    {
        RingBuffer::ReadHandle r;
        CHECK_FALSE(rb.readSequence(i, r));
    }
}

// ---------------------------------------------------------------------------
// Producer discipline
// ---------------------------------------------------------------------------

TEST_CASE("only one producer may hold a write at a time", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    RingBuffer::WriteHandle a;
    RingBuffer::WriteHandle b;
    REQUIRE(rb.beginWrite(a));
    CHECK_FALSE(rb.beginWrite(b));   // refused, not silently interleaved
    CHECK_FALSE(b.held());

    rb.abortWrite(a);
    CHECK(rb.beginWrite(b));
    rb.abortWrite(b);
}

TEST_CASE("abortWrite leaves no gap in the frame numbering", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    RingBuffer::WriteHandle a;
    REQUIRE(rb.beginWrite(a));
    CHECK(a.sequence() == 0);
    rb.abortWrite(a);
    CHECK(rb.framesAborted() == 1);
    CHECK(rb.framesWritten() == 0);

    RingBuffer::WriteHandle b;
    REQUIRE(rb.beginWrite(b));
    CHECK(b.sequence() == 0);   // the abandoned index is handed out again
    std::memset(b.data(), 3, static_cast<size_t>(b.capacity()));
    REQUIRE(rb.commitWrite(b, rb.frameBytes()));
    CHECK(rb.framesWritten() == 1);

    RingBuffer::ReadHandle h;
    CHECK(rb.readLatest(h));
    CHECK(h.sequence() == 0);
}

TEST_CASE("a WriteHandle that falls out of scope aborts its slot", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    {
        RingBuffer::WriteHandle w;
        REQUIRE(rb.beginWrite(w));
        std::memset(w.data(), 9, static_cast<size_t>(w.capacity()));
        // no commit: the destructor must abort
    }
    CHECK(rb.framesAborted() == 1);
    CHECK(rb.framesWritten() == 0);

    RingBuffer::ReadHandle h;
    CHECK_FALSE(rb.readLatest(h));   // nothing was published
}

TEST_CASE("an oversized frame is truncated, not written out of bounds", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb, 4, 4, 1));   // 16 bytes per frame

    const std::vector<uint8_t> src(64, 0xAB);
    REQUIRE(rb.writeFrame(src.data(), src.size()));

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readLatest(h));
    CHECK(h.nbytes() == rb.frameBytes());
}

TEST_CASE("a short frame keeps its own length", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb, 4, 4, 1));   // 16 bytes per frame

    const std::vector<uint8_t> src(4, 0x5A);
    REQUIRE(rb.writeFrame(src.data(), src.size()));

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readLatest(h));
    CHECK(h.nbytes() == 4);
}

TEST_CASE("writing without a configured buffer fails instead of crashing", "[ring]")
{
    RingBuffer rb(memConfig(4));
    const uint8_t byte = 0;
    CHECK_FALSE(rb.writeFrame(&byte, 1));

    RingBuffer::WriteHandle w;
    CHECK_FALSE(rb.beginWrite(w));
}

TEST_CASE("writeFrame rejects a null source", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));
    CHECK_FALSE(rb.writeFrame(nullptr, 16));
}

// ---------------------------------------------------------------------------
// Full-slot policies
// ---------------------------------------------------------------------------

TEST_CASE("Overwrite never blocks and counts the frames it recycled", "[ring][policy]")
{
    RingBuffer::Config c = memConfig(2);
    c.policy = hik::FullPolicy::Overwrite;

    RingBuffer rb(c);
    REQUIRE(configureSmall(rb));

    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < 5; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(rb.framesWritten() == 5);
    CHECK(rb.framesOverwritten() == 3);   // sequences 0,1,2 were never read
    CHECK(rb.waitTimeouts() == 0);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 200);
}

TEST_CASE("Overwrite does not count frames that were read in time", "[ring][policy]")
{
    RingBuffer::Config c = memConfig(2);
    c.policy = hik::FullPolicy::Overwrite;

    RingBuffer rb(c);
    REQUIRE(configureSmall(rb));

    for (uint64_t i = 0; i < 5; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
        RingBuffer::ReadHandle h;
        REQUIRE(rb.readLatest(h));
        h.release(true);
    }
    CHECK(rb.framesOverwritten() == 0);
}

TEST_CASE("Wait overwrites after the timeout rather than stalling for ever", "[ring][policy]")
{
    RingBuffer::Config c = memConfig(2);
    c.policy        = hik::FullPolicy::Wait;
    c.waitTimeoutMs = 40;

    RingBuffer rb(c);
    REQUIRE(configureSmall(rb));

    // A consumer that registers and then never acknowledges anything -- the
    // stand-in for a hung or crashed reader process.
    const uint32_t deadConsumer = rb.registerConsumer();
    REQUIRE(deadConsumer != hik::kAnonymousConsumer);

    for (uint64_t i = 0; i < 4; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }

    CHECK(rb.framesWritten() == 4);
    CHECK(rb.waitTimeouts() == 2);   // sequences 2 and 3 had to wait out the gate
    rb.unregisterConsumer(deadConsumer);
}

TEST_CASE("Wait does not stall when the consumer keeps up", "[ring][policy]")
{
    RingBuffer::Config c = memConfig(4);
    c.policy        = hik::FullPolicy::Wait;
    c.waitTimeoutMs = 2000;

    RingBuffer rb(c);
    REQUIRE(configureSmall(rb));

    const uint32_t consumer = rb.registerConsumer();
    REQUIRE(consumer != hik::kAnonymousConsumer);

    for (uint64_t i = 0; i < 12; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));

        RingBuffer::ReadHandle h;
        REQUIRE(rb.readSequence(i, h, consumer));
        CHECK(h.sequence() == i);
        h.release(true);
    }

    CHECK(rb.framesWritten() == 12);
    CHECK(rb.waitTimeouts() == 0);
    CHECK(rb.framesOverwritten() == 0);
    rb.unregisterConsumer(consumer);
}

TEST_CASE("an anonymous reader never holds the producer back", "[ring][policy]")
{
    RingBuffer::Config c = memConfig(2);
    c.policy        = hik::FullPolicy::Wait;
    c.waitTimeoutMs = 40;

    RingBuffer rb(c);
    REQUIRE(configureSmall(rb));

    // Nobody registered, so the Wait gate has nobody to wait for.
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < 6; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(rb.waitTimeouts() == 0);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 200);
}

TEST_CASE("an unregistered consumer stops gating the producer", "[ring][policy]")
{
    RingBuffer::Config c = memConfig(2);
    c.policy        = hik::FullPolicy::Wait;
    c.waitTimeoutMs = 40;

    RingBuffer rb(c);
    REQUIRE(configureSmall(rb));

    const uint32_t consumer = rb.registerConsumer();
    REQUIRE(consumer != hik::kAnonymousConsumer);
    rb.unregisterConsumer(consumer);

    for (uint64_t i = 0; i < 6; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    CHECK(rb.waitTimeouts() == 0);
}

TEST_CASE("the consumer table is bounded and reports exhaustion", "[ring][policy]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < hik::kMaxConsumers; ++i)
    {
        const uint32_t id = rb.registerConsumer();
        CHECK(id != hik::kAnonymousConsumer);
        ids.push_back(id);
    }
    CHECK(rb.registerConsumer() == hik::kAnonymousConsumer);

    rb.unregisterConsumer(ids.front());
    CHECK(rb.registerConsumer() != hik::kAnonymousConsumer);
}

// ---------------------------------------------------------------------------
// Seqlock
// ---------------------------------------------------------------------------

TEST_CASE("a recycled slot invalidates a held ReadHandle", "[ring][seqlock]")
{
    RingBuffer::Config c = memConfig(2);
    c.policy = hik::FullPolicy::Overwrite;

    RingBuffer rb(c);
    REQUIRE(configureSmall(rb));

    const std::vector<uint8_t> first = pattern(0, rb.frameBytes());
    REQUIRE(rb.writeFrame(first.data(), first.size()));

    RingBuffer::ReadHandle h;
    REQUIRE(rb.readLatest(h));
    CHECK(h.stillValid());

    // Two more frames: sequence 2 lands back on slot 0, under the handle.
    for (uint64_t i = 1; i < 3; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }

    CHECK_FALSE(h.stillValid());   // the contract that keeps stale reads honest

    // Releasing a stale handle must not stamp flags onto the newer frame.
    h.release(true, true);
    CHECK((rb.slotFlags(0) & hik::kFlagSaved) == 0);
}

TEST_CASE("a slot being written cannot be read", "[ring][seqlock]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    RingBuffer::WriteHandle w;
    REQUIRE(rb.beginWrite(w));

    RingBuffer::ReadHandle h;
    CHECK_FALSE(rb.readSlot(w.slot(), h));
    CHECK(rb.rejectedReads() >= 1);

    rb.abortWrite(w);
}

// ---------------------------------------------------------------------------
// Memory vtable
// ---------------------------------------------------------------------------

TEST_CASE("the memory vtable is what actually allocates the region", "[ring][vtable]")
{
    AllocStats stats;
    hik::UC2_MemoryOps ops;
    ops.abiVersion = hik::kAbiVersion;
    ops.ctx        = &stats;
    ops.acquire    = &testAcquire;
    ops.release    = &testRelease;

    {
        RingBuffer rb(memConfig(8));
        REQUIRE(rb.setMemoryOps(ops));
        REQUIRE(configureSmall(rb, 32, 32, 1));

        CHECK(stats.acquires == 1);
        CHECK(stats.releases == 0);
        CHECK(stats.bytes == rb.regionBytes());
        CHECK(stats.alignment == hik::kCacheLine);

        const std::vector<uint8_t> src = pattern(0, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    CHECK(stats.releases == 1);   // released exactly once, on destruction
}

TEST_CASE("a memory vtable with the wrong ABI version is refused", "[ring][vtable]")
{
    AllocStats stats;
    hik::UC2_MemoryOps ops;
    ops.abiVersion = hik::kAbiVersion + 1;
    ops.ctx        = &stats;
    ops.acquire    = &testAcquire;
    ops.release    = &testRelease;

    RingBuffer rb(memConfig(4));
    CHECK_FALSE(rb.setMemoryOps(ops));

    ops.abiVersion = hik::kAbiVersion;
    ops.acquire    = nullptr;
    CHECK_FALSE(rb.setMemoryOps(ops));   // incomplete vtable
}

TEST_CASE("a sink vtable with the wrong ABI version is refused", "[ring][vtable]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    hik::UC2_SinkOps ops;
    std::memset(&ops, 0, sizeof(ops));
    ops.abiVersion = hik::kAbiVersion + 1;
    CHECK_FALSE(rb.addSink(ops));

    ops.abiVersion = hik::kAbiVersion;
    CHECK(rb.addSink(ops));   // all callbacks optional
}

// ---------------------------------------------------------------------------
// Storage opt-out
// ---------------------------------------------------------------------------

TEST_CASE("an empty storage path keeps everything in memory", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    CHECK(rb.streamFilePath().empty());
    CHECK(rb.streamFramesSaved() == 0);
    CHECK_FALSE(rb.streamHasError());

    REQUIRE(rb.startRecording());
    CHECK(rb.isRecording());

    const std::vector<uint8_t> src = pattern(0, rb.frameBytes());
    REQUIRE(rb.writeFrame(src.data(), src.size()));

    rb.stopRecording();
    CHECK_FALSE(rb.isRecording());
    CHECK(rb.streamFilePath().empty());
}

TEST_CASE("recording cannot start before the buffer is configured", "[ring]")
{
    RingBuffer rb(memConfig(4));
    CHECK_FALSE(rb.startRecording());
    CHECK_FALSE(rb.isRecording());
}

TEST_CASE("frames can be buffered without ever recording", "[ring]")
{
    RingBuffer rb(memConfig(4));
    REQUIRE(configureSmall(rb));

    for (uint64_t i = 0; i < 3; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    CHECK(rb.framesWritten() == 3);
    CHECK_FALSE(rb.isRecording());
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

TEST_CASE("the wall-clock formatter produces an ISO-8601 UTC stamp", "[ring][time]")
{
    // 2021-01-01T00:00:00.000Z
    const std::string s = hik::formatWallClock(1609459200000000000ll);
    CHECK(s == "2021-01-01T00:00:00.000Z");
}

TEST_CASE("the monotonic clock advances and the wall clock is plausible", "[ring][time]")
{
    const int64_t a = hik::monotonicNs();
    const int64_t b = hik::monotonicNs();
    CHECK(b >= a);
    // Later than 2020-01-01, so we are not looking at an uninitialised zero.
    CHECK(hik::wallClockNs() > 1577836800000000000ll);
}
