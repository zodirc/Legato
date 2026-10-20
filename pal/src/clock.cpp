#include "legato/pal/clock.hpp"

#include <time.h>

namespace legato {

std::int64_t monotonic_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

}  /* namespace legato */
