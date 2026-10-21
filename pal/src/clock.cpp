#include "legato/pal/clock.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace legato {

/* 单调时钟按平台分叉。Windows 没有 clock_gettime，用 QueryPerformanceCounter。 */
std::int64_t monotonic_us() {
#if defined(_WIN32)
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (frequency.QuadPart <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(now.QuadPart * 1000000 / frequency.QuadPart);
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
#endif
}

}  /* namespace legato */
