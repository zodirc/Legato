#include "legato/render/linux_audio.hpp"

#include "legato/engine/test_hooks.hpp"

namespace legato {

int LinuxAudioSink::open(int sample_rate, int channels) {
    close();
    if (test_no_audio_device()) {
        return -1;
    }
    if (pipewire_.open(sample_rate, channels) == 0) {
        active_ = &pipewire_;
        return 0;
    }
#if defined(LEGATO_HAS_ALSA)
    if (alsa_.open(sample_rate, channels) == 0) {
        active_ = &alsa_;
        return 0;
    }
#endif
    return -1;
}

void LinuxAudioSink::present(std::int64_t pts_us) {
    if (active_ != nullptr) {
        active_->present(pts_us);
    }
}

void LinuxAudioSink::close() {
    pipewire_.close();
#if defined(LEGATO_HAS_ALSA)
    alsa_.close();
#endif
    active_ = nullptr;
}

int LinuxAudioSink::write_frames(const std::int16_t* interleaved, int frames) {
    return active_ != nullptr ? active_->write_frames(interleaved, frames) : -1;
}

void LinuxAudioSink::drop() {
    if (active_ != nullptr) {
        active_->drop();
    }
}

void LinuxAudioSink::drain() {
    if (active_ != nullptr) {
        active_->drain();
    }
}

void LinuxAudioSink::end_interrupt() {
    if (active_ != nullptr) {
        active_->end_interrupt();
    }
}

int LinuxAudioSink::delay_frames() const { return active_ != nullptr ? active_->delay_frames() : 0; }

DelaySnapshot LinuxAudioSink::delay_snapshot() const {
    return active_ != nullptr ? active_->delay_snapshot() : DelaySnapshot{};
}

int LinuxAudioSink::rate() const { return active_ != nullptr ? active_->rate() : 0; }

int LinuxAudioSink::channels() const { return active_ != nullptr ? active_->channels() : 0; }

}  /* namespace legato */
