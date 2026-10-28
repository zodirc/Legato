#include "legato/engine/file_playback.hpp"

#include "legato/engine/media_queue.hpp"
#include "legato/engine/player.hpp"
#include "legato/engine/test_hooks.hpp"
#include "legato/engine/video_presenter.hpp"
#include "legato/pal/clock.hpp"
#include "legato/legato_idl.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#if defined(LEGATO_HAS_FFMPEG)
#include "legato/codec/ffmpeg_sw_decoder.hpp"
#include "legato/demux/ffmpeg_demuxer.hpp"
#include "legato/io/file_source.hpp"
#include "legato/io/data_source.hpp"
#endif

namespace legato {
namespace {

std::int64_t seconds_us(double seconds) {
    if (seconds <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(seconds * 1000000.0);
}

void release_packet(OwnedPacket* packet) {
    if (packet != nullptr && packet->release != nullptr && packet->handle != nullptr) {
        packet->release(packet->handle);
        packet->handle = nullptr;
    }
}

}  /* namespace */

struct FilePlayback::Impl {
#if defined(LEGATO_HAS_FFMPEG)
    std::unique_ptr<DataSource> source;
    std::unique_ptr<FFmpegDemuxer> demux;
    std::unique_ptr<FFmpegSwDecoder> audio;
    std::unique_ptr<FFmpegSwDecoder> video;
    int audio_index = -1;
    int video_index = -1;
    int rate = 48000;
    int channels = 2;
    int drop_left = 0;
#endif
    std::unique_ptr<PacketQueue> audio_q;
    std::unique_ptr<PacketQueue> video_q;
    std::unique_ptr<FrameQueue> frames;
    std::unique_ptr<PcmQueue> pcm;
};

FilePlayback::FilePlayback(Player* player, Clock* clock, std::unique_ptr<AudioSink> audio, std::unique_ptr<VideoSink> video,
                           std::unique_ptr<AudioMasterClock> master, bool decode_video, double buffer_start_s,
                           double buffer_resume_s, double buffer_max_s, bool allow_audio_fallback)
    : player_(player),
      clock_(clock),
      audio_own_(std::move(audio)),
      video_own_(std::move(video)),
      master_own_(std::move(master)),
      audio_sink_(audio_own_.get()),
      video_sink_(video_own_.get()),
      master_(master_own_.get()),
      impl_(std::make_unique<Impl>()),
      presenter_(std::make_unique<VideoPresenter>()),
      decode_video_(decode_video),
      allow_audio_fallback_(allow_audio_fallback),
      buffer_start_s_(buffer_start_s > 0 ? buffer_start_s : 1.5),
      buffer_resume_s_(buffer_resume_s > 0 ? buffer_resume_s : buffer_start_s_),
      buffer_max_s_(buffer_max_s > 0 ? buffer_max_s : 50) {
    /* 预设只有秒数。字节上限按 buffer_max_s 乘以 1 MiB/s，时长和字节谁先到谁截断。 */
    const std::int64_t span = seconds_us(buffer_max_s_);
    const std::int64_t bytes = static_cast<std::int64_t>(buffer_max_s_ * 1024.0 * 1024.0);
    impl_->audio_q = std::make_unique<PacketQueue>(span, bytes > 0 ? bytes : 1024 * 1024);
    impl_->video_q = std::make_unique<PacketQueue>(span, bytes > 0 ? bytes : 1024 * 1024);
    impl_->frames = std::make_unique<FrameQueue>();
    impl_->pcm = std::make_unique<PcmQueue>();
}

FilePlayback::~FilePlayback() { stop(); }

int FilePlayback::open(const char* path) {
#if !defined(LEGATO_HAS_FFMPEG)
    (void)path;
    error_name_ = "source_open_failed";
    error_message_ = "打不开这个文件";
    return -1;
#else
    auto source = std::make_unique<FileSource>(path);
    return open_source(std::move(source));
#endif
}

int FilePlayback::open_source(std::unique_ptr<DataSource> source) {
#if !defined(LEGATO_HAS_FFMPEG)
    (void)source;
    error_name_ = "source_open_failed";
    error_message_ = "打不开这个文件";
    return -1;
#else
    impl_->drop_left = test_drop_video_frames();
    const bool paced = clock_ != nullptr && clock_->externally_paced();
    const std::int64_t t0 = paced ? clock_->now_us() : monotonic_us();
    auto elapsed = [this, t0, paced]() {
        const std::int64_t now = paced ? clock_->now_us() : monotonic_us();
        return now - t0;
    };
    if (source == nullptr) {
        error_name_ = "source_open_failed";
        error_message_ = "打不开这个文件";
        return -1;
    }
    impl_->source = std::move(source);
    if (impl_->source->open_io() != 0) {
        error_name_ = "source_open_failed";
        error_message_ = "打不开这个文件";
        return -1;
    }
    session_seekable_ = impl_->source->session_seekable();
    if (impl_->source->has_net_timing()) {
        const DataSource::NetTiming timing = impl_->source->net_timing();
        stages_.dns_us = timing.dns_us;
        stages_.connect_us = timing.connect_us;
        stages_.tls_us = timing.tls_us;
        stages_.first_byte_us = timing.first_byte_us;
    }
    impl_->demux = std::make_unique<FFmpegDemuxer>(impl_->source.get());
    if (impl_->demux->open() != 0) {
        error_name_ = "demux_failed";
        error_message_ = "读包失败";
        return -1;
    }
    stages_.probe_us = elapsed();
    duration_us_ = impl_->demux->duration_us();
    if (const FFmpegDemuxer::Stream* audio = impl_->demux->audio()) {
        impl_->audio_index = audio->index;
        impl_->rate = audio->sample_rate > 0 ? audio->sample_rate : 48000;
        impl_->channels = audio->channels > 0 ? audio->channels : 2;
        if (audio_sink_ == nullptr) {
            error_name_ = "audio_device_unavailable";
            error_message_ = "声卡写入失败";
            return -1;
        }
        audio_sink_->bind_cancel(&stop_);
        /* 外部时钟始终打开注入的 Sink。本地文件在测试钩子要求软件节拍时不打开设备。 */
        if (paced || test_pcm_output() == 0) {
            if (audio_sink_->open(impl_->rate, impl_->channels) != 0) {
                if (paced) {
                    error_name_ = "source_open_failed";
                    error_message_ = "打开被取消";
                    return -1;
                }
                /* music 和 realtime 不回落。其余预设改绑单调时钟，输出线程按样本时长走到流结束。 */
                if (!allow_audio_fallback_) {
                    error_name_ = "audio_device_unavailable";
                    error_message_ = "声卡写入失败";
                    return -1;
                }
                audio_own_ = std::make_unique<FallbackAudioSink>();
                audio_sink_ = audio_own_.get();
                audio_sink_->bind_cancel(&stop_);
                if (audio_sink_->open(impl_->rate, impl_->channels) != 0) {
                    error_name_ = "audio_device_unavailable";
                    error_message_ = "声卡写入失败";
                    return -1;
                }
                master_ = nullptr;
                bind_clock(&monotonic_);
                audio_fallback_ = true;
            } else {
                if (audio_sink_->rate() > 0) {
                    impl_->rate = audio_sink_->rate();
                }
                if (audio_sink_->channels() > 0) {
                    impl_->channels = audio_sink_->channels();
                }
            }
            if (!paced) {
                realtime_audio_ = true;
            }
        }
        impl_->audio = std::make_unique<FFmpegSwDecoder>(audio->codecpar, audio->time_base_num, audio->time_base_den,
                                                         true, impl_->rate, impl_->channels);
        if (impl_->audio->open() != 0) {
            error_name_ = "decode_failed";
            error_message_ = "音频解码失败";
            return -1;
        }
    }
    if (decode_video_) {
        note_test_video_decoder();
        if (const FFmpegDemuxer::Stream* video = impl_->demux->video()) {
            auto video_dec = std::make_unique<FFmpegSwDecoder>(video->codecpar, video->time_base_num,
                                                               video->time_base_den, false, 0, 0);
            if (video_dec->open() == 0) {
                impl_->video = std::move(video_dec);
                impl_->video_index = video->index;
            } else if (!impl_->audio) {
                error_name_ = "decode_failed";
                error_message_ = "视频解码失败";
                return -1;
            }
        }
    } else if (impl_->demux->video() != nullptr) {
        /* 预设不要视频时，视频包不能进入解复用循环。交错文件仍要读到后面的音频，
           所以让 FFmpeg 丢弃视频轨，而不是把包读出来再释放。 */
        impl_->demux->discard_video();
    }
    stages_.decoder_open_us = elapsed();
    if (!impl_->audio && !impl_->video) {
        error_name_ = "demux_failed";
        error_message_ = "读包失败";
        return -1;
    }
    return 0;
#endif
}

void FilePlayback::wake() {
    wait_gen_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_all();
}

void FilePlayback::publish_audio_media_us(std::int64_t media_us) {
    audio_media_us_.store(media_us, std::memory_order_release);
    /* 取帧线程在等这个位置。不唤醒的话，它会停到下一次解复用。 */
    wake();
}

void FilePlayback::note_video_dropped() { video_dropped_.fetch_add(1, std::memory_order_release); }

int FilePlayback::render_gl(unsigned fbo, int width, int height) {
    auto* sink = dynamic_cast<GlVideoSink*>(video_sink_);
    if (sink == nullptr) {
        return -1;
    }
    return sink->draw(fbo, width, height);
}

std::int64_t FilePlayback::buffered_span_us() const {
#if !defined(LEGATO_HAS_FFMPEG)
    return 0;
#else
    std::int64_t span = -1;
    if (impl_->audio) {
        span = impl_->audio_q->span_us();
    }
    if (impl_->video) {
        const std::int64_t video = impl_->video_q->span_us();
        span = span < 0 ? video : std::min(span, video);
    }
    return span < 0 ? 0 : span;
#endif
}

bool FilePlayback::queues_bottom() const {
#if !defined(LEGATO_HAS_FFMPEG)
    return false;
#else
    if (demux_eof_.load()) {
        return false;
    }
    if (impl_->audio && (!impl_->audio_q->empty() || !impl_->pcm->empty())) {
        return false;
    }
    if (impl_->video && (!impl_->video_q->empty() || !impl_->frames->empty() ||
                         (presenter_ != nullptr && presenter_->held_count() > 0))) {
        return false;
    }
    return impl_->audio || impl_->video;
#endif
}

void FilePlayback::note_bottom() {
    if (!reached_start_.load() || demux_eof_.load() || stall_latched_.load()) {
        empty_since_us_.store(0);
        return;
    }
    if (!queues_bottom()) {
        empty_since_us_.store(0);
        return;
    }
    const std::int64_t now = monotonic_us();
    const std::int64_t since = empty_since_us_.load();
    if (since == 0) {
        empty_since_us_.store(now);
        return;
    }
    if (now - since < 200000) {
        return;
    }
    bool expected = false;
    if (stall_latched_.compare_exchange_strong(expected, true)) {
        OutputNote note{};
        note.kind = OutputNote::Stall;
        emit_output(note);
    }
}

void FilePlayback::note_resumed(bool via_ring) {
    if (!stall_latched_.load()) {
        return;
    }
    if (!demux_eof_.load() && buffered_span_us() < seconds_us(buffer_resume_s_)) {
        return;
    }
    stall_latched_.store(false);
    empty_since_us_.store(0);
    /* 解复用线程直接回调。输出线程只入环，避免和 Player::mu_ 交叉。 */
    if (!via_ring) {
        player_->note_buffer_resumed();
        return;
    }
    OutputNote note{};
    note.kind = OutputNote::Resume;
    emit_output(note);
}

void FilePlayback::launch_threads() {
#if !defined(LEGATO_HAS_FFMPEG)
    return;
#else
    if (threads_started_) {
        return;
    }
    stop_.store(false);
    threads_started_ = true;
    demux_thread_ = std::thread([this] { demux_main(); });
    if (impl_->audio) {
        audio_thread_ = std::thread([this] { audio_main(); });
    }
    if (impl_->video) {
        video_thread_ = std::thread([this] { video_main(); });
    }
    const bool paced = clock_ != nullptr && clock_->externally_paced();
    const bool software = paced && test_pcm_output() != 0;
    if (software || realtime_audio_) {
        audio_out_thread_ = std::thread([this, software] { audio_out_main(software); });
    }
    /* 没人取视频帧时，帧队列满会把解复用堵住，音频包也读不出来。 */
    if (impl_->video && (!paced || software)) {
        video_drain_thread_ = std::thread([this] {
            if (presenter_ != nullptr) {
                presenter_->drain_main(this);
            }
        });
    }
#endif
}

void FilePlayback::start() {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        if (at_end_) {
            at_end_ = false;
            seek_req_.store(true);
            seek_to_ = 0;
            seek_serial_ = serial_.load();
        }
        playing_.store(true);
    }
    launch_threads();
    wake();
}

void FilePlayback::pause() {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        playing_.store(false);
    }
    wake();
#if defined(LEGATO_HAS_FFMPEG)
    if (audio_sink_ != nullptr) {
        audio_sink_->drop();
    }
#endif
}

