#ifndef LEGATO_ENGINE_CLOCK_HPP
#define LEGATO_ENGINE_CLOCK_HPP

#include <cstdint>

namespace legato {

/* 会话用来回答「现在应该播到哪」。三种实现对应三条运行形态。 */
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::int64_t now_us() const = 0;
};

class VirtualClock : public Clock {
public:
    std::int64_t now_us() const override { return now_; }
    void advance_us(std::int64_t delta_us);

private:
    std::int64_t now_ = 0;
};

class MonotonicClock : public Clock {
public:
    std::int64_t now_us() const override;
};

/* 音频主时钟。当前取样来自已写入帧数和 snd_pcm_delay。
   硬件时间戳接口留在这里，阶段 B 再换成 snd_pcm_htimestamp。 */
class AudioMasterClock : public Clock {
public:
    void reset(std::int64_t origin_us);
    void observe_delay(std::int64_t written_frames, std::int64_t delay_frames, int sample_rate);
    std::int64_t now_us() const override { return position_us_; }

private:
    std::int64_t origin_us_ = 0;
    std::int64_t position_us_ = 0;
};

}  /* namespace legato */

#endif
