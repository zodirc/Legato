#ifndef LEGATO_DEMUX_FFMPEG_DEMUXER_HPP
#define LEGATO_DEMUX_FFMPEG_DEMUXER_HPP

#include "legato/demux/demuxer.hpp"

namespace legato {

class DataSource;

/* FFmpeg demuxer. Bytes come from DataSource through AVIO, not from FFmpeg's own IO. */
class FFmpegDemuxer : public Demuxer {
public:
    struct Stream {
        int index = -1;
        bool audio = false;
        bool video = false;
        int sample_rate = 0;
        int channels = 0;
        int width = 0;
        int height = 0;
        int time_base_num = 1;
        int time_base_den = 1000000;
        const void* codecpar = nullptr;
    };

    explicit FFmpegDemuxer(DataSource* source);
    ~FFmpegDemuxer() override;

    int open() override;
    int read_packet(PacketView* out) override;
    void flush(std::uint32_t serial) override;
    int seek_us(std::int64_t pts_us);

    std::int64_t duration_us() const { return duration_us_; }
    const Stream* audio() const { return audio_.index >= 0 ? &audio_ : nullptr; }
    const Stream* video() const { return video_.index >= 0 ? &video_ : nullptr; }

private:
    DataSource* source_ = nullptr;
    void* fmt_ = nullptr;
    void* io_ = nullptr;
    void* io_buffer_ = nullptr;
    void* packet_ = nullptr;
    Stream audio_{};
    Stream video_{};
    std::int64_t duration_us_ = 0;
    std::uint32_t serial_ = 1;
};

}  /* namespace legato */

#endif
