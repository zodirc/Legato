#ifndef LEGATO_RENDER_LINUX_AUDIO_HPP
#define LEGATO_RENDER_LINUX_AUDIO_HPP

#include "legato/render/pipewire_sink.hpp"
#if defined(LEGATO_HAS_ALSA)
#include "legato/render/alsa_sink.hpp"
#endif

namespace legato {

/* PipeWire 优先。它失败后再打开 ALSA。两条都失败时 open 返回 -1，由会话决定回落还是报错。 */
class LinuxAudioSink : public AudioSink {
public:
    int open(int sample_rate, int channels) override;
    void present(std::int64_t pts_us) override;
    void close() override;
    int write_frames(const std::int16_t* interleaved, int frames) override;
    void drop() override;
    void drain() override;
    void end_interrupt() override;
    int delay_frames() const override;
    DelaySnapshot delay_snapshot() const override;
    int rate() const override;
    int channels() const override;

private:
    AudioSink* active_ = nullptr;
    PipeWireSink pipewire_;
#if defined(LEGATO_HAS_ALSA)
    AlsaSink alsa_;
#endif
};

}  /* namespace legato */

#endif
