#include "legato/render/pipewire_sink.hpp"

#include "legato/core/log.hpp"
#include "legato/engine/test_hooks.hpp"
#include "legato/pal/clock.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <vector>

namespace legato {
namespace {

constexpr std::uint32_t kSpaTypeId = 3;
constexpr std::uint32_t kSpaTypeInt = 4;
constexpr std::uint32_t kSpaTypeObject = 15;
constexpr std::uint32_t kSpaObjectFormat = 0x40003;
constexpr std::uint32_t kSpaParamEnumFormat = 3;
constexpr std::uint32_t kSpaParamFormat = 4;
constexpr std::uint32_t kSpaFormatMediaType = 1;
constexpr std::uint32_t kSpaFormatMediaSubtype = 2;
constexpr std::uint32_t kSpaFormatAudioFormat = 0x10001;
constexpr std::uint32_t kSpaFormatAudioRate = 0x10003;
constexpr std::uint32_t kSpaFormatAudioChannels = 0x10004;
constexpr std::uint32_t kSpaAudioS16Le = 0x103;
constexpr std::uint32_t kMediaAudio = 1;
constexpr std::uint32_t kMediaRaw = 1;
constexpr int kDirectionOutput = 1;
constexpr std::uint32_t kIdAny = 0xffffffffu;
constexpr int kFlagAutoconnect = 1;
constexpr int kFlagMapBuffers = 4;
constexpr int kFlagRtProcess = 16;
constexpr int kStatePaused = 2;
constexpr int kStateStreaming = 3;

struct SpaChunk {
    std::uint32_t offset;
    std::uint32_t size;
    std::int32_t stride;
    std::int32_t flags;
};

struct SpaData {
    std::uint32_t type;
    std::uint32_t flags;
    std::int64_t fd;
    std::uint32_t mapoffset;
    std::uint32_t maxsize;
    void* data;
    SpaChunk* chunk;
};

struct SpaBuffer {
    std::uint32_t n_metas;
    std::uint32_t n_datas;
    void* metas;
    SpaData* datas;
};

struct PwBuffer {
    SpaBuffer* buffer;
    void* user_data;
    std::uint64_t size;
};

struct PwTime {
    std::int64_t now;
    std::uint32_t rate_num;
    std::uint32_t rate_denom;
    std::uint64_t ticks;
    std::int64_t delay;
    std::uint64_t queued;
};

/* version 后面是函数指针。64 位上 uint32 之后要补 4 字节，否则回调会错位。 */
struct StreamEvents {
    std::uint32_t version;
    std::uint32_t pad;
    void (*destroy)(void*);
    void (*state_changed)(void*, int, int, const char*);
    void (*control_info)(void*, std::uint32_t, const void*);
    void (*io_changed)(void*, std::uint32_t, void*, std::uint32_t);
    void (*param_changed)(void*, std::uint32_t, const void*);
    void (*add_buffer)(void*, void*);
    void (*remove_buffer)(void*, void*);
    void (*process)(void*);
    void (*drained)(void*);
};

struct Api {
    void* handle = nullptr;
    bool ready = false;
    void (*init)(int*, char***) = nullptr;
    void* (*loop_new)(const char*, const void*) = nullptr;
    void (*loop_destroy)(void*) = nullptr;
    int (*loop_start)(void*) = nullptr;
    void (*loop_stop)(void*) = nullptr;
    void (*loop_lock)(void*) = nullptr;
    void (*loop_unlock)(void*) = nullptr;
    void* (*loop_get)(void*) = nullptr;
    void* (*context_new)(void*, void*, std::size_t) = nullptr;
    void (*context_destroy)(void*) = nullptr;
    void* (*context_connect)(void*, void*, std::size_t) = nullptr;
    int (*core_disconnect)(void*) = nullptr;
    void* (*props_new)(const char*, ...) = nullptr;
    void* (*stream_new)(void*, const char*, void*, const StreamEvents*, void*) = nullptr;
    void (*stream_destroy)(void*) = nullptr;
    int (*stream_connect)(void*, int, std::uint32_t, int, const void**, std::uint32_t) = nullptr;
    int (*stream_disconnect)(void*) = nullptr;
    int (*stream_get_state)(void*, const char**) = nullptr;
    void* (*dequeue)(void*) = nullptr;
    int (*queue)(void*, void*) = nullptr;
    int (*get_time)(void*, PwTime*) = nullptr;
    int (*update_params)(void*, const void**, std::uint32_t) = nullptr;
    int (*flush)(void*, bool) = nullptr;
};

template <typename T>
bool load_sym(void* handle, const char* name, T* out) {
    void* symbol = dlsym(handle, name);
    if (symbol == nullptr) {
        return false;
    }
    *out = reinterpret_cast<T>(symbol);
    return true;
}

Api& api() {
    static Api lib;
    if (lib.handle != nullptr || lib.ready) {
        return lib;
    }
    lib.handle = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (lib.handle == nullptr) {
        return lib;
    }
    lib.ready = load_sym(lib.handle, "pw_init", &lib.init) && load_sym(lib.handle, "pw_thread_loop_new", &lib.loop_new) &&
                load_sym(lib.handle, "pw_thread_loop_destroy", &lib.loop_destroy) &&
                load_sym(lib.handle, "pw_thread_loop_start", &lib.loop_start) &&
                load_sym(lib.handle, "pw_thread_loop_stop", &lib.loop_stop) &&
                load_sym(lib.handle, "pw_thread_loop_lock", &lib.loop_lock) &&
                load_sym(lib.handle, "pw_thread_loop_unlock", &lib.loop_unlock) &&
                load_sym(lib.handle, "pw_thread_loop_get_loop", &lib.loop_get) &&
                load_sym(lib.handle, "pw_context_new", &lib.context_new) &&
                load_sym(lib.handle, "pw_context_destroy", &lib.context_destroy) &&
                load_sym(lib.handle, "pw_context_connect", &lib.context_connect) &&
                load_sym(lib.handle, "pw_core_disconnect", &lib.core_disconnect) &&
                load_sym(lib.handle, "pw_properties_new", &lib.props_new) &&
                load_sym(lib.handle, "pw_stream_new_simple", &lib.stream_new) &&
                load_sym(lib.handle, "pw_stream_destroy", &lib.stream_destroy) &&
                load_sym(lib.handle, "pw_stream_connect", &lib.stream_connect) &&
                load_sym(lib.handle, "pw_stream_disconnect", &lib.stream_disconnect) &&
                load_sym(lib.handle, "pw_stream_get_state", &lib.stream_get_state) &&
                load_sym(lib.handle, "pw_stream_dequeue_buffer", &lib.dequeue) &&
                load_sym(lib.handle, "pw_stream_queue_buffer", &lib.queue) &&
                load_sym(lib.handle, "pw_stream_get_time", &lib.get_time) &&
                load_sym(lib.handle, "pw_stream_update_params", &lib.update_params) &&
                load_sym(lib.handle, "pw_stream_flush", &lib.flush);
    if (lib.ready) {
        lib.init(nullptr, nullptr);
    }
    return lib;
}

void pw_on_process(void* data) {
    if (data != nullptr) {
        static_cast<PipeWireSink*>(data)->on_process();
    }
}

void pw_on_param(void* data, std::uint32_t id, const void* /*param*/) {
    if (data != nullptr) {
        static_cast<PipeWireSink*>(data)->on_param(id);
    }
}

void put_prop(std::vector<std::uint32_t>* body, std::uint32_t key, std::uint32_t type, std::uint32_t value) {
    body->push_back(key);
    body->push_back(0);
    body->push_back(4);
    body->push_back(type);
    body->push_back(value);
    body->push_back(0);
}

std::vector<std::uint32_t> format_pod(int rate, int channels) {
    std::vector<std::uint32_t> body;
    body.push_back(kSpaObjectFormat);
    body.push_back(kSpaParamEnumFormat);
    put_prop(&body, kSpaFormatMediaType, kSpaTypeId, kMediaAudio);
    put_prop(&body, kSpaFormatMediaSubtype, kSpaTypeId, kMediaRaw);
    put_prop(&body, kSpaFormatAudioFormat, kSpaTypeId, kSpaAudioS16Le);
    put_prop(&body, kSpaFormatAudioRate, kSpaTypeInt, static_cast<std::uint32_t>(rate));
    put_prop(&body, kSpaFormatAudioChannels, kSpaTypeInt, static_cast<std::uint32_t>(channels));
    std::vector<std::uint32_t> pod;
    pod.push_back(static_cast<std::uint32_t>(body.size() * sizeof(std::uint32_t)));
    pod.push_back(kSpaTypeObject);
    pod.insert(pod.end(), body.begin(), body.end());
    return pod;
}

}  /* namespace */

PipeWireSink::~PipeWireSink() { close(); }

int PipeWireSink::open(int sample_rate, int channels) {
    close();
    if (test_no_audio_device() || sample_rate <= 0 || channels <= 0) {
        return -1;
    }
    Api& lib = api();
    if (!lib.ready) {
        return -1;
    }
    loop_ = lib.loop_new("legato", nullptr);
    if (loop_ == nullptr || lib.loop_start(loop_) != 0) {
        close();
        return -1;
    }
    started_ = true;
    context_ = lib.context_new(lib.loop_get(loop_), nullptr, 0);
    if (context_ == nullptr) {
        close();
        return -1;
    }
    core_ = lib.context_connect(context_, nullptr, 0);
    if (core_ == nullptr) {
        close();
        return -1;
    }
    void* props = lib.props_new("media.type", "Audio", "media.category", "Playback", "media.role", "Music",
                                static_cast<const char*>(nullptr));
    static StreamEvents events{};
    events.process = pw_on_process;
    events.param_changed = pw_on_param;
    stream_ = lib.stream_new(lib.loop_get(loop_), "legato", props, &events, this);
    if (stream_ == nullptr) {
        close();
        return -1;
    }
    std::vector<std::uint32_t> pod = format_pod(sample_rate, channels);
    const void* params[1] = {pod.data()};
    lib.loop_lock(loop_);
    const int connected = lib.stream_connect(stream_, kDirectionOutput, kIdAny,
                                             kFlagAutoconnect | kFlagMapBuffers | kFlagRtProcess, params, 1);
    lib.loop_unlock(loop_);
    if (connected != 0) {
        close();
        return -1;
    }
    /* 协商停在 CONNECTING 时，后面的写入会堵在环上。到不了暂停就放弃，改走 ALSA。 */
    bool negotiated = false;
    for (int spin = 0; spin < 50; ++spin) {
        const int state = lib.stream_get_state(stream_, nullptr);
        if (state < 0) {
            break;
        }
        if (state == kStatePaused || state == kStateStreaming) {
            negotiated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!negotiated) {
        close();
        return -1;
    }
    rate_ = sample_rate;
    channels_ = channels;
    ring_.assign(static_cast<std::size_t>(sample_rate) * static_cast<std::size_t>(channels), 0);
    read_ = 0;
    count_ = 0;
    interrupted_.store(false);
    dying_.store(false);
    log_write(LogLevel::Info, "pipewire %d Hz %d ch", rate_, channels_);
    return 0;
}

void PipeWireSink::present(std::int64_t /*pts_us*/) {}

void PipeWireSink::close() {
    dying_.store(true);
    interrupted_.store(true);
    ring_cv_.notify_all();
    Api& lib = api();
    if (lib.ready && loop_ != nullptr && started_) {
        lib.loop_lock(loop_);
        if (stream_ != nullptr) {
            lib.stream_disconnect(stream_);
        }
        lib.loop_unlock(loop_);
        lib.loop_stop(loop_);
        started_ = false;
    }
    if (lib.ready && stream_ != nullptr) {
        lib.stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (lib.ready && core_ != nullptr) {
        lib.core_disconnect(core_);
        core_ = nullptr;
    }
    if (lib.ready && context_ != nullptr) {
        lib.context_destroy(context_);
        context_ = nullptr;
    }
    if (lib.ready && loop_ != nullptr) {
        lib.loop_destroy(loop_);
        loop_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(ring_mu_);
    count_ = 0;
    read_ = 0;
}

int PipeWireSink::write_frames(const std::int16_t* interleaved, int frames) {
    if (stream_ == nullptr || interleaved == nullptr || frames <= 0 || channels_ <= 0) {
        return -1;
    }
    if (interrupted_.load()) {
        return 0;
    }
    const std::size_t need = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_);
    std::size_t stored = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    std::unique_lock<std::mutex> lock(ring_mu_);
    while (stored < need) {
        if (interrupted_.load() || dying_.load()) {
            return 0;
        }
        const std::size_t room = ring_.size() - count_;
        if (room == 0) {
            if (ring_cv_.wait_until(lock, deadline) == std::cv_status::timeout &&
                std::chrono::steady_clock::now() >= deadline) {
                return -1;
            }
            continue;
        }
        const std::size_t take = room < (need - stored) ? room : (need - stored);
        for (std::size_t i = 0; i < take; ++i) {
            const std::size_t index = (read_ + count_) % ring_.size();
            ring_[index] = interleaved[stored + i];
            ++count_;
        }
        stored += take;
    }
    return frames;
}

void PipeWireSink::drop() {
    interrupted_.store(true);
    {
        std::lock_guard<std::mutex> lock(ring_mu_);
        count_ = 0;
        read_ = 0;
    }
    ring_cv_.notify_all();
    if (stream_ != nullptr && api().ready && api().flush != nullptr) {
        api().flush(stream_, false);
    }
}

void PipeWireSink::end_interrupt() { interrupted_.store(false); }

void PipeWireSink::drain() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::unique_lock<std::mutex> lock(ring_mu_);
    while (count_ > 0 && !interrupted_.load() && !dying_.load()) {
        if (ring_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            break;
        }
    }
    lock.unlock();
    if (stream_ != nullptr && api().ready && api().flush != nullptr && !interrupted_.load()) {
        api().flush(stream_, true);
    }
}

int PipeWireSink::delay_frames() const { return delay_snapshot().frames; }

DelaySnapshot PipeWireSink::delay_snapshot() const {
    DelaySnapshot snap;
    std::size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(ring_mu_);
        queued = count_;
    }
    if (channels_ > 0) {
        snap.frames = static_cast<int>(queued / static_cast<std::size_t>(channels_));
    }
    if (stream_ == nullptr || !api().ready || api().get_time == nullptr) {
        return snap;
    }
    PwTime time{};
    if (api().get_time(stream_, &time) != 0) {
        return snap;
    }
    if (time.delay < 0) {
        snap.frames += static_cast<int>(-time.delay);
    }
    if (time.now > 0) {
        snap.tstamp_us = time.now / 1000;
        snap.have_tstamp = snap.tstamp_us > 0;
    }
    return snap;
}

void PipeWireSink::on_process() {
    if (dying_.load() || stream_ == nullptr) {
        return;
    }
    void* raw = api().dequeue(stream_);
    if (raw == nullptr) {
        return;
    }
    auto* buffer = static_cast<PwBuffer*>(raw);
    if (buffer->buffer == nullptr || buffer->buffer->n_datas < 1 || buffer->buffer->datas == nullptr) {
        api().queue(stream_, raw);
        return;
    }
    SpaData* data = &buffer->buffer->datas[0];
    if (data->data == nullptr || data->chunk == nullptr || channels_ <= 0) {
        api().queue(stream_, raw);
        return;
    }
    const int max_frames = static_cast<int>(data->maxsize / static_cast<std::uint32_t>(channels_ * 2));
    auto* dst = static_cast<std::int16_t*>(data->data);
    int got = 0;
    if (max_frames > 0) {
        std::lock_guard<std::mutex> lock(ring_mu_);
        const std::size_t samples = static_cast<std::size_t>(max_frames) * static_cast<std::size_t>(channels_);
        const std::size_t take = count_ < samples ? count_ : samples;
        for (std::size_t i = 0; i < take; ++i) {
            dst[i] = ring_[read_];
            read_ = (read_ + 1) % ring_.size();
        }
        count_ -= take;
        got = static_cast<int>(take / static_cast<std::size_t>(channels_));
        ring_cv_.notify_all();
    }
    data->chunk->offset = 0;
    data->chunk->stride = channels_ * 2;
    data->chunk->size = static_cast<std::uint32_t>(got * channels_ * 2);
    api().queue(stream_, raw);
}

void PipeWireSink::on_param(std::uint32_t id) {
    /* 格式谈妥之后要 update_params，否则流停在协商，process 不会来。 */
    if (id == kSpaParamFormat && stream_ != nullptr && api().ready) {
        api().update_params(stream_, nullptr, 0);
    }
}

}  /* namespace legato */
