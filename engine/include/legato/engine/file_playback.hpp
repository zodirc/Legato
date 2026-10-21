#ifndef LEGATO_ENGINE_FILE_PLAYBACK_HPP
#define LEGATO_ENGINE_FILE_PLAYBACK_HPP

#include "legato/engine/pipeline.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace legato {

class Clock;
class Player;

/* 本地文件会话。解复用、每路解码、PCM 输出各占一个线程。
   控制线程只改标志。持有 Player::mu_ 时不能 join 这些线程，
   因为它们回调 media_first_frame 需要同一把锁。 */

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

    FilePlayback(Player* player, Clock* clock, bool virtual_clock, bool decode_video, double buffer_start_s,
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
    bool is_virtual() const { return virtual_; }

private:
    struct Impl;
    void launch_threads();
    void demux_main();
    void audio_main();
    void video_main();
    void audio_out_main(bool software);
    void video_drain_main();
    bool apply_seek();
    std::int64_t buffered_span_us() const;
    bool queues_bottom() const;
    void note_bottom();
    void note_resumed();
    void wake();

    Player* player_ = nullptr;
    Clock* clock_ = nullptr;
    std::unique_ptr<Impl> impl_;
    bool virtual_ = false;
    bool decode_video_ = true;
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
};

}  /* namespace legato */

#endif
