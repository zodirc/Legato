#include "legato/engine/clock.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "FAIL clock_tests:%d %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void test_htimestamp_extrapolates() {
    legato::AudioMasterClock clock;
    clock.reset(1000);
    clock.observe_snapshot(48000, 4800, 48000, 5000, 15000, true);
    CHECK(clock.now_us() == 1000 + 900000 + 10000);
    CHECK(clock.source() == legato::kAudioClockHtimestamp);
}

void test_backwards_tstamp_uses_delay() {
    legato::AudioMasterClock clock;
    clock.reset(0);
    clock.observe_snapshot(48000, 4800, 48000, 20000, 25000, true);
    CHECK(clock.source() == legato::kAudioClockHtimestamp);
    clock.observe_snapshot(48000, 0, 48000, 10000, 40000, true);
    CHECK(clock.now_us() == 1000000);
    CHECK(clock.source() == legato::kAudioClockDelay);
}

void test_stale_tstamp_uses_delay() {
    legato::AudioMasterClock clock;
    clock.reset(0);
    clock.observe_snapshot(48000, 4800, 48000, 1000, 1000 + 500001, true);
    CHECK(clock.now_us() == 900000);
    CHECK(clock.source() == legato::kAudioClockDelay);
}

}  /* namespace */

int main() {
    test_htimestamp_extrapolates();
    test_backwards_tstamp_uses_delay();
    test_stale_tstamp_uses_delay();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d clock checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