void FilePlayback::seek(std::int64_t position_us, std::uint32_t serial) {
    seek_to(position_us, serial, LEGATO_SEEK_MODE_PRECISE);
}

void FilePlayback::seek_to(std::int64_t position_us, std::uint32_t serial, int mode) {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        seek_to_ = position_us;
        seek_serial_ = serial;
        seek_mode_ = mode;
        seek_req_.store(true);
        at_end_ = false;
    }
    /* 目标先于 demux 可见。连续 Seek 只改这里，解复用线程看到更新的 serial 就放弃上一次。 */
    drop_before_us_.store(mode == LEGATO_SEEK_MODE_PRECISE ? position_us : -1, std::memory_order_release);
    seek_first_pts_.store(-1, std::memory_order_release);
    /* 先发布目标位置，再发布 serial。输出线程见到新 serial 时 origin 已经可见。 */
    origin_us_.store(position_us, std::memory_order_relaxed);
    audio_media_us_.store(position_us, std::memory_order_release);
    serial_.store(serial, std::memory_order_release);
    demux_eof_.store(false);
    audio_done_serial_.store(0);
    video_done_serial_.store(0);
    reached_start_.store(false);
    stall_latched_.store(false);
    empty_since_us_.store(0);
    decoded_video_us_.store(-1);
