#ifndef LEGATO_ENGINE_VIDEO_PRESENTER_HPP
#define LEGATO_ENGINE_VIDEO_PRESENTER_HPP

#include "legato/engine/clock.hpp"
#include "legato/engine/file_playback.hpp"
#include "legato/engine/media_queue.hpp"

#include <cstdint>

namespace legato {

/* 挑帧只在这里。present_until 由控制线程在虚拟媒体时钟推进时调用。
   drain_main 由取帧线程调用。两条都不拿 Player::mu_。 */
class VideoPresenter {
public:
    VideoPresenter() = default;
    ~VideoPresenter();

    void present_until(FilePlayback* playback, std::int64_t elapsed_us, FilePlayback::PresentUpdate* out);
    void drain_main(FilePlayback* playback);
    int held_count() const { return held_count_; }

private:
    void note_serial(FilePlayback* playback, std::uint32_t serial);
    void keep_shown(VideoFrame* frame);
    void clear_held();
    void pace_audio(FilePlayback* playback);
    void emit_paced(FilePlayback* playback, VideoFrame* frame);

    /* 固定槽位。稳态里不能再为挑帧分配。 */
    static constexpr int kHeldMax = 16;
    VideoFrame held_[kHeldMax]{};
    int held_count_ = 0;
    VideoFrame shown_{};
    std::int64_t present_elapsed_us_ = 0;
    bool saw_present_ = false;
    bool present_ended_ = false;
    bool saw_video_ = false;
    std::uint32_t present_epoch_ = 0;
    std::uint32_t video_epoch_ = 0;
    AudioMasterClock sample_clock_;
    std::int64_t sample_written_ = 0;
    std::int64_t wall_origin_ = 0;
    std::int64_t media_origin_ = 0;
};

}  /* namespace legato */

#endif
