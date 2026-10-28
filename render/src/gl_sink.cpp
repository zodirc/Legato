#include "legato/render/sink.hpp"

#include "legato/core/alloc.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#endif

#if defined(LEGATO_HAS_FFMPEG)
extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#endif

namespace legato {
namespace {

constexpr int kMaxSyncSamples = 8192;

struct GlApi {
    bool tried = false;
    bool ready = false;
    void* lib = nullptr;
    void (*BindFramebuffer)(unsigned target, unsigned fbo) = nullptr;
    void (*Viewport)(int x, int y, int w, int h) = nullptr;
    void (*GenTextures)(int n, unsigned* textures) = nullptr;
    void (*BindTexture)(unsigned target, unsigned texture) = nullptr;
    void (*TexImage2D)(unsigned target, int level, int internal, int width, int height, int border, unsigned format,
                       unsigned type, const void* data) = nullptr;
    void (*TexParameteri)(unsigned target, unsigned pname, int param) = nullptr;
    void (*PixelStorei)(unsigned pname, int param) = nullptr;
    void (*Enable)(unsigned cap) = nullptr;
    void (*MatrixMode)(unsigned mode) = nullptr;
    void (*LoadIdentity)() = nullptr;
    void (*Ortho)(double l, double r, double b, double t, double n, double f) = nullptr;
    void (*Begin)(unsigned mode) = nullptr;
    void (*End)() = nullptr;
    void (*TexCoord2f)(float s, float t) = nullptr;
    void (*Vertex2f)(float x, float y) = nullptr;
    void (*Finish)() = nullptr;
};

void* load_sym(void* lib, const char* name) {
#if defined(__linux__)
    return lib != nullptr ? dlsym(lib, name) : nullptr;
#else
    (void)lib;
    (void)name;
    return nullptr;
#endif
}

GlApi& gl_api() {
    static GlApi api;
    if (api.tried) {
        return api;
    }
    api.tried = true;
    void* libgl = nullptr;
    void* libegl = nullptr;
#if defined(__linux__)
    libgl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    api.lib = libgl;
    if (libgl == nullptr) {
        return api;
    }
    auto sym = [&](const char* name) { return load_sym(libgl, name); };
    api.BindFramebuffer = reinterpret_cast<decltype(api.BindFramebuffer)>(sym("glBindFramebuffer"));
    api.Viewport = reinterpret_cast<decltype(api.Viewport)>(sym("glViewport"));
    api.GenTextures = reinterpret_cast<decltype(api.GenTextures)>(sym("glGenTextures"));
    api.BindTexture = reinterpret_cast<decltype(api.BindTexture)>(sym("glBindTexture"));
    api.TexImage2D = reinterpret_cast<decltype(api.TexImage2D)>(sym("glTexImage2D"));
    api.TexParameteri = reinterpret_cast<decltype(api.TexParameteri)>(sym("glTexParameteri"));
    api.PixelStorei = reinterpret_cast<decltype(api.PixelStorei)>(sym("glPixelStorei"));
    api.Enable = reinterpret_cast<decltype(api.Enable)>(sym("glEnable"));
    api.MatrixMode = reinterpret_cast<decltype(api.MatrixMode)>(sym("glMatrixMode"));
    api.LoadIdentity = reinterpret_cast<decltype(api.LoadIdentity)>(sym("glLoadIdentity"));
    api.Ortho = reinterpret_cast<decltype(api.Ortho)>(sym("glOrtho"));
    api.Begin = reinterpret_cast<decltype(api.Begin)>(sym("glBegin"));
    api.End = reinterpret_cast<decltype(api.End)>(sym("glEnd"));
    api.TexCoord2f = reinterpret_cast<decltype(api.TexCoord2f)>(sym("glTexCoord2f"));
    api.Vertex2f = reinterpret_cast<decltype(api.Vertex2f)>(sym("glVertex2f"));
    api.Finish = reinterpret_cast<decltype(api.Finish)>(sym("glFinish"));
    api.ready = api.BindFramebuffer && api.Viewport && api.GenTextures && api.BindTexture && api.TexImage2D &&
                api.TexParameteri && api.PixelStorei && api.Enable && api.MatrixMode && api.LoadIdentity && api.Ortho &&
                api.Begin && api.End && api.TexCoord2f && api.Vertex2f && api.Finish;
    (void)libegl;
    return api;
}

bool context_current() {
#if defined(__linux__)
    static void* libgl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    static void* libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    using GlxCurrent = void* (*)();
    using EglCurrent = void* (*)();
    static auto glx = libgl != nullptr ? reinterpret_cast<GlxCurrent>(dlsym(libgl, "glXGetCurrentContext")) : nullptr;
    static auto egl = libegl != nullptr ? reinterpret_cast<EglCurrent>(dlsym(libegl, "eglGetCurrentContext")) : nullptr;
    if (glx != nullptr && glx() != nullptr) {
        return true;
    }
    if (egl != nullptr) {
        void* ctx = egl();
        /* EGL_NO_CONTEXT 是 0。 */
        return ctx != nullptr;
    }
#else
    (void)0;
#endif
    return false;
}

}  /* namespace */

struct GlVideoSink::State {
    mutable std::mutex mu;
    int presented = 0;
    std::int64_t worst = 0;
    std::int64_t sync_us[kMaxSyncSamples]{};
    int sync_count = 0;
#if defined(LEGATO_HAS_FFMPEG)
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    int sws_width = 0;
    int sws_height = 0;
    int sws_format = -1;
#endif
    std::uint64_t generation = 0;
    std::uint64_t uploaded = 0;
    std::vector<std::uint8_t> rgb;
    unsigned texture = 0;
    bool texture_ready = false;
};

GlVideoSink::GlVideoSink() : state_(std::make_unique<State>()) {}

GlVideoSink::~GlVideoSink() {
#if defined(LEGATO_HAS_FFMPEG)
    if (state_->frame != nullptr) {
        av_frame_free(&state_->frame);
    }
    if (state_->sws != nullptr) {
        sws_freeContext(state_->sws);
    }
#endif
}

void GlVideoSink::present(std::int64_t pts_us) {
    VideoPresent sample;
    sample.pts_us = pts_us;
    present_sample(sample);
}

void GlVideoSink::present_sample(const VideoPresent& sample) {
    std::lock_guard<std::mutex> lock(state_->mu);
    ++state_->presented;
    const std::int64_t sync = sample.pts_us - (sample.origin_us + sample.audio_clock_us);
    const std::int64_t magnitude = sync < 0 ? -sync : sync;
    if (state_->sync_count < kMaxSyncSamples) {
        state_->sync_us[state_->sync_count++] = magnitude;
    }
    if (magnitude > state_->worst) {
        state_->worst = magnitude;
    }
#if defined(LEGATO_HAS_FFMPEG)
    if (sample.retain != nullptr) {
        AllocExempt exempt;
        if (state_->frame == nullptr) {
            state_->frame = av_frame_alloc();
        }
        if (state_->frame != nullptr) {
            av_frame_unref(state_->frame);
            if (av_frame_ref(state_->frame, static_cast<const AVFrame*>(sample.retain)) == 0) {
                ++state_->generation;
            }
        }
    }
#else
    (void)sample;
#endif
}

int GlVideoSink::presented() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->presented;
}

