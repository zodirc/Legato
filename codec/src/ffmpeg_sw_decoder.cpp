#include "legato/codec/ffmpeg_sw_decoder.hpp"

#include "legato/core/alloc.hpp"

#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace legato {
namespace {

void release_av_frame(void* handle) {
    /* 归还 AVFrame 时 FFmpeg 的缓冲池仍可能分配。这是 receive 里那份引用的配对释放。 */
    AllocExempt exempt;
    auto* frame = static_cast<AVFrame*>(handle);
    av_frame_free(&frame);
}

std::int64_t to_us(std::int64_t pts, AVRational time_base) {
    if (pts == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(pts, time_base, AVRational{1, 1000000});
}

}  /* namespace */

FFmpegSwDecoder::FFmpegSwDecoder(const void* codecpar, int time_base_num, int time_base_den, bool audio,
                                 int out_rate, int out_channels)
    : codecpar_(codecpar),
      time_base_num_(time_base_num),
      time_base_den_(time_base_den),
      audio_(audio),
      out_rate_(out_rate),
      out_channels_(out_channels) {}

FFmpegSwDecoder::~FFmpegSwDecoder() {
    if (codec_ != nullptr) {
        AVCodecContext* codec = static_cast<AVCodecContext*>(codec_);
        avcodec_free_context(&codec);
    }
    if (frame_ != nullptr) {
        AVFrame* frame = static_cast<AVFrame*>(frame_);
        av_frame_free(&frame);
    }
    if (swr_ != nullptr) {
        swr_free(reinterpret_cast<SwrContext**>(&swr_));
    }
    std::free(pcm_);
}

int FFmpegSwDecoder::open() {
    const auto* par = static_cast<const AVCodecParameters*>(codecpar_);
    if (par == nullptr) {
        return -1;
    }
    const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
    if (decoder == nullptr) {
        return -1;
    }
    AVCodecContext* codec = avcodec_alloc_context3(decoder);
    if (codec == nullptr) {
        return -1;
    }
    if (avcodec_parameters_to_context(codec, par) < 0 || avcodec_open2(codec, decoder, nullptr) < 0) {
        avcodec_free_context(&codec);
        return -1;
    }
    codec_ = codec;
    frame_ = av_frame_alloc();
    if (frame_ == nullptr) {
        return -1;
    }
    if (!audio_) {
        return 0;
    }
    if (out_rate_ <= 0) {
        out_rate_ = par->sample_rate;
    }
    AVChannelLayout in_layout;
    if (par->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&in_layout, &par->ch_layout) < 0) {
            return -1;
        }
    } else {
        av_channel_layout_default(&in_layout, 1);
    }
    if (out_channels_ <= 0) {
        out_channels_ = in_layout.nb_channels > 0 ? in_layout.nb_channels : 2;
    }
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_channels_);
    SwrContext* swr = nullptr;
    const int swr_err = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, out_rate_, &in_layout,
                                            static_cast<AVSampleFormat>(par->format), par->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    av_channel_layout_uninit(&in_layout);
    if (swr_err < 0 || swr == nullptr || swr_init(swr) < 0) {
        swr_free(&swr);
        return -1;
    }
    swr_ = swr;
    return 0;
}

int FFmpegSwDecoder::decode(const std::uint8_t* data, int size, std::int64_t pts_us) {
    AllocExempt exempt;
    AVCodecContext* codec = static_cast<AVCodecContext*>(codec_);
    if (codec == nullptr) {
        return -1;
    }
    if (data == nullptr || size <= 0) {
        return avcodec_send_packet(codec, nullptr) < 0 ? -1 : 0;
    }
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr || av_new_packet(packet, size) < 0) {
        av_packet_free(&packet);
        return -1;
    }
    std::memcpy(packet->data, data, static_cast<std::size_t>(size));
    packet->pts = av_rescale_q(pts_us, AVRational{1, 1000000}, AVRational{time_base_num_, time_base_den_});
    const int err = avcodec_send_packet(codec, packet);
    av_packet_free(&packet);
    return err < 0 && err != AVERROR(EAGAIN) ? -1 : 0;
}

