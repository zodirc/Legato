#ifndef LEGATO_RENDER_SINK_HPP
#define LEGATO_RENDER_SINK_HPP

#include <atomic>
#include <cstdint>
#include <memory>

namespace legato {

class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual int open(int sample_rate, int channels) = 0;
    virtual void present(std::int64_t pts_us) = 0;
    virtual void close() = 0;
};

class VideoSink {
public:
    virtual ~VideoSink() = default;
    virtual void present(std::int64_t pts_us) = 0;
};

/* 确定性回归的音频 Sink。按虚拟媒体时钟消费样本，不写 PCM 设备。 */
class NullAudioSink : public AudioSink {
public:
    int open(int sample_rate, int channels) override;
    void present(std::int64_t pts_us) override;
    void close() override;
    void bind_cancel(const std::atomic<bool>* cancel);
    void reset();
    std::int64_t consume(int samples);
    std::int64_t clock_us() const;

private:
    const std::atomic<bool>* cancel_ = nullptr;
    int rate_ = 0;
    std::int64_t consumed_ = 0;
};

/* 确定性回归的视频 Sink。呈现 PTS 序列哈希和音画偏差只在这里计算。 */
class RecordingVideoSink : public VideoSink {
public:
    RecordingVideoSink();
    ~RecordingVideoSink() override;

    void present(std::int64_t pts_us) override;
    void record(std::int64_t pts_us, std::uint64_t content, std::int64_t origin_us, std::int64_t audio_clock_us);
    int presented() const { return presented_; }
    std::int64_t worst_sync_us() const { return worst_sync_us_; }
    bool copy_hash(char* out, std::size_t out_len) const;

private:
    struct State;
    int presented_ = 0;
    std::int64_t worst_sync_us_ = 0;
    bool started_ = false;
    std::unique_ptr<State> state_;
};

/* Headless sinks. present() only counts; it does not allocate. */
void null_audio_present(std::int64_t pts_us);
void null_video_present(std::int64_t pts_us);
std::uint64_t null_audio_frames();
std::uint64_t null_video_frames();

}  /* namespace legato */

#endif