std::int64_t GlVideoSink::worst_sync_us() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->worst;
}

int GlVideoSink::copy_av_sync_us(std::int64_t* dst, int capacity) const {
    std::lock_guard<std::mutex> lock(state_->mu);
    const int count = state_->sync_count;
    if (dst == nullptr || capacity <= 0) {
        return count;
    }
    const int take = count < capacity ? count : capacity;
    for (int i = 0; i < take; ++i) {
        dst[i] = state_->sync_us[i];
    }
    return count;
}

int GlVideoSink::draw(unsigned fbo, int width, int height) {
    if (width <= 0 || height <= 0) {
        return -1;
    }
    /* 上下文属于调用方。这里创建窗口或切上下文，就会和示例程序的 SDL2 窗口抢当前上下文。 */
    if (!context_current()) {
        return -1;
    }
    GlApi& api = gl_api();
    if (!api.ready) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
#if defined(LEGATO_HAS_FFMPEG)
    if (state_->frame != nullptr && state_->generation != state_->uploaded && state_->frame->width > 0 &&
        state_->frame->height > 0) {
        AllocExempt exempt;
        const int src_w = state_->frame->width;
        const int src_h = state_->frame->height;
        const int src_fmt = state_->frame->format;
        if (state_->sws == nullptr || state_->sws_width != src_w || state_->sws_height != src_h ||
            state_->sws_format != src_fmt) {
            if (state_->sws != nullptr) {
                sws_freeContext(state_->sws);
            }
            state_->sws = sws_getContext(src_w, src_h, static_cast<AVPixelFormat>(src_fmt), src_w, src_h,
                                         AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
            state_->sws_width = src_w;
            state_->sws_height = src_h;
            state_->sws_format = src_fmt;
        }
        if (state_->sws == nullptr) {
            return -1;
        }
        state_->rgb.resize(static_cast<std::size_t>(src_w) * static_cast<std::size_t>(src_h) * 3u);
        std::uint8_t* dst_data[1] = {state_->rgb.data()};
        int dst_stride[1] = {src_w * 3};
        if (sws_scale(state_->sws, state_->frame->data, state_->frame->linesize, 0, src_h, dst_data, dst_stride) <= 0) {
            return -1;
        }
        if (!state_->texture_ready) {
            api.GenTextures(1, &state_->texture);
            state_->texture_ready = state_->texture != 0;
        }
        if (!state_->texture_ready) {
            return -1;
        }
        constexpr unsigned kTexture2D = 0x0DE1;
        constexpr unsigned kUnpackAlignment = 0x0CF5;
        constexpr unsigned kRgb = 0x1907;
        constexpr unsigned kUnsignedByte = 0x1401;
        constexpr unsigned kMinFilter = 0x2801;
        constexpr unsigned kMagFilter = 0x2800;
        constexpr int kLinear = 0x2601;
        api.PixelStorei(kUnpackAlignment, 1);
        api.BindTexture(kTexture2D, state_->texture);
        api.TexParameteri(kTexture2D, kMinFilter, kLinear);
        api.TexParameteri(kTexture2D, kMagFilter, kLinear);
        api.TexImage2D(kTexture2D, 0, static_cast<int>(kRgb), src_w, src_h, 0, kRgb, kUnsignedByte, state_->rgb.data());
        state_->uploaded = state_->generation;
    }
#endif
    if (!state_->texture_ready) {
        return -1;
    }
    constexpr unsigned kFramebuffer = 0x8D40;
    constexpr unsigned kTexture2D = 0x0DE1;
    constexpr unsigned kProjection = 0x1701;
    constexpr unsigned kModelview = 0x1700;
    constexpr unsigned kQuads = 0x0007;
    api.BindFramebuffer(kFramebuffer, fbo);
    api.Viewport(0, 0, width, height);
    api.MatrixMode(kProjection);
    api.LoadIdentity();
    api.Ortho(0.0, static_cast<double>(width), static_cast<double>(height), 0.0, -1.0, 1.0);
    api.MatrixMode(kModelview);
    api.LoadIdentity();
    api.Enable(kTexture2D);
    api.BindTexture(kTexture2D, state_->texture);
    api.Begin(kQuads);
    api.TexCoord2f(0.f, 0.f);
    api.Vertex2f(0.f, 0.f);
    api.TexCoord2f(1.f, 0.f);
    api.Vertex2f(static_cast<float>(width), 0.f);
    api.TexCoord2f(1.f, 1.f);
    api.Vertex2f(static_cast<float>(width), static_cast<float>(height));
    api.TexCoord2f(0.f, 1.f);
    api.Vertex2f(0.f, static_cast<float>(height));
    api.End();
    api.Finish();
    return 0;
}

}  /* namespace legato */
