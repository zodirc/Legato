#ifndef LEGATO_ENGINE_PLAYER_HPP
#define LEGATO_ENGINE_PLAYER_HPP

#include "legato/engine/engine.hpp"
#include "legato/engine/preset.hpp"
#include "legato/legato.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace legato {

class FilePlayback;

class Player {
public:
    Player(std::shared_ptr<Engine> engine, const PresetDesc* preset);
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    int load_uri(const char* uri, std::uint64_t* command_id);
    int play(std::uint64_t* command_id);
    int pause(std::uint64_t* command_id);
    int seek(std::int64_t position_us, int mode, std::uint64_t* command_id);
    int stop(std::uint64_t* command_id);
    int set_preset_name(const char* name, std::uint64_t* command_id);
    int get_json(const char* property, char** out_json);
    int set_json(const char* property, const char* value_json);
    int observe(const char* property, double min_interval_s, std::uint64_t tag);
    std::int64_t position_us() const;
    int read_stats(legato_session_stats* out);
    const legato_event* next_event();
    void set_wakeup(void (*callback)(void*), void* userdata);
    int attach_surface(void* handle);

    /* Called with Engine::mu_ held. */
    void on_clock_advanced();
    bool take_wake();
    /* Called without locks. Must not touch this after the callback returns. */
    void invoke_wakeup();

    void set_media_position(std::int64_t position_us);
    void media_first_frame(std::int64_t position_us, std::int64_t startup_us);
    void media_ended();
    void media_failed(const char* message);
    std::int64_t file_load_mono() const { return file_load_mono_; }

private:
    struct EventSlot {
        int type = 0;
        std::int64_t time_us = 0;
        char json[512]{};
        bool live = false;
    };

    struct Watch {
        bool used = false;
        char name[64]{};
        double min_interval_s = 0;
        std::uint64_t tag = 0;
    };

    bool load_locked(const char* uri);
    bool load_file_locked(const char* path);
    void stop_file();
    bool play_locked();
    bool pause_locked();
    bool seek_locked(std::int64_t position_us, int mode);
    bool stop_locked();
    void on_first_frame(std::int64_t when_us);
    void update_position_locked();
    void emit_locked(int type, std::int64_t time_us, const char* json);
    void set_error_locked(const char* name, const char* message);
    void fill_startup_locked(bool reuse);
    void dispatch(bool wake);
    std::uint64_t bump_locked(std::uint64_t* command_id);
    bool ignore_if_released_locked();

    std::shared_ptr<Engine> engine_;
    std::mutex mu_;
    const PresetDesc* preset_ = nullptr;
    std::uint64_t id_ = 0;
    bool released_ = false;
    bool warned_released_ = false;
    bool playing_intent_ = false;
    bool has_decoder_ = false;
    bool pending_reuse_ = false;
    bool ended_sent_ = false;
    bool first_frame_sent_ = false;
    bool file_mode_ = false;
    std::int64_t file_load_mono_ = 0;
    std::unique_ptr<FilePlayback> playback_;
    int state_ = LEGATO_STATE_IDLE;
    int serial_ = 1;
    std::int64_t media_start_us_ = 0;
    std::int64_t origin_us_ = 0;
    std::int64_t load_us_ = 0;
    std::int64_t duration_us_ = 60 * 1000 * 1000;
    std::int64_t startup_us_ = 120000;
    std::int64_t stage_probe_us_ = 0;
    std::int64_t stage_first_packet_us_ = 0;
    std::int64_t stage_decoder_open_us_ = 0;
    std::int64_t stage_decoded_us_ = 0;
    std::int64_t stage_presented_us_ = 0;
    int cache_hit_ = 0;
    int decoder_reused_last_ = 0;
    int presented_ = 0;
    int decoder_reused_ = 0;
    int stall_count_ = 0;
    std::int64_t last_first_frame_us_ = -1;
    std::int64_t av_sync_us_ = 0;
    char output_path_[16] = "none";
    char last_error_[512]{};
    std::atomic<std::int64_t> position_us_{0};
    std::uint64_t command_seq_ = 0;
    EventSlot events_[32]{};
    uint32_t ev_read_ = 0;
    uint32_t ev_write_ = 0;
    legato_event published_{};
    char published_json_[512]{};
    bool wake_ = false;
    void (*wakeup_)(void*) = nullptr;
    void* wakeup_ud_ = nullptr;
    Watch watches_[8]{};
    void* surface_ = nullptr;
};

}  /* namespace legato */

#endif
