#include "legato/engine/file_playback.hpp"

#include "legato/engine/player.hpp"
#include "legato/pal/clock.hpp"

#include <chrono>

#if defined(LEGATO_HAS_FFMPEG)
#include "legato/codec/ffmpeg_sw_decoder.hpp"
#include "legato/demux/ffmpeg_demuxer.hpp"
#include "legato/io/file_source.hpp"
#include "legato/render/alsa_sink.hpp"
#endif

namespace legato {

struct FilePlayback::Impl {
#if defined(LEGATO_HAS_FFMPEG)
    std::unique_ptr<FileSource> source;
    std::unique_ptr<FFmpegDemuxer> demux;
    std::unique_ptr<FFmpegSwDecoder> audio;
    std::unique_ptr<FFmpegSwDecoder> video;
    std::unique_ptr<AlsaSink> alsa;
    int audio_index = -1;
    int video_index = -1;
    int rate = 48000;
    int channels = 2;
    std::int64_t origin_us = 0;
    std::int64_t written_frames = 0;
    std::int64_t wall_origin = 0;
    std::int64_t media_origin = 0;
    bool saw_frame = false;
#endif
};

FilePlayback::FilePlayback(Player* player) : player_(player), impl_(std::make_unique<Impl>()) {}

FilePlayback::~FilePlayback() { stop_and_join(); }

int FilePlayback::open(const char* path) {
#if !defined(LEGATO_HAS_FFMPEG)
    (void)path;
    return -1;
#else
    impl_->source = std::make_unique<FileSource>(path);
    if (impl_->source->open_file() != 0) {
        return -1;
    }
    impl_->demux = std::make_unique<FFmpegDemuxer>(impl_->source.get());
    if (impl_->demux->open() != 0) {
        return -1;
    }
    duration_us_ = impl_->demux->duration_us();
    if (const FFmpegDemuxer::Stream* audio = impl_->demux->audio()) {
        impl_->audio_index = audio->index;
        impl_->rate = audio->sample_rate > 0 ? audio->sample_rate : 48000;
        impl_->channels = audio->channels > 0 ? audio->channels : 2;
        impl_->alsa = std::make_unique<AlsaSink>();
        if (impl_->alsa->open(impl_->rate, impl_->channels) != 0) {
            return -1;
        }
        impl_->rate = impl_->alsa->rate();
        impl_->channels = impl_->alsa->channels();
        impl_->audio = std::make_unique<FFmpegSwDecoder>(audio->codecpar, audio->time_base_num, audio->time_base_den,
                                                         true, impl_->rate, impl_->channels);
        if (impl_->audio->open() != 0) {
            return -1;
        }
    }
    if (const FFmpegDemuxer::Stream* video = impl_->demux->video()) {
        auto video_dec = std::make_unique<FFmpegSwDecoder>(video->codecpar, video->time_base_num, video->time_base_den,
                                                           false, 0, 0);
        /* 视频只解码并计时，没有窗口。带音频时视频解码失败可以忽略。 */
        if (video_dec->open() == 0) {
            impl_->video = std::move(video_dec);
            impl_->video_index = video->index;
        } else if (!impl_->audio) {
            return -1;
        }
    }
    if (!impl_->audio && !impl_->video) {
        return -1;
    }
    return 0;
#endif
}

void FilePlayback::start() {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        if (at_end_) {
            at_end_ = false;
            seek_req_ = true;
            seek_to_ = 0;
        }
        playing_ = true;
    }
    cv_.notify_all();
    if (!thread_.joinable()) {
        stop_ = false;
        thread_ = std::thread([this] { run(); });
    }
}

void FilePlayback::pause() {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        playing_ = false;
    }
    cv_.notify_all();
#if defined(LEGATO_HAS_FFMPEG)
    if (impl_->alsa) {
        impl_->alsa->drop();
    }
#endif
}

void FilePlayback::request_seek(std::int64_t position_us) {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        seek_to_ = position_us;
        seek_req_ = true;
        at_end_ = false;
    }
    cv_.notify_all();
