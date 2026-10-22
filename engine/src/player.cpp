#include "legato/engine/player.hpp"

#include "legato/engine/uri_path.hpp"

#include "legato/core/alloc.hpp"
#include "legato/core/log.hpp"
#include "legato/engine/clock.hpp"
#include "legato/engine/corpus.hpp"
#include "legato/engine/error_info.hpp"
#include "legato/engine/file_playback.hpp"
#include "legato/engine/output_note.hpp"
#include "legato/engine/test_hooks.hpp"
#include "legato/pal/clock.hpp"
#include "legato/render/sink.hpp"
#if defined(LEGATO_HAS_ALSA)
#include "legato/render/alsa_sink.hpp"
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <utility>

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
    std::snprintf(trace_id_, sizeof(trace_id_), "p-%016llx", static_cast<unsigned long long>(id_));
}

void Player::register_self(const std::shared_ptr<Player>& self) {
    io_ = std::thread([this] { io_main(); });
    /* 登记只碰引擎名单，不经过 post_and_wait。post_and_wait 只留给虚拟媒体时钟推进。 */
    std::lock_guard<std::mutex> lock(engine_->mu_);
    engine_->register_player(self);
}

/* 调用线程只投递回收。join IO 线程和解码线程在回收线程上完成。 */
void Player::request_release() {
    std::shared_ptr<Player> self = shared_from_this();
    engine_->post([self] {
        std::lock_guard<std::mutex> lock(self->mu_);
        self->released_ = true;
    });
    engine_->post_reclaim([self] {
        self->io_stop_.store(true);
        self->io_cv_.notify_all();
        if (self->io_.joinable()) {
            self->io_.join();
        }
        /* playback_ 可能正被控制线程的 finish_open_job 换掉，先在锁里取出再停。 */
        std::shared_ptr<FilePlayback> playback;
        {
            std::lock_guard<std::mutex> lock(self->mu_);
            playback = self->playback_;
        }
        if (playback) {
            playback->stop();
        }
        std::lock_guard<std::mutex> lock(self->engine_->mu_);
        self->engine_->scheduler_.cancel(self->id_);
        self->engine_->unregister_player(self.get());
    });
}

