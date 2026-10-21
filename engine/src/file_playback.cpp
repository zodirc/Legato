#include "legato/engine/file_playback.hpp"

#include "legato/engine/clock.hpp"
#include "legato/engine/media_queue.hpp"
#include "legato/engine/player.hpp"
#include "legato/engine/test_hooks.hpp"
#include "legato/pal/clock.hpp"
#include "legato/render/sink.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#if defined(LEGATO_HAS_FFMPEG)
#include "legato/codec/ffmpeg_sw_decoder.hpp"
#include "legato/demux/ffmpeg_demuxer.hpp"
#include "legato/io/file_source.hpp"
#endif
#if defined(LEGATO_HAS_ALSA)
#include "legato/render/alsa_sink.hpp"
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
    std::unique_ptr<FileSource> source;
    std::unique_ptr<FFmpegDemuxer> demux;
    std::unique_ptr<FFmpegSwDecoder> audio;
    std::unique_ptr<FFmpegSwDecoder> video;
#if defined(LEGATO_HAS_ALSA)
    std::unique_ptr<AlsaSink> alsa;
#else
    struct AudioOut {
        int open(int, int) { return -1; }
        int write_frames(const std::int16_t*, int) { return -1; }
        void drop() {}
        void end_interrupt() {}
        void drain() {}
        std::int64_t delay_frames() const { return 0; }
        int rate() const { return 0; }
        int channels() const { return 0; }
    };
    std::unique_ptr<AudioOut> alsa;
#endif
    int audio_index = -1;
    int video_index = -1;
    int rate = 48000;
    int channels = 2;
    std::int64_t written_frames = 0;
    bool saw_frame = false;
    NullAudioSink null_audio;
    RecordingVideoSink recording;
    int drop_left = 0;
    std::int64_t present_elapsed_us = 0;
    bool saw_present = false;
    bool present_ended = false;
    std::deque<VideoFrame> held;
#endif
    std::unique_ptr<PacketQueue> audio_q;
    std::unique_ptr<PacketQueue> video_q;
    std::unique_ptr<FrameQueue> frames;
    std::unique_ptr<PcmQueue> pcm;
};

