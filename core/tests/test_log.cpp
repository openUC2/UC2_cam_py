// Unit tests for the replaceable log sink (hikLog.h). No camera needed.
// hikLog.h comes first: Catch2 defines function-like INFO/WARN macros that would
// otherwise mangle the definitions of hik::INFO/WARN. In the tests, (hik::INFO)(...)
// keeps the preprocessor from expanding the macro at the call sites.
#include "hikLog.h"

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Capture
{
    std::mutex                                        mtx;
    std::vector<std::pair<hik::LogLevel, std::string>> msgs;
};

void captureSink(hik::LogLevel level, const char* message, void* ctx)
{
    auto* c = static_cast<Capture*>(ctx);
    std::lock_guard<std::mutex> lk(c->mtx);
    c->msgs.emplace_back(level, message);
}

/// Restores the stderr default and the full level range when a test ends,
/// whatever happened inside it.
struct SinkGuard
{
    ~SinkGuard()
    {
        hik::setLogSink(nullptr, nullptr);
        hik::setLogLevel(hik::LogLevel::Debug);
    }
};

} // namespace

TEST_CASE("log levels carry the Python logging values", "[log]")
{
    CHECK(static_cast<int>(hik::LogLevel::Debug) == 10);
    CHECK(static_cast<int>(hik::LogLevel::Info) == 20);
    CHECK(static_cast<int>(hik::LogLevel::Warning) == 30);
    CHECK(static_cast<int>(hik::LogLevel::Error) == 40);
}

TEST_CASE("a custom sink receives every level with its text", "[log]")
{
    SinkGuard guard;
    Capture   cap;
    hik::setLogSink(&captureSink, &cap);

    hik::DBG("d");
    (hik::INFO)("i");
    (hik::WARN)("w");
    hik::ERR("e");

    REQUIRE(cap.msgs.size() == 4);
    CHECK(cap.msgs[0] == std::make_pair(hik::LogLevel::Debug, std::string("d")));
    CHECK(cap.msgs[1] == std::make_pair(hik::LogLevel::Info, std::string("i")));
    CHECK(cap.msgs[2] == std::make_pair(hik::LogLevel::Warning, std::string("w")));
    CHECK(cap.msgs[3] == std::make_pair(hik::LogLevel::Error, std::string("e")));
}

TEST_CASE("messages below the minimum level never reach the sink", "[log]")
{
    SinkGuard guard;
    Capture   cap;
    hik::setLogSink(&captureSink, &cap);
    hik::setLogLevel(hik::LogLevel::Warning);
    CHECK(hik::logLevel() == hik::LogLevel::Warning);

    hik::DBG("dropped");
    (hik::INFO)("dropped");
    (hik::WARN)("kept");
    hik::ERR("kept");

    REQUIRE(cap.msgs.size() == 2);
    CHECK(cap.msgs[0].first == hik::LogLevel::Warning);
    CHECK(cap.msgs[1].first == hik::LogLevel::Error);
}

TEST_CASE("replacing the sink stops calls to the previous one", "[log]")
{
    SinkGuard guard;
    Capture   first;
    Capture   second;

    hik::setLogSink(&captureSink, &first);
    (hik::INFO)("one");
    hik::setLogSink(&captureSink, &second);
    (hik::INFO)("two");
    hik::setLogSink(nullptr, nullptr);   // back to stderr
    hik::DBG("stderr only (expected in the test output)");

    REQUIRE(first.msgs.size() == 1);
    CHECK(first.msgs[0].second == "one");
    REQUIRE(second.msgs.size() == 1);
    CHECK(second.msgs[0].second == "two");
}

TEST_CASE("concurrent logging from several threads reaches the sink intact", "[log][mt]")
{
    SinkGuard guard;
    Capture   cap;
    hik::setLogSink(&captureSink, &cap);

    const int threads = 4;
    const int perThread = 500;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
    {
        pool.emplace_back([t] {
            for (int i = 0; i < perThread; ++i)
            {
                (hik::INFO)("t" + std::to_string(t) + ":" + std::to_string(i));
            }
        });
    }
    for (std::thread& th : pool)
    {
        th.join();
    }

    REQUIRE(cap.msgs.size() == static_cast<size_t>(threads * perThread));
    for (const auto& m : cap.msgs)
    {
        CHECK(m.second.size() >= 4);   // "tN:i" - never torn or empty
    }
}
