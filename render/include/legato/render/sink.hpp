#ifndef LEGATO_RENDER_SINK_HPP
#define LEGATO_RENDER_SINK_HPP

#include <cstdint>

namespace legato {

class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual int open(int sample_rate, int channels) = 0;
    virtual void present(std::int64_t pts_us) = 0;
    virtual void close() = 0;
};

class VideoSink {
public:
    virtual ~VideoSink() = default;
    virtual void present(std::int64_t pts_us) = 0;
};

/* Headless sinks. present() only counts; it does not allocate. */
void null_audio_present(std::int64_t pts_us);
void null_video_present(std::int64_t pts_us);
std::uint64_t null_audio_frames();
std::uint64_t null_video_frames();

}  /* namespace legato */

#endif
