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

/* 音频主时钟。0 还没有采样，1 是硬件时间戳外推，2 是 delay。 */
enum AudioClockSource {
    kAudioClockNone = 0,
    kAudioClockHtimestamp = 1,
    kAudioClockDelay = 2
};

/* 位置在 observe_snapshot 或 observe_delay 里计算。
   硬件时间戳倒退，或和现在相差超过 500 毫秒时，退回 delay。部分驱动和 plughw 的时间戳不能外推。 */
class AudioMasterClock : public Clock {
public:
    void reset(std::int64_t origin_us);
    void observe_delay(std::int64_t written_frames, std::int64_t delay_frames, int sample_rate);
    void observe_snapshot(std::int64_t written_frames, std::int64_t delay_frames, int sample_rate,
                          std::int64_t tstamp_us, std::int64_t now_us, bool have_tstamp);
    std::int64_t now_us() const override { return position_us_; }
    int source() const { return source_.load(std::memory_order_acquire); }

private:
    std::int64_t origin_us_ = 0;
    std::int64_t position_us_ = 0;
    std::int64_t last_tstamp_us_ = -1;
    std::atomic<int> source_{kAudioClockNone};
};

}  /* namespace legato */

#endif