#if defined(LEGATO_HAS_FFMPEG)
    impl_->audio_q->set_serial(serial);
    impl_->video_q->set_serial(serial);
    impl_->frames->set_serial(serial);
    impl_->pcm->set_serial(serial);
    if (audio_sink_ != nullptr) {
        audio_sink_->drop();
    }
#endif
    wake();
}

bool FilePlayback::apply_seek() {
#if !defined(LEGATO_HAS_FFMPEG)
    return false;
#else
    for (;;) {
        std::int64_t target = 0;
        std::uint32_t serial = 1;
        int mode = LEGATO_SEEK_MODE_PRECISE;
        {
            std::lock_guard<std::mutex> lock(wait_mu_);
            if (!seek_req_.load()) {
                return false;
            }
            seek_req_.store(false);
            target = seek_to_;
            serial = seek_serial_;
            mode = seek_mode_;
        }
        if (impl_->demux->seek_us(target) != 0) {
            player_->media_failed("seek_failed", "定位失败");
            playing_.store(false);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(wait_mu_);
            /* av_seek_frame 期间控制线程可能已经改了目标。这一次不 flush，接着定位到最新位置。 */
            if (seek_req_.load() && seek_serial_ != serial) {
                continue;
            }
        }
        impl_->demux->flush(serial);
        origin_us_.store(target, std::memory_order_relaxed);
        drop_before_us_.store(mode == LEGATO_SEEK_MODE_PRECISE ? target : -1, std::memory_order_release);
        seek_first_pts_.store(-1, std::memory_order_release);
        demux_eof_.store(false);
        if (audio_sink_ != nullptr) {
            audio_sink_->end_interrupt();
        }
        player_->set_media_position(target);
        seek_commits_.fetch_add(1, std::memory_order_release);
        wake();
        return true;
    }
#endif
}