int FFmpegSwDecoder::receive(FrameView* out) {
    AllocExempt exempt;
    if (out == nullptr || frame_ == nullptr || codec_ == nullptr) {
        return -1;
    }
    AVFrame* frame = static_cast<AVFrame*>(frame_);
    const int err = avcodec_receive_frame(static_cast<AVCodecContext*>(codec_), frame);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
        return 1;
    }
    if (err < 0) {
        return -1;
    }
    out->serial = serial_;
    out->hardware = false;
    out->pts_us = to_us(frame->pts, AVRational{time_base_num_, time_base_den_});
    out->width = frame->width;
    out->height = frame->height;
    out->audio = audio_;
    out->pcm = nullptr;
    out->data0 = nullptr;
    out->linesize0 = 0;
    out->nb_samples = 0;
    out->sample_rate = out_rate_;
    out->channels = out_channels_;
    if (!audio_) {
        /* 交给队列的是另一份引用。解码器下一次 receive 可以复用自己的 AVFrame。 */
        AVFrame* copy = av_frame_alloc();
        if (copy == nullptr || av_frame_ref(copy, frame) < 0) {
            av_frame_free(&copy);
            return -1;
        }
        out->data0 = copy->data[0];
        out->linesize0 = copy->linesize[0];
        out->retain = copy;
        out->release_retain = release_av_frame;
        return 0;
    }
    const int out_samples = swr_get_out_samples(static_cast<SwrContext*>(swr_), frame->nb_samples);
    if (out_samples < 0) {
        av_frame_unref(frame);
        return -1;
    }
    const int need = (out_samples + 64) * out_channels_;
    if (need > pcm_capacity_) {
        auto* grown = static_cast<std::int16_t*>(std::realloc(pcm_, static_cast<std::size_t>(need) * sizeof(std::int16_t)));
        if (grown == nullptr) {
            av_frame_unref(frame);
            return -1;
        }
        pcm_ = grown;
        pcm_capacity_ = need;
    }
    std::uint8_t* dest[1] = {reinterpret_cast<std::uint8_t*>(pcm_)};
    const int got = swr_convert(static_cast<SwrContext*>(swr_), dest, out_samples,
                                const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
    av_frame_unref(frame);
    if (got < 0) {
        return -1;
    }
    out->pcm = pcm_;
    out->nb_samples = got;
    if (out->pts_us == 0 && sample_origin_ > 0) {
        out->pts_us = sample_origin_;
    }
    sample_origin_ += static_cast<std::int64_t>(got) * 1000000 / out_rate_;
    return 0;
}

int FFmpegSwDecoder::pull_delayed(FrameView* out) {
    AllocExempt exempt;
    if (!audio_ || swr_ == nullptr || out == nullptr || out_rate_ <= 0 || out_channels_ <= 0) {
        return 1;
    }
    auto* swr = static_cast<SwrContext*>(swr_);
    const int delayed = static_cast<int>(swr_get_delay(swr, out_rate_));
    if (delayed <= 0) {
        return 1;
    }
    const int need = (delayed + 64) * out_channels_;
    if (need > pcm_capacity_) {
        auto* grown = static_cast<std::int16_t*>(std::realloc(pcm_, static_cast<std::size_t>(need) * sizeof(std::int16_t)));
        if (grown == nullptr) {
            return -1;
        }
        pcm_ = grown;
        pcm_capacity_ = need;
    }
    std::uint8_t* dest[1] = {reinterpret_cast<std::uint8_t*>(pcm_)};
    const int got = swr_convert(swr, dest, delayed, nullptr, 0);
    if (got < 0) {
        return -1;
    }
    if (got == 0) {
        return 1;
    }
    out->serial = serial_;
    out->hardware = false;
    out->pts_us = sample_origin_;
    out->width = 0;
    out->height = 0;
    out->audio = true;
    out->pcm = pcm_;
    out->nb_samples = got;
    out->sample_rate = out_rate_;
    out->channels = out_channels_;
    sample_origin_ += static_cast<std::int64_t>(got) * 1000000 / out_rate_;
    return 0;
}

void FFmpegSwDecoder::flush(std::uint32_t serial) {
    AllocExempt exempt;
    serial_ = serial;
    sample_origin_ = 0;
    if (codec_ != nullptr) {
        avcodec_flush_buffers(static_cast<AVCodecContext*>(codec_));
    }
    /* 再初始化会清掉重采样延迟。不清的话，seek 前的样本会接到新位置后面。 */
    if (swr_ != nullptr) {
        swr_init(static_cast<SwrContext*>(swr_));
    }
}

}  /* namespace legato */
