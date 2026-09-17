// Concurrency tests. The property under test is the one the whole design rests
// on: a reader that is handed a frame and afterwards finds stillValid() true
// must have seen a self-consistent image -- never a mixture of two frames.
// These are the tests that would fail if the seqlock or the memory ordering
// were wrong, so they run a few thousand frames rather than a handful.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "UC2_RingBuff.h"

using hik::PixelDepth;
using hik::RingBuffer;

namespace {

/// Stamp @p seq into the first and last eight bytes and paint the rest with a
/// byte derived from it, so that any mixture of two frames is detectable.
void paint(uint8_t* dst, uint64_t nbytes, uint64_t seq)
{
    std::memset(dst, static_cast<int>(seq & 0xFF), static_cast<size_t>(nbytes));
    std::memcpy(dst, &seq, sizeof(seq));
    std::memcpy(dst + nbytes - sizeof(seq), &seq, sizeof(seq));
}

/// @return true if @p data is a complete, untorn frame carrying @p seq.
bool intact(const uint8_t* data, uint64_t nbytes, uint64_t seq)
{
    uint64_t head = 0;
    uint64_t tail = 0;
    std::memcpy(&head, data, sizeof(head));
    std::memcpy(&tail, data + nbytes - sizeof(tail), sizeof(tail));
    const uint8_t middle = data[nbytes / 2];
    return head == seq && tail == seq && middle == static_cast<uint8_t>(seq & 0xFF);
}

} // namespace

TEST_CASE("a frame that still validates is never torn", "[ring][mt]")
{
    RingBuffer::Config cfg;
    cfg.slotCount = 8;
    cfg.policy    = hik::FullPolicy::Overwrite;   // let the producer run free

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(128, 128));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());

    const uint64_t frames = 4000;
    std::atomic<bool>     producerDone(false);
    std::atomic<uint64_t> consistent(0);
    std::atomic<uint64_t> torn(0);
    std::atomic<uint64_t> corrupt(0);   // must stay zero

    std::thread producer([&]
    {
        for (uint64_t i = 0; i < frames; ++i)
        {
            RingBuffer::WriteHandle w;
            if (!rb.beginWrite(w))
            {
                continue;
            }
            paint(w.data(), w.capacity(), i);
            rb.commitWrite(w, w.capacity());
        }
        producerDone.store(true);
    });

    // The loop deliberately does not stop the instant the producer is done: on a
    // machine that serialises threads (valgrind, a single core) the producer can
    // finish before a consumer is ever scheduled, and a reader that never read
    // anything would make this test pass vacuously.
    auto consumerBody = [&]
    {
        bool sawAny = false;
        while (!producerDone.load() || !sawAny)
        {
            RingBuffer::ReadHandle h;
            if (!rb.readLatest(h))
            {
                if (producerDone.load() && rb.framesWritten() == 0)
                {
                    break;   // nothing was ever published; nothing to wait for
                }
                std::this_thread::yield();
                continue;
            }
            sawAny = true;
            // Read the bytes FIRST, then ask whether they were stable. Doing it
            // the other way round would prove nothing.
            const bool ok  = intact(h.data(), h.nbytes(), h.sequence());
            const bool val = h.stillValid();
            if (val)
            {
                if (ok) { consistent.fetch_add(1); }
                else    { corrupt.fetch_add(1); }
            }
            else
            {
                torn.fetch_add(1);
            }
            h.release(false);
        }
    };

    std::thread c1(consumerBody);
    std::thread c2(consumerBody);

    producer.join();
    c1.join();
    c2.join();

    CHECK(rb.framesWritten() == frames);
    CHECK(corrupt.load() == 0);        // the property that matters
    CHECK(consistent.load() > 0);      // and the readers really did see frames
    INFO("consistent=" << consistent.load() << " torn-and-detected=" << torn.load());
}

