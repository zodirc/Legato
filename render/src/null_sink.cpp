#include "legato/render/sink.hpp"

#include "legato/engine/test_hooks.hpp"
#include "sha256.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace legato {
namespace {

std::atomic<std::uint64_t> g_audio_frames{0};
std::atomic<std::uint64_t> g_video_frames{0};

}  /* namespace */

void null_audio_present(std::int64_t /*pts_us*/) {
    g_audio_frames.fetch_add(1, std::memory_order_relaxed);
}

void null_video_present(std::int64_t /*pts_us*/) {
    g_video_frames.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t null_audio_frames() { return g_audio_frames.load(std::memory_order_relaxed); }
std::uint64_t null_video_frames() { return g_video_frames.load(std::memory_order_relaxed); }

void NullAudioSink::bind_cancel(const std::atomic<bool>* cancel) { cancel_ = cancel; }

/* 验收把这次打开拉长。阻塞留在 Sink 上，调用线程上的 load 和 release 仍然立即返回。
   取消后返回失败，避免回收已经开始后还继续建解码器。 */
int NullAudioSink::open(int sample_rate, int channels) {
    rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;
    consumed_ = 0;
    std::int64_t left = test_open_delay_us();
    while (left > 0) {
        if (cancel_ != nullptr && cancel_->load()) {
            return -1;
        }
        const std::int64_t slice = left > 20000 ? 20000 : left;
        std::this_thread::sleep_for(std::chrono::microseconds(slice));
        left -= slice;
    }
    if (cancel_ != nullptr && cancel_->load()) {
        return -1;
    }
    return 0;
}

void NullAudioSink::present(std::int64_t /*pts_us*/) {}

void NullAudioSink::close() {}

int NullAudioSink::write_frames(const std::int16_t* /*interleaved*/, int frames) {
    if (frames > 0) {
        consumed_ += frames;
    }
    return frames > 0 ? frames : 0;
}

void NullAudioSink::drop() {}

void NullAudioSink::drain() {}

void NullAudioSink::end_interrupt() {}

int NullAudioSink::delay_frames() const { return 0; }

int FallbackAudioSink::open(int sample_rate, int channels) {
    rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;
    interrupted_.store(false);
    return 0;
}

void FallbackAudioSink::present(std::int64_t /*pts_us*/) {}

void FallbackAudioSink::close() {}

int FallbackAudioSink::write_frames(const std::int16_t* /*interleaved*/, int frames) {
    if (frames <= 0 || rate_ <= 0) {
        return -1;
    }
    if (interrupted_.load()) {
        return 0;
    }
    std::int64_t left = static_cast<std::int64_t>(frames) * 1000000 / rate_;
    while (left > 0) {
        if (interrupted_.load()) {
            return 0;
        }
        const std::int64_t slice = left > 10000 ? 10000 : left;
        std::this_thread::sleep_for(std::chrono::microseconds(slice));
        left -= slice;
    }
    return interrupted_.load() ? 0 : frames;
}

void FallbackAudioSink::drop() { interrupted_.store(true); }

void FallbackAudioSink::drain() {}

void FallbackAudioSink::end_interrupt() { interrupted_.store(false); }

int FallbackAudioSink::delay_frames() const { return 0; }

void NullAudioSink::reset() { consumed_ = 0; }

std::int64_t NullAudioSink::consume(int samples) {
    if (samples > 0) {
        consumed_ += samples;
    }
    if (rate_ <= 0) {
        return 0;
    }
    return clock_us();
}

std::int64_t NullAudioSink::clock_us() const {
    if (rate_ <= 0) {
        return 0;
    }
    return consumed_ * 1000000 / rate_;
}

struct RecordingVideoSink::State {
    bench::Sha256 hash;
};

RecordingVideoSink::RecordingVideoSink() : state_(std::make_unique<State>()) {}

RecordingVideoSink::~RecordingVideoSink() = default;

void RecordingVideoSink::present(std::int64_t pts_us) { record(pts_us, 0, 0, 0); }

void RecordingVideoSink::present_sample(const VideoPresent& sample) {
    record(sample.pts_us, sample.content, sample.origin_us, sample.audio_clock_us);
}

void RecordingVideoSink::record(std::int64_t pts_us, std::uint64_t content, std::int64_t origin_us,
                                std::int64_t audio_clock_us) {
    unsigned char bytes[16];
    const std::uint64_t pts = static_cast<std::uint64_t>(pts_us);
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<unsigned char>((pts >> (i * 8)) & 0xffu);
        bytes[8 + i] = static_cast<unsigned char>((content >> (i * 8)) & 0xffu);
    }
    state_->hash.update(bytes, sizeof(bytes));
    started_ = true;
    ++presented_;
    const std::int64_t sync = pts_us - (origin_us + audio_clock_us);
    const std::int64_t magnitude = sync < 0 ? -sync : sync;
    if (sync_count_ < kMaxSyncSamples) {
        sync_us_[sync_count_++] = magnitude;
    }
    if (magnitude > worst_sync_us_) {
        worst_sync_us_ = magnitude;
    }
}

int RecordingVideoSink::copy_av_sync_us(std::int64_t* dst, int capacity) const {
    const int count = sync_count_;
    if (dst == nullptr || capacity <= 0) {
        return count;
    }
    const int take = count < capacity ? count : capacity;
    for (int i = 0; i < take; ++i) {
        dst[i] = sync_us_[i];
    }
    return count;
}

bool RecordingVideoSink::copy_hash(char* out, std::size_t out_len) const {
    if (!started_ || out == nullptr || out_len == 0) {
        return false;
    }
    const std::string hex = bench::Sha256(state_->hash).hex();
    std::snprintf(out, out_len, "%s", hex.c_str());
    return true;
}

}  /* namespace legato */