bool FilePlayback::note_video_present(std::int64_t pts_us, std::uint32_t frame_serial) {
    if (frame_serial != serial_.load(std::memory_order_acquire)) {
        seek_stale_.fetch_add(1, std::memory_order_release);
        return false;
    }
    std::int64_t empty = -1;
    seek_first_pts_.compare_exchange_strong(empty, pts_us, std::memory_order_release, std::memory_order_relaxed);
    return true;
}

void FilePlayback::stop() {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        stop_.store(true);
        playing_.store(false);
    }
#if defined(LEGATO_HAS_FFMPEG)
    if (impl_) {
        impl_->audio_q->abort();
        impl_->video_q->abort();
        impl_->frames->abort();
        impl_->pcm->abort();
        if (audio_sink_ != nullptr) {
            audio_sink_->drop();
        }
    }
#endif
    wake();
    if (demux_thread_.joinable()) {
        demux_thread_.join();
    }
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
    if (video_thread_.joinable()) {
        video_thread_.join();
    }
    if (audio_out_thread_.joinable()) {
        audio_out_thread_.join();
    }
    if (video_drain_thread_.joinable()) {
        video_drain_thread_.join();
    }
    threads_started_ = false;
}

void FilePlayback::demux_main() {
#if !defined(LEGATO_HAS_FFMPEG)
    return;
#else
    const std::int64_t start_us = seconds_us(buffer_start_s_);
    while (!stop_.load()) {
        note_thread_wake();
        if (seek_req_.load()) {
            apply_seek();
            continue;
        }
        if (test_hold_demux() != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (!playing_.load() || demux_eof_.load()) {
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait(lock, [this] { return stop_.load() || seek_req_.load() || (playing_.load() && !demux_eof_.load()); });
            continue;
        }
        PacketView view;
        void* owned = nullptr;
        const int rc = impl_->demux->take_packet(&view, &owned);
        if (rc == 1) {
            demux_eof_.store(true);
            impl_->audio_q->close_write();
            impl_->video_q->close_write();
            reached_start_.store(true);
            wake();
            continue;
        }
        if (rc < 0) {
            player_->media_failed("demux_failed", "读包失败");
            playing_.store(false);
            continue;
        }
        if (stages_.first_packet_us == 0) {
            const bool paced_now = clock_ != nullptr && clock_->externally_paced();
            stages_.first_packet_us = paced_now ? clock_->now_us() : 0;
        }
        OwnedPacket packet;
        packet.handle = owned;
        packet.data = view.data;
        packet.size = view.size;
        packet.pts_us = view.pts_us;
        packet.duration_us = view.duration_us;
        packet.serial = view.serial;
        packet.release = release_owned_packet;
        if (const FFmpegDemuxer::Stream* video = impl_->demux->video()) {
            if (view.stream_index == video->index) {
                note_test_video_packet();
            }
        }
        bool queued = false;
        if (view.stream_index == impl_->audio_index && impl_->audio) {
            queued = impl_->audio_q->put(packet) == 0;
        } else if (decode_video_ && view.stream_index == impl_->video_index && impl_->video) {
            queued = impl_->video_q->put(packet) == 0;
        }
        if (!queued) {
            release_owned_packet(owned);
        } else if (!reached_start_.load() && buffered_span_us() >= start_us) {
            reached_start_.store(true);
        }
        note_resumed(false);
        wake();
    }
#endif
}

void FilePlayback::audio_main() {
#if !defined(LEGATO_HAS_FFMPEG)
    return;
#else
    std::uint32_t flushed = 0;
    while (!stop_.load()) {
        note_thread_wake();
        if (!playing_.load() && !seek_req_.load()) {
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait(lock, [this] { return stop_.load() || playing_.load() || seek_req_.load(); });
            continue;
        }
        const std::uint32_t serial = serial_.load();
        if (flushed != serial && impl_->audio) {
            impl_->audio->flush(serial);
            flushed = serial;
        }
        OwnedPacket packet;
        const int rc = impl_->audio_q->get(&packet, serial);
        if (rc < 0) {
            continue;
        }
        if (rc == 1) {
            if (serial_.load() == serial && impl_->audio) {
                FrameView frame;
                for (;;) {
                    const int delayed = impl_->audio->pull_delayed(&frame);
                    if (delayed == 1) {
                        break;
                    }
                    if (delayed < 0) {
                        player_->media_failed("decode_failed", "音频解码失败");
                        break;
                    }
                    const std::int64_t drop_before = drop_before_us_.load(std::memory_order_acquire);
                    if (frame.pcm != nullptr && frame.nb_samples > 0 &&
                        (drop_before < 0 || frame.pts_us >= drop_before)) {
                        impl_->pcm->push(frame.pcm, frame.nb_samples, impl_->channels, serial);
                    }
                }
                audio_done_serial_.store(serial);
                wake();
            }
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait(lock, [this, serial] { return stop_.load() || seek_req_.load() || serial_.load() != serial; });
            continue;
        }
        const std::int64_t delay = take_test_decode_delay_us();
        if (delay > 0) {
            note_test_decode_delay_started();
            std::this_thread::sleep_for(std::chrono::microseconds(delay));
            note_test_decode_delay_applied();
        }
        if (serial_.load() != serial) {
            release_packet(&packet);
            continue;
        }
        if (impl_->audio->decode(packet.data, packet.size, packet.pts_us) < 0) {
            release_packet(&packet);
            player_->media_failed("decode_failed", "音频解码失败");
            playing_.store(false);
            continue;
        }
        FrameView frame;
        for (;;) {
            const int got = impl_->audio->receive(&frame);
            if (got == 1) {
                break;
            }
            if (got < 0) {
                player_->media_failed("decode_failed", "音频解码失败");
                playing_.store(false);
                break;
            }
            /* 有视频解码线程时也只由本线程写 decoded_us，避免两路同时写。 */
            if (stages_.decoded_us == 0 && clock_ != nullptr && clock_->externally_paced()) {
                stages_.decoded_us = clock_->now_us();
            }
            const std::int64_t drop_before = drop_before_us_.load(std::memory_order_acquire);
            if (frame.pcm != nullptr && frame.nb_samples > 0 && serial_.load() == serial &&
                (drop_before < 0 || frame.pts_us >= drop_before)) {
                impl_->pcm->push(frame.pcm, frame.nb_samples, impl_->channels, serial);
            }
        }
        release_packet(&packet);
        wake();
    }
#endif
}

void FilePlayback::video_main() {
#if !defined(LEGATO_HAS_FFMPEG)
    return;
#else
    std::uint32_t flushed = 0;
    while (!stop_.load()) {
        note_thread_wake();
        if (!playing_.load() && !seek_req_.load()) {
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait(lock, [this] { return stop_.load() || playing_.load() || seek_req_.load(); });
            continue;
        }
        const std::uint32_t serial = serial_.load();
        if (flushed != serial && impl_->video) {
            impl_->video->flush(serial);
            flushed = serial;
        }
        OwnedPacket packet;
        const int rc = impl_->video_q->get(&packet, serial);
        if (rc < 0) {
            continue;
        }
        if (rc == 1) {
            if (serial_.load() == serial) {
                video_done_serial_.store(serial);
                wake();
            }
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait(lock, [this, serial] { return stop_.load() || seek_req_.load() || serial_.load() != serial; });
            continue;
        }
        if (serial_.load() != serial) {
            release_packet(&packet);
            continue;
        }
        if (impl_->video->decode(packet.data, packet.size, packet.pts_us) < 0) {
            release_packet(&packet);
            player_->media_failed("decode_failed", "视频解码失败");
            playing_.store(false);
            continue;
        }
        FrameView frame;
        for (;;) {
            const int got = impl_->video->receive(&frame);
            if (got == 1) {
                break;
            }
            if (got < 0) {
                player_->media_failed("decode_failed", "视频解码失败");
                playing_.store(false);
                break;
            }
            if (!has_audio() && stages_.decoded_us == 0 && clock_ != nullptr && clock_->externally_paced()) {
                stages_.decoded_us = clock_->now_us();
            }
            /* 像素留到下一次 receive 就失效，所以队列里只放 PTS、serial 和指纹。 */
            VideoFrame sample;
            sample.handle = frame.retain;
            sample.data0 = frame.data0;
            sample.linesize0 = frame.linesize0;
            sample.width = frame.width;
            sample.height = frame.height;
            sample.pts_us = frame.pts_us;
            sample.serial = serial;
            sample.release = frame.release_retain;
            frame.retain = nullptr;
            frame.release_retain = nullptr;
            const std::int64_t drop_before = drop_before_us_.load(std::memory_order_acquire);
            if (serial_.load() != serial || impl_->drop_left > 0 || (drop_before >= 0 && frame.pts_us < drop_before)) {
                if (impl_->drop_left > 0 && serial_.load() == serial) {
                    --impl_->drop_left;
                }
                release_frame(&sample);
                continue;
            }
            if (impl_->frames->push(sample) != 0) {
                release_frame(&sample);
            } else {
                sample.release = nullptr;
                sample.handle = nullptr;
            }
            decoded_video_us_.store(frame.pts_us);
        }
        release_packet(&packet);
        wake();
    }
#endif
}

void FilePlayback::audio_out_main(bool software) {
#if !defined(LEGATO_HAS_FFMPEG)
    (void)software;
    return;
#else
    bool armed = false;
    int empty_spins = 0;
    std::vector<std::int16_t> buf(static_cast<std::size_t>(std::max(impl_->channels, 1)) * 1024);
    while (!stop_.load()) {
        note_thread_wake();
        /* 暂停时阻塞在条件变量上，不设超时。超时会在静默窗口里把线程唤醒。 */
        if (!playing_.load() || seek_req_.load()) {
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait(lock, [this] { return stop_.load() || (playing_.load() && !seek_req_.load()); });
            continue;
        }
        if (!armed) {
            if (!impl_->pcm->empty() || audio_done_serial_.load() == serial_.load()) {
                armed = true;
            } else {
                std::unique_lock<std::mutex> lock(wait_mu_);
                cv_.wait(lock, [this] {
                    return stop_.load() || !playing_.load() || !impl_->pcm->empty() ||
                           audio_done_serial_.load() == serial_.load();
                });
                continue;
            }
        }
        if (stall_latched_.load()) {
            note_resumed(true);
            if (stall_latched_.load()) {
                std::unique_lock<std::mutex> lock(wait_mu_);
                cv_.wait(lock, [this] { return stop_.load() || !playing_.load() || !stall_latched_.load(); });
                continue;
            }
        }
        /* 写入帧数和首帧标记只属于本线程。控制线程发布 serial 之后，在这里清零。 */
        const std::uint32_t serial = serial_.load(std::memory_order_acquire);
        if (serial != audio_epoch_) {
            audio_epoch_ = serial;
            written_frames_ = 0;
            saw_audio_ = false;
            if (master_ != nullptr) {
                master_->reset(origin_us_.load(std::memory_order_relaxed));
            }
        }
        const int got = impl_->pcm->pop(buf.data(), 256, impl_->channels, serial);
        if (got > 0) {
            empty_spins = 0;
            if (software) {
                note_pcm_pop();
                const std::int64_t us = impl_->rate > 0 ? static_cast<std::int64_t>(got) * 1000000 / impl_->rate : 0;
                if (us > 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(us));
                }
                continue;
            }
            const int wrote = audio_sink_->write_frames(buf.data(), got);
            if (wrote == 0) {
                continue;
            }
            if (wrote < 0) {
                playing_.store(false);
                OutputNote note{};
                note.kind = OutputNote::Failed;
                std::snprintf(note.name, sizeof(note.name), "audio_device_unavailable");
                std::snprintf(note.message, sizeof(note.message), "声卡写入失败");
                emit_output(note);
                continue;
            }
            written_frames_ += got;
            if (master_ != nullptr) {
                const DelaySnapshot snap = audio_sink_->delay_snapshot();
                master_->observe_snapshot(written_frames_, snap.frames, impl_->rate, snap.tstamp_us, monotonic_us(),
                                          snap.have_tstamp);
                player_->set_media_position(master_->now_us());
                publish_audio_media_us(master_->now_us());
            } else if (impl_->rate > 0) {
                const std::int64_t pos =
                    origin_us_.load(std::memory_order_relaxed) + written_frames_ * 1000000 / impl_->rate;
                player_->set_media_position(pos);
                publish_audio_media_us(pos);
            }
            if (!saw_audio_) {
                saw_audio_ = true;
                OutputNote note{};
                note.kind = OutputNote::FirstFrame;
                note.position_us = origin_us_.load(std::memory_order_relaxed);
                note.extra_us = monotonic_us() - player_->file_load_mono();
                emit_output(note);
            }
            continue;
        }
        if (audio_done_serial_.load() == serial && impl_->pcm->empty()) {
            if (software) {
                std::unique_lock<std::mutex> lock(wait_mu_);
                cv_.wait(lock, [this] { return stop_.load() || !playing_.load() || seek_req_.load(); });
                continue;
            }
            {
                std::unique_lock<std::mutex> lock(wait_mu_);
                if (at_end_) {
                    cv_.wait(lock, [this] { return stop_.load() || seek_req_.load() || !at_end_; });
                    continue;
                }
            }
            audio_sink_->drain();
            if (stop_.load() || !playing_.load() || seek_req_.load()) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(wait_mu_);
                at_end_ = true;
                playing_.store(false);
            }
            OutputNote note{};
            note.kind = OutputNote::Ended;
            note.position_us = duration_us_ > 0 ? duration_us_ : origin_us_.load();
            player_->set_media_position(note.position_us);
            emit_output(note);
            continue;
        }
        note_bottom();
        if (software && armed && audio_done_serial_.load() != serial && !stall_latched_.load()) {
            ++empty_spins;
            if (empty_spins == 40) {
                note_pcm_xrun();
            }
        }
        const std::uint64_t seen = wait_gen_.load(std::memory_order_relaxed);
        std::unique_lock<std::mutex> lock(wait_mu_);
        cv_.wait(lock, [this, seen, serial] {
            return stop_.load() || !playing_.load() || seek_req_.load() || !impl_->pcm->empty() ||
                   audio_done_serial_.load() == serial || wait_gen_.load(std::memory_order_relaxed) != seen;
        });
    }
