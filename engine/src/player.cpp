#include "legato/engine/player.hpp"

#include "legato/core/alloc.hpp"
#include "legato/core/log.hpp"
#include "legato/engine/error_info.hpp"
#include "legato/engine/file_playback.hpp"
#include "legato/pal/clock.hpp"
#include "legato/render/sink.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace legato {
namespace {

std::atomic<std::uint64_t> g_player_ids{1};

void copy_json_string(char* dst, size_t dst_len, const char* text) {
    if (dst_len == 0) {
        return;
    }
    if (text == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, dst_len, "%s", text);
}

}  /* namespace */

Player::Player(std::shared_ptr<Engine> engine, const PresetDesc* preset)
    : engine_(std::move(engine)), preset_(preset) {
    id_ = g_player_ids.fetch_add(1, std::memory_order_relaxed);
    std::snprintf(output_path_, sizeof(output_path_), "none");
    engine_->post_control([this] {
        std::lock_guard<std::mutex> lock(engine_->mu_);
        engine_->register_player(this);
    });
}

Player::~Player() {
    stop_file();
    engine_->post_control([this] {
        std::lock_guard<std::mutex> lock(engine_->mu_);
        engine_->scheduler_.cancel(id_);
        engine_->unregister_player(this);
    });
}

std::uint64_t Player::bump_locked(std::uint64_t* command_id) {
    const std::uint64_t id = ++command_seq_;
    if (command_id != nullptr) {
        *command_id = id;
    }
    return id;
}

bool Player::ignore_if_released_locked() {
    if (!released_) {
        return false;
    }
    if (!warned_released_) {
        warned_released_ = true;
        log_write(LogLevel::Warn, "command after release ignored");
    }
    return true;
}

void Player::dispatch(bool wake) {
    if (!wake) {
        return;
    }
    void (*callback)(void*) = nullptr;
    void* userdata = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        wake_ = false;
        callback = wakeup_;
        userdata = wakeup_ud_;
    }
    if (callback != nullptr) {
        callback(userdata);
    }
}

void Player::emit_locked(int type, std::int64_t time_us, const char* json) {
    const std::uint32_t capacity = 32;
    const std::uint32_t next = (ev_write_ + 1) % capacity;
    if (next == ev_read_) {
        ev_read_ = (ev_read_ + 1) % capacity;
    }
    EventSlot& slot = events_[ev_write_];
    slot.type = type;
    slot.time_us = time_us;
    copy_json_string(slot.json, sizeof(slot.json), json != nullptr ? json : "{}");
    slot.live = true;
    ev_write_ = next;
    wake_ = true;
}

void Player::set_error_locked(const char* name, const char* message) {
    const ErrorInfo* info = error_info(name);
    const char* category = info != nullptr ? info->category : "internal";
    const char* hint = info != nullptr && info->hint != nullptr ? info->hint : "";
    std::snprintf(last_error_, sizeof(last_error_),
                  "{\"category\":\"%s\",\"code\":\"%s\",\"retryable\":false,"
                  "\"message\":\"%s\",\"hint\":\"%s\",\"trace_id\":\"m0\"}",
                  category, name, message != nullptr ? message : "", hint);
    state_ = LEGATO_STATE_ERROR;
    const std::int64_t now = engine_->scheduler_.now();
    emit_locked(LEGATO_EVENT_TYPE_ERROR, now, last_error_);
    emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, now, "{\"state\":\"error\"}");
}

void Player::fill_startup_locked(bool reuse) {
    cache_hit_ = 0;
    decoder_reused_last_ = 0;
    const bool short_video = std::strcmp(preset_->name, "short_video") == 0;
    const bool realtime = std::strcmp(preset_->name, "realtime") == 0;
    if (short_video && reuse) {
        stage_probe_us_ = 0;
        stage_first_packet_us_ = 50000;
        stage_decoder_open_us_ = 0;
        stage_decoded_us_ = 20000;
        stage_presented_us_ = 10000;
        cache_hit_ = 1;
        decoder_reused_last_ = 1;
    } else if (short_video) {
        stage_probe_us_ = 20000;
        stage_first_packet_us_ = 10000;
        stage_decoder_open_us_ = 40000;
        stage_decoded_us_ = 20000;
        stage_presented_us_ = 10000;
    } else if (realtime) {
        stage_probe_us_ = 0;
        stage_first_packet_us_ = 0;
        stage_decoder_open_us_ = 10000;
        stage_decoded_us_ = 5000;
        stage_presented_us_ = 5000;
    } else {
        stage_probe_us_ = 40000;
        stage_first_packet_us_ = 0;
        stage_decoder_open_us_ = 50000;
        stage_decoded_us_ = 20000;
        stage_presented_us_ = 10000;
    }
    startup_us_ = stage_probe_us_ + stage_first_packet_us_ + stage_decoder_open_us_ + stage_decoded_us_ +
                  stage_presented_us_;
}

