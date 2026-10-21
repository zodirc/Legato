#include "legato/demux/ffmpeg_demuxer.hpp"

#include "legato/io/data_source.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace legato {
namespace {

int read_cb(void* opaque, std::uint8_t* buf, int size) {
    auto* source = static_cast<DataSource*>(opaque);
    const int got = source->read(buf, size);
    if (got == 0) {
        return AVERROR_EOF;
    }
    if (got < 0) {
        return AVERROR(EIO);
    }
    return got;
}

std::int64_t seek_cb(void* opaque, std::int64_t offset, int whence) {
    auto* source = static_cast<DataSource*>(opaque);
    if ((whence & AVSEEK_SIZE) != 0) {
        return source->length();
    }
    whence &= ~AVSEEK_FORCE;
    return source->seek(offset, whence);
}

FFmpegDemuxer::Stream make_stream(const AVStream* stream) {
    FFmpegDemuxer::Stream out;
    out.index = stream->index;
    out.time_base_num = stream->time_base.num;
    out.time_base_den = stream->time_base.den;
    out.codecpar = stream->codecpar;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        out.audio = true;
        out.sample_rate = stream->codecpar->sample_rate;
        out.channels = stream->codecpar->ch_layout.nb_channels;
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        out.video = true;
        out.width = stream->codecpar->width;
        out.height = stream->codecpar->height;
    }
    return out;
}

}  /* namespace */

FFmpegDemuxer::FFmpegDemuxer(DataSource* source) : source_(source) {}

/* io 缓冲是 av_malloc 的，avio_context_free 不会释放它。
   先把 fmt->pb 置空，close_input 才不会把同一块缓冲再释放一次。 */
FFmpegDemuxer::~FFmpegDemuxer() {
    if (packet_ != nullptr) {
        AVPacket* packet = static_cast<AVPacket*>(packet_);
        av_packet_free(&packet);
        packet_ = nullptr;
    }
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_);
    AVIOContext* io = static_cast<AVIOContext*>(io_);
    if (fmt != nullptr) {
        fmt->pb = nullptr;
        avformat_close_input(&fmt);
        fmt_ = nullptr;
    }
    if (io != nullptr) {
        unsigned char* buffer = io->buffer;
        avio_context_free(&io);
        av_free(buffer);
        io_ = nullptr;
        io_buffer_ = nullptr;
    }
}

int FFmpegDemuxer::open() {
    if (source_ == nullptr) {
        return -1;
    }
    av_log_set_level(AV_LOG_ERROR);
    const int buffer_size = 32 * 1024;
    unsigned char* buffer = static_cast<unsigned char*>(av_malloc(static_cast<std::size_t>(buffer_size)));
    if (buffer == nullptr) {
        return -1;
    }
    AVIOContext* io = avio_alloc_context(buffer, buffer_size, 0, source_, read_cb, nullptr, seek_cb);
    if (io == nullptr) {
        av_free(buffer);
        return -1;
    }
    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        unsigned char* owned = io->buffer;
        avio_context_free(&io);
        av_free(owned);
        return -1;
    }
    /* url 留空，字节全部来自 DataSource。FFmpeg 自己的文件协议不参与。 */
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
        fmt->pb = nullptr;
        avformat_free_context(fmt);
        unsigned char* owned = io->buffer;
        avio_context_free(&io);
        av_free(owned);
        return -1;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        fmt->pb = nullptr;
        avformat_close_input(&fmt);
        unsigned char* owned = io->buffer;
        avio_context_free(&io);
        av_free(owned);
        return -1;
    }
    fmt_ = fmt;
    io_ = io;
    io_buffer_ = buffer;
    if (fmt->duration > 0) {
        duration_us_ = fmt->duration;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        Stream stream = make_stream(fmt->streams[i]);
        if (stream.audio && audio_.index < 0) {
            audio_ = stream;
        } else if (stream.video && video_.index < 0) {
            video_ = stream;
        }
    }
    if (audio_.index < 0 && video_.index < 0) {
        return -1;
    }
    packet_ = av_packet_alloc();
    return packet_ != nullptr ? 0 : -1;
}

int FFmpegDemuxer::read_packet(PacketView* out) {
    if (out == nullptr || fmt_ == nullptr || packet_ == nullptr) {
        return -1;
    }
    AVPacket* packet = static_cast<AVPacket*>(packet_);
    av_packet_unref(packet);
    const int err = av_read_frame(static_cast<AVFormatContext*>(fmt_), packet);
    if (err == AVERROR_EOF) {
        return 1;
    }
    if (err < 0) {
        return -1;
    }
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_);
    if (packet->stream_index < 0 || static_cast<unsigned>(packet->stream_index) >= fmt->nb_streams) {
        av_packet_unref(packet);
        return -1;
    }
    AVStream* stream = fmt->streams[packet->stream_index];
    out->data = packet->data;
    out->size = packet->size;
    out->pts_us = packet->pts == AV_NOPTS_VALUE
                      ? 0
                      : av_rescale_q(packet->pts, stream->time_base, AVRational{1, 1000000});
    out->dts_us = packet->dts == AV_NOPTS_VALUE
                      ? out->pts_us
                      : av_rescale_q(packet->dts, stream->time_base, AVRational{1, 1000000});
    out->stream_index = packet->stream_index;
    out->keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    out->serial = serial_;
    out->duration_us = packet->duration > 0
                           ? av_rescale_q(packet->duration, stream->time_base, AVRational{1, 1000000})
                           : 0;
    return 0;
}

int FFmpegDemuxer::take_packet(PacketView* out, void** owned) {
    if (owned == nullptr) {
        return -1;
    }
    *owned = nullptr;
    const int rc = read_packet(out);
    if (rc != 0) {
        return rc;
    }
    AVPacket* packet = static_cast<AVPacket*>(packet_);
    AVPacket* clone = av_packet_alloc();
    if (clone == nullptr || av_packet_ref(clone, packet) < 0) {
        av_packet_free(&clone);
        av_packet_unref(packet);
        return -1;
    }
    out->data = clone->data;
    out->size = clone->size;
    *owned = clone;
    av_packet_unref(packet);
    return 0;
}

void release_owned_packet(void* owned) {
    AVPacket* packet = static_cast<AVPacket*>(owned);
    av_packet_free(&packet);
}

void FFmpegDemuxer::flush(std::uint32_t serial) {
    serial_ = serial;
    if (packet_ != nullptr) {
        av_packet_unref(static_cast<AVPacket*>(packet_));
    }
}

int FFmpegDemuxer::seek_us(std::int64_t pts_us) {
    if (fmt_ == nullptr) {
        return -1;
    }
    if (av_seek_frame(static_cast<AVFormatContext*>(fmt_), -1, pts_us, AVSEEK_FLAG_BACKWARD) < 0) {
        return -1;
    }
    if (packet_ != nullptr) {
        av_packet_unref(static_cast<AVPacket*>(packet_));
    }
    return 0;
}

}  /* namespace legato */
