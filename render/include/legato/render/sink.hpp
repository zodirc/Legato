#ifndef LEGATO_RENDER_SINK_HPP
#define LEGATO_RENDER_SINK_HPP

#include <atomic>
#include <cstdint>
#include <memory>

namespace legato {

struct VideoPresent {
    std::int64_t pts_us = 0;
    std::uint64_t content = 0;
    std::int64_t origin_us = 0;
    std::int64_t audio_clock_us = 0;
    const std::uint8_t* data0 = nullptr;
    int linesize0 = 0;
    int width = 0;
    int height = 0;
    /* 调用 present_sample 期间有效。显示 Sink 要留到下一帧，就在这里自己再引用一份。 */
    void* retain = nullptr;
};

/* delay 和硬件时间戳必须同一次快照。分开查时，两者之间设备已经前进。 */
struct DelaySnapshot {
    int frames = 0;
    std::int64_t tstamp_us = 0;
    bool have_tstamp = false;
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual int open(int sample_rate, int channels) = 0;
    virtual void present(std::int64_t pts_us) = 0;
    virtual void close() = 0;
    virtual int write_frames(const std::int16_t* interleaved, int frames) = 0;
    virtual void drop() = 0;
    virtual void drain() = 0;
    virtual void end_interrupt() = 0;
    virtual int delay_frames() const = 0;
    virtual DelaySnapshot delay_snapshot() const {
        DelaySnapshot snap;
        snap.frames = delay_frames();
        return snap;
    }
    virtual int rate() const = 0;
    virtual int channels() const = 0;
    virtual void bind_cancel(const std::atomic<bool>* /*cancel*/) {}
    virtual void reset() {}
};

class VideoSink {
public:
    virtual ~VideoSink() = default;
    virtual void present(std::int64_t pts_us) = 0;
    virtual void present_sample(const VideoPresent& sample) { present(sample.pts_us); }
    virtual int presented() const { return 0; }
    virtual std::int64_t worst_sync_us() const { return 0; }
    virtual int copy_av_sync_us(std::int64_t* /*dst*/, int /*capacity*/) const { return 0; }
    virtual bool copy_hash(char* /*out*/, std::size_t /*out_len*/) const { return false; }
};

/* 确定性回归的音频 Sink。按虚拟媒体时钟消费样本，不写 PCM 设备。 */
class NullAudioSink : public AudioSink {
public:
    int open(int sample_rate, int channels) override;
    void present(std::int64_t pts_us) override;
    void close() override;
    int write_frames(const std::int16_t* interleaved, int frames) override;
    void drop() override;
    void drain() override;
    void end_interrupt() override;
    int delay_frames() const override;
    int rate() const override { return rate_; }
    int channels() const override { return channels_; }
    void bind_cancel(const std::atomic<bool>* cancel) override;
    void reset() override;
    std::int64_t consume(int samples);
    std::int64_t clock_us() const;

private:
    const std::atomic<bool>* cancel_ = nullptr;
    int rate_ = 0;
    int channels_ = 0;
    std::int64_t consumed_ = 0;
};

/* 打不开 PCM 设备时按样本时长推进。输出线程因此能走到流结束，软件节拍那条分支不会发 EOS。 */
class FallbackAudioSink : public AudioSink {
public:
    int open(int sample_rate, int channels) override;
    void present(std::int64_t pts_us) override;
    void close() override;
    int write_frames(const std::int16_t* interleaved, int frames) override;
    void drop() override;
    void drain() override;
    void end_interrupt() override;
    int delay_frames() const override;
    int rate() const override { return rate_; }
    int channels() const override { return channels_; }

private:
    int rate_ = 0;
    int channels_ = 0;
    std::atomic<bool> interrupted_{false};
};

/* 本机没有 PCM 设备时的占位。open 失败，调用方报 audio_device_unavailable。 */
class UnavailableAudioSink : public AudioSink {
public:
    int open(int /*sample_rate*/, int /*channels*/) override { return -1; }
    void present(std::int64_t /*pts_us*/) override {}
    void close() override {}
    int write_frames(const std::int16_t* /*interleaved*/, int /*frames*/) override { return -1; }
    void drop() override {}
    void drain() override {}
    void end_interrupt() override {}
    int delay_frames() const override { return 0; }
    int rate() const override { return 0; }
    int channels() const override { return 0; }
};

/* 本地文件的显示 Sink。音画偏差口径与 RecordingVideoSink 相同。
   像素在 draw 里上传到纹理，同一帧只上传一次。draw 必须在调用方已经设为当前的 GL 上下文里执行，本对象不创建窗口。 */
class GlVideoSink : public VideoSink {
public:
    GlVideoSink();
    ~GlVideoSink() override;

    void present(std::int64_t pts_us) override;
    void present_sample(const VideoPresent& sample) override;
    int presented() const override;
    std::int64_t worst_sync_us() const override;
    int copy_av_sync_us(std::int64_t* dst, int capacity) const override;
    int draw(unsigned fbo, int width, int height);

private:
    struct State;
    std::unique_ptr<State> state_;
};

/* 本地文件在显示 Sink 接入之前使用。present 只计数，不画。 */
class CountingVideoSink : public VideoSink {
public:
    void present(std::int64_t /*pts_us*/) override { ++presented_; }
    int presented() const override { return presented_; }

private:
    int presented_ = 0;
};

/* 确定性回归的视频 Sink。呈现 PTS 序列哈希和音画偏差只在这里计算。 */
class RecordingVideoSink : public VideoSink {
public:
    RecordingVideoSink();
    ~RecordingVideoSink() override;

    void present(std::int64_t pts_us) override;
    void present_sample(const VideoPresent& sample) override;
    void record(std::int64_t pts_us, std::uint64_t content, std::int64_t origin_us, std::int64_t audio_clock_us);
    int presented() const override { return presented_; }
    std::int64_t worst_sync_us() const override { return worst_sync_us_; }
    int copy_av_sync_us(std::int64_t* dst, int capacity) const override;
    bool copy_hash(char* out, std::size_t out_len) const override;

private:
    struct State;
    int presented_ = 0;
    std::int64_t worst_sync_us_ = 0;
    bool started_ = false;
    static constexpr int kMaxSyncSamples = 8192;
    std::int64_t sync_us_[kMaxSyncSamples]{};
    int sync_count_ = 0;
    std::unique_ptr<State> state_;
};

/* Headless sinks. present() only counts; it does not allocate. */
void null_audio_present(std::int64_t pts_us);
void null_video_present(std::int64_t pts_us);
std::uint64_t null_audio_frames();
std::uint64_t null_video_frames();

}  /* namespace legato */

#endif