Player::~Player() {
    io_stop_.store(true);
    io_cv_.notify_all();
    if (io_.joinable()) {
        io_.join();
    }
    if (playback_) {
        playback_->stop();
    }
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

bool Player::protected_event(int type) const {
    return type == LEGATO_EVENT_TYPE_ERROR || type == LEGATO_EVENT_TYPE_ENDED || type == LEGATO_EVENT_TYPE_FIRST_FRAME ||
           type == LEGATO_EVENT_TYPE_STATE_CHANGED;
}

bool Player::same_merge(const EventSlot& slot, int type, const char* json) const {
    if (!slot.live || slot.type != type) {
        return false;
    }
    if (type == LEGATO_EVENT_TYPE_SEEK) {
        return true;
    }
    /* 位置通过 property_changed 发出。同一属性只留最新一条，避免挤掉终态事件。 */
    if (type != LEGATO_EVENT_TYPE_PROPERTY_CHANGED || json == nullptr) {
        return false;
    }
    const char* key = "\"property\":\"";
    const char* left = std::strstr(slot.json, key);
    const char* right = std::strstr(json, key);
    if (left == nullptr || right == nullptr) {
        return false;
    }
    left += std::strlen(key);
    right += std::strlen(key);
    const char* left_end = std::strchr(left, '"');
    const char* right_end = std::strchr(right, '"');
    if (left_end == nullptr || right_end == nullptr) {
        return false;
    }
    const std::size_t left_len = static_cast<std::size_t>(left_end - left);
    const std::size_t right_len = static_cast<std::size_t>(right_end - right);
    return left_len == right_len && std::memcmp(left, right, left_len) == 0;
}

void Player::emit_locked(int type, std::int64_t time_us, const char* json) {
    if (type == LEGATO_EVENT_TYPE_SEEK || type == LEGATO_EVENT_TYPE_PROPERTY_CHANGED) {
        std::uint32_t index = ev_read_;
        while (index != ev_write_) {
            EventSlot& slot = events_[index];
            if (same_merge(slot, type, json)) {
                slot.time_us = time_us;
                copy_json_string(slot.json, sizeof(slot.json), json != nullptr ? json : "{}");
                wake_ = true;
                return;
            }
            index = (index + 1) % 32;
        }
    }
    const std::uint32_t capacity = 32;
    const std::uint32_t next = (ev_write_ + 1) % capacity;
    if (next == ev_read_) {
        if (protected_event(events_[ev_read_].type) || !protected_event(type)) {
            ++events_dropped_;
            return;
        }
        ev_read_ = (ev_read_ + 1) % capacity;
        ++events_dropped_;
    }
    EventSlot& slot = events_[ev_write_];
    slot.type = type;
    slot.time_us = time_us;
    copy_json_string(slot.json, sizeof(slot.json), json != nullptr ? json : "{}");
    slot.live = true;
    ev_write_ = next;
    wake_ = true;
}

void Player::complete_command_locked(std::uint64_t command_id, bool ok) {
    char json[64];
    std::snprintf(json, sizeof(json), "{\"id\":%llu,\"ok\":%s}", static_cast<unsigned long long>(command_id),
                  ok ? "true" : "false");
    emit_locked(LEGATO_EVENT_TYPE_COMMAND_DONE, engine_->scheduler_.now(), json);
}

void Player::set_error_locked(const char* name, const char* message) {
    const ErrorInfo* info = error_info(name);
    const char* category = info != nullptr ? info->category : "internal";
    const char* hint = info != nullptr && info->hint != nullptr ? info->hint : "";
    const bool retryable = info != nullptr && info->retryable;
    std::snprintf(last_error_, sizeof(last_error_),
                  "{\"category\":\"%s\",\"code\":\"%s\",\"retryable\":%s,"
                  "\"message\":\"%s\",\"hint\":\"%s\",\"trace_id\":\"%s\"}",
                  category, name, retryable ? "true" : "false", message != nullptr ? message : "", hint, trace_id_);
    state_ = LEGATO_STATE_ERROR;
    const std::int64_t now = engine_->scheduler_.now();
    emit_locked(LEGATO_EVENT_TYPE_ERROR, now, last_error_);
    emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, now, "{\"state\":\"error\"}");
}

bool Player::begin_load_locked(const char* uri, std::uint64_t command_id) {
    leave_steady_state();
    /* 同一次会话可以反复 load。trace_id 跟这一次打开走，不跟对象寿命走。 */
    std::snprintf(trace_id_, sizeof(trace_id_), "p-%016llx",
                  static_cast<unsigned long long>(g_player_ids.fetch_add(1, std::memory_order_relaxed)));
    ended_sent_ = false;
    first_frame_sent_ = false;
    file_mode_ = false;
    ++generation_;
    if (uri == nullptr || uri[0] == '\0') {
        set_error_locked("invalid_argument", "uri 为空");
        complete_command_locked(command_id, false);
        return true;
    }
    char path[4096];
    bool virtual_clock = false;
    if (std::strncmp(uri, "corpus:", 7) == 0) {
        const char* err_name = nullptr;
        const char* err_message = nullptr;
        /* 根目录由调用方在创建引擎时传入。编进动态库就会把构建机路径带进交付件。 */
        const std::string& corpus_root = engine_->corpus_root();
        if (corpus_root.empty()) {
            set_error_locked("source_open_failed", "corpus 根目录未设置");
            complete_command_locked(command_id, false);
            return true;
        }
        if (resolve_corpus(corpus_root.c_str(), uri, path, static_cast<int>(sizeof(path)), &err_name, &err_message) !=
            0) {
            set_error_locked(err_name != nullptr ? err_name : "source_open_failed",
                             err_message != nullptr ? err_message : "打不开这个文件");
            complete_command_locked(command_id, false);
            return true;
        }
        virtual_clock = true;
    } else {
        const int kind = local_file_path(uri, path, sizeof(path));
        if (kind < 0) {
            set_error_locked("invalid_argument", "路径太长");
            complete_command_locked(command_id, false);
            return true;
        }
        if (kind == 0) {
            set_error_locked("unsupported_scheme", "这个协议还不能播");
            complete_command_locked(command_id, false);
            return true;
        }
    }
    virtual_session_ = virtual_clock;
    segment_set_ = false;
    io_busy_ = true;
    state_ = LEGATO_STATE_LOADING;
    load_us_ = engine_->scheduler_.now();
    file_load_mono_ = monotonic_us();
    emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, load_us_, "{\"state\":\"loading\"}");
    {
        std::lock_guard<std::mutex> io_lock(io_mu_);
        std::snprintf(io_job_.path, sizeof(io_job_.path), "%s", path);
        io_job_.generation = generation_;
        io_job_.command_id = command_id;
        io_job_.virtual_clock = virtual_clock;
        io_job_.decode_video = preset_ != nullptr && preset_->video;
        io_job_.buffer_start_s = preset_ != nullptr ? preset_->buffer_start_s : 1.5;
        io_job_.buffer_resume_s = preset_ != nullptr ? preset_->buffer_resume_s : 3;
        io_job_.buffer_max_s = preset_ != nullptr ? preset_->buffer_max_s : 50;
        io_job_.pending = true;
    }
    io_cv_.notify_all();
    return true;
}