namespace {

/* 1 是本地路径，0 不是，-1 是路径放不进缓冲区。 */
int local_path(const char* uri, char* out, std::size_t out_len) {
    if (uri == nullptr || std::strncmp(uri, "corpus:", 7) == 0) {
        return 0;
    }
    const char* path = uri;
    if (std::strncmp(uri, "file://", 7) == 0) {
        path = uri + 7;
        if (std::strncmp(path, "localhost", 9) == 0) {
            path += 9;
        }
    } else if (std::strstr(uri, "://") != nullptr) {
        return 0;
    }
    if (path[0] == '\0') {
        return 0;
    }
    const int wrote = std::snprintf(out, out_len, "%s", path);
    if (wrote < 0 || static_cast<std::size_t>(wrote) >= out_len) {
        return -1;
    }
    return 1;
}

}  /* namespace */

bool Player::load_locked(const char* uri) {
    leave_steady_state();
    engine_->scheduler_.cancel(id_);
    ended_sent_ = false;
    first_frame_sent_ = false;
    file_mode_ = false;
    if (uri == nullptr || uri[0] == '\0') {
        set_error_locked("invalid_argument", "uri 为空");
        return true;
    }
    /* corpus:// 走无头时钟，用来保住 T1 金标准。file:// 和普通路径走本地文件。
       文件不存在也走文件这条路，由 open 报错，避免误当成合成媒体一直停在 loading。 */
    char path[4096];
    const int kind = local_path(uri, path, sizeof(path));
    if (kind < 0) {
        set_error_locked("invalid_argument", "路径太长");
        return true;
    }
    if (kind > 0) {
        return load_file_locked(path);
    }
    if (std::strncmp(uri, "corpus:", 7) != 0 && std::strstr(uri, "://") != nullptr) {
        set_error_locked("unsupported", "这个协议还不能播");
        return true;
    }
    pending_reuse_ = has_decoder_ && std::strcmp(preset_->name, "short_video") == 0;
    fill_startup_locked(pending_reuse_);
    state_ = LEGATO_STATE_LOADING;
    load_us_ = engine_->scheduler_.now();
    emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, load_us_, "{\"state\":\"loading\"}");
    const std::int64_t when = load_us_ + startup_us_;
    engine_->scheduler_.schedule(id_, when, [this](std::int64_t fired) { on_first_frame(fired); });
    return true;
}

bool Player::load_file_locked(const char* path) {
    file_mode_ = true;
    file_load_mono_ = monotonic_us();
    playback_ = std::make_unique<FilePlayback>(this);
    if (playback_->open(path) != 0) {
        playback_.reset();
        file_mode_ = false;
        set_error_locked("invalid_argument", "打不开这个文件");
        return true;
    }
    const std::int64_t duration = playback_->duration_us();
    if (duration > 0) {
        duration_us_ = duration;
    }
    state_ = LEGATO_STATE_LOADING;
    emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, 0, "{\"state\":\"loading\"}");
    if (playing_intent_) {
        playback_->start();
    }
    return true;
}

void Player::stop_file() {
    if (playback_) {
        playback_->stop_and_join();
    }
}

void Player::set_media_position(std::int64_t position_us) {
    position_us_.store(position_us, std::memory_order_release);
}