#endif
}

void FilePlayback::wait_decoded(std::int64_t media_us) {
    (void)media_us;
#if !defined(LEGATO_HAS_FFMPEG)
    return;
#else
    if (clock_ == nullptr || !clock_->externally_paced()) {
        return;
    }
    std::unique_lock<std::mutex> lock(wait_mu_);
    cv_.wait(lock, [this] {
        return stop_.load() || !playing_.load() || reached_start_.load() || demux_eof_.load();
    });
#endif
}

void FilePlayback::present_until(std::int64_t elapsed_us, PresentUpdate* out) {
    if (out == nullptr) {
        return;
    }
    *out = PresentUpdate();
    if (clock_ == nullptr || !clock_->externally_paced() || presenter_ == nullptr) {
        return;
    }
    presenter_->present_until(this, elapsed_us, out);
}

void FilePlayback::emit_output(const OutputNote& note) {
    if (!notes_.push(note) || player_ == nullptr) {
        return;
    }
    player_->schedule_note_drain();
}

bool FilePlayback::pop_note(OutputNote* out) { return out != nullptr && notes_.pop(out); }

bool FilePlayback::notes_pending() const { return notes_.size() != 0; }

void FilePlayback::bind_clock(Clock* clock) { clock_ = clock; }

int FilePlayback::audio_clock_source() const { return master_ != nullptr ? master_->source() : 0; }