/* IO 线程只打开文件。这里不持有 Engine::mu_ 或 Player::mu_，打开可以阻塞。 */
void Player::io_main() {
    while (!io_stop_.load()) {
        OpenJob job;
        {
            std::unique_lock<std::mutex> lock(io_mu_);
            io_cv_.wait(lock, [this] { return io_stop_.load() || io_job_.pending; });
            if (io_stop_.load()) {
                return;
            }
            job = io_job_;
            io_job_.pending = false;
        }
        std::unique_ptr<AudioSink> audio;
        std::unique_ptr<VideoSink> video;
        std::unique_ptr<AudioMasterClock> master;
        Clock* clock = nullptr;
        if (job.virtual_clock) {
            audio = std::make_unique<NullAudioSink>();
            video = std::make_unique<RecordingVideoSink>();
            clock = &engine_->virtual_clock();
        } else {
#if defined(LEGATO_HAS_ALSA)
            audio = std::make_unique<AlsaSink>();
#else
            audio = std::make_unique<UnavailableAudioSink>();
#endif
            video = std::make_unique<CountingVideoSink>();
            master = std::make_unique<AudioMasterClock>();
            clock = master.get();
        }
        auto playback = std::make_shared<FilePlayback>(this, clock, std::move(audio), std::move(video), std::move(master),
                                                       job.decode_video, job.buffer_start_s, job.buffer_resume_s,
                                                       job.buffer_max_s);
        const int rc = playback->open(job.path);
        if (rc == 0 && !job.virtual_clock && !playback->has_audio()) {
            playback->bind_clock(playback->monotonic_clock());
        }
        /* 本线程随后就回到等待。回收线程 join 完可能丢掉 Player 的最后一份所有权，
           控制线程还没跑 finish_open_job。回调留着 shared_ptr，对象才活得到那一次。 */
        std::shared_ptr<Player> self = shared_from_this();
        engine_->post([self, job, playback, rc] { self->finish_open_job(job, playback, rc); });
    }
}

void Player::finish_open(std::uint64_t generation, std::uint64_t command_id, bool ok) {
    (void)generation;
    (void)command_id;
    (void)ok;
}

