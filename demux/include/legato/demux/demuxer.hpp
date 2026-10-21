#ifndef LEGATO_DEMUX_DEMUXER_HPP
#define LEGATO_DEMUX_DEMUXER_HPP

#include <cstdint>

namespace legato {

struct PacketView {
    const std::uint8_t* data = nullptr;
    int size = 0;
    std::int64_t pts_us = 0;
    std::int64_t dts_us = 0;
    std::int64_t duration_us = 0;
    int stream_index = 0;
    bool keyframe = false;
    std::uint32_t serial = 0;
};

class Demuxer {
public:
    virtual ~Demuxer() = default;
    virtual int open() = 0;
    virtual int read_packet(PacketView* out) = 0;
    virtual void flush(std::uint32_t serial) = 0;
};

}  /* namespace legato */

#endif
