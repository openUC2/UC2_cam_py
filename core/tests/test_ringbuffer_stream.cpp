// Tests for the disk streamer: the on-disk container must be readable without
// the writer, byte-exact, and complete. Uses a scratch directory under the
// system temp path; no camera involved.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "UC2_RingBuff.h"

using hik::PixelDepth;
using hik::RingBuffer;

namespace {

/// Scratch directory that removes itself again.
class TempDir
{
public:
    explicit TempDir(const std::string& tag)
    {
        static int counter = 0;
        m_path = std::filesystem::temp_directory_path()
               / ("uc2rb_" + tag + "_" + std::to_string(::getpid())
                  + "_" + std::to_string(counter++));
        std::filesystem::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string str() const { return m_path.string(); }

private:
    std::filesystem::path m_path;
};

/// One frame as read back from a container file.
struct Record
{
    hik::UC2_FrameHeader header{};
    std::vector<uint8_t> payload;
};

/// Parse a .uc2rb container into its header and frame records.
bool readContainer(const std::string& path, hik::UC2_SessionHeader& header,
                   std::vector<Record>& records)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in)
    {
        return false;
    }
    for (;;)
    {
        Record r;
        in.read(reinterpret_cast<char*>(&r.header), sizeof(r.header));
        if (!in)
        {
            break;   // clean end of file
        }
        r.payload.resize(static_cast<size_t>(r.header.nbytes));
        in.read(reinterpret_cast<char*>(r.payload.data()),
                static_cast<std::streamsize>(r.payload.size()));
        if (!in)
        {
            return false;   // truncated record
        }
        records.push_back(std::move(r));
    }
    return true;
}

std::vector<uint8_t> pattern(uint64_t seq, uint64_t nbytes)
{
    std::vector<uint8_t> v(static_cast<size_t>(nbytes));
    for (size_t i = 0; i < v.size(); ++i)
    {
        v[i] = static_cast<uint8_t>((seq * 31u + i) & 0xFF);
    }
    return v;
}

std::string slurp(const std::string& path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("a recorded session round-trips through the container file", "[ring][stream]")
{
    TempDir dir("basic");

    RingBuffer::Config cfg;
    cfg.storagePath   = dir.str();
    cfg.slotCount     = 8;
    cfg.comment       = "container round trip";
    cfg.policy        = hik::FullPolicy::Wait;   // lossless: no frame may be dropped
    cfg.waitTimeoutMs = 5000;

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(4, 3));
    REQUIRE(rb.setChannels(2));
    REQUIRE(rb.setDepth(PixelDepth::UInt16));
    REQUIRE(rb.setCameraId("cam-test"));
    REQUIRE(rb.configure());
    REQUIRE(rb.frameBytes() == 4 * 3 * 2 * 2);

    REQUIRE(rb.startRecording());
    const std::string path = rb.streamFilePath();
    REQUIRE_FALSE(path.empty());

    const uint64_t count = 20;
    for (uint64_t i = 0; i < count; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    rb.stopRecording();   // joins the writer thread, so the file is complete here

    CHECK_FALSE(rb.streamHasError());
    CHECK(rb.streamFramesSaved() == count);
    CHECK(rb.streamFramesDropped() == 0);

    // The file must be exactly header + count * (frame header + payload).
    REQUIRE(std::filesystem::exists(path));
    CHECK(std::filesystem::file_size(path)
          == sizeof(hik::UC2_SessionHeader)
             + count * (sizeof(hik::UC2_FrameHeader) + rb.frameBytes()));

    hik::UC2_SessionHeader header{};
    std::vector<Record>    records;
    REQUIRE(readContainer(path, header, records));

    CHECK(std::memcmp(header.magic, "UC2RB", 5) == 0);
    CHECK(header.formatVersion == hik::kFormatVersion);
    CHECK(header.headerBytes == sizeof(hik::UC2_SessionHeader));
    CHECK(header.width == 4);
    CHECK(header.height == 3);
    CHECK(header.channels == 2);
    CHECK(header.depth == static_cast<uint8_t>(PixelDepth::UInt16));
    CHECK(header.frameBytes == rb.frameBytes());
    CHECK(header.slotCount == 8);
    CHECK(std::string(header.cameraId) == "cam-test");
    CHECK(std::string(header.comment) == "container round trip");
    CHECK(header.startWallNs > 0);
    CHECK(header.startMonoNs > 0);

    REQUIRE(records.size() == count);
    for (uint64_t i = 0; i < count; ++i)
    {
        const Record& r = records[static_cast<size_t>(i)];
        INFO("record " << i);
        CHECK(r.header.sequence == i);
        CHECK(r.header.nbytes == rb.frameBytes());
        CHECK(r.header.width == 4);
        CHECK(r.header.height == 3);
        CHECK(r.header.channels == 2);
        CHECK(r.header.depth == static_cast<uint8_t>(PixelDepth::UInt16));
        CHECK((r.header.flags & hik::kFlagWritten) != 0);
        CHECK((r.header.flags & hik::kFlagSaved) != 0);
        CHECK(r.header.tsAfterNs >= r.header.tsBeforeNs);
        CHECK(r.header.tsBeforeNs >= header.startMonoNs);

        const std::vector<uint8_t> expect = pattern(i, rb.frameBytes());
        CHECK(r.payload == expect);
    }
}

TEST_CASE("the JSON sidecar describes the recording", "[ring][stream]")
{
    TempDir dir("sidecar");

    RingBuffer::Config cfg;
    cfg.storagePath = dir.str();
    cfg.slotCount   = 4;
    cfg.comment     = "with \"quotes\" and a\nnewline";

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(6, 2));
    REQUIRE(rb.setChannels(5));            // deliberately not 1 or 3
    REQUIRE(rb.setDepth(PixelDepth::Float64));
    REQUIRE(rb.setCameraId("cam/with:odd chars"));
    REQUIRE(rb.configure());
    REQUIRE(rb.startRecording());

    const std::string container = rb.streamFilePath();
    rb.stopRecording();

    const std::string sidecar =
        std::filesystem::path(container).replace_extension(".json").string();
    REQUIRE(std::filesystem::exists(sidecar));

    const std::string text = slurp(sidecar);
    CHECK(text.find("\"format\": \"uc2rb\"") != std::string::npos);
    CHECK(text.find("\"channels\": 5") != std::string::npos);
    CHECK(text.find("\"dtype\": \"float64\"") != std::string::npos);
    CHECK(text.find("\"frameBytes\": 480") != std::string::npos);   // 6*2*5*8
    CHECK(text.find("\"cameraId\": \"cam/with:odd chars\"") != std::string::npos);
    CHECK(text.find("\\\"quotes\\\"") != std::string::npos);   // escaped, not raw
    CHECK(text.find("\\n") != std::string::npos);
    CHECK(text.find("\"startTimeUtc\": \"20") != std::string::npos);

    // Odd characters are sanitised out of the file name but kept in the metadata.
    const std::string name = std::filesystem::path(container).filename().string();
    CHECK(name.find('/') == std::string::npos);
    CHECK(name.find(':') == std::string::npos);
    CHECK(name.find(' ') == std::string::npos);
}