TEST_CASE("under Wait a keeping-up consumer loses nothing", "[ring][mt]")
{
    RingBuffer::Config cfg;
    cfg.slotCount     = 16;
    cfg.policy        = hik::FullPolicy::Wait;
    cfg.waitTimeoutMs = 5000;

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(64, 64));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());

    const uint64_t frames = 1500;
    std::atomic<uint64_t> received(0);
    std::atomic<uint64_t> mismatched(0);
    std::atomic<bool>     timedOut(false);

    std::thread consumer([&]
    {
        const uint32_t id = rb.registerConsumer();
        REQUIRE(id != hik::kAnonymousConsumer);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (uint64_t i = 0; i < frames; ++i)
        {
            RingBuffer::ReadHandle h;
            while (!rb.readSequence(i, h, id))
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    timedOut.store(true);
                    rb.unregisterConsumer(id);
                    return;
                }
                std::this_thread::yield();
            }
            if (!intact(h.data(), h.nbytes(), i) || h.sequence() != i)
            {
                mismatched.fetch_add(1);
            }
            received.fetch_add(1);
            h.release(true);
        }
        rb.unregisterConsumer(id);
    });

    std::thread producer([&]
    {
        for (uint64_t i = 0; i < frames; ++i)
        {
            RingBuffer::WriteHandle w;
            if (!rb.beginWrite(w))
            {
                continue;
            }
            paint(w.data(), w.capacity(), i);
            rb.commitWrite(w, w.capacity());
        }
    });

    producer.join();
    consumer.join();

    CHECK_FALSE(timedOut.load());
    CHECK(rb.framesWritten() == frames);
    CHECK(received.load() == frames);       // every single frame reached the consumer
    CHECK(mismatched.load() == 0);
    CHECK(rb.framesOverwritten() == 0);
    CHECK(rb.waitTimeouts() == 0);
}

TEST_CASE("readSlot and readLatest are safe from several threads at once", "[ring][mt]")
{
    RingBuffer::Config cfg;
    cfg.slotCount = 6;
    cfg.policy    = hik::FullPolicy::Overwrite;

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(32, 32));
    REQUIRE(rb.setChannels(3));
    REQUIRE(rb.setDepth(PixelDepth::UInt16));
    REQUIRE(rb.configure());

    const uint64_t frames = 2000;
    std::atomic<bool>     done(false);
    std::atomic<uint64_t> corrupt(0);
    std::atomic<uint64_t> observed(0);

    std::thread producer([&]
    {
        for (uint64_t i = 0; i < frames; ++i)
        {
            RingBuffer::WriteHandle w;
            if (rb.beginWrite(w))
            {
                paint(w.data(), w.capacity(), i);
                rb.commitWrite(w, w.capacity());
            }
        }
        done.store(true);
    });

    // Four readers hammering every access path the public API offers.
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
    {
        readers.emplace_back([&, t]
        {
            uint32_t slot   = static_cast<uint32_t>(t);
            bool     sawAny = false;
            while (!done.load() || !sawAny)
            {
                RingBuffer::ReadHandle h;
                const bool got = (t % 2 == 0)
                    ? rb.readSlot(slot % rb.slotCount(), h)
                    : rb.readLatest(h);
                ++slot;
                if (!got)
                {
                    if (done.load() && rb.framesWritten() == 0)
                    {
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }
                sawAny = true;
                observed.fetch_add(1);
                const bool ok  = intact(h.data(), h.nbytes(), h.sequence());
                const bool val = h.stillValid();
                if (val && !ok)
                {
                    corrupt.fetch_add(1);
                }
                h.release(false);
            }
        });
    }

    producer.join();
    for (std::thread& r : readers)
    {
        r.join();
    }

    CHECK(rb.framesWritten() == frames);
    CHECK(corrupt.load() == 0);
    CHECK(observed.load() > 0);
}

TEST_CASE("streaming to disk keeps up with a live producer", "[ring][mt][stream]")
{
    RingBuffer::Config cfg;
    cfg.slotCount     = 32;
    cfg.policy        = hik::FullPolicy::Wait;
    cfg.waitTimeoutMs = 10000;

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(96, 96));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());

    // No storage path: this exercises the producer/consumer handshake at speed
    // without putting a few hundred megabytes through the file system. The
    // container format itself is covered in test_ringbuffer_stream.cpp.
    const uint32_t id = rb.registerConsumer();
    REQUIRE(id != hik::kAnonymousConsumer);

    const uint64_t frames = 3000;
    std::atomic<uint64_t> saved(0);

    std::thread saver([&]
    {
        for (uint64_t i = 0; i < frames; ++i)
        {
            RingBuffer::ReadHandle h;
            while (!rb.readSequence(i, h, id))
            {
                std::this_thread::yield();
            }
            saved.fetch_add(1);
            h.release(true, true);   // read and saved, as the streamer does
        }
    });

    for (uint64_t i = 0; i < frames; ++i)
    {
        RingBuffer::WriteHandle w;
        REQUIRE(rb.beginWrite(w));
        paint(w.data(), w.capacity(), i);
        REQUIRE(rb.commitWrite(w, w.capacity()));
    }
    saver.join();
    rb.unregisterConsumer(id);

    CHECK(saved.load() == frames);
    CHECK(rb.waitTimeouts() == 0);
    CHECK(rb.framesOverwritten() == 0);
}
