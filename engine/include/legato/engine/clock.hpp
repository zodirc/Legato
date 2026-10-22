#ifndef LEGATO_ENGINE_CLOCK_HPP
#define LEGATO_ENGINE_CLOCK_HPP

#include <atomic>
#include <cstdint>

namespace legato {

/* 会话用来回答「现在应该播到哪」。三种实现对应三条运行形态。
   externally_paced 为真时，时钟只在测试推进时前进，管线不能自己睡觉等到它。 */
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::int64_t now_us() const = 0;
    virtual bool externally_paced() const { return false; }
};

class VirtualClock : public Clock {
public:
    std::int64_t now_us() const override { return now_.load(std::memory_order_acquire); }
    bool externally_paced() const override { return true; }
    void advance_us(std::int64_t delta_us);

private:
    /* 控制线程推进，解码线程读取。原子量避免这两边同时碰同一个普通整数。 */
    std::atomic<std::int64_t> now_{0};
};

class MonotonicClock : public Clock {
public:
    std::int64_t now_us() const override;
};

/* 音频主时钟。位置只在 observe_delay 里计算。
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