void Player::finish_open_job(const OpenJob& job, std::shared_ptr<FilePlayback> playback, int rc) {
    std::shared_ptr<FilePlayback> drop;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        io_busy_ = false;
        if (released_ || job.generation != generation_) {
            drop = std::move(playback);
            complete_command_locked(job.command_id, false);
            wake = true;
        } else if (rc != 0) {
            const char* name = playback != nullptr ? playback->error_name() : "source_open_failed";
            const char* message = playback != nullptr ? playback->error_message() : "打不开这个文件";
            set_error_locked(name != nullptr ? name : "source_open_failed",
                             message != nullptr ? message : "打不开这个文件");
            drop = std::move(playback);
            complete_command_locked(job.command_id, false);
            wake = true;
        } else {
            playback_ = std::move(playback);
            file_mode_ = true;
            const FilePlayback::Stages stages = playback_->stages();
            stage_probe_us_ = stages.probe_us;
            stage_first_packet_us_ = stages.first_packet_us;
            stage_decoder_open_us_ = stages.decoder_open_us;
            stage_decoded_us_ = stages.decoded_us;
            if (playback_->duration_us() > 0) {
                duration_us_ = playback_->duration_us();
            }
            if (job.virtual_clock) {
                std::snprintf(output_path_, sizeof(output_path_), "record");
            }
            if (!segment_set_) {
                play_virtual_us_ = engine_->virtual_clock().now_us();
                segment_virtual_us_ = play_virtual_us_;
                segment_media_us_ = media_start_us_;
                segment_set_ = true;
            }
            /* load 之前的 Seek 只写过 media_start。打开完成后补一次，解码从目标位置开始。 */
            if (media_start_us_ > 0) {
                playback_->seek(media_start_us_, static_cast<std::uint32_t>(serial_));
            }
            if (playing_intent_) {
                playback_->start();
            }
            complete_command_locked(job.command_id, true);
            wake = true;
        }
    }
    drop.reset();
    dispatch(wake);
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

void Player::media_failed(const char* name, const char* message) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!file_mode_) {
            return;
        }
        /* 错误名由通知方传入。文案只写入 message，不参与选择 category。 */
        set_error_locked(name != nullptr ? name : "decode_failed", message != nullptr ? message : "播放失败");
        wake = true;
    }
    dispatch(wake);
}

void Player::schedule_note_drain() {
    if (note_drain_posted_.exchange(true)) {
        return;
    }
    std::shared_ptr<Player> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        note_drain_posted_.store(false);
        return;
    }
    engine_->post([self] { self->apply_notes(); });
}

void Player::apply_notes() {
    note_drain_posted_.store(false);
    std::shared_ptr<FilePlayback> playback;
    {
        std::lock_guard<std::mutex> lock(mu_);
        playback = playback_;
    }
    if (!playback) {
        return;
    }
    OutputNote note;
    while (playback->pop_note(&note)) {
        switch (note.kind) {
        case OutputNote::FirstFrame:
            media_first_frame(note.position_us, note.extra_us);
            break;
        case OutputNote::Stall:
            note_stall();
            break;
        case OutputNote::Resume:
            note_buffer_resumed();
            break;
        case OutputNote::Failed:
            media_failed(note.name, note.message);
            break;
        case OutputNote::Ended:
            media_ended();
            break;
        case OutputNote::None:
            break;
        }
    }
    if (playback->notes_pending()) {
        schedule_note_drain();
    }
}

void Player::note_stall() {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!file_mode_ || released_) {
            return;
        }
        ++stall_count_;
        if (state_ == LEGATO_STATE_PLAYING && playing_intent_) {
            state_ = LEGATO_STATE_BUFFERING;
            emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, monotonic_us(), "{\"state\":\"buffering\"}");
            wake = true;
        }
    }
    dispatch(wake);
}

void Player::note_buffer_resumed() {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!file_mode_ || released_ || !playing_intent_ || state_ != LEGATO_STATE_BUFFERING) {
            return;
        }
        state_ = LEGATO_STATE_PLAYING;
        emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, monotonic_us(), "{\"state\":\"playing\"}");
        wake = true;
    }
    dispatch(wake);
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
    if (state_ == LEGATO_STATE_ENDED) {
        media_start_us_ = 0;
        position_us_.store(0, std::memory_order_release);
        ended_sent_ = false;
        first_frame_sent_ = false;
        segment_media_us_ = 0;
        segment_virtual_us_ = engine_->virtual_clock().now_us();
        segment_set_ = true;
    }
    if (playback_) {
        if (!segment_set_) {
            play_virtual_us_ = engine_->virtual_clock().now_us();
            segment_virtual_us_ = play_virtual_us_;
            segment_media_us_ = media_start_us_;
            segment_set_ = true;
        }
        playback_->start();
        if (state_ == LEGATO_STATE_READY) {
            state_ = LEGATO_STATE_PLAYING;
            emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, engine_->scheduler_.now(), "{\"state\":\"playing\"}");
            return true;
        }
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
    note_test_seek_mode(mode);
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
        playback_->seek(position_us, static_cast<std::uint32_t>(serial_));
        segment_media_us_ = position_us;
        segment_virtual_us_ = engine_->virtual_clock().now_us();
        segment_set_ = true;
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
    const std::string copy = uri != nullptr ? uri : "";
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = bump_locked(command_id);
    }
    engine_->post([this, copy, id] {
        std::shared_ptr<FilePlayback> old;
        bool wake = false;
        {
            std::lock_guard<std::mutex> self_lock(mu_);
            if (ignore_if_released_locked()) {
                complete_command_locked(id, false);
                wake = true;
            } else {
                old = std::move(playback_);
                file_mode_ = false;
                wake = begin_load_locked(copy.c_str(), id);
            }
        }
        old.reset();
        dispatch(wake);
    });
    return LEGATO_ERR_OK;
}