bool FilePlayback::has_audio() const {
#if !defined(LEGATO_HAS_FFMPEG)
    return false;
#else
    return impl_ != nullptr && impl_->audio != nullptr;
#endif
}

bool FilePlayback::has_video() const {
#if !defined(LEGATO_HAS_FFMPEG)
    return false;
#else
    return impl_ != nullptr && impl_->video != nullptr;
#endif
}

FrameQueue* FilePlayback::frames() {
#if !defined(LEGATO_HAS_FFMPEG)
    return nullptr;
#else
    return impl_ != nullptr ? impl_->frames.get() : nullptr;
#endif
}

int FilePlayback::copy_av_sync_us(std::int64_t* dst, int capacity) const {
    if (video_sink_ == nullptr) {
        return 0;
    }
    return video_sink_->copy_av_sync_us(dst, capacity);
}

PcmQueue* FilePlayback::pcm() {
#if !defined(LEGATO_HAS_FFMPEG)
    return nullptr;
#else
    return impl_ != nullptr ? impl_->pcm.get() : nullptr;
#endif
}

int FilePlayback::sample_rate() const {
#if !defined(LEGATO_HAS_FFMPEG)
    return 0;
#else
    return impl_ != nullptr ? impl_->rate : 0;
#endif
}

int FilePlayback::sample_channels() const {
#if !defined(LEGATO_HAS_FFMPEG)
    return 0;
#else
    return impl_ != nullptr ? impl_->channels : 0;
#endif
}

}  /* namespace legato */
