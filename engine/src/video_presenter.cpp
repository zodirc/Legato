#include "legato/engine/video_presenter.hpp"

#include "legato/core/actor.hpp"
#include "legato/engine/player.hpp"
#include "legato/engine/test_hooks.hpp"
#include "legato/pal/clock.hpp"

#include <chrono>
#include <cstdint>

namespace legato {
namespace {

std::uint64_t hash_plane(const VideoFrame& frame) {
    std::uint64_t hash = 14695981039346656037ull;
    if (frame.data0 == nullptr || frame.height <= 0 || frame.linesize0 <= 0) {
        return hash;
    }
    const int width = frame.width > 0 && frame.width < frame.linesize0 ? frame.width : frame.linesize0;
    for (int y = 0; y < frame.height; y += 2) {
        const std::uint8_t* row = frame.data0 + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.linesize0);
        for (int x = 0; x < width; x += 16) {
            hash ^= row[x];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

}  /* namespace */

VideoPresenter::~VideoPresenter() { clear_held(); }

void VideoPresenter::clear_held() {
    for (int i = 0; i < held_count_; ++i) {
        release_frame(&held_[i]);
    }
    held_count_ = 0;
    release_frame(&shown_);
}

void VideoPresenter::keep_shown(VideoFrame* frame) {
    if (frame == nullptr) {
        return;
    }
    /* 旧帧还活着。先核对像素，再换成这一帧。下一次 receive 不能改掉 shown_。 */
    note_retained_frame(shown_.data0, frame->data0);
    release_frame(&shown_);
    shown_ = *frame;
    frame->release = nullptr;
    frame->handle = nullptr;
}

void VideoPresenter::note_serial(FilePlayback* playback, std::uint32_t serial) {
    if (serial == present_epoch_) {
        return;
    }
    /* Seek 只改 serial。本对象上的呈现进度由调用 present_until 的线程清掉。 */
    present_epoch_ = serial;
    clear_held();
    present_elapsed_us_ = 0;
    saw_present_ = false;
    present_ended_ = false;
    sample_written_ = 0;
    sample_clock_.reset(0);
    if (playback->audio_sink_ != nullptr) {
        playback->audio_sink_->reset();
    }
}

void VideoPresenter::present_until(FilePlayback* playback, std::int64_t elapsed_us, FilePlayback::PresentUpdate* out) {
    if (playback == nullptr || out == nullptr || playback->frames() == nullptr || playback->pcm() == nullptr) {
        return;
    }
    constexpr std::int64_t kVsyncUs = 16667;
    const std::uint32_t serial = playback->serial_.load();
    note_serial(playback, serial);
    int kept = 0;
    for (int i = 0; i < held_count_; ++i) {
        if (held_[i].serial == serial) {
            if (kept != i) {
                held_[kept] = held_[i];
                held_[i].release = nullptr;
                held_[i].handle = nullptr;
            }
            ++kept;
        } else {
            release_frame(&held_[i]);
        }
    }
    held_count_ = kept;
    /* 同一拍里更早的帧可以丢掉。晚于 limit 的帧都还要，后面的拍子会用到。 */
    auto pull_frames = [this, playback, serial](std::int64_t limit) {
        auto compact = [this, playback, limit]() {
            int best = -1;
            for (int i = 0; i < held_count_; ++i) {
                if (held_[i].pts_us <= limit && (best < 0 || held_[i].pts_us >= held_[best].pts_us)) {
                    best = i;
                }
            }
            VideoFrame kept[kHeldMax]{};
            int count = 0;
            auto take = [&](int index) {
                kept[count++] = held_[index];
                held_[index].release = nullptr;
                held_[index].handle = nullptr;
            };
            if (best >= 0) {
                take(best);
            }
            for (int i = 0; i < held_count_; ++i) {
                if (held_[i].pts_us > limit) {
                    take(i);
                }
            }
            for (int i = 0; i < held_count_; ++i) {
                if (held_[i].handle != nullptr) {
                    playback->note_video_dropped();
                }
                release_frame(&held_[i]);
            }
            for (int i = 0; i < count; ++i) {
                held_[i] = kept[i];
            }
            for (int i = count; i < held_count_; ++i) {
                held_[i] = VideoFrame{};
            }
            held_count_ = count;
        };
        compact();
        VideoFrame frame;
        while (held_count_ < kHeldMax && playback->frames()->pop(&frame) == 0) {
            if (frame.serial != serial) {
                release_frame(&frame);
                continue;
            }
            held_[held_count_] = frame;
            frame.release = nullptr;
            frame.handle = nullptr;
            ++held_count_;
            compact();
        }
    };
    auto video_ready = [this, playback, serial](std::int64_t limit) {
        if (!playback->has_video()) {
            return true;
        }
        const bool done = playback->video_done_serial_.load() == serial;
        bool any = false;
        bool newer = false;
        for (int i = 0; i < held_count_; ++i) {
            if (held_[i].pts_us <= limit) {
                any = true;
            } else {
                newer = true;
            }
        }
        /* 已经看到更晚的帧，或解码结束，这一拍的帧就定了。 */
        if (any && (newer || done || playback->decoded_video_us_.load() > limit)) {
            return true;
        }
        /* 手里的帧都晚于这一拍。时钟照走，这一拍不画。 */
        if (!any && held_count_ > 0) {
            return true;
        }
        return !any && done && playback->frames()->empty();
    };
    auto audio_ready = [playback, serial](int need) {
        if (!playback->has_audio()) {
            return true;
        }
        if (playback->audio_done_serial_.load() == serial) {
            return true;
        }
        return playback->pcm()->buffered_frames() >= need;
    };
    if (elapsed_us < present_elapsed_us_) {
        present_elapsed_us_ = 0;
    }
    const int rate = playback->sample_rate();
    const int channels = playback->sample_channels();
    while (present_elapsed_us_ + kVsyncUs <= elapsed_us) {
        const int want = rate > 0 ? static_cast<int>(rate * kVsyncUs / 1000000) : 0;
        const std::int64_t limit = playback->origin_us_.load() + present_elapsed_us_ + kVsyncUs + kVsyncUs / 2;
        bool ready = false;
        /* 最多等 200ms。中途被唤醒就再看一眼，到点仍没有就让这一拍照走。 */
        const std::int64_t deadline = monotonic_us() + 200000;
        for (;;) {
            pull_frames(limit);
            playback->note_resumed(true);
            if (audio_ready(want) && video_ready(limit)) {
                ready = true;
                break;
            }
            if (playback->stop_.load() || !playback->playing_.load()) {
                break;
            }
            playback->note_bottom();
            if (playback->stall_latched_.load() && playback->queues_bottom()) {
                break;
            }
            const std::int64_t now = monotonic_us();
            if (now >= deadline) {
                break;
            }
            const std::uint64_t seen = playback->wait_gen_.load(std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(playback->wait_mu_);
            playback->cv_.wait_for(lock, std::chrono::microseconds(deadline - now), [playback, seen] {
                return playback->stop_.load() || !playback->playing_.load() ||
                       playback->wait_gen_.load(std::memory_order_relaxed) != seen;
            });
        }
        pull_frames(limit);
        if (playback->stop_.load() || !playback->playing_.load()) {
            break;
        }
        if (!ready && playback->stall_latched_.load() && playback->queues_bottom()) {
            break;
        }
        present_elapsed_us_ += kVsyncUs;
        int left = want;
        while (left > 0) {
            const int got = playback->pcm()->pop(nullptr, left, channels, serial);
            if (got <= 0) {
                break;
            }
            if (playback->audio_sink_ != nullptr) {
                playback->audio_sink_->write_frames(nullptr, got);
            }
            sample_written_ += got;
            left -= got;
        }
        if (rate > 0) {
            const int delay = playback->audio_sink_ != nullptr ? playback->audio_sink_->delay_frames() : 0;
            sample_clock_.observe_delay(sample_written_, delay, rate);
        }
        const std::int64_t audio_clock = rate > 0 ? sample_clock_.now_us() : present_elapsed_us_;
        int best = -1;
        for (int i = 0; i < held_count_; ++i) {
            if (held_[i].pts_us <= limit) {
                best = static_cast<int>(i);
            } else {
                break;
            }
        }
        if (!playback->has_video() && !saw_present_) {
            saw_present_ = true;
            out->first = true;
            out->first_frame_us = present_elapsed_us_;
        }
        if (best >= 0) {
            VideoFrame chosen = held_[best];
            held_[best].release = nullptr;
            held_[best].handle = nullptr;
            for (int index = 0; index <= best; ++index) {
                release_frame(&held_[index]);
            }
            const int remain = held_count_ - (best + 1);
            for (int index = 0; index < remain; ++index) {
                held_[index] = held_[best + 1 + index];
                held_[best + 1 + index].release = nullptr;
                held_[best + 1 + index].handle = nullptr;
            }
            held_count_ = remain;
            const std::uint64_t content = hash_plane(chosen);
            keep_shown(&chosen);
            if (playback->video_sink_ != nullptr &&
                playback->note_video_present(shown_.pts_us, chosen.serial)) {
                VideoPresent sample;
                sample.pts_us = shown_.pts_us;
                sample.content = content;
                sample.origin_us = playback->origin_us_.load();
                sample.audio_clock_us = audio_clock + test_av_sync_offset_us();
                sample.data0 = shown_.data0;
                sample.linesize0 = shown_.linesize0;
                sample.width = shown_.width;
                sample.height = shown_.height;
                sample.retain = shown_.handle;
                playback->video_sink_->present_sample(sample);
            }
            if (!saw_present_) {
                saw_present_ = true;
                out->first = true;
                out->first_frame_us = present_elapsed_us_;
            }
        }
        out->position_us = playback->origin_us_.load() + audio_clock;
    }
    if (playback->video_sink_ != nullptr) {
        out->presented = playback->video_sink_->presented();
        out->av_sync_us = playback->video_sink_->worst_sync_us();
        playback->video_sink_->copy_hash(out->sha256, sizeof(out->sha256));
    }
    if (playback->demux_eof_.load() && playback->duration_us_ > 0 &&
        playback->origin_us_.load() + elapsed_us >= playback->duration_us_ && !present_ended_) {
        present_ended_ = true;
        out->ended = true;
        out->position_us = playback->duration_us_;
    }
}

void VideoPresenter::emit_paced(FilePlayback* playback, VideoFrame* frame) {
    if (playback->video_sink_ != nullptr && playback->note_video_present(frame->pts_us, frame->serial)) {
        VideoPresent sample;
        sample.pts_us = frame->pts_us;
        sample.content = hash_plane(*frame);
        sample.origin_us = playback->origin_us_.load();
        std::int64_t relative = playback->audio_media_us() - sample.origin_us;
        if (relative < 0) {
            relative = 0;
        }
        sample.audio_clock_us = relative + test_av_sync_offset_us();
        sample.data0 = frame->data0;
        sample.linesize0 = frame->linesize0;
        sample.width = frame->width;
        sample.height = frame->height;
        sample.retain = frame->handle;
        keep_shown(frame);
        playback->video_sink_->present_sample(sample);
    } else {
        release_frame(frame);
    }
}

void VideoPresenter::pace_audio(FilePlayback* playback) {
    constexpr std::int64_t kHalfVsyncUs = 8333;
    constexpr int kHold = 8;
    VideoFrame held[kHold]{};
    int count = 0;
    while (!playback->stop_.load()) {
        note_thread_wake();
        const std::uint32_t serial = playback->serial_.load(std::memory_order_acquire);
        if (serial != video_epoch_) {
            video_epoch_ = serial;
            for (int i = 0; i < count; ++i) {
                release_frame(&held[i]);
            }
            count = 0;
        }
        if (playback->seek_req_.load() || !playback->playing_.load()) {
            for (int i = 0; i < count; ++i) {
                release_frame(&held[i]);
            }
            count = 0;
            std::unique_lock<std::mutex> lock(playback->wait_mu_);
            playback->cv_.wait(lock, [playback] {
                return playback->stop_.load() || (playback->playing_.load() && !playback->seek_req_.load());
            });
            continue;
        }
        while (count < kHold) {
            VideoFrame frame;
            if (playback->frames()->pop(&frame) != 0) {
                break;
            }
            if (frame.serial != serial) {
                release_frame(&frame);
                continue;
            }
            held[count++] = frame;
        }
        const std::int64_t clock = playback->audio_media_us();
        const std::int64_t limit = clock + kHalfVsyncUs;
        /* 晚过一帧的不再补画。补画会把音画偏差拉过 40 毫秒。 */
        const std::int64_t earliest = clock - 33333;
        int best = -1;
        for (int i = 0; i < count; ++i) {
            if (held[i].handle == nullptr) {
                continue;
            }
            if (held[i].pts_us < earliest) {
                playback->note_video_dropped();
                release_frame(&held[i]);
                continue;
            }
            if (held[i].pts_us <= limit && (best < 0 || held[i].pts_us >= held[best].pts_us)) {
                best = i;
            }
        }
        if (best < 0) {
            VideoFrame keep[kHold]{};
            int kept = 0;
            for (int i = 0; i < count; ++i) {
                if (held[i].handle == nullptr) {
                    continue;
                }
                keep[kept++] = held[i];
                held[i].release = nullptr;
                held[i].handle = nullptr;
            }
            for (int i = 0; i < kept; ++i) {
                held[i] = keep[i];
            }
            for (int i = kept; i < count; ++i) {
                held[i] = VideoFrame{};
            }
            count = kept;
            const std::uint64_t seen = playback->wait_gen_.load(std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(playback->wait_mu_);
            playback->cv_.wait_for(lock, std::chrono::milliseconds(20), [playback, seen] {
                return playback->stop_.load() || !playback->playing_.load() || playback->seek_req_.load() ||
                       playback->wait_gen_.load(std::memory_order_relaxed) != seen;
            });
            continue;
        }
        for (int i = 0; i < count; ++i) {
            if (i != best && held[i].handle != nullptr && held[i].pts_us <= limit) {
                playback->note_video_dropped();
                release_frame(&held[i]);
            }
        }
        emit_paced(playback, &held[best]);
        VideoFrame keep[kHold]{};
        int kept = 0;
        for (int i = 0; i < count; ++i) {
            if (i == best || held[i].handle == nullptr) {
                continue;
            }
            keep[kept++] = held[i];
            held[i].release = nullptr;
            held[i].handle = nullptr;
        }
        for (int i = 0; i < kept; ++i) {
            held[i] = keep[i];
        }
        for (int i = kept; i < count; ++i) {
            held[i] = VideoFrame{};
        }
        count = kept;
    }
    for (int i = 0; i < count; ++i) {
        release_frame(&held[i]);
    }
}

void VideoPresenter::drain_main(FilePlayback* playback) {
    if (playback == nullptr || playback->frames() == nullptr) {
        return;
    }
    if (playback->has_audio() && (playback->clock_ == nullptr || !playback->clock_->externally_paced())) {
        pace_audio(playback);
        return;
    }
    while (!playback->stop_.load()) {
        note_thread_wake();
        const std::uint32_t serial = playback->serial_.load(std::memory_order_acquire);
        if (serial != video_epoch_) {
            video_epoch_ = serial;
            saw_video_ = false;
            wall_origin_ = 0;
        }
        if (playback->seek_req_.load() || !playback->playing_.load()) {
            wall_origin_ = 0;
            std::unique_lock<std::mutex> lock(playback->wait_mu_);
            playback->cv_.wait(lock, [playback] {
                return playback->stop_.load() || (playback->playing_.load() && !playback->seek_req_.load());
            });
            continue;
        }
        /* 虚拟媒体时钟的帧只交给 present_until。这里再取就会把该画的帧丢掉。 */
        if (playback->clock_ != nullptr && playback->clock_->externally_paced()) {
            std::unique_lock<std::mutex> lock(playback->wait_mu_);
            playback->cv_.wait(lock, [playback] {
                return playback->stop_.load() || !playback->playing_.load() || playback->seek_req_.load();
            });
            continue;
        }
        VideoFrame frame;
        if (playback->frames()->pop(&frame) != 0) {
            const bool video_done = playback->video_done_serial_.load() == playback->serial_.load();
            const bool paced = playback->clock_ != nullptr && playback->clock_->externally_paced();
            if (!paced && !playback->has_audio() && playback->demux_eof_.load() && video_done &&
                playback->frames()->empty() && held_count_ == 0 && playback->playing_.load()) {
                {
                    std::lock_guard<std::mutex> lock(playback->wait_mu_);
                    playback->at_end_ = true;
                    playback->playing_.store(false);
                }
                OutputNote note{};
                note.kind = OutputNote::Ended;
                note.position_us = playback->duration_us_ > 0 ? playback->duration_us_ : playback->origin_us_.load();
                playback->player_->set_media_position(note.position_us);
                playback->emit_output(note);
            }
            const std::uint64_t seen = playback->wait_gen_.load(std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(playback->wait_mu_);
            playback->cv_.wait(lock, [playback, seen] {
                return playback->stop_.load() || !playback->playing_.load() || playback->seek_req_.load() ||
                       playback->wait_gen_.load(std::memory_order_relaxed) != seen;
            });
            continue;
        }
        if (playback->has_audio()) {
            if (playback->video_sink_ != nullptr && playback->note_video_present(frame.pts_us, frame.serial)) {
                VideoPresent sample;
                sample.pts_us = frame.pts_us;
                sample.content = hash_plane(frame);
                keep_shown(&frame);
                playback->video_sink_->present_sample(sample);
            } else {
                release_frame(&frame);
            }
            continue;
        }
        const std::int64_t now_clock = playback->clock_ != nullptr ? playback->clock_->now_us() : monotonic_us();
        if (wall_origin_ == 0) {
            wall_origin_ = now_clock;
            media_origin_ = frame.pts_us;
        }
        const std::int64_t due = wall_origin_ + (frame.pts_us - media_origin_);
        {
            std::unique_lock<std::mutex> lock(playback->wait_mu_);
            while (!playback->stop_.load() && playback->playing_.load() && !playback->seek_req_.load()) {
                const std::int64_t now = playback->clock_ != nullptr ? playback->clock_->now_us() : monotonic_us();
                if (now >= due) {
                    break;
                }
                const std::int64_t left = due - now;
                playback->cv_.wait_for(lock, std::chrono::microseconds(left), [playback] {
                    return playback->stop_.load() || !playback->playing_.load() || playback->seek_req_.load();
                });
            }
        }
        if (playback->stop_.load() || !playback->playing_.load() || playback->seek_req_.load()) {
            release_frame(&frame);
            continue;
        }
        if (playback->video_sink_ != nullptr && playback->note_video_present(frame.pts_us, frame.serial)) {
            VideoPresent sample;
            sample.pts_us = frame.pts_us;
            sample.content = hash_plane(frame);
            keep_shown(&frame);
            playback->video_sink_->present_sample(sample);
        } else {
            release_frame(&frame);
        }
        playback->player_->set_media_position(shown_.pts_us);
        if (!saw_video_) {
            saw_video_ = true;
            OutputNote note{};
            note.kind = OutputNote::FirstFrame;
            note.position_us = shown_.pts_us;
            note.extra_us = monotonic_us() - playback->player_->file_load_mono();
            playback->emit_output(note);
        }
    }
}

}  /* namespace legato */