FilePlayback::FilePlayback(Player* player, Clock* clock, bool virtual_clock, bool decode_video, double buffer_start_s,
                           double buffer_resume_s, double buffer_max_s)
    : player_(player),
      clock_(clock),
      impl_(std::make_unique<Impl>()),
      virtual_(virtual_clock),
      decode_video_(decode_video),
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
    impl_->drop_left = test_drop_video_frames();
    const std::int64_t t0 = clock_ != nullptr ? clock_->now_us() : monotonic_us();
    auto elapsed = [this, t0]() {
        const std::int64_t now = clock_ != nullptr ? clock_->now_us() : monotonic_us();
        return now - t0;
    };
    impl_->source = std::make_unique<FileSource>(path);
    if (impl_->source->open_file() != 0) {
        error_name_ = "source_open_failed";
        error_message_ = "打不开这个文件";
        return -1;
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
        if (virtual_) {
            impl_->null_audio.bind_cancel(&stop_);
            if (impl_->null_audio.open(impl_->rate, impl_->channels) != 0) {
                error_name_ = "source_open_failed";
                error_message_ = "打开被取消";
                return -1;
            }
        } else if (!test_pcm_output()) {
#if defined(LEGATO_HAS_ALSA)
            impl_->alsa = std::make_unique<AlsaSink>();
#else
            impl_->alsa = std::make_unique<Impl::AudioOut>();
#endif
            if (impl_->alsa->open(impl_->rate, impl_->channels) != 0) {
                error_name_ = "audio_device_unavailable";
                error_message_ = "声卡写入失败";
                return -1;
            }
            impl_->rate = impl_->alsa->rate();
            impl_->channels = impl_->alsa->channels();
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

void FilePlayback::wake() { cv_.notify_all(); }

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
    if (impl_->video && (!impl_->video_q->empty() || !impl_->frames->empty() || !impl_->held.empty())) {
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
        player_->note_stall();
    }
}

void FilePlayback::note_resumed() {
    if (!stall_latched_.load()) {
        return;
    }
    if (!demux_eof_.load() && buffered_span_us() < seconds_us(buffer_resume_s_)) {
        return;
    }
    stall_latched_.store(false);
    empty_since_us_.store(0);
    player_->note_buffer_resumed();
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
    const bool software = virtual_ && test_pcm_output() != 0;
    if (software || impl_->alsa) {
        audio_out_thread_ = std::thread([this, software] { audio_out_main(software); });
    }
    /* 没人取视频帧时，帧队列满会把解复用堵住，音频包也读不出来。 */
    if (impl_->video && (!virtual_ || software)) {
        video_drain_thread_ = std::thread([this] { video_drain_main(); });
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
    if (impl_->alsa) {
        impl_->alsa->drop();
    }
#endif
}

void FilePlayback::seek(std::int64_t position_us, std::uint32_t serial) {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        seek_to_ = position_us;
        seek_serial_ = serial;
        seek_req_.store(true);
        at_end_ = false;
    }
    serial_.store(serial);
    origin_us_.store(position_us);
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
    impl_->present_elapsed_us = 0;
    impl_->saw_present = false;
    impl_->present_ended = false;
    impl_->null_audio.reset();
    impl_->held.clear();
    impl_->written_frames = 0;
    impl_->saw_frame = false;
    if (impl_->alsa) {
        impl_->alsa->drop();
    }
#endif
    wake();
}

bool FilePlayback::apply_seek() {
#if !defined(LEGATO_HAS_FFMPEG)
    return false;
#else
    std::int64_t target = 0;
    std::uint32_t serial = 1;
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        if (!seek_req_.load()) {
            return false;
        }
        seek_req_.store(false);
        target = seek_to_;
        serial = seek_serial_;
    }
    if (impl_->demux->seek_us(target) != 0) {
        player_->media_failed("定位失败");
        playing_.store(false);
        return false;
    }
    impl_->demux->flush(serial);
    serial_.store(serial);
    impl_->audio_q->set_serial(serial);
    impl_->video_q->set_serial(serial);
    impl_->frames->set_serial(serial);
    impl_->pcm->set_serial(serial);
    origin_us_.store(target);
    demux_eof_.store(false);
    if (impl_->alsa) {
        impl_->alsa->end_interrupt();
    }
    player_->set_media_position(target);
    wake();
    return true;
#endif
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
        if (impl_->alsa) {
            impl_->alsa->drop();
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
            player_->media_failed("读包失败");
            playing_.store(false);
            continue;
        }
        if (stages_.first_packet_us == 0) {
            stages_.first_packet_us = clock_ != nullptr ? clock_->now_us() : 0;
        }
        OwnedPacket packet;
        packet.handle = owned;
        packet.data = view.data;
        packet.size = view.size;
        packet.pts_us = view.pts_us;
        packet.duration_us = view.duration_us;
        packet.serial = view.serial;
        packet.release = release_owned_packet;
        bool queued = false;
        if (view.stream_index == impl_->audio_index && impl_->audio) {
            queued = impl_->audio_q->put(packet) == 0;
        } else if (decode_video_ && view.stream_index == impl_->video_index && impl_->video) {
            queued = impl_->video_q->put(packet) == 0;
        }
        /* video 为假时不建视频解码器。交错文件仍要取出视频包，否则读不到后面的音频。这些包不入队。 */
        if (!queued) {
            release_owned_packet(owned);
        } else if (!reached_start_.load() && buffered_span_us() >= start_us) {
            reached_start_.store(true);
        }
        note_resumed();
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
                        player_->media_failed("音频解码失败");
                        break;
                    }
                    if (frame.pcm != nullptr && frame.nb_samples > 0) {
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
            player_->media_failed("音频解码失败");
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
                player_->media_failed("音频解码失败");
                playing_.store(false);
                break;
            }
            if (stages_.decoded_us == 0 && clock_ != nullptr) {
                stages_.decoded_us = clock_->now_us();
            }
            if (frame.pcm != nullptr && frame.nb_samples > 0 && serial_.load() == serial) {
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
    auto hash_plane = [](const FrameView& frame) {
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
    };
    std::uint32_t flushed = 0;
    while (!stop_.load()) {
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
            player_->media_failed("视频解码失败");
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
                player_->media_failed("视频解码失败");
                playing_.store(false);
                break;
            }
            if (stages_.decoded_us == 0 && clock_ != nullptr) {
                stages_.decoded_us = clock_->now_us();
            }
            /* 像素留到下一次 receive 就失效，所以队列里只放 PTS、serial 和指纹。 */
            if (serial_.load() != serial) {
                continue;
            }
            if (impl_->drop_left > 0) {
                --impl_->drop_left;
                continue;
            }
            VideoFrame sample;
            sample.pts_us = frame.pts_us;
            sample.content = hash_plane(frame);
            sample.serial = serial;
            impl_->frames->push(sample);
            decoded_video_us_.store(frame.pts_us);
        }
        release_packet(&packet);
        wake();
    }
