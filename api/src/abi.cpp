#include "legato/legato.h"

#include "legato/core/json_mini.hpp"
#include "legato/core/log.hpp"
#include "legato/engine/engine.hpp"
#include "legato/engine/player.hpp"
#include "legato/engine/preset.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace {

struct EngineShell {
    std::shared_ptr<legato::Engine> engine;
};

struct PlayerShell {
    std::mutex mu;
    std::shared_ptr<legato::Player> player;
    bool warned = false;
};

EngineShell* engine_shell(legato_engine* engine) { return reinterpret_cast<EngineShell*>(engine); }

PlayerShell* player_shell(legato_player* player) { return reinterpret_cast<PlayerShell*>(player); }

std::shared_ptr<legato::Player> lock_player(PlayerShell* shell) {
    if (shell == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(shell->mu);
    if (!shell->player) {
        if (!shell->warned) {
            shell->warned = true;
            legato::log_write(legato::LogLevel::Warn, "command after release ignored");
        }
        return nullptr;
    }
    return shell->player;
}

const char* kCapabilities =
    "{\"abi_version\":1,\"skeleton\":true,\"mode\":\"headless\","
    "\"backend\":[\"legato\"],"
#if defined(LEGATO_HAS_FFMPEG)
    "\"ffmpeg\":\"lgpl\","
#else
    "\"ffmpeg\":\"none\","
#endif
    "\"presets\":[\"vod\",\"short_video\",\"live\",\"live_low_latency\",\"music\",\"realtime\"]}";

}  /* namespace */

extern "C" {

uint32_t legato_abi_version(void) { return LEGATO_ABI_VERSION; }

const char* legato_capabilities(void) { return kCapabilities; }

legato_engine* legato_engine_create(const char* config_json) {
    auto* shell = new (std::nothrow) EngineShell();
    if (shell == nullptr) {
        return nullptr;
    }
    shell->engine = std::make_shared<legato::Engine>(config_json);
    return reinterpret_cast<legato_engine*>(shell);
}

void legato_engine_destroy(legato_engine* engine) { delete engine_shell(engine); }

void legato_warmup(legato_engine* engine) {
    std::shared_ptr<legato::Engine> held;
    if (engine == nullptr) {
        held = legato::Engine::shared_default();
    } else if (engine_shell(engine) != nullptr) {
        held = engine_shell(engine)->engine;
    }
    if (held) {
        held->warmup();
    }
}

int legato_engine_advance_us(legato_engine* engine, int64_t delta_us) {
    EngineShell* shell = engine_shell(engine);
    if (shell == nullptr || !shell->engine) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    const int rc = shell->engine->advance_us(delta_us);
    return rc == 0 ? LEGATO_ERR_OK : LEGATO_ERR_INVALID_ARGUMENT;
}

legato_player* legato_player_create(legato_engine* engine, const char* options_json) {
    std::shared_ptr<legato::Engine> held;
    if (engine == nullptr) {
        held = legato::Engine::shared_default();
    } else {
        EngineShell* shell = engine_shell(engine);
        if (shell == nullptr || !shell->engine) {
            return nullptr;
        }
        held = shell->engine;
    }
    const legato::PresetDesc* preset = legato::default_preset();
    char name[64];
    if (legato::json_copy_string(options_json, "preset", name, sizeof(name))) {
        preset = legato::find_preset(name);
        if (preset == nullptr) {
            return nullptr;
        }
    }
    auto* shell = new (std::nothrow) PlayerShell();
    if (shell == nullptr) {
        return nullptr;
    }
    shell->player = std::make_shared<legato::Player>(held, preset);
    return reinterpret_cast<legato_player*>(shell);
}

void legato_player_release(legato_player* player) {
    PlayerShell* shell = player_shell(player);
    if (shell == nullptr) {
        return;
    }
    std::shared_ptr<legato::Player> dropped;
    {
        std::lock_guard<std::mutex> lock(shell->mu);
        dropped = std::move(shell->player);
    }
    dropped.reset();
}

uint64_t legato_player_command(legato_player* player, const char* command_json) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held || command_json == nullptr) {
        return 0;
    }
    char cmd[32];
    if (!legato::json_copy_string(command_json, "cmd", cmd, sizeof(cmd))) {
        return 0;
    }
    std::uint64_t id = 0;
    if (std::strcmp(cmd, LEGATO_CMD_LOAD) == 0) {
        char uri[1024];
        uri[0] = '\0';
        legato::json_copy_string(command_json, "uri", uri, sizeof(uri));
        held->load_uri(uri, &id);
        return id;
    }
    if (std::strcmp(cmd, LEGATO_CMD_PLAY) == 0) {
        held->play(&id);
        return id;
    }
    if (std::strcmp(cmd, LEGATO_CMD_PAUSE) == 0) {
        held->pause(&id);
        return id;
    }
    if (std::strcmp(cmd, LEGATO_CMD_STOP) == 0) {
        held->stop(&id);
        return id;
    }
    if (std::strcmp(cmd, LEGATO_CMD_SEEK) == 0) {
        std::int64_t position = 0;
        if (!legato::json_copy_i64(command_json, "position_us", &position)) {
            return 0;
        }
        int mode = LEGATO_SEEK_MODE_PRECISE;
        char mode_name[32];
        if (legato::json_copy_string(command_json, "mode", mode_name, sizeof(mode_name))) {
            if (std::strcmp(mode_name, "keyframe") == 0) {
                mode = LEGATO_SEEK_MODE_KEYFRAME;
            } else if (std::strcmp(mode_name, "nearest") == 0) {
                mode = LEGATO_SEEK_MODE_NEAREST;
            }
        }
        held->seek(position, mode, &id);
        return id;
    }
    return 0;
}

