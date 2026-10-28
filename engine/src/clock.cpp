#include "legato/engine/clock.hpp"

#include "legato/pal/clock.hpp"

namespace legato {

void VirtualClock::advance_us(std::int64_t delta_us) {
    if (delta_us > 0) {
        now_.fetch_add(delta_us, std::memory_order_release);
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
    source_.store(kAudioClockDelay, std::memory_order_release);
}

void AudioMasterClock::observe_snapshot(std::int64_t written_frames, std::int64_t delay_frames, int sample_rate,
                                        std::int64_t tstamp_us, std::int64_t now_us, bool have_tstamp) {
    if (sample_rate <= 0) {
        return;
    }
    std::int64_t played = written_frames - delay_frames;
    if (played < 0) {
        played = 0;
    }
    const std::int64_t base = origin_us_ + played * 1000000 / sample_rate;
    const bool fresh = have_tstamp && tstamp_us > 0 && now_us >= tstamp_us && (now_us - tstamp_us) <= 500000 &&
                       (last_tstamp_us_ < 0 || tstamp_us >= last_tstamp_us_);
    if (!fresh) {
        position_us_ = base;
        source_.store(kAudioClockDelay, std::memory_order_release);
        return;
    }
    position_us_ = base + (now_us - tstamp_us);
    last_tstamp_us_ = tstamp_us;
    source_.store(kAudioClockHtimestamp, std::memory_order_release);
}

}  /* namespace legato */
