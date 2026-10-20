#ifndef LEGATO_CORE_LOG_HPP
#define LEGATO_CORE_LOG_HPP

namespace legato {

enum class LogLevel { Debug, Info, Warn, Error };

void log_write(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  /* namespace legato */

#endif