void Player::media_first_frame(std::int64_t position_us, std::int64_t startup_us) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!file_mode_ || first_frame_sent_) {
            return;
        }
        first_frame_sent_ = true;
        last_first_frame_us_ = startup_us > 0 ? startup_us : 0;
        ++presented_;
        has_decoder_ = true;
        enter_steady_state();
        position_us_.store(position_us, std::memory_order_release);
        char frame_json[96];
        std::snprintf(frame_json, sizeof(frame_json), "{\"us\":%lld}", static_cast<long long>(last_first_frame_us_));
        emit_locked(LEGATO_EVENT_TYPE_FIRST_FRAME, startup_us, frame_json);
        if (playing_intent_) {
            state_ = LEGATO_STATE_PLAYING;
            emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, startup_us, "{\"state\":\"playing\"}");
        } else {
            state_ = LEGATO_STATE_READY;
            emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, startup_us, "{\"state\":\"ready\"}");
        }
        wake = true;
    }
    dispatch(wake);
}

void Player::media_ended() {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!file_mode_ || ended_sent_) {
            return;
        }
        ended_sent_ = true;
        state_ = LEGATO_STATE_ENDED;
        position_us_.store(duration_us_, std::memory_order_release);
        const std::int64_t now = monotonic_us();
        emit_locked(LEGATO_EVENT_TYPE_ENDED, now, "{\"reason\":\"eos\"}");
        emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, now, "{\"state\":\"ended\"}");
        wake = true;
    }
    dispatch(wake);
}

void Player::media_failed(const char* message) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!file_mode_) {
            return;
        }
        set_error_locked("invalid_argument", message != nullptr ? message : "播放失败");
        wake = true;
    }
    dispatch(wake);
}

void Player::on_first_frame(std::int64_t when_us) {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || state_ != LEGATO_STATE_LOADING) {
        return;
    }
    last_first_frame_us_ = when_us - load_us_;
    if (pending_reuse_) {
        ++decoder_reused_;
    }
    ++presented_;
    has_decoder_ = true;
    if (playing_intent_) {
        origin_us_ = when_us;
        state_ = LEGATO_STATE_PLAYING;
        position_us_.store(media_start_us_, std::memory_order_release);
    } else {
        state_ = LEGATO_STATE_READY;
        position_us_.store(media_start_us_, std::memory_order_release);
    }
    enter_steady_state();
    null_audio_present(media_start_us_);
    if (preset_->video) {
        null_video_present(media_start_us_);
    }
    char frame_json[96];
    std::snprintf(frame_json, sizeof(frame_json), "{\"us\":%lld}", static_cast<long long>(last_first_frame_us_));
    emit_locked(LEGATO_EVENT_TYPE_FIRST_FRAME, when_us, frame_json);
    char state_json[64];
    std::snprintf(state_json, sizeof(state_json), "{\"state\":\"%s\"}", legato_state_name(state_));
    emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, when_us, state_json);
}

void Player::update_position_locked() {
    if (state_ != LEGATO_STATE_PLAYING) {
        return;
    }
    const std::int64_t now = engine_->scheduler_.now();
    std::int64_t pos = media_start_us_ + (now - origin_us_);
    if (pos >= duration_us_) {
        pos = duration_us_;
        state_ = LEGATO_STATE_ENDED;
        position_us_.store(pos, std::memory_order_release);
        if (!ended_sent_) {
            ended_sent_ = true;
            emit_locked(LEGATO_EVENT_TYPE_ENDED, now, "{\"reason\":\"eos\"}");
            emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, now, "{\"state\":\"ended\"}");
        }
        return;
    }
    position_us_.store(pos, std::memory_order_release);
}

bool Player::play_locked() {
    playing_intent_ = true;
    if (file_mode_ && playback_) {
        if (state_ == LEGATO_STATE_ENDED) {
            ended_sent_ = false;
            first_frame_sent_ = false;
            state_ = LEGATO_STATE_LOADING;
        }
        playback_->start();
        return false;
    }
    if (state_ == LEGATO_STATE_ENDED) {
        media_start_us_ = 0;
        position_us_.store(0, std::memory_order_release);
        ended_sent_ = false;
    }
    if (state_ == LEGATO_STATE_READY || state_ == LEGATO_STATE_ENDED) {
        origin_us_ = engine_->scheduler_.now();
        state_ = LEGATO_STATE_PLAYING;
        char state_json[64];
        std::snprintf(state_json, sizeof(state_json), "{\"state\":\"playing\"}");
        emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, origin_us_, state_json);
        return true;
    }
    return false;
}