int legato_player_get(legato_player* player, const char* property, char** out_json) {
    if (out_json == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    *out_json = nullptr;
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        char* copy = static_cast<char*>(std::malloc(std::strlen("{\"released\":true}") + 1));
        if (copy == nullptr) {
            return LEGATO_ERR_INVALID_ARGUMENT;
        }
        std::memcpy(copy, "{\"released\":true}", std::strlen("{\"released\":true}") + 1);
        *out_json = copy;
        return LEGATO_ERR_OK;
    }
    return held->get_json(property, out_json);
}

int legato_player_set(legato_player* player, const char* property, const char* value_json) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return LEGATO_ERR_OK;
    }
    return held->set_json(property, value_json);
}

int legato_player_observe(legato_player* player, const char* property, double min_interval_s, uint64_t tag) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return LEGATO_ERR_OK;
    }
    return held->observe(property, min_interval_s, tag);
}

int legato_player_load(legato_player* player, const legato_media_item* item) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return LEGATO_ERR_OK;
    }
    if (item == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    const size_t uri_end = offsetof(legato_media_item, uri) + sizeof(const char*);
    if (item->struct_size < uri_end || item->uri == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    const size_t start_end = offsetof(legato_media_item, start_position_us) + sizeof(std::int64_t);
    if (item->struct_size >= start_end && item->start_position_us > 0) {
        held->seek(item->start_position_us, LEGATO_SEEK_MODE_PRECISE, nullptr);
    }
    return held->load_uri(item->uri, nullptr);
}

int legato_player_play(legato_player* player) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return LEGATO_ERR_OK;
    }
    return held->play(nullptr);
}

int legato_player_pause(legato_player* player) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return LEGATO_ERR_OK;
    }
    return held->pause(nullptr);
}

int legato_player_seek(legato_player* player, int64_t position_us, legato_seek_mode mode) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return LEGATO_ERR_OK;
    }
    return held->seek(position_us, static_cast<int>(mode), nullptr);
}

int64_t legato_player_position_us(legato_player* player) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return 0;
    }
    return held->position_us();
}

int legato_player_read_stats(legato_player* player, legato_session_stats* out) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        if (out == nullptr || out->struct_size < sizeof(legato_session_stats)) {
            return LEGATO_ERR_INVALID_ARGUMENT;
        }
        const uint32_t size = out->struct_size;
        std::memset(out, 0, sizeof(legato_session_stats));
        out->struct_size = size;
        out->state = -1;
        out->first_frame_us = -1;
        return LEGATO_ERR_OK;
    }
    return held->read_stats(out);
}

void legato_player_set_wakeup(legato_player* player, void (*callback)(void*), void* userdata) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return;
    }
    held->set_wakeup(callback, userdata);
}

const legato_event* legato_player_next_event(legato_player* player) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return nullptr;
    }
    return held->next_event();
}

int legato_player_attach_surface(legato_player* player, const legato_surface* surface) {
    std::shared_ptr<legato::Player> held = lock_player(player_shell(player));
    if (!held) {
        return LEGATO_ERR_OK;
    }
    if (surface == nullptr || surface->struct_size < sizeof(uint32_t) || surface->handle == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    return held->attach_surface(surface->handle);
}

legato_render_ctx* legato_render_create(legato_player* /*player*/, const legato_render_params* /*params*/) {
    return nullptr;
}

int legato_render_frame(legato_render_ctx* /*ctx*/, const legato_render_target* /*target*/) {
    return LEGATO_ERR_UNSUPPORTED;
}

int legato_register_source(legato_engine* engine, const char* scheme, const legato_source_vtable* vtable,
                           void* userdata) {
    EngineShell* shell = engine_shell(engine);
    if (shell == nullptr || !shell->engine || scheme == nullptr || vtable == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    if (vtable->struct_size < sizeof(uint32_t) || vtable->read == nullptr) {
        return LEGATO_ERR_INVALID_ARGUMENT;
    }
    const size_t seek_end = offsetof(legato_source_vtable, seek) + sizeof(vtable->seek);
    const size_t length_end = offsetof(legato_source_vtable, length) + sizeof(vtable->length);
    auto* seek_fn = vtable->struct_size >= seek_end ? vtable->seek : nullptr;
    auto* length_fn = vtable->struct_size >= length_end ? vtable->length : nullptr;
    const int rc = shell->engine->register_source(scheme, vtable->read, seek_fn, length_fn, userdata);
    return rc == 0 ? LEGATO_ERR_OK : LEGATO_ERR_INVALID_ARGUMENT;
}

uint64_t legato_player_tap(legato_player* /*player*/, const char* /*tap_spec_json*/,
                           void (* /*on_data*/)(const legato_buffer*, void*), void* /*userdata*/) {
    return 0;
}

void legato_free(void* ptr) { std::free(ptr); }

}  /* extern "C" */
