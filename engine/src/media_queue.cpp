#include "legato/engine/media_queue.hpp"

#include <algorithm>
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
    for (OwnedPacket& packet : packets_) {
        release_packet(&packet);
    }
    packets_.clear();
    bytes_ = 0;
    span_us_ = 0;
}

bool PacketQueue::over_limit_locked() const {
    if (packets_.empty()) {
        return false;
    }
    return span_us_ >= max_span_us_ || bytes_ >= max_bytes_;
}

int PacketQueue::put(OwnedPacket packet) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this, &packet] {
        return abort_ || packet.serial != serial_ || !over_limit_locked();
    });
    if (abort_ || packet.serial != serial_) {
        return -1;
    }
    bytes_ += packet.size > 0 ? packet.size : 0;
    if (packets_.empty()) {
        span_us_ = packet.duration_us > 0 ? packet.duration_us : 0;
    } else {
        const std::int64_t newest = std::max(packets_.back().pts_us, packet.pts_us);
        const std::int64_t oldest = std::min(packets_.front().pts_us, packet.pts_us);
        std::int64_t span = newest - oldest;
        if (span < 0) {
            span = 0;
        }
        if (span == 0 && packet.duration_us > span_us_) {
            span = packet.duration_us;
        }
        span_us_ = span;
    }
    packets_.push_back(packet);
    cv_.notify_all();
    return 0;
}

int PacketQueue::get(OwnedPacket* out, std::uint32_t serial) {
    if (out == nullptr) {
        return -1;
    }
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this, serial] { return abort_ || serial_ != serial || eof_ || !packets_.empty(); });
    if (abort_ || serial_ != serial) {
        return -1;
    }
    if (packets_.empty()) {
        return 1;
    }
    *out = packets_.front();
    packets_.pop_front();
    bytes_ -= out->size > 0 ? out->size : 0;
    if (bytes_ < 0) {
        bytes_ = 0;
    }
    if (packets_.empty()) {
        span_us_ = 0;
    } else if (packets_.size() == 1) {
        span_us_ = packets_.front().duration_us > 0 ? packets_.front().duration_us : 0;
    } else {
        std::int64_t span = packets_.back().pts_us - packets_.front().pts_us;
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
    return packets_.empty();
}

std::int64_t PacketQueue::span_us() const {
    std::lock_guard<std::mutex> lock(mu_);
    return span_us_;
}

int FrameQueue::push(VideoFrame frame) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this, &frame] { return abort_ || frame.serial != serial_ || count_ < kCapacity; });
    if (abort_ || frame.serial != serial_) {
        return -1;
    }
    const int tail = (head_ + count_) % kCapacity;
    frames_[tail] = frame;
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
    *out = frames_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
    cv_.notify_all();
    return 0;
}

void FrameQueue::set_serial(std::uint32_t serial) {
    std::lock_guard<std::mutex> lock(mu_);
    serial_ = serial;
    head_ = 0;
    count_ = 0;
    cv_.notify_all();
}

void FrameQueue::abort() {
    std::lock_guard<std::mutex> lock(mu_);
    abort_ = true;
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

int PcmQueue::push(const std::int16_t* interleaved, int frames, int channels, std::uint32_t serial) {
    if (interleaved == nullptr || frames <= 0 || channels <= 0) {
        return 0;
    }
    int offset = 0;
    while (offset < frames) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this, serial] { return abort_ || serial != serial_ || buffered_frames_ < kMaxFrames; });
        if (abort_ || serial != serial_) {
            return -1;
        }
        const int room = kMaxFrames - buffered_frames_;
        const int take = std::min(frames - offset, room);
        Chunk chunk;
        chunk.serial = serial;
        chunk.channels = channels;
        chunk.frames = take;
        chunk.samples.assign(interleaved + static_cast<std::size_t>(offset) * static_cast<std::size_t>(channels),
                             interleaved + static_cast<std::size_t>(offset + take) * static_cast<std::size_t>(channels));
        buffered_frames_ += take;
        offset += take;
        chunks_.push_back(std::move(chunk));
        cv_.notify_all();
    }
    return 0;
}

int PcmQueue::pop(std::int16_t* dst, int max_frames, int channels, std::uint32_t serial) {
    if (max_frames <= 0 || channels <= 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mu_);
    while (!chunks_.empty() && chunks_.front().serial != serial) {
        buffered_frames_ -= chunks_.front().frames;
        chunks_.pop_front();
    }
    if (buffered_frames_ < 0) {
        buffered_frames_ = 0;
    }
    if (chunks_.empty()) {
        return 0;
    }
    Chunk& chunk = chunks_.front();
    const int have = chunk.frames - chunk.used;
    const int take = std::min(max_frames, have);
    if (dst != nullptr && chunk.channels == channels) {
        const std::int16_t* src = chunk.samples.data() + static_cast<std::size_t>(chunk.used) * static_cast<std::size_t>(channels);
        std::copy(src, src + static_cast<std::size_t>(take) * static_cast<std::size_t>(channels), dst);
    }
    chunk.used += take;
    buffered_frames_ -= take;
    if (chunk.used >= chunk.frames) {
        chunks_.pop_front();
    }
    cv_.notify_all();
    return take;
}

void PcmQueue::set_serial(std::uint32_t serial) {
    std::lock_guard<std::mutex> lock(mu_);
    serial_ = serial;
    chunks_.clear();
    buffered_frames_ = 0;
    cv_.notify_all();
}

void PcmQueue::abort() {
    std::lock_guard<std::mutex> lock(mu_);
    abort_ = true;
    cv_.notify_all();
}

bool PcmQueue::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return chunks_.empty();
}

int PcmQueue::buffered_frames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return buffered_frames_;
}

}  /* namespace legato */
