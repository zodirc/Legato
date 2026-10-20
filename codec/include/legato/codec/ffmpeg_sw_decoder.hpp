#ifndef LEGATO_CODEC_FFMPEG_SW_DECODER_HPP
#define LEGATO_CODEC_FFMPEG_SW_DECODER_HPP

#include "legato/codec/decoder.hpp"

namespace legato {

class FFmpegSwDecoder : public Decoder {
public:
    FFmpegSwDecoder(const void* codecpar, int time_base_num, int time_base_den, bool audio, int out_rate,
                    int out_channels);
    ~FFmpegSwDecoder() override;

    int open() override;
    int decode(const std::uint8_t* data, int size, std::int64_t pts_us) override;
    int receive(FrameView* out) override;
    /* 最后一个包之后，重采样器里可能还留着尾部样本。0 表示取出一帧，1 表示已经空了。 */
    int pull_delayed(FrameView* out);
    void flush(std::uint32_t serial) override;

private:
    const void* codecpar_ = nullptr;
    int time_base_num_ = 1;
    int time_base_den_ = 1000000;
    bool audio_ = false;
    int out_rate_ = 0;
    int out_channels_ = 0;
    void* codec_ = nullptr;
    void* frame_ = nullptr;
    void* swr_ = nullptr;
    std::int16_t* pcm_ = nullptr;
    int pcm_capacity_ = 0;
    std::uint32_t serial_ = 1;
    std::int64_t sample_origin_ = 0;
};

}  /* namespace legato */

#endif