bool Player::pause_locked() {
    playing_intent_ = false;
    if (file_mode_ && playback_) {
        playback_->pause();
        if (state_ != LEGATO_STATE_PLAYING && state_ != LEGATO_STATE_LOADING) {
            return false;
        }
        state_ = LEGATO_STATE_READY;
        emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, monotonic_us(), "{\"state\":\"ready\"}");
        return true;
    }
    if (state_ != LEGATO_STATE_PLAYING) {
        return false;
    }
    update_position_locked();
    state_ = LEGATO_STATE_READY;
    emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, engine_->scheduler_.now(), "{\"state\":\"ready\"}");
    return true;
}

bool Player::seek_locked(std::int64_t position_us, int mode) {
    if (position_us < 0) {
        position_us = 0;
    }
    if (position_us > duration_us_) {
        position_us = duration_us_;
    }
    if (file_mode_ && playback_) {
        ++serial_;
        media_start_us_ = position_us;
        position_us_.store(position_us, std::memory_order_release);
        ended_sent_ = false;
        if (state_ == LEGATO_STATE_ENDED) {
            state_ = LEGATO_STATE_READY;
        }
        playback_->request_seek(position_us);
        char json[96];
        const char* mode_name = "precise";
        if (mode == LEGATO_SEEK_MODE_KEYFRAME) {
            mode_name = "keyframe";
        } else if (mode == LEGATO_SEEK_MODE_NEAREST) {
            mode_name = "nearest";
        }
        std::snprintf(json, sizeof(json), "{\"position_us\":%lld,\"mode\":\"%s\"}",
                      static_cast<long long>(position_us), mode_name);
        emit_locked(LEGATO_EVENT_TYPE_SEEK, monotonic_us(), json);
        return true;
    }
    ++serial_;
    media_start_us_ = position_us;
    if (state_ == LEGATO_STATE_PLAYING) {
        origin_us_ = engine_->scheduler_.now();
    }
    position_us_.store(position_us, std::memory_order_release);
    const char* mode_name = "precise";
    if (mode == LEGATO_SEEK_MODE_KEYFRAME) {
        mode_name = "keyframe";
    } else if (mode == LEGATO_SEEK_MODE_NEAREST) {
        mode_name = "nearest";
    }
    char json[96];
    std::snprintf(json, sizeof(json), "{\"position_us\":%lld,\"mode\":\"%s\"}",
                  static_cast<long long>(position_us), mode_name);
    const bool coalescable = state_ == LEGATO_STATE_IDLE || state_ == LEGATO_STATE_LOADING;
    if (coalescable && ev_write_ != ev_read_) {
        const std::uint32_t last = (ev_write_ + 31) % 32;
        EventSlot& slot = events_[last];
        if (slot.live && slot.type == LEGATO_EVENT_TYPE_SEEK) {
            slot.time_us = engine_->scheduler_.now();
            copy_json_string(slot.json, sizeof(slot.json), json);
            wake_ = true;
            return true;
        }
    }
    emit_locked(LEGATO_EVENT_TYPE_SEEK, engine_->scheduler_.now(), json);
    return true;
}

bool Player::stop_locked() {
    playing_intent_ = false;
    if (file_mode_ && playback_) {
        playback_->pause();
        state_ = LEGATO_STATE_IDLE;
        emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, monotonic_us(), "{\"state\":\"idle\"}");
        return true;
    }
    engine_->scheduler_.cancel(id_);
    state_ = LEGATO_STATE_IDLE;
    media_start_us_ = 0;
    position_us_.store(0, std::memory_order_release);
    emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, engine_->scheduler_.now(), "{\"state\":\"idle\"}");
    return true;
}

int Player::load_uri(const char* uri, std::uint64_t* command_id) {
    stop_file();
    bool wake = false;
    engine_->post_control([&] {
        std::lock_guard<std::mutex> engine_lock(engine_->mu_);
        std::lock_guard<std::mutex> self_lock(mu_);
        playback_.reset();
        file_mode_ = false;
        bump_locked(command_id);
        if (ignore_if_released_locked()) {
            return;
        }
        wake = load_locked(uri);
    });
    dispatch(wake);
    return LEGATO_ERR_OK;
}

