#include "hikLog.h"

#include <atomic>
#include <mutex>

namespace hik {

namespace {

void stderrSink(LogLevel level, const char* message, void* /*ctx*/)
{
    const char* tag = "DEBUG";
    switch (level)
    {
    case LogLevel::Error:   tag = "ERROR";   break;
    case LogLevel::Warning: tag = "WARNING"; break;
    case LogLevel::Info:    tag = "INFO";    break;
    case LogLevel::Debug:   tag = "DEBUG";   break;
    }
    std::fprintf(stderr, "%s: HIK::%s\n", tag, message);
}

// The sink is called with g_mtx held. That is what lets setLogSink() promise
// that the old sink is no longer running once it returns, and it serialises
// the stderr output of concurrent threads.
std::mutex       g_mtx;
LogSink          g_sink = &stderrSink;
void*            g_ctx  = nullptr;
std::atomic<int> g_minLevel(static_cast<int>(LogLevel::Debug));

thread_local int t_source = 0;

} // namespace

int currentLogSource()
{
    return t_source;
}

ScopedLogSource::ScopedLogSource(int source)
    : m_previous(t_source)
{
    t_source = source;
}

ScopedLogSource::~ScopedLogSource()
{
    t_source = m_previous;
}

void setLogSink(LogSink sink, void* ctx)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_sink = (sink != nullptr) ? sink : &stderrSink;
    g_ctx  = (sink != nullptr) ? ctx : nullptr;
}

void setLogLevel(LogLevel minLevel)
{
    g_minLevel.store(static_cast<int>(minLevel), std::memory_order_relaxed);
}

LogLevel logLevel()
{
    return static_cast<LogLevel>(g_minLevel.load(std::memory_order_relaxed));
}

void logMessage(LogLevel level, const std::string& message)
{
    if (static_cast<int>(level) < g_minLevel.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_sink(level, message.c_str(), g_ctx);
}

} // namespace hik
