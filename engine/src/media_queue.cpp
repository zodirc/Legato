#include "legato/engine/media_queue.hpp"

#include "legato/core/alloc.hpp"
#include "legato/engine/test_hooks.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace legato {
namespace {

void release_packet(OwnedPacket* packet) {
    if (packet == nullptr) {
        return;
    }
    if (packet->release != nullptr && packet->handle != nullptr) {
        packet->release(packet->handle);
    }
    packet->handle = nullptr;
    packet->data = nullptr;
    packet->release = nullptr;
}

}  /* namespace */

PacketQueue::PacketQueue(std::int64_t max_span_us, std::int64_t max_bytes)
    : max_span_us_(max_span_us > 0 ? max_span_us : 1000000),
      max_bytes_(max_bytes > 0 ? max_bytes : 1024 * 1024) {}

PacketQueue::~PacketQueue() {
    std::lock_guard<std::mutex> lock(mu_);
    clear_locked();
}

void PacketQueue::clear_locked() {
    for (int i = 0; i < count_; ++i) {
        OwnedPacket* packet = order_[(head_ + i) % kSlotCount];
        release_packet(packet);
        pool_.release(packet);
    }
    head_ = 0;
    count_ = 0;
    bytes_ = 0;
    span_us_ = 0;
}

bool PacketQueue::over_limit_locked() const {
    if (count_ == 0) {
        return false;
    }
    return span_us_ >= max_span_us_ || bytes_ >= max_bytes_;
}

const OwnedPacket& PacketQueue::front_locked() const { return *order_[head_]; }

const OwnedPacket& PacketQueue::back_locked() const {
    return *order_[(head_ + count_ - 1) % kSlotCount];
}

int PacketQueue::put(OwnedPacket packet) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this, &packet] {
        return abort_ || packet.serial != serial_ || (!over_limit_locked() && count_ < kSlotCount);
    });
    if (abort_ || packet.serial != serial_) {
        return -1;
    }
    OwnedPacket* slot = pool_.acquire();
    if (slot == nullptr) {
        return -1;
    }
    bytes_ += packet.size > 0 ? packet.size : 0;
    if (count_ == 0) {
        span_us_ = packet.duration_us > 0 ? packet.duration_us : 0;
    } else {
        const std::int64_t newest = std::max(back_locked().pts_us, packet.pts_us);
        const std::int64_t oldest = std::min(front_locked().pts_us, packet.pts_us);
        std::int64_t span = newest - oldest;
        if (span < 0) {
            span = 0;
        }
        if (span == 0 && packet.duration_us > span_us_) {
            span = packet.duration_us;
        }
        span_us_ = span;
    }
    *slot = packet;
    order_[(head_ + count_) % kSlotCount] = slot;
    ++count_;
    cv_.notify_all();
    return 0;
}

int PacketQueue::get(OwnedPacket* out, std::uint32_t serial) {
    if (out == nullptr) {
        return -1;
    }
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this, serial] { return abort_ || serial_ != serial || eof_ || count_ > 0; });
    if (abort_ || serial_ != serial) {
        return -1;
    }
    if (count_ == 0) {
        return 1;
    }
    OwnedPacket* slot = order_[head_];
    *out = *slot;
    *slot = OwnedPacket{};
    pool_.release(slot);
    head_ = (head_ + 1) % kSlotCount;
    --count_;
    bytes_ -= out->size > 0 ? out->size : 0;
    if (bytes_ < 0) {
        bytes_ = 0;
    }
    if (count_ == 0) {
        span_us_ = 0;
    } else if (count_ == 1) {
        span_us_ = front_locked().duration_us > 0 ? front_locked().duration_us : 0;
    } else {
        std::int64_t span = back_locked().pts_us - front_locked().pts_us;
        if (span < 0) {
            span = 0;
        }
        span_us_ = span;
    }
    cv_.notify_all();
    return 0;
}

void PacketQueue::close_write() {
    std::lock_guard<std::mutex> lock(mu_);
    eof_ = true;
    cv_.notify_all();
}

void PacketQueue::set_serial(std::uint32_t serial) {
    std::lock_guard<std::mutex> lock(mu_);
    serial_ = serial;
    eof_ = false;
    clear_locked();
    cv_.notify_all();
}

void PacketQueue::abort() {
    std::lock_guard<std::mutex> lock(mu_);
    abort_ = true;
    cv_.notify_all();
}

bool PacketQueue::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return count_ == 0;
}

std::int64_t PacketQueue::span_us() const {
    std::lock_guard<std::mutex> lock(mu_);
    return span_us_;
}

FrameQueue::FrameQueue() = default;

FrameQueue::~FrameQueue() {
    std::lock_guard<std::mutex> lock(mu_);
    clear_locked();
}