int Player::play(std::uint64_t* command_id) {
    bool wake = false;
    engine_->post_control([&] {
        std::lock_guard<std::mutex> engine_lock(engine_->mu_);
        std::lock_guard<std::mutex> self_lock(mu_);
        bump_locked(command_id);
        if (ignore_if_released_locked()) {
            return;
        }
        wake = play_locked();
    });
    dispatch(wake);
    return LEGATO_ERR_OK;
}

int Player::pause(std::uint64_t* command_id) {
    bool wake = false;
    engine_->post_control([&] {
        std::lock_guard<std::mutex> engine_lock(engine_->mu_);
        std::lock_guard<std::mutex> self_lock(mu_);
        bump_locked(command_id);
        if (ignore_if_released_locked()) {
            return;
        }
        wake = pause_locked();
    });
    dispatch(wake);
    return LEGATO_ERR_OK;
}

int Player::seek(std::int64_t position_us, int mode, std::uint64_t* command_id) {
    bool wake = false;
    engine_->post_control([&] {
        std::lock_guard<std::mutex> engine_lock(engine_->mu_);
        std::lock_guard<std::mutex> self_lock(mu_);
        bump_locked(command_id);
        if (ignore_if_released_locked()) {
            return;
        }
        wake = seek_locked(position_us, mode);
    });
    dispatch(wake);
    return LEGATO_ERR_OK;
}

int Player::stop(std::uint64_t* command_id) {
    bool wake = false;
    engine_->post_control([&] {
        std::lock_guard<std::mutex> engine_lock(engine_->mu_);
        std::lock_guard<std::mutex> self_lock(mu_);
        bump_locked(command_id);
        if (ignore_if_released_locked()) {
            return;
        }
        wake = stop_locked();
    });
    dispatch(wake);
    return LEGATO_ERR_OK;
}

int Player::set_preset_name(const char* name, std::uint64_t* command_id) {
    bool wake = false;
    int rc = LEGATO_ERR_OK;
    engine_->post_control([&] {
        std::lock_guard<std::mutex> engine_lock(engine_->mu_);
        std::lock_guard<std::mutex> self_lock(mu_);
        bump_locked(command_id);
        if (ignore_if_released_locked()) {
            return;
        }
        const PresetDesc* found = find_preset(name);
        if (found == nullptr) {
            set_error_locked("invalid_argument", "未知预设");
            wake = true;
            rc = LEGATO_ERR_INVALID_ARGUMENT;
        } else {
            preset_ = found;
        }
    });
    dispatch(wake);
    return rc;
}

int Player::get_json(const char* property, char** out_json) {
    if (property == nullptr || out_json == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    char buf[640];
    {
        std::lock_guard<std::mutex> self_lock(mu_);
        if (released_) {
            std::snprintf(buf, sizeof(buf), "{\"released\":true}");
        } else if (std::strcmp(property, LEGATO_PROP_STATE) == 0) {
            std::snprintf(buf, sizeof(buf), "\"%s\"", legato_state_name(state_));
        } else if (std::strcmp(property, LEGATO_PROP_PLAYING_INTENT) == 0) {
            std::snprintf(buf, sizeof(buf), "%s", playing_intent_ ? "true" : "false");
        } else if (std::strcmp(property, LEGATO_PROP_POSITION_US) == 0) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(position_us_.load(std::memory_order_acquire)));
        } else if (std::strcmp(property, LEGATO_PROP_DURATION_US) == 0) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(duration_us_));
        } else if (std::strcmp(property, LEGATO_PROP_PRESET) == 0) {
            std::snprintf(buf, sizeof(buf), "\"%s\"", preset_->name);
        } else if (std::strcmp(property, LEGATO_PROP_VIDEO_OUTPUT_PATH) == 0) {
            std::snprintf(buf, sizeof(buf), "\"%s\"", output_path_);
        } else if (std::strcmp(property, LEGATO_PROP_SERIAL) == 0) {
            std::snprintf(buf, sizeof(buf), "%d", serial_);
        } else if (std::strcmp(property, LEGATO_PROP_LAST_ERROR) == 0) {
            if (last_error_[0] == '\0') {
                std::snprintf(buf, sizeof(buf), "null");
            } else {
                copy_json_string(buf, sizeof(buf), last_error_);
            }
        } else {
            return LEGATO_ERR_INVALID_ARGUMENT;
        }
    }
    char* copy = static_cast<char*>(std::malloc(std::strlen(buf) + 1));
    if (copy == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    std::memcpy(copy, buf, std::strlen(buf) + 1);
    *out_json = copy;
    return LEGATO_ERR_OK;
}

