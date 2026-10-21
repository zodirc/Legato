#include "legato/engine/clock.hpp"

#include "legato/pal/clock.hpp"

namespace legato {

void VirtualClock::advance_us(std::int64_t delta_us) {
    if (delta_us > 0) {
        now_ += delta_us;
    }
}

std::int64_t MonotonicClock::now_us() const { return monotonic_us(); }

void AudioMasterClock::reset(std::int64_t origin_us) {
    origin_us_ = origin_us;
    position_us_ = origin_us;
}

void AudioMasterClock::observe_delay(std::int64_t written_frames, std::int64_t delay_frames, int sample_rate) {
    if (sample_rate <= 0) {
        return;
    }
    std::int64_t played = written_frames - delay_frames;
    if (played < 0) {
        played = 0;
    }
    position_us_ = origin_us_ + played * 1000000 / sample_rate;
}

}  /* namespace legato */
