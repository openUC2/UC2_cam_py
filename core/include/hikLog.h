#ifndef UC2HIK_LOG_H
#define UC2HIK_LOG_H

#include <cstdio>
#include <string>

// Defined inline: this header is included by every core translation unit, so
// non-inline definitions would collide at link time.

namespace hik {
    inline void ERR(const std::string& msg)
    {
        std::fprintf(stderr, "ERROR: HIK::%s\n", msg.c_str());
    }
    inline void WARN(const std::string& msg)
    {
        std::fprintf(stderr, "WARNING: HIK::%s\n", msg.c_str());
    }
    inline void INFO(const std::string& msg)
    {
        std::fprintf(stderr, "INFO: HIK::%s\n", msg.c_str());
    }
    inline void DBG(const std::string& msg)
    {
        std::fprintf(stderr, "DEBUG: HIK::%s\n", msg.c_str());
    }
}

#endif // UC2HIK_LOG_H