#if defined(LEGATO_HAS_FFMPEG)
    if (impl_->alsa) {
        impl_->alsa->drop();
    }
#endif
}

void FilePlayback::stop_and_join() {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        stop_ = true;
        playing_ = false;
    }
    cv_.notify_all();
#if defined(LEGATO_HAS_FFMPEG)
    if (impl_ && impl_->alsa) {
        impl_->alsa->drop();
    }
#endif
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FilePlayback::run() {
#if !defined(LEGATO_HAS_FFMPEG)
    return;
#else
    /* Ok 继续；Stop 是暂停或 seek 打断了 writei；Fail 设备或解码已经不可用。 */
    enum class Step { Ok, Stop, Fail };
    auto park = [this] {
        std::lock_guard<std::mutex> lock(wait_mu_);
        playing_ = false;
    };
    auto note_audio = [&](const FrameView& frame) {
        if (frame.nb_samples <= 0 || frame.pcm == nullptr || impl_->alsa == nullptr) {
            return Step::Ok;
        }
        if (impl_->rate <= 0) {
            player_->media_failed("采样率为 0");
            return Step::Fail;
        }
        const int wrote = impl_->alsa->write_frames(frame.pcm, frame.nb_samples);
        if (wrote == 0) {
            return Step::Stop;
        }
        if (wrote < 0) {
            player_->media_failed("声卡写入失败");
            return Step::Fail;
        }
        impl_->written_frames += frame.nb_samples;
        const std::int64_t delay = impl_->alsa->delay_frames();
        std::int64_t played = impl_->written_frames - delay;
        if (played < 0) {
            played = 0;
        }
        const std::int64_t position = impl_->origin_us + played * 1000000 / impl_->rate;
        player_->set_media_position(position);
        if (!impl_->saw_frame) {
            impl_->saw_frame = true;
            const std::int64_t startup = monotonic_us() - player_->file_load_mono();
            player_->media_first_frame(impl_->origin_us, startup);
        }
        return Step::Ok;
    };
    /* 没有声卡时用视频时间戳睡觉，避免纯视频文件一解码完就报结束。 */
    auto pace_video = [&](std::int64_t pts_us) {
        if (impl_->alsa) {
            return;
        }
        if (impl_->wall_origin == 0) {
            impl_->wall_origin = monotonic_us();
            impl_->media_origin = pts_us;
        }
        const std::int64_t due = impl_->wall_origin + (pts_us - impl_->media_origin);
        while (!stop_ && playing_.load() && !seek_req_.load()) {
            const std::int64_t left = due - monotonic_us();
            if (left <= 0) {
                break;
            }
            const auto slice = left > 20000 ? 20000 : left;
            std::this_thread::sleep_for(std::chrono::microseconds(slice));
        }
        if (!stop_ && playing_.load() && !seek_req_.load()) {
            player_->set_media_position(pts_us);
        }
    };
    auto drain_decoder = [&](FFmpegSwDecoder* decoder, bool audio) {
        if (decoder == nullptr) {
            return Step::Ok;
        }
        if (decoder->decode(nullptr, 0, 0) < 0) {
            player_->media_failed(audio ? "音频解码失败" : "视频解码失败");
            return Step::Fail;
        }
        FrameView frame;
        for (;;) {
            const int rc = decoder->receive(&frame);
            if (rc == 1) {
                break;
            }
            if (rc < 0) {
                player_->media_failed(audio ? "音频解码失败" : "视频解码失败");
                return Step::Fail;
            }
            if (!audio) {
                continue;
            }
            const Step step = note_audio(frame);
            if (step != Step::Ok) {
                return step;
            }
        }
        if (!audio) {
            return Step::Ok;
        }
        for (;;) {
            const int rc = decoder->pull_delayed(&frame);
            if (rc == 1) {
                return Step::Ok;
            }
            if (rc < 0) {
                player_->media_failed("音频解码失败");
                return Step::Fail;
            }
            const Step step = note_audio(frame);
            if (step != Step::Ok) {
                return step;
            }
        }
    };
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lock(wait_mu_);
            cv_.wait(lock, [this] { return stop_ || playing_; });
        }
        if (stop_) {
            return;
        }
        if (seek_req_) {
            std::int64_t target = 0;
            {
                std::lock_guard<std::mutex> lock(wait_mu_);
                seek_req_ = false;
                target = seek_to_;
            }
            if (impl_->demux->seek_us(target) != 0) {
                player_->media_failed("定位失败");
                park();
                continue;
            }
            if (impl_->audio) {
                impl_->audio->flush(1);
            }
            if (impl_->video) {
                impl_->video->flush(1);
            }
            impl_->origin_us = target;
            impl_->written_frames = 0;
            impl_->wall_origin = 0;
            impl_->saw_frame = false;
            if (impl_->alsa) {
                impl_->alsa->drop();
                impl_->alsa->end_interrupt();
            }
            player_->set_media_position(target);
        }
        bool eof = false;
        while (!stop_ && playing_ && !seek_req_) {
            PacketView packet;
            const int rc = impl_->demux->read_packet(&packet);
            if (rc == 1) {
                eof = true;
                break;
            }
            if (rc < 0) {
                player_->media_failed("读包失败");
                park();
                break;
            }
            Step step = Step::Ok;
            if (packet.stream_index == impl_->audio_index && impl_->audio) {
                if (impl_->audio->decode(packet.data, packet.size, packet.pts_us) < 0) {
                    player_->media_failed("音频解码失败");
                    step = Step::Fail;
                } else {
                    FrameView frame;
                    for (;;) {
                        const int got = impl_->audio->receive(&frame);
                        if (got == 1) {
                            break;
                        }
                        if (got < 0) {
                            player_->media_failed("音频解码失败");
                            step = Step::Fail;
                            break;
                        }
                        step = note_audio(frame);
                        if (step != Step::Ok) {
                            break;
                        }
                    }
                }
            } else if (packet.stream_index == impl_->video_index && impl_->video) {
                if (impl_->video->decode(packet.data, packet.size, packet.pts_us) < 0) {
                    player_->media_failed("视频解码失败");
                    step = Step::Fail;
                } else {
                    FrameView frame;
                    for (;;) {
                        const int got = impl_->video->receive(&frame);
                        if (got == 1) {
                            break;
                        }
                        if (got < 0) {
                            player_->media_failed("视频解码失败");
                            step = Step::Fail;
                            break;
                        }
                        pace_video(frame.pts_us);
                        if (!impl_->audio && !impl_->saw_frame) {
                            impl_->saw_frame = true;
                            player_->media_first_frame(frame.pts_us, monotonic_us() - player_->file_load_mono());
                        }
                    }
                }
            }
            if (step == Step::Fail) {
                park();
                break;
            }
            if (step == Step::Stop || !playing_ || seek_req_) {
                break;
            }
        }
        if (stop_) {
            return;
        }
        if (seek_req_ || !playing_) {
            continue;
        }
        if (eof) {
            const Step audio_tail = drain_decoder(impl_->audio.get(), true);
            const Step video_tail = audio_tail == Step::Ok ? drain_decoder(impl_->video.get(), false) : audio_tail;
            if (video_tail == Step::Fail) {
                park();
                continue;
            }
            if (video_tail == Step::Stop || stop_ || !playing_ || seek_req_) {
                continue;
            }
            if (impl_->alsa) {
                impl_->alsa->drain();
            }
            /* drain 期间可能被暂停打断。那种情况不算播完。 */
            if (stop_ || !playing_ || seek_req_) {
                continue;
            }
            player_->set_media_position(duration_us_ > 0 ? duration_us_ : impl_->origin_us);
            {
                std::lock_guard<std::mutex> lock(wait_mu_);
                at_end_ = true;
                playing_ = false;
            }
            player_->media_ended();
        }
    }
#endif
}

}  /* namespace legato */
