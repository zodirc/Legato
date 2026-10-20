#include "legato/core/log.hpp"

#include <cstdarg>
#include <cstdio>

namespace legato {

void log_write(LogLevel level, const char* fmt, ...) {
    const char* tag = "I";
    switch (level) {
    case LogLevel::Debug: tag = "D"; break;
    case LogLevel::Info: tag = "I"; break;
    case LogLevel::Warn: tag = "W"; break;
    case LogLevel::Error: tag = "E"; break;
    }
    std::fprintf(stderr, "legato %s: ", tag);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

}  /* namespace legato */