int Player::play(std::uint64_t* command_id) {
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = bump_locked(command_id);
    }
    engine_->post([this, id] {
        bool wake = false;
        {
            std::lock_guard<std::mutex> self_lock(mu_);
            if (ignore_if_released_locked()) {
                complete_command_locked(id, false);
            } else {
                play_locked();
                complete_command_locked(id, true);
            }
            wake = wake_;
        }
        dispatch(wake);
    });
    return LEGATO_ERR_OK;
}

int Player::pause(std::uint64_t* command_id) {
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = bump_locked(command_id);
    }
    engine_->post([this, id] {
        bool wake = false;
        {
            std::lock_guard<std::mutex> self_lock(mu_);
            if (ignore_if_released_locked()) {
                complete_command_locked(id, false);
            } else {
                pause_locked();
                complete_command_locked(id, true);
            }
            wake = wake_;
        }
        dispatch(wake);
    });
    return LEGATO_ERR_OK;
}

int Player::seek(std::int64_t position_us, int mode, std::uint64_t* command_id) {
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = bump_locked(command_id);
    }
    engine_->post([this, position_us, mode, id] {
        bool wake = false;
        {
            std::lock_guard<std::mutex> self_lock(mu_);
            if (ignore_if_released_locked()) {
                complete_command_locked(id, false);
            } else {
                seek_locked(position_us, mode);
                complete_command_locked(id, true);
            }
            wake = wake_;
        }
        dispatch(wake);
    });
    return LEGATO_ERR_OK;
}

int Player::stop(std::uint64_t* command_id) {
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = bump_locked(command_id);
    }
    engine_->post([this, id] {
        bool wake = false;
        {
            std::lock_guard<std::mutex> self_lock(mu_);
            if (ignore_if_released_locked()) {
                complete_command_locked(id, false);
            } else {
                stop_locked();
                complete_command_locked(id, true);
            }
            wake = wake_;
        }
        dispatch(wake);
    });
    return LEGATO_ERR_OK;
}

int Player::set_preset_name(const char* name, std::uint64_t* command_id) {
    const std::string copy = name != nullptr ? name : "";
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = bump_locked(command_id);
    }
    int rc = LEGATO_ERR_OK;
    bool wake = false;
    {
        std::lock_guard<std::mutex> self_lock(mu_);
        if (ignore_if_released_locked()) {
            complete_command_locked(id, false);
            wake = true;
        } else {
            const PresetDesc* found = find_preset(copy.c_str());
            if (found == nullptr) {
                set_error_locked("invalid_argument", "未知预设");
                complete_command_locked(id, false);
                rc = LEGATO_ERR_INVALID_ARGUMENT;
            } else {
                preset_ = found;
                complete_command_locked(id, true);
            }
            wake = wake_;
        }
    }
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
    out->events_dropped = events_dropped_;
    std::snprintf(out->presentation_sha256, sizeof(out->presentation_sha256), "%s", presentation_sha256_);
    return LEGATO_ERR_OK;
}

int Player::default_seek_mode() const {
    if (preset_ != nullptr && preset_->exact_seek) {
        return LEGATO_SEEK_MODE_PRECISE;
    }
    return LEGATO_SEEK_MODE_KEYFRAME;
}

