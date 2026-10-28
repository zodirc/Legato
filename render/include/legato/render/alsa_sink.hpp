#ifndef LEGATO_RENDER_ALSA_SINK_HPP
#define LEGATO_RENDER_ALSA_SINK_HPP

#include "legato/render/sink.hpp"

#include <atomic>

namespace legato {

/* Linux 声卡。交错 S16_LE。本机没有 ALSA 头文件，实现里自己声明用到的 C 接口。 */
class AlsaSink : public AudioSink {
public:
    ~AlsaSink() override;
    int open(int sample_rate, int channels) override;
    void present(std::int64_t pts_us) override;
    void close() override;
    /* >0 写入的帧数；0 被 drop 打断（暂停或 seek）；<0 设备错误。 */
    int write_frames(const std::int16_t* interleaved, int frames) override;
    void drop() override;
    /* drop 会留下中断标记。Seek 在解复用线程上 end_interrupt，否则下一包会被直接丢掉。 */
    void end_interrupt() override;
    void drain() override;
    int delay_frames() const override;
    DelaySnapshot delay_snapshot() const override;
    int rate() const override { return rate_; }
    int channels() const override { return channels_; }

private:
    void* pcm_ = nullptr;
    /* open 时分配，输出线程反复查询时复用，避免稳态分配。 */
    void* status_ = nullptr;
    int rate_ = 0;
    int channels_ = 0;
    std::atomic<bool> interrupted_{false};
};

}  /* namespace legato */

#endif