void FrameQueue::clear_locked() {
    for (int i = 0; i < count_; ++i) {
        VideoFrame* slot = order_[(head_ + i) % kCapacity];
        release_frame(slot);
        pool_.release(slot);
    }
    head_ = 0;
    count_ = 0;
}

int FrameQueue::push(VideoFrame frame) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this, &frame] { return abort_ || frame.serial != serial_ || count_ < kCapacity; });
    if (abort_ || frame.serial != serial_) {
        return -1;
    }
    VideoFrame* slot = pool_.acquire();
    if (slot == nullptr) {
        return -1;
    }
    *slot = frame;
    order_[(head_ + count_) % kCapacity] = slot;
    ++count_;
    cv_.notify_all();
    return 0;
}

int FrameQueue::pop(VideoFrame* out) {
    if (out == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (count_ == 0) {
        return -1;
    }
    VideoFrame* slot = order_[head_];
    *out = *slot;
    *slot = VideoFrame{};
    pool_.release(slot);
    head_ = (head_ + 1) % kCapacity;
    --count_;
    cv_.notify_all();
    return 0;
}

void FrameQueue::set_serial(std::uint32_t serial) {
    std::lock_guard<std::mutex> lock(mu_);
    serial_ = serial;
    clear_locked();
    cv_.notify_all();
}

void FrameQueue::abort() {
    std::lock_guard<std::mutex> lock(mu_);
    abort_ = true;
    clear_locked();
    cv_.notify_all();
}

bool FrameQueue::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return count_ == 0;
}

int FrameQueue::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return count_;
}

PcmQueue::PcmQueue() { samples_.assign(static_cast<std::size_t>(kMaxFrames) * kMaxChannels, 0); }

int PcmQueue::push(const std::int16_t* interleaved, int frames, int channels, std::uint32_t serial) {
    if (interleaved == nullptr || frames <= 0 || channels <= 0 || channels > kMaxChannels) {
        return 0;
    }
    if (in_steady_state() && test_pcm_steady_alloc_once()) {
        void* block = std::malloc(16);
        std::free(block);
    }
    int offset = 0;
    while (offset < frames) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this, serial] { return abort_ || serial != serial_ || buffered_frames_ < kMaxFrames; });
        if (abort_ || serial != serial_) {
            return -1;
        }
        if (channels_ == 0) {
            channels_ = channels;
        }
        if (channels != channels_) {
            return -1;
        }
        const int room = kMaxFrames - buffered_frames_;
        const int take = std::min(frames - offset, room);
        const std::size_t width = static_cast<std::size_t>(channels_);
        int left = take;
        int src_off = offset;
        int dst = (read_ + buffered_frames_) % kMaxFrames;
        while (left > 0) {
            const int run = std::min(left, kMaxFrames - dst);
            std::copy(interleaved + static_cast<std::size_t>(src_off) * width,
                      interleaved + static_cast<std::size_t>(src_off + run) * width,
                      samples_.data() + static_cast<std::size_t>(dst) * width);
            left -= run;
            src_off += run;
            dst = (dst + run) % kMaxFrames;
        }
        buffered_frames_ += take;
        offset += take;
        cv_.notify_all();
    }
    return 0;
}

int PcmQueue::pop(std::int16_t* dst, int max_frames, int channels, std::uint32_t serial) {
    if (max_frames <= 0 || channels <= 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (serial != serial_) {
        read_ = 0;
        buffered_frames_ = 0;
        return 0;
    }
    if (buffered_frames_ == 0 || channels_ == 0 || channels != channels_) {
        return 0;
    }
    const int take = std::min(max_frames, buffered_frames_);
    const std::size_t width = static_cast<std::size_t>(channels_);
    int left = take;
    int src = read_;
    int dst_off = 0;
    while (left > 0) {
        const int run = std::min(left, kMaxFrames - src);
        if (dst != nullptr) {
            std::copy(samples_.data() + static_cast<std::size_t>(src) * width,
                      samples_.data() + static_cast<std::size_t>(src + run) * width,
                      dst + static_cast<std::size_t>(dst_off) * width);
        }
        left -= run;
        src = (src + run) % kMaxFrames;
        dst_off += run;
    }
    read_ = src;
    buffered_frames_ -= take;
    cv_.notify_all();
    return take;
}

void PcmQueue::set_serial(std::uint32_t serial) {
    std::lock_guard<std::mutex> lock(mu_);
    serial_ = serial;
    read_ = 0;
    buffered_frames_ = 0;
    channels_ = 0;
    cv_.notify_all();
}

void PcmQueue::abort() {
    std::lock_guard<std::mutex> lock(mu_);
    abort_ = true;
    cv_.notify_all();
}

bool PcmQueue::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return buffered_frames_ == 0;
}

int PcmQueue::buffered_frames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return buffered_frames_;
}

}  /* namespace legato */