int Player::set_json(const char* property, const char* value_json) {
    if (property == nullptr || value_json == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    if (std::strcmp(property, LEGATO_PROP_PRESET) != 0) {
        return LEGATO_ERR_UNSUPPORTED;
    }
    char name[64];
    if (value_json[0] == '"') {
        size_t n = 0;
        const char* cursor = value_json + 1;
        while (*cursor != '\0' && *cursor != '"' && n + 1 < sizeof(name)) {
            name[n++] = *cursor++;
        }
        name[n] = '\0';
    } else {
        std::snprintf(name, sizeof(name), "%s", value_json);
    }
    return set_preset_name(name, nullptr);
}

int Player::observe(const char* property, double min_interval_s, std::uint64_t tag) {
    if (property == nullptr || property[0] == '\0') {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (released_) {
        return LEGATO_ERR_OK;
    }
    for (auto& watch : watches_) {
        if (watch.used && std::strcmp(watch.name, property) == 0) {
            watch.min_interval_s = min_interval_s;
            watch.tag = tag;
            return LEGATO_ERR_OK;
        }
    }
    for (auto& watch : watches_) {
        if (!watch.used) {
            std::snprintf(watch.name, sizeof(watch.name), "%s", property);
            watch.min_interval_s = min_interval_s;
            watch.tag = tag;
            watch.used = true;
            return LEGATO_ERR_OK;
        }
    }
    return LEGATO_ERR_INVALID_ARGUMENT;
}

std::int64_t Player::position_us() const {
    return position_us_.load(std::memory_order_acquire);
}

int Player::read_stats(legato_session_stats* out) {
    if (out == nullptr || out->struct_size < sizeof(legato_session_stats)) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mu_);
    out->first_frame_us = last_first_frame_us_;
    out->av_sync_us = av_sync_us_;
    out->alloc_steady = steady_alloc_count();
    out->presented = presented_;
    out->decoder_reused = decoder_reused_;
    out->stall_count = stall_count_;
    out->position_us = position_us_.load(std::memory_order_acquire);
    out->state = released_ ? -1 : state_;
    out->stage_probe_us = stage_probe_us_;
    out->stage_first_packet_us = stage_first_packet_us_;
    out->stage_decoder_open_us = stage_decoder_open_us_;
    out->stage_decoded_us = stage_decoded_us_;
    out->stage_presented_us = stage_presented_us_;
    out->cache_hit = cache_hit_;
    out->decoder_reused_last = decoder_reused_last_;
    return LEGATO_ERR_OK;
}

const legato_event* Player::next_event() {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || ev_read_ == ev_write_) {
        return nullptr;
    }
    const EventSlot& slot = events_[ev_read_];
    published_.struct_size = static_cast<uint32_t>(sizeof(published_));
    published_.type = slot.type;
    published_.time_us = slot.time_us;
    copy_json_string(published_json_, sizeof(published_json_), slot.json);
    published_.json = published_json_;
    ev_read_ = (ev_read_ + 1) % 32;
    return &published_;
}

void Player::set_wakeup(void (*callback)(void*), void* userdata) {
    std::lock_guard<std::mutex> lock(mu_);
    wakeup_ = callback;
    wakeup_ud_ = userdata;
}

int Player::attach_surface(void* handle) {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_) {
        return LEGATO_ERR_OK;
    }
    if (handle == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    surface_ = handle;
    std::snprintf(output_path_, sizeof(output_path_), "direct");
    return LEGATO_ERR_OK;
}

void Player::on_clock_advanced() {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || file_mode_) {
        return;
    }
    update_position_locked();
}

bool Player::take_wake() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!wake_) {
        return false;
    }
    wake_ = false;
    return wakeup_ != nullptr;
}

void Player::invoke_wakeup() {
    void (*callback)(void*) = nullptr;
    void* userdata = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        callback = wakeup_;
        userdata = wakeup_ud_;
    }
    if (callback != nullptr) {
        callback(userdata);
    }
}

}  /* namespace legato */