TEST_CASE("only frames committed after startRecording are saved", "[ring][stream]")
{
    TempDir dir("late");

    RingBuffer::Config cfg;
    cfg.storagePath   = dir.str();
    cfg.slotCount     = 8;
    cfg.policy        = hik::FullPolicy::Wait;
    cfg.waitTimeoutMs = 5000;

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(4, 4));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());

    // Three frames buffered before the session opens.
    for (uint64_t i = 0; i < 3; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }

    REQUIRE(rb.startRecording());
    const std::string path = rb.streamFilePath();

    for (uint64_t i = 3; i < 8; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    rb.stopRecording();

    CHECK(rb.framesWritten() == 8);
    CHECK(rb.streamFramesSaved() == 5);

    hik::UC2_SessionHeader header{};
    std::vector<Record>    records;
    REQUIRE(readContainer(path, header, records));
    REQUIRE(records.size() == 5);
    CHECK(records.front().header.sequence == 3);
    CHECK(records.back().header.sequence == 7);
}

TEST_CASE("a second session in the same second does not clobber the first", "[ring][stream]")
{
    TempDir dir("restart");

    RingBuffer::Config cfg;
    cfg.storagePath   = dir.str();
    cfg.slotCount     = 4;
    cfg.policy        = hik::FullPolicy::Wait;
    cfg.waitTimeoutMs = 5000;

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(4, 4));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());

    REQUIRE(rb.startRecording());
    const std::string first = rb.streamFilePath();
    const std::vector<uint8_t> src = pattern(0, rb.frameBytes());
    REQUIRE(rb.writeFrame(src.data(), src.size()));
    rb.stopRecording();

    REQUIRE(rb.startRecording());
    const std::string second = rb.streamFilePath();
    REQUIRE(rb.writeFrame(src.data(), src.size()));
    rb.stopRecording();

    CHECK(first != second);
    CHECK(std::filesystem::exists(first));
    CHECK(std::filesystem::exists(second));
    CHECK(std::filesystem::file_size(first) > sizeof(hik::UC2_SessionHeader));
}

