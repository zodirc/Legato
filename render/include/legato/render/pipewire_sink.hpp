#ifndef LEGATO_RENDER_PIPEWIRE_SINK_HPP
#define LEGATO_RENDER_PIPEWIRE_SINK_HPP

#include "legato/render/sink.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace legato {

/* Linux 桌面优先走 PipeWire。运行时 dlopen，本机没有开发头文件。打不开则由 LinuxAudioSink 改试 ALSA。 */
class PipeWireSink : public AudioSink {
public:
    ~PipeWireSink() override;
    int open(int sample_rate, int channels) override;
    void present(std::int64_t pts_us) override;
    void close() override;
    int write_frames(const std::int16_t* interleaved, int frames) override;
    void drop() override;
    void drain() override;
    void end_interrupt() override;
    int delay_frames() const override;
    DelaySnapshot delay_snapshot() const override;
    int rate() const override { return rate_; }
    int channels() const override { return channels_; }
    void on_process();
    void on_param(std::uint32_t id);

private:
    void* loop_ = nullptr;
    void* context_ = nullptr;
    void* core_ = nullptr;
    void* stream_ = nullptr;
    bool started_ = false;
    int rate_ = 0;
    int channels_ = 0;
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> dying_{false};
    mutable std::mutex ring_mu_;
    std::condition_variable ring_cv_;
    std::vector<std::int16_t> ring_;
    std::size_t read_ = 0;
    std::size_t count_ = 0;
};

}  /* namespace legato */

#endif
