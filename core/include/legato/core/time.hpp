#ifndef LEGATO_CORE_TIME_HPP
#define LEGATO_CORE_TIME_HPP

#include <cstdint>

namespace legato {

using TimeUs = std::int64_t;

constexpr TimeUs kTimeUnset = -1;

constexpr TimeUs us_from_ms(TimeUs ms) { return ms * 1000; }
constexpr TimeUs us_from_s(TimeUs seconds) { return seconds * 1000000; }

}  /* namespace legato */

#endif