TEST_CASE("a storage path that cannot be created is reported, not ignored", "[ring][stream]")
{
    RingBuffer::Config cfg;
    cfg.storagePath = "/dev/null/this-cannot-be-a-directory";
    cfg.slotCount   = 4;

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(4, 4));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());          // the buffer itself is fine

    CHECK_FALSE(rb.startRecording()); // but the session must refuse to open
    CHECK_FALSE(rb.isRecording());
    CHECK(rb.streamHasError());

    // The in-memory ring keeps working regardless of the storage failure.
    const std::vector<uint8_t> src = pattern(0, rb.frameBytes());
    CHECK(rb.writeFrame(src.data(), src.size()));
    CHECK(rb.framesWritten() == 1);
}

TEST_CASE("a large multi-channel frame survives the container round trip", "[ring][stream]")
{
    TempDir dir("large");

    RingBuffer::Config cfg;
    cfg.storagePath   = dir.str();
    cfg.slotCount     = 4;
    cfg.policy        = hik::FullPolicy::Wait;
    cfg.waitTimeoutMs = 5000;

    RingBuffer rb(cfg);
    REQUIRE(rb.setImageSize(320, 240));
    REQUIRE(rb.setChannels(7));                  // > 3 channels, as required
    REQUIRE(rb.setDepth(PixelDepth::UInt16));
    REQUIRE(rb.configure());
    REQUIRE(rb.frameBytes() == 320ull * 240ull * 7ull * 2ull);

    REQUIRE(rb.startRecording());
    const std::string path = rb.streamFilePath();

    for (uint64_t i = 0; i < 6; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
    rb.stopRecording();

    CHECK(rb.streamFramesSaved() == 6);
    CHECK_FALSE(rb.streamHasError());

    hik::UC2_SessionHeader header{};
    std::vector<Record>    records;
    REQUIRE(readContainer(path, header, records));
    REQUIRE(records.size() == 6);
    for (uint64_t i = 0; i < 6; ++i)
    {
        CHECK(records[static_cast<size_t>(i)].payload == pattern(i, rb.frameBytes()));
    }
}

TEST_CASE("destroying a recording buffer closes the file cleanly", "[ring][stream]")
{
    TempDir dir("dtor");
    std::string path;
    uint64_t    frameBytes = 0;

    {
        RingBuffer::Config cfg;
        cfg.storagePath   = dir.str();
        cfg.slotCount     = 4;
        cfg.policy        = hik::FullPolicy::Wait;
        cfg.waitTimeoutMs = 5000;

        RingBuffer rb(cfg);
        REQUIRE(rb.setImageSize(4, 4));
        REQUIRE(rb.setChannels(1));
        REQUIRE(rb.configure());
        REQUIRE(rb.startRecording());
        path       = rb.streamFilePath();
        frameBytes = rb.frameBytes();

        for (uint64_t i = 0; i < 4; ++i)
        {
            const std::vector<uint8_t> src = pattern(i, frameBytes);
            REQUIRE(rb.writeFrame(src.data(), src.size()));
        }
        // No stopRecording(): the destructor has to do it.
    }

    hik::UC2_SessionHeader header{};
    std::vector<Record>    records;
    REQUIRE(readContainer(path, header, records));
    CHECK(records.size() == 4);
    CHECK(records.back().header.sequence == 3);
}

namespace {

/// Small in-memory ring (no storage path) for the streaming switch tests.
RingBuffer::Config toggleConfig(hik::FullPolicy policy)
{
    RingBuffer::Config cfg;
    cfg.slotCount     = 8;
    cfg.policy        = policy;
    cfg.waitTimeoutMs = 5000;
    return cfg;
}

void writeFrames(RingBuffer& rb, uint64_t from, uint64_t to)
{
    for (uint64_t i = from; i < to; ++i)
    {
        const std::vector<uint8_t> src = pattern(i, rb.frameBytes());
        REQUIRE(rb.writeFrame(src.data(), src.size()));
    }
}

} // namespace

TEST_CASE("streaming can be switched on and off during a session", "[ring][stream]")
{
    TempDir dir("toggle");
    RingBuffer rb(toggleConfig(hik::FullPolicy::Wait));
    REQUIRE(rb.setImageSize(4, 4));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());
    REQUIRE(rb.startRecording());
    CHECK_FALSE(rb.isStreaming());
    CHECK(rb.streamFilePath().empty());

    writeFrames(rb, 0, 3);                    // RAM only
    REQUIRE(rb.startStreaming(dir.str()));
    CHECK(rb.isStreaming());
    const std::string path = rb.streamFilePath();
    writeFrames(rb, 3, 7);                    // these four go to the file
    rb.stopStreaming();
    CHECK_FALSE(rb.isStreaming());
    CHECK(rb.isRecording());                  // the session carries on
    writeFrames(rb, 7, 10);                   // RAM only again
    rb.stopRecording();

    CHECK(rb.framesWritten() == 10);
    CHECK(rb.streamFramesSaved() == 4);
    hik::UC2_SessionHeader header{};
    std::vector<Record>    records;
    REQUIRE(readContainer(path, header, records));
    REQUIRE(records.size() == 4);
    for (size_t k = 0; k < records.size(); ++k)
    {
        CHECK(records[k].header.sequence == 3 + k);
        CHECK(records[k].payload == pattern(3 + k, rb.frameBytes()));
    }
}

