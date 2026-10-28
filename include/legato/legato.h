#ifndef LEGATO_H
#define LEGATO_H

#include "legato_idl.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(LEGATO_BUILDING)
#    define LEGATO_API __declspec(dllexport)
#  else
#    define LEGATO_API __declspec(dllimport)
#  endif
#else
#  define LEGATO_API __attribute__((visibility("default")))
#endif

typedef struct legato_engine legato_engine;
typedef struct legato_player legato_player;
typedef struct legato_render_ctx legato_render_ctx;

typedef struct legato_media_item {
    uint32_t struct_size;
    const char* uri;
    const char* id;
    int64_t start_position_us;
    const char* cache_key;
    const char* hints_json;
} legato_media_item;

typedef struct legato_event {
    uint32_t struct_size;
    int32_t type;
    int64_t time_us;
    const char* json;
} legato_event;

typedef struct legato_surface {
    uint32_t struct_size;
    const char* platform;
    void* handle;
} legato_surface;

typedef struct legato_render_params {
    uint32_t struct_size;
    int32_t api;
} legato_render_params;

typedef struct legato_render_target {
    uint32_t struct_size;
    uint32_t gl_fbo;
    int32_t width;
    int32_t height;
} legato_render_target;

typedef struct legato_source_vtable {
    uint32_t struct_size;
    int (*read)(void* userdata, uint8_t* dst, int size);
    int64_t (*seek)(void* userdata, int64_t offset, int whence);
    int64_t (*length)(void* userdata);
} legato_source_vtable;

typedef struct legato_buffer {
    uint32_t struct_size;
    const uint8_t* data;
    size_t size;
    int64_t pts_us;
    int64_t wall_us;
} legato_buffer;

/* Snapshot used by legato-bench. Hot fields are plain values, not JSON. */
typedef struct legato_session_stats {
    uint32_t struct_size;
    int64_t first_frame_us;
    int64_t av_sync_us;
    uint64_t alloc_steady;
    int32_t presented;
    int32_t decoder_reused;
    int32_t stall_count;
    int64_t position_us;
    int32_t state;
    int64_t stage_probe_us;
    int64_t stage_first_packet_us;
    int64_t stage_decoder_open_us;
    int64_t stage_decoded_us;
    int64_t stage_presented_us;
    int32_t cache_hit;
    int32_t decoder_reused_last;
    int32_t events_dropped;
    /* 0 没有音频主时钟，1 硬件时间戳，2 delay。 */
    int32_t audio_clock_source;
    char presentation_sha256[65];
    /* Seek 之后第一帧的 PTS。还没有呈现时为 -1。 */
    int64_t seek_first_pts_us;
    int32_t seek_stale_frames;
    int32_t seek_commits;
    /* VideoPresenter 因晚于当前拍而丢掉的帧。 */
    int32_t dropped_frames;
    /* HTTP 打开的四段。不是 HTTP 时为 -1。明文 HTTP 的 TLS 段是 0。 */
    int64_t stage_dns_us;
    int64_t stage_connect_us;
    int64_t stage_tls_us;
    int64_t stage_first_byte_us;
    /* 1 可以 Seek。服务端没有 Accept-Ranges 时是 0，会话不因此进入 error。 */
    int32_t source_seekable;
} legato_session_stats;

LEGATO_API uint32_t legato_abi_version(void);
LEGATO_API const char* legato_capabilities(void);

LEGATO_API legato_engine* legato_engine_create(const char* config_json);
LEGATO_API void legato_engine_destroy(legato_engine* engine);
LEGATO_API void legato_warmup(legato_engine* engine);
/* Headless clock. M0 time moves only when this is called. */
LEGATO_API int legato_engine_advance_us(legato_engine* engine, int64_t delta_us);

LEGATO_API legato_player* legato_player_create(legato_engine* engine, const char* options_json);
LEGATO_API void legato_player_release(legato_player* player);

LEGATO_API uint64_t legato_player_command(legato_player* player, const char* command_json);
LEGATO_API int legato_player_get(legato_player* player, const char* property, char** out_json);
LEGATO_API int legato_player_set(legato_player* player, const char* property, const char* value_json);
LEGATO_API int legato_player_observe(legato_player* player, const char* property,
                                     double min_interval_s, uint64_t tag);

LEGATO_API int legato_player_load(legato_player* player, const legato_media_item* item);
LEGATO_API int legato_player_play(legato_player* player);
LEGATO_API int legato_player_pause(legato_player* player);
LEGATO_API int legato_player_seek(legato_player* player, int64_t position_us, legato_seek_mode mode);
LEGATO_API int64_t legato_player_position_us(legato_player* player);
LEGATO_API int legato_player_read_stats(legato_player* player, legato_session_stats* out);
/* 返回本次播放已经记录的音画偏差样本数，单位微秒，取绝对值。dst 可空，只查询数量。 */
LEGATO_API int legato_player_copy_av_sync_us(legato_player* player, int64_t* dst, int capacity);

LEGATO_API void legato_player_set_wakeup(legato_player* player, void (*callback)(void*), void* userdata);
LEGATO_API const legato_event* legato_player_next_event(legato_player* player);

LEGATO_API int legato_player_attach_surface(legato_player* player, const legato_surface* surface);
LEGATO_API legato_render_ctx* legato_render_create(legato_player* player,
                                                   const legato_render_params* params);
LEGATO_API int legato_render_frame(legato_render_ctx* ctx, const legato_render_target* target);

LEGATO_API int legato_register_source(legato_engine* engine, const char* scheme,
                                      const legato_source_vtable* vtable, void* userdata);
LEGATO_API uint64_t legato_player_tap(legato_player* player, const char* tap_spec_json,
                                      void (*on_data)(const legato_buffer*, void*), void* userdata);

LEGATO_API void legato_free(void* ptr);

/* 测试钩子。打开文件前睡眠 delay_us，用来证明 load 和 release 不在调用线程上等待。 */
LEGATO_API void legato_test_set_open_delay_us(int64_t delay_us);
LEGATO_API int legato_test_last_seek_mode(void);
LEGATO_API int legato_test_video_decoder_opens(void);
LEGATO_API void legato_test_reset_video_decoder_opens(void);
LEGATO_API int legato_test_video_packets(void);
LEGATO_API void legato_test_reset_video_packets(void);
LEGATO_API void legato_test_set_pcm_output(int enable);
LEGATO_API void legato_test_kick_decode_delay_us(int64_t delay_us);
LEGATO_API int legato_test_decode_delay_started(void);
LEGATO_API int legato_test_decode_delay_applied(void);
LEGATO_API int legato_test_pcm_pops(void);
LEGATO_API int legato_test_pcm_xruns(void);
LEGATO_API void legato_test_set_hold_demux(int hold);

#ifdef __cplusplus
}
#endif

#endif /* LEGATO_H */
