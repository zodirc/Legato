#ifndef LEGATO_ENGINE_VIDEO_PRESENTER_HPP
#define LEGATO_ENGINE_VIDEO_PRESENTER_HPP

#include "legato/engine/clock.hpp"
#include "legato/engine/file_playback.hpp"
#include "legato/engine/media_queue.hpp"

#include <cstdint>
#include <deque>

namespace legato {

/* 挑帧只在这里。present_until 由控制线程在虚拟媒体时钟推进时调用。
   drain_main 由取帧线程调用。两条都不拿 Player::mu_。 */
class VideoPresenter {
public:
    void present_until(FilePlayback* playback, std::int64_t elapsed_us, FilePlayback::PresentUpdate* out);
    void drain_main(FilePlayback* playback);
    int held_count() const { return static_cast<int>(held_.size()); }

private:
    void note_serial(FilePlayback* playback, std::uint32_t serial);

    std::deque<VideoFrame> held_;
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
