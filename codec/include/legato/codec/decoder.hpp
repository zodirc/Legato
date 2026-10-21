#ifndef LEGATO_CODEC_DECODER_HPP
#define LEGATO_CODEC_DECODER_HPP

#include <cstdint>

namespace legato {

struct FrameView {
    int width = 0;
    int height = 0;
    int sample_rate = 0;
    int channels = 0;
    int nb_samples = 0;
    const std::int16_t* pcm = nullptr;
    const std::uint8_t* data0 = nullptr;
    int linesize0 = 0;
    std::int64_t pts_us = 0;
    std::uint32_t serial = 0;
    bool audio = false;
    bool hardware = false;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual int open() = 0;
    virtual int decode(const std::uint8_t* data, int size, std::int64_t pts_us) = 0;
    virtual int receive(FrameView* out) = 0;
    virtual void flush(std::uint32_t serial) = 0;
};

}  /* namespace legato */

#endif
