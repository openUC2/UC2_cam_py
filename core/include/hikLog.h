#ifndef UC2HIK_LOG_H
#define UC2HIK_LOG_H

#include <cstdio>
#include <string>

/// @file hikLog.h
/// @brief Diagnostic logging for the C++ core with a replaceable sink.
///
/// Every diagnostic of the core goes through ERR/WARN/INFO/DBG. By default the
/// messages are printed to stderr ("ERROR: HIK::..."), so the C++ programs need
/// no setup. The Python binding installs its own sink that queues the messages
/// for a Python thread, which forwards them to a `logging` logger. No C++ thread
/// ever has to acquire the GIL to log.

namespace hik {

/// Severity of a log message. The values equal the Python `logging` levels.
enum class LogLevel : int
{
    Debug   = 10,
    Info    = 20,
    Warning = 30,
    Error   = 40
};

/// Receives every message at or above the configured level. Called on the
/// thread that logs (camera callback, writer thread, ...), so it must be
/// thread-safe, must return quickly and must not log itself.
using LogSink = void (*)(LogLevel level, const char* message, void* ctx);

/// Install @p sink (with its @p ctx) for all subsequent messages. nullptr
/// restores the stderr default. Once this returns, the previous sink is no
/// longer being called, so its context may be destroyed.
void setLogSink(LogSink sink, void* ctx);

/// Drop messages below @p minLevel before they reach the sink (default: Debug).
void setLogLevel(LogLevel minLevel);

/// @return the current minimum level.
LogLevel logLevel();

/// Deliver @p message at @p level to the current sink.
void logMessage(LogLevel level, const std::string& message);

/// @return the source tag of the calling thread: 0 unless a ScopedLogSource is
///         active on it. A sink reads it to tell which camera a message is about.
int currentLogSource();

/// Tag every message logged on this thread with @p source while the guard
/// lives (nestable). RemoteCamera uses it so that the messages of a camera
/// process reach the logger of that camera.
class ScopedLogSource
{
public:
    explicit ScopedLogSource(int source);
    ~ScopedLogSource();
    ScopedLogSource(const ScopedLogSource&)            = delete;
    ScopedLogSource& operator=(const ScopedLogSource&) = delete;

private:
    int m_previous;
};

inline void ERR(const std::string& msg)  { logMessage(LogLevel::Error, msg); }
inline void WARN(const std::string& msg) { logMessage(LogLevel::Warning, msg); }
inline void INFO(const std::string& msg) { logMessage(LogLevel::Info, msg); }
inline void DBG(const std::string& msg)  { logMessage(LogLevel::Debug, msg); }

} // namespace hik

#endif // UC2HIK_LOG_H
