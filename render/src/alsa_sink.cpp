#include "legato/render/alsa_sink.hpp"

#include "legato/core/log.hpp"

#include <cerrno>
#include <cstring>

/* The machine has the ALSA runtime library but not the development headers. */
extern "C" {
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

enum {
    LEGATO_PCM_STREAM_PLAYBACK = 0,
    LEGATO_PCM_ACCESS_RW_INTERLEAVED = 3,
    LEGATO_PCM_FORMAT_S16_LE = 2
};

int snd_pcm_open(snd_pcm_t** pcm, const char* name, int stream, int mode);
int snd_pcm_close(snd_pcm_t* pcm);
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t** ptr);
void snd_pcm_hw_params_free(snd_pcm_hw_params_t* obj);
int snd_pcm_hw_params_any(snd_pcm_t* pcm, snd_pcm_hw_params_t* params);
int snd_pcm_hw_params_set_access(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, int access);
int snd_pcm_hw_params_set_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, int format);
int snd_pcm_hw_params_set_channels(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, unsigned int val);
int snd_pcm_hw_params_set_rate_near(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, unsigned int* val, int* dir);
int snd_pcm_hw_params(snd_pcm_t* pcm, snd_pcm_hw_params_t* params);
int snd_pcm_prepare(snd_pcm_t* pcm);
int snd_pcm_drop(snd_pcm_t* pcm);
int snd_pcm_drain(snd_pcm_t* pcm);
int snd_pcm_delay(snd_pcm_t* pcm, snd_pcm_sframes_t* delay);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t* pcm, const void* buffer, snd_pcm_uframes_t size);
int snd_pcm_recover(snd_pcm_t* pcm, int err, int silent);
const char* snd_strerror(int err);
}

namespace legato {
namespace {

bool configure(snd_pcm_t* pcm, unsigned int* rate, unsigned int channels) {
    snd_pcm_hw_params_t* params = nullptr;
    if (snd_pcm_hw_params_malloc(&params) < 0) {
        return false;
    }
    bool ok = snd_pcm_hw_params_any(pcm, params) >= 0 &&
              snd_pcm_hw_params_set_access(pcm, params, LEGATO_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
              snd_pcm_hw_params_set_format(pcm, params, LEGATO_PCM_FORMAT_S16_LE) >= 0 &&
              snd_pcm_hw_params_set_channels(pcm, params, channels) >= 0;
    int dir = 0;
    ok = ok && snd_pcm_hw_params_set_rate_near(pcm, params, rate, &dir) >= 0;
    ok = ok && snd_pcm_hw_params(pcm, params) >= 0;
    snd_pcm_hw_params_free(params);
    return ok && snd_pcm_prepare(pcm) >= 0;
}

}  /* namespace */

AlsaSink::~AlsaSink() { close(); }

int AlsaSink::open(int sample_rate, int channels) {
    close();
    if (sample_rate <= 0 || channels <= 0) {
        return -1;
    }
    const char* devices[] = {"default", "plughw:0,0", "hw:0,0"};
    unsigned int rate = static_cast<unsigned int>(sample_rate);
    const unsigned int want_channels = static_cast<unsigned int>(channels);
    for (const char* device : devices) {
        snd_pcm_t* pcm = nullptr;
        if (snd_pcm_open(&pcm, device, LEGATO_PCM_STREAM_PLAYBACK, 0) < 0) {
            continue;
        }
        unsigned int opened_rate = rate;
        if (!configure(pcm, &opened_rate, want_channels)) {
            snd_pcm_close(pcm);
            continue;
        }
        pcm_ = pcm;
        rate_ = static_cast<int>(opened_rate);
        channels_ = channels;
        interrupted_.store(false);
        log_write(LogLevel::Info, "alsa %s %d Hz %d ch", device, rate_, channels_);
        return 0;
    }
    log_write(LogLevel::Error, "alsa open failed");
    return -1;
}

void AlsaSink::present(std::int64_t pts_us) { (void)pts_us; }

void AlsaSink::close() {
    if (pcm_ != nullptr) {
        snd_pcm_close(static_cast<snd_pcm_t*>(pcm_));
        pcm_ = nullptr;
    }
}

int AlsaSink::write_frames(const std::int16_t* interleaved, int frames) {
    if (pcm_ == nullptr || interleaved == nullptr || frames <= 0) {
        return -1;
    }
    auto* pcm = static_cast<snd_pcm_t*>(pcm_);
    /* 暂停和 seek 从控制线程调用 drop，用来打断阻塞中的 writei。
       欠载也返回负数，但那种情况要 recover。两者都像 EPIPE，靠 interrupted_ 区分。 */
    if (interrupted_.load()) {
        return 0;
    }
    int left = frames;
    const std::int16_t* cursor = interleaved;
    while (left > 0) {
        const snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, cursor, static_cast<snd_pcm_uframes_t>(left));
        if (wrote < 0) {
            if (interrupted_.load()) {
                return 0;
            }
            if (snd_pcm_recover(pcm, static_cast<int>(wrote), 1) < 0) {
                return -1;
            }
            continue;
        }
        left -= static_cast<int>(wrote);
        cursor += static_cast<int>(wrote) * channels_;
    }
    return frames;
}

void AlsaSink::drop() {
    interrupted_.store(true);
    if (pcm_ != nullptr) {
        snd_pcm_drop(static_cast<snd_pcm_t*>(pcm_));
        snd_pcm_prepare(static_cast<snd_pcm_t*>(pcm_));
    }
}

void AlsaSink::end_interrupt() { interrupted_.store(false); }

void AlsaSink::drain() {
    if (pcm_ != nullptr) {
        snd_pcm_drain(static_cast<snd_pcm_t*>(pcm_));
        snd_pcm_prepare(static_cast<snd_pcm_t*>(pcm_));
    }
}

int AlsaSink::delay_frames() const {
    if (pcm_ == nullptr) {
        return 0;
    }
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(static_cast<snd_pcm_t*>(pcm_), &delay) < 0 || delay < 0) {
        return 0;
    }
    return static_cast<int>(delay);
}

}  /* namespace legato */