void Player::prepare_advance(std::int64_t delta_us) {
    std::shared_ptr<FilePlayback> playback;
    std::int64_t target = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (released_ || !virtual_session_ || playback_ == nullptr || !playing_intent_) {
            return;
        }
        playback = playback_;
        const std::int64_t now = engine_->virtual_clock().now_us();
        target = segment_media_us_ + (now + delta_us - segment_virtual_us_);
    }
    if (playback) {
        playback->wait_decoded(target);
    }
}

void Player::poll_observe() {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (released_) {
            return;
        }
        const std::int64_t now = monotonic_us();
        for (std::size_t i = 0; i < 8; ++i) {
            Watch& watch = watches_[i];
            if (!watch.used) {
                continue;
            }
            char text[128];
            if (std::strcmp(watch.name, LEGATO_PROP_STATE) == 0) {
                std::snprintf(text, sizeof(text), "%s", legato_state_name(state_));
            } else if (std::strcmp(watch.name, LEGATO_PROP_POSITION_US) == 0) {
                std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(position_us_.load()));
            } else {
                std::snprintf(text, sizeof(text), "%s", watch.name);
            }
            const std::int64_t interval = static_cast<std::int64_t>(watch.min_interval_s * 1000000.0);
            if (std::strcmp(text, watch_text_[i]) == 0) {
                continue;
            }
            if (watch_at_us_[i] != 0 && now - watch_at_us_[i] < interval) {
                continue;
            }
            std::snprintf(watch_text_[i], sizeof(watch_text_[i]), "%s", text);
            watch_at_us_[i] = now;
            char json[256];
            std::snprintf(json, sizeof(json), "{\"property\":\"%s\",\"tag\":%llu,\"value\":\"%s\"}", watch.name,
                          static_cast<unsigned long long>(watch.tag), text);
            emit_locked(LEGATO_EVENT_TYPE_PROPERTY_CHANGED, now, json);
            wake = true;
        }
    }
    dispatch(wake);
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
    std::shared_ptr<FilePlayback> playback;
    std::int64_t elapsed = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (released_) {
            return;
        }
        if (!virtual_session_ || playback_ == nullptr) {
            update_position_locked();
            return;
        }
        playback = playback_;
        elapsed = engine_->virtual_clock().now_us() - segment_virtual_us_;
        if (elapsed < 0) {
            elapsed = 0;
        }
    }
    FilePlayback::PresentUpdate update;
    playback->present_until(elapsed, &update);
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (released_ || playback_.get() != playback.get()) {
            return;
        }
        const std::int64_t now = engine_->virtual_clock().now_us();
        if (update.sha256[0] != '\0') {
            std::snprintf(presentation_sha256_, sizeof(presentation_sha256_), "%s", update.sha256);
        }
        if (update.presented > presented_) {
            presented_ = update.presented;
        }
        av_sync_us_ = update.av_sync_us;
        if (update.first && !first_frame_sent_) {
            first_frame_sent_ = true;
            last_first_frame_us_ = update.first_frame_us;
            stage_presented_us_ = update.first_frame_us;
            startup_us_ = update.first_frame_us;
            has_decoder_ = true;
            enter_steady_state();
            char frame_json[96];
            std::snprintf(frame_json, sizeof(frame_json), "{\"us\":%lld}", static_cast<long long>(last_first_frame_us_));
            emit_locked(LEGATO_EVENT_TYPE_FIRST_FRAME, now, frame_json);
            if (playing_intent_) {
                state_ = LEGATO_STATE_PLAYING;
                emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, now, "{\"state\":\"playing\"}");
            } else {
                state_ = LEGATO_STATE_READY;
                emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, now, "{\"state\":\"ready\"}");
            }
        }
        std::int64_t pos = segment_media_us_ + elapsed;
        if (duration_us_ > 0 && pos >= duration_us_) {
            pos = duration_us_;
            if (!ended_sent_ && update.ended) {
                ended_sent_ = true;
                state_ = LEGATO_STATE_ENDED;
                emit_locked(LEGATO_EVENT_TYPE_ENDED, now, "{\"reason\":\"eos\"}");
                emit_locked(LEGATO_EVENT_TYPE_STATE_CHANGED, now, "{\"state\":\"ended\"}");
            }
        }
        position_us_.store(pos, std::memory_order_release);
        wake = wake_;
    }
    dispatch(wake);
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
