#ifndef LEGATO_ENGINE_MEDIA_QUEUE_HPP
#define LEGATO_ENGINE_MEDIA_QUEUE_HPP

#include "legato/core/object_pool.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace legato {

struct OwnedPacket {
    void* handle = nullptr;
    const std::uint8_t* data = nullptr;
    int size = 0;
    std::int64_t pts_us = 0;
    std::int64_t duration_us = 0;
    std::uint32_t serial = 0;
    void (*release)(void*) = nullptr;
};

/* 每路一个包队列。满和空都会阻塞，serial 变化或 abort 把等待唤醒。
   不按包的个数封顶：音频包和视频包的大小差太多。 */
class PacketQueue {
public:
    PacketQueue(std::int64_t max_span_us, std::int64_t max_bytes);
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    /* 0 已入队。-1 被拒绝，调用方仍持有 packet。空队列至少收下当前这一个包。 */
    int put(OwnedPacket packet);
    /* 0 取出。1 写端已经结束且队列空。-1 abort 或 serial 已经变了。 */
    int get(OwnedPacket* out, std::uint32_t serial);
    void close_write();
    void set_serial(std::uint32_t serial);
    void abort();
    bool empty() const;
    std::int64_t span_us() const;

private:
    static constexpr int kSlotCount = 4096;

    void clear_locked();
    bool over_limit_locked() const;
    const OwnedPacket& front_locked() const;
    const OwnedPacket& back_locked() const;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    ObjectPool<OwnedPacket, kSlotCount> pool_;
    OwnedPacket* order_[kSlotCount]{};
    int head_ = 0;
    int count_ = 0;
    std::int64_t max_span_us_ = 0;
    std::int64_t max_bytes_ = 0;
    std::int64_t bytes_ = 0;
    std::int64_t span_us_ = 0;
    std::uint32_t serial_ = 1;
    bool eof_ = false;
    bool abort_ = false;
};

struct VideoFrame {
    void* handle = nullptr;
    const std::uint8_t* data0 = nullptr;
    int linesize0 = 0;
    int width = 0;
    int height = 0;
    std::int64_t pts_us = 0;
    std::uint32_t serial = 0;
    void (*release)(void*) = nullptr;
};

inline void release_frame(VideoFrame* frame) {
    if (frame == nullptr) {
        return;
    }
    if (frame->release != nullptr && frame->handle != nullptr) {
        frame->release(frame->handle);
    }
    *frame = VideoFrame{};
}

/* 视频帧队列，容量 8。外壳来自对象池，像素是 av_frame_ref。
   serial 变化时先释放帧再唤醒：像素不属于解码器的下一帧。 */
class FrameQueue {
public:
    static constexpr int kCapacity = 8;

    FrameQueue();
    ~FrameQueue();

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    /* 0 已入队。-1 被拒绝，调用方仍持有 frame。 */
    int push(VideoFrame frame);
    /* 0 取出，调用方持有像素。-1 当前为空。 */
    int pop(VideoFrame* out);
    void set_serial(std::uint32_t serial);
    void abort();
    bool empty() const;
    int size() const;

private:
    void clear_locked();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    ObjectPool<VideoFrame, kCapacity> pool_;
    VideoFrame* order_[kCapacity]{};
    int head_ = 0;
    int count_ = 0;
    std::uint32_t serial_ = 1;
    bool abort_ = false;
};

/* PCM 块。音频输出线程只从这里取样本。serial 变化时丢掉旧块并唤醒。 */
class PcmQueue {
public:
    PcmQueue();

    int push(const std::int16_t* interleaved, int frames, int channels, std::uint32_t serial);
    int pop(std::int16_t* dst, int max_frames, int channels, std::uint32_t serial);
    void set_serial(std::uint32_t serial);
    void abort();
    bool empty() const;
    int buffered_frames() const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    /* 构造时按最大声道数预分配。push 只拷贝，不再为每块 PCM 分配。 */
    std::vector<std::int16_t> samples_;
    int read_ = 0;
    int buffered_frames_ = 0;
    int channels_ = 0;
    std::uint32_t serial_ = 1;
    bool abort_ = false;
    /* 只留约 200ms。30ms 的解码延迟靠这段垫住；再大的话包队列见底会被 PCM 盖住。 */
    static constexpr int kMaxFrames = 48000 / 5;
    static constexpr int kMaxChannels = 8;
};

}  /* namespace legato */

#endif
