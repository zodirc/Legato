#ifndef LEGATO_ENGINE_FILE_PLAYBACK_HPP
#define LEGATO_ENGINE_FILE_PLAYBACK_HPP

#include "legato/engine/clock.hpp"
#include "legato/engine/output_note.hpp"
#include "legato/engine/pipeline.hpp"
#include "legato/render/sink.hpp"

#include "legato/core/spsc_ring.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace legato {

class FrameQueue;
class PcmQueue;
class Player;
class VideoPresenter;

/* 本地文件与确定性回归共用这一条管线。时钟和 Sink 由 Player::io_main 注入。
   控制线程只写 serial 和目标位置。写入帧数、首帧标记由输出线程在看到新 serial 时清零。
   输出线程不拿 Player::mu_，首帧、卡顿、恢复和失败写入通知环，由控制线程改 state。
   持有 Player::mu_ 时不能 join 解复用线程：它回调 media_failed 需要这把锁。 */

class FilePlayback : public Pipeline {
public:
    struct Stages {
        std::int64_t probe_us = 0;
        std::int64_t first_packet_us = 0;
        std::int64_t decoder_open_us = 0;
        std::int64_t decoded_us = 0;
    };

    struct PresentUpdate {
        bool first = false;
        std::int64_t first_frame_us = 0;
        int presented = 0;
        std::int64_t av_sync_us = 0;
        bool ended = false;
        std::int64_t position_us = 0;
        char sha256[65]{};
    };

    FilePlayback(Player* player, Clock* clock, std::unique_ptr<AudioSink> audio, std::unique_ptr<VideoSink> video,
                 std::unique_ptr<AudioMasterClock> master, bool decode_video, double buffer_start_s,
                 double buffer_resume_s, double buffer_max_s);
    ~FilePlayback() override;

    FilePlayback(const FilePlayback&) = delete;
    FilePlayback& operator=(const FilePlayback&) = delete;

    int open(const char* path) override;
    void start() override;
    void pause() override;
    void seek(std::int64_t position_us, std::uint32_t serial) override;
    void stop() override;

    /* 调用线程等到起播缓冲够了，或流结束。不等整段目标，避免帧队列满了之后解不出来。 */
    void wait_decoded(std::int64_t media_us);
    void present_until(std::int64_t elapsed_us, PresentUpdate* out);

    std::int64_t duration_us() const { return duration_us_; }
    const Stages& stages() const { return stages_; }
    const char* error_name() const { return error_name_; }
    const char* error_message() const { return error_message_; }
    bool has_audio() const;
    void bind_clock(Clock* clock);
    Clock* monotonic_clock() { return &monotonic_; }
    bool pop_note(OutputNote* out);
    bool notes_pending() const;

private:
    friend class VideoPresenter;

    struct Impl;
    void launch_threads();
    void demux_main();
    void audio_main();
    void video_main();
    void audio_out_main(bool software);
    bool apply_seek();
    std::int64_t buffered_span_us() const;
    bool queues_bottom() const;
    void note_bottom();
    void note_resumed(bool via_ring);
    void wake();
    void emit_output(const OutputNote& note);
    FrameQueue* frames();
    PcmQueue* pcm();
    bool has_video() const;
    int sample_rate() const;
    int sample_channels() const;

    Player* player_ = nullptr;
    Clock* clock_ = nullptr;
    std::unique_ptr<AudioSink> audio_own_;
    std::unique_ptr<VideoSink> video_own_;
    std::unique_ptr<AudioMasterClock> master_own_;
    AudioSink* audio_sink_ = nullptr;
    VideoSink* video_sink_ = nullptr;
    AudioMasterClock* master_ = nullptr;
    MonotonicClock monotonic_;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<VideoPresenter> presenter_;
    bool decode_video_ = true;
    bool realtime_audio_ = false;
    double buffer_start_s_ = 1.5;
    double buffer_resume_s_ = 3;
    double buffer_max_s_ = 50;
    Stages stages_{};
    const char* error_name_ = "source_open_failed";
    const char* error_message_ = "打不开这个文件";
    std::int64_t duration_us_ = 0;
    std::mutex wait_mu_;
    std::condition_variable cv_;
    std::thread demux_thread_;
    std::thread audio_thread_;
    std::thread video_thread_;
    std::thread audio_out_thread_;
    std::thread video_drain_thread_;
    bool threads_started_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> playing_{false};
    bool at_end_ = false;
    std::atomic<bool> seek_req_{false};
    std::int64_t seek_to_ = 0;
    std::uint32_t seek_serial_ = 1;
    std::atomic<std::uint32_t> serial_{1};
    std::atomic<std::int64_t> origin_us_{0};
    std::atomic<bool> demux_eof_{false};
    /* 流结束时写下当时的 serial。Seek 改了 serial 之后，旧的结束标记不再算数。 */
    std::atomic<std::uint32_t> audio_done_serial_{0};
    std::atomic<std::uint32_t> video_done_serial_{0};
    std::atomic<std::int64_t> decoded_video_us_{-1};
    std::atomic<bool> reached_start_{false};
    std::atomic<bool> stall_latched_{false};
    std::atomic<std::int64_t> empty_since_us_{0};
    /* 只由 PCM 输出线程写。控制线程改 serial 后，本线程看到新值再清零。 */
    std::int64_t written_frames_ = 0;
    bool saw_audio_ = false;
    std::uint32_t audio_epoch_ = 0;
    SpscRing<OutputNote, 32> notes_;
};

}  /* namespace legato */

#endif
