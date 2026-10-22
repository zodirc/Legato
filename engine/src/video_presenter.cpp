#include "legato/engine/video_presenter.hpp"

#include "legato/engine/player.hpp"
#include "legato/pal/clock.hpp"

#include <chrono>
#include <thread>

namespace legato {

void VideoPresenter::note_serial(FilePlayback* playback, std::uint32_t serial) {
    if (serial == present_epoch_) {
        return;
    }
    /* Seek 只改 serial。本对象上的呈现进度由调用 present_until 的线程清掉。 */
    present_epoch_ = serial;
    held_.clear();
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
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->serial != serial) {
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    auto pull_frames = [this, playback, serial]() {
        VideoFrame frame;
        while (playback->frames()->pop(&frame) == 0) {
            if (frame.serial == serial) {
                held_.push_back(frame);
            }
        }
    };
    auto video_ready = [this, playback, serial](std::int64_t limit) {
        if (!playback->has_video()) {
            return true;
        }
        const bool done = playback->video_done_serial_.load() == serial;
        bool any = false;
        bool newer = false;
        for (const VideoFrame& frame : held_) {
            if (frame.pts_us <= limit) {
                any = true;
            } else {
                newer = true;
            }
        }
        /* 队列只有 8 帧。先把已经解出的帧挪到本线程，解码才能继续，直到出现超过 limit 的帧。 */
        if (any && (newer || done || playback->decoded_video_us_.load() > limit)) {
            return true;
        }
        if (!any && done && playback->frames()->empty()) {
            return true;
        }
        return false;
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
        for (int spin = 0; spin < 40; ++spin) {
            pull_frames();
            playback->note_resumed(true);
            if (audio_ready(want) && video_ready(limit)) {
                ready = true;
                break;
            }
            playback->note_bottom();
            if (playback->stall_latched_.load() && playback->queues_bottom()) {
                break;
            }
            std::unique_lock<std::mutex> lock(playback->wait_mu_);
            playback->cv_.wait_for(lock, std::chrono::milliseconds(5), [playback] {
                return playback->stop_.load() || !playback->playing_.load();
            });
            if (playback->stop_.load() || !playback->playing_.load()) {
                break;
            }
        }
        pull_frames();
        if (playback->stop_.load() || !playback->playing_.load()) {
            break;
        }
        if (!ready && playback->stall_latched_.load() && playback->queues_bottom()) {
            break;
        }
        if (!ready) {
            playback->note_bottom();
            if (playback->stall_latched_.load() && playback->queues_bottom()) {
                break;
            }
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
        for (std::size_t i = 0; i < held_.size(); ++i) {
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
            const VideoFrame chosen = held_[static_cast<std::size_t>(best)];
            held_.erase(held_.begin(), held_.begin() + best + 1);
            if (playback->video_sink_ != nullptr) {
                VideoPresent sample;
                sample.pts_us = chosen.pts_us;
                sample.content = chosen.content;
                sample.origin_us = playback->origin_us_.load();
                sample.audio_clock_us = audio_clock;
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

void VideoPresenter::drain_main(FilePlayback* playback) {
    if (playback == nullptr || playback->frames() == nullptr) {
        return;
    }
    while (!playback->stop_.load()) {
        const std::uint32_t serial = playback->serial_.load(std::memory_order_acquire);
        if (serial != video_epoch_) {
            video_epoch_ = serial;
            saw_video_ = false;
            wall_origin_ = 0;
        }
        if (playback->seek_req_.load()) {
            wall_origin_ = 0;
        }
        VideoFrame frame;
        if (playback->frames()->pop(&frame) != 0) {
            const bool video_done = playback->video_done_serial_.load() == playback->serial_.load();
            const bool paced = playback->clock_ != nullptr && playback->clock_->externally_paced();
            if (!paced && !playback->has_audio() && playback->demux_eof_.load() && video_done &&
                playback->frames()->empty() && held_.empty() && !playback->seek_req_.load() &&
                playback->playing_.load()) {
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
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        /* 外部时钟上的呈现走 present_until。这里把帧取走，避免帧队列堵住解复用。 */
        if (playback->clock_ != nullptr && playback->clock_->externally_paced()) {
            continue;
        }
        if (playback->has_audio() || playback->seek_req_.load() || !playback->playing_.load()) {
            if (playback->video_sink_ != nullptr && frame.serial == serial) {
                VideoPresent sample;
                sample.pts_us = frame.pts_us;
                sample.content = frame.content;
                playback->video_sink_->present_sample(sample);
            }
            continue;
        }
        const std::int64_t now_clock = playback->clock_ != nullptr ? playback->clock_->now_us() : monotonic_us();
        if (wall_origin_ == 0) {
            wall_origin_ = now_clock;
            media_origin_ = frame.pts_us;
        }
        const std::int64_t due = wall_origin_ + (frame.pts_us - media_origin_);
        while (!playback->stop_.load() && playback->playing_.load() && !playback->seek_req_.load()) {
            const std::int64_t now = playback->clock_ != nullptr ? playback->clock_->now_us() : monotonic_us();
            if (now >= due) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (playback->stop_.load() || !playback->playing_.load() || playback->seek_req_.load()) {
            continue;
        }
        if (playback->video_sink_ != nullptr) {
            VideoPresent sample;
            sample.pts_us = frame.pts_us;
            sample.content = frame.content;
            playback->video_sink_->present_sample(sample);
        }
        playback->player_->set_media_position(frame.pts_us);
        if (!saw_video_) {
            saw_video_ = true;
            OutputNote note{};
            note.kind = OutputNote::FirstFrame;
            note.position_us = frame.pts_us;
            note.extra_us = monotonic_us() - playback->player_->file_load_mono();
            playback->emit_output(note);
        }
    }
}

}  /* namespace legato */