#endif
}

void FilePlayback::video_drain_main() {
#if !defined(LEGATO_HAS_FFMPEG)
    return;
#else
    std::int64_t wall_origin = 0;
    std::int64_t media_origin = 0;
    while (!stop_.load()) {
        if (seek_req_.load()) {
            wall_origin = 0;
        }
        VideoFrame frame;
        if (impl_->frames->pop(&frame) != 0) {
            const bool video_done = video_done_serial_.load() == serial_.load();
            if (!virtual_ && impl_->audio == nullptr && demux_eof_.load() && video_done && impl_->frames->empty() &&
                !seek_req_.load() && playing_.load()) {
                {
                    std::lock_guard<std::mutex> lock(wait_mu_);
                    at_end_ = true;
                    playing_.store(false);
                }
                player_->set_media_position(duration_us_ > 0 ? duration_us_ : origin_us_.load());
                player_->media_ended();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        /* 有音频时只把帧从队列取走，避免帧队列满了堵住解复用。没有音频时按 PTS 对齐单调时钟。 */
        if (virtual_ || impl_->audio != nullptr || seek_req_.load() || !playing_.load()) {
            continue;
        }
        if (wall_origin == 0) {
            wall_origin = monotonic_us();
            media_origin = frame.pts_us;
        }
        const std::int64_t due = wall_origin + (frame.pts_us - media_origin);
        while (!stop_.load() && playing_.load() && !seek_req_.load() && monotonic_us() < due) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (stop_.load() || !playing_.load() || seek_req_.load()) {
            continue;
        }
        player_->set_media_position(frame.pts_us);
        if (!impl_->saw_frame) {
            impl_->saw_frame = true;
            player_->media_first_frame(frame.pts_us, monotonic_us() - player_->file_load_mono());
        }
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
        if (!playing_.load() || seek_req_.load()) {
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return stop_.load() || (playing_.load() && !seek_req_.load());
            });
            continue;
        }
        if (!armed) {
            if (!impl_->pcm->empty() || audio_done_serial_.load() == serial_.load()) {
                armed = true;
            } else {
                std::unique_lock<std::mutex> lock(wait_mu_);
                cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                    return stop_.load() || !playing_.load() || !impl_->pcm->empty() ||
                           audio_done_serial_.load() == serial_.load();
                });
                continue;
            }
        }
        if (stall_latched_.load()) {
            note_resumed();
            if (stall_latched_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
        }
        const std::uint32_t serial = serial_.load();
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
            const int wrote = impl_->alsa->write_frames(buf.data(), got);
            if (wrote == 0) {
                continue;
            }
            if (wrote < 0) {
                player_->media_failed("声卡写入失败");
                playing_.store(false);
                continue;
            }
            impl_->written_frames += got;
            std::int64_t played = impl_->written_frames - impl_->alsa->delay_frames();
            if (played < 0) {
                played = 0;
            }
            const std::int64_t position = origin_us_.load() + (impl_->rate > 0 ? played * 1000000 / impl_->rate : 0);
            player_->set_media_position(position);
            if (!impl_->saw_frame) {
                impl_->saw_frame = true;
                player_->media_first_frame(origin_us_.load(), monotonic_us() - player_->file_load_mono());
            }
            continue;
        }
        if (audio_done_serial_.load() == serial && impl_->pcm->empty()) {
            if (software) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (at_end_) {
                std::unique_lock<std::mutex> lock(wait_mu_);
                cv_.wait(lock, [this] { return stop_.load() || seek_req_.load() || !at_end_; });
                continue;
            }
            impl_->alsa->drain();
            if (stop_.load() || !playing_.load() || seek_req_.load()) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(wait_mu_);
                at_end_ = true;
                playing_.store(false);
            }
            player_->set_media_position(duration_us_ > 0 ? duration_us_ : origin_us_.load());
            player_->media_ended();
            continue;
        }
        note_bottom();
        if (software && armed && audio_done_serial_.load() != serial && !stall_latched_.load()) {
            ++empty_spins;
            if (empty_spins == 40) {
                note_pcm_xrun();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
}

void FilePlayback::wait_decoded(std::int64_t media_us) {
    (void)media_us;
#if !defined(LEGATO_HAS_FFMPEG)
    return;
#else
    if (!virtual_) {
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
#if !defined(LEGATO_HAS_FFMPEG)
    (void)elapsed_us;
#else
    if (!virtual_) {
        return;
    }
    constexpr std::int64_t kVsyncUs = 16667;
    const std::uint32_t serial = serial_.load();
    for (auto it = impl_->held.begin(); it != impl_->held.end();) {
        if (it->serial != serial) {
            it = impl_->held.erase(it);
        } else {
            ++it;
        }
    }
    auto pull_frames = [this, serial]() {
        VideoFrame frame;
        while (impl_->frames->pop(&frame) == 0) {
            if (frame.serial == serial) {
                impl_->held.push_back(frame);
            }
        }
    };
    auto video_ready = [this, serial](std::int64_t limit) {
        if (impl_->video == nullptr) {
            return true;
        }
        const bool done = video_done_serial_.load() == serial;
        bool any = false;
        bool newer = false;
        for (const VideoFrame& frame : impl_->held) {
            if (frame.pts_us <= limit) {
                any = true;
            } else {
                newer = true;
            }
        }
        /* 队列只有 8 帧。先把已经解出的帧挪到本线程，解码才能继续，直到出现超过 limit 的帧。 */
        if (any && (newer || done || decoded_video_us_.load() > limit)) {
            return true;
        }
        if (!any && done && impl_->frames->empty()) {
            return true;
        }
        return false;
    };
    auto audio_ready = [this, serial](int need) {
        if (impl_->audio == nullptr) {
            return true;
        }
        if (audio_done_serial_.load() == serial) {
            return true;
        }
        return impl_->pcm->buffered_frames() >= need;
    };
    if (elapsed_us < impl_->present_elapsed_us) {
        impl_->present_elapsed_us = 0;
    }
    while (impl_->present_elapsed_us + kVsyncUs <= elapsed_us) {
        const int want = impl_->rate > 0 ? static_cast<int>(impl_->rate * kVsyncUs / 1000000) : 0;
        const std::int64_t limit = origin_us_.load() + impl_->present_elapsed_us + kVsyncUs + kVsyncUs / 2;
        bool ready = false;
        for (int spin = 0; spin < 40; ++spin) {
            pull_frames();
            note_resumed();
            if (audio_ready(want) && video_ready(limit)) {
                ready = true;
                break;
            }
            note_bottom();
            if (stall_latched_.load() && queues_bottom()) {
                break;
            }
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(5), [this] { return stop_.load() || !playing_.load(); });
            if (stop_.load() || !playing_.load()) {
                break;
            }
        }
        pull_frames();
        if (stop_.load() || !playing_.load()) {
            break;
        }
        if (!ready && stall_latched_.load() && queues_bottom()) {
            break;
        }
        if (!ready) {
            note_bottom();
            if (stall_latched_.load() && queues_bottom()) {
                break;
            }
        }
        impl_->present_elapsed_us += kVsyncUs;
        int left = want;
        while (left > 0) {
            const int got = impl_->pcm->pop(nullptr, left, impl_->channels, serial);
            if (got <= 0) {
                break;
            }
            impl_->null_audio.consume(got);
            left -= got;
        }
        const std::int64_t audio_clock = impl_->rate > 0 ? impl_->null_audio.clock_us() : impl_->present_elapsed_us;
        int best = -1;
        for (std::size_t i = 0; i < impl_->held.size(); ++i) {
            if (impl_->held[i].pts_us <= limit) {
                best = static_cast<int>(i);
            } else {
                break;
            }
        }
        if (impl_->video == nullptr && !impl_->saw_present) {
            impl_->saw_present = true;
            out->first = true;
            out->first_frame_us = impl_->present_elapsed_us;
        }
        if (best >= 0) {
            const VideoFrame chosen = impl_->held[static_cast<std::size_t>(best)];
            impl_->held.erase(impl_->held.begin(), impl_->held.begin() + best + 1);
            impl_->recording.record(chosen.pts_us, chosen.content, origin_us_.load(), audio_clock);
            if (!impl_->saw_present) {
                impl_->saw_present = true;
                out->first = true;
                out->first_frame_us = impl_->present_elapsed_us;
            }
        }
        out->position_us = origin_us_.load() + audio_clock;
    }
    out->presented = impl_->recording.presented();
    out->av_sync_us = impl_->recording.worst_sync_us();
    impl_->recording.copy_hash(out->sha256, sizeof(out->sha256));
    if (demux_eof_.load() && duration_us_ > 0 && origin_us_.load() + elapsed_us >= duration_us_ && !impl_->present_ended) {
        impl_->present_ended = true;
        out->ended = true;
        out->position_us = duration_us_;
    }
#endif
}

}  /* namespace legato */