TEST_CASE("each switch-on opens a new file, a directory set early opens with the session", "[ring][stream]")
{
    TempDir dir("again");
    RingBuffer rb(toggleConfig(hik::FullPolicy::Wait));
    REQUIRE(rb.setImageSize(4, 4));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());

    REQUIRE(rb.startStreaming(dir.str()));    // before the session: only remembered
    CHECK_FALSE(rb.isStreaming());
    REQUIRE(rb.startRecording());
    CHECK(rb.isStreaming());
    const std::string first = rb.streamFilePath();
    writeFrames(rb, 0, 1);
    rb.stopStreaming();

    REQUIRE(rb.startStreaming(dir.str()));
    const std::string second = rb.streamFilePath();
    writeFrames(rb, 1, 3);
    rb.stopRecording();                       // also closes the second file
    CHECK_FALSE(rb.isStreaming());

    CHECK(first != second);
    CHECK(rb.streamFramesSaved() == 2);       // the counters follow the current file
    hik::UC2_SessionHeader header{};
    std::vector<Record>    a, b;
    REQUIRE(readContainer(first, header, a));
    REQUIRE(readContainer(second, header, b));
    CHECK(a.size() == 1);
    REQUIRE(b.size() == 2);
    CHECK(b.front().header.sequence == 1);
}

TEST_CASE("a streaming directory that cannot be created leaves the session running", "[ring][stream]")
{
    RingBuffer rb(toggleConfig(hik::FullPolicy::Overwrite));
    REQUIRE(rb.setImageSize(4, 4));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());
    REQUIRE(rb.startRecording());

    CHECK_FALSE(rb.startStreaming("/dev/null/this-cannot-be-a-directory"));
    CHECK_FALSE(rb.startStreaming(""));
    CHECK_FALSE(rb.isStreaming());
    CHECK(rb.streamHasError());
    CHECK(rb.isRecording());
    writeFrames(rb, 0, 2);
    CHECK(rb.framesWritten() == 2);
}

TEST_CASE("stopStreaming finishes although the producer keeps writing", "[ring][stream][mt]")
{
    TempDir dir("live");
    // Wait: the writer holds the producer back, so it never overwrites a frame
    // still being saved (no intentional payload race, see tsan.supp) and the
    // writer can never catch up with the tip on its own - stopping has to end it.
    RingBuffer rb(toggleConfig(hik::FullPolicy::Wait));
    REQUIRE(rb.setImageSize(4, 4));
    REQUIRE(rb.setChannels(1));
    REQUIRE(rb.configure());
    REQUIRE(rb.startRecording());

    std::atomic<bool> done(false);
    std::thread producer([&]
    {
        const std::vector<uint8_t> src = pattern(0, rb.frameBytes());
        while (!done.load())
        {
            rb.writeFrame(src.data(), src.size());   // as fast as it goes
        }
    });

    REQUIRE(rb.startStreaming(dir.str()));
    const std::string path = rb.streamFilePath();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto t0 = std::chrono::steady_clock::now();
    rb.stopStreaming();                        // must not chase the producer's moving tip
    const auto took = std::chrono::steady_clock::now() - t0;
    const uint64_t writtenAfterStop = rb.framesWritten();
    const uint64_t handled          = rb.streamFramesSaved() + rb.streamFramesDropped();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    done.store(true);
    producer.join();
    rb.stopRecording();

    CHECK(took < std::chrono::seconds(2));
    CHECK(rb.streamFramesSaved() + rb.streamFramesDropped() == handled);   // nothing after the stop
    CHECK(rb.streamFramesDropped() == 0);
    CHECK(rb.framesWritten() > writtenAfterStop);                          // the producer went on

    hik::UC2_SessionHeader header{};
    std::vector<Record>    records;
    REQUIRE(readContainer(path, header, records));
    REQUIRE_FALSE(records.empty());
    CHECK(records.size() == rb.streamFramesSaved());
    CHECK(records.back().header.sequence < writtenAfterStop);
}
