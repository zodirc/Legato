#define _POSIX_C_SOURCE 200809L

#include "legato/legato.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef LEGATO_SOURCE_DIR
#define LEGATO_SOURCE_DIR "."
#endif

static int g_failures = 0;

static legato_engine* open_engine(void) {
    char config[4096];
    snprintf(config, sizeof(config), "{\"mode\":\"headless\",\"corpus_root\":\"%s\"}", LEGATO_SOURCE_DIR);
    return legato_engine_create(config);
}

static void check(int cond, const char* expr, int line) {
    if (!cond) {
        fprintf(stderr, "FAIL m05:%d %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

/* 验收是 1 毫秒内返回。TSan 和 ASan 插桩会把同一次调用拉长。
   打开钩子仍堵 2 秒，所以插桩构建用 20 毫秒仍能证明没有在调用线程上等待。 */
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#    define M05_RETURN_BUDGET_US 20000
#  endif
#endif
#ifndef M05_RETURN_BUDGET_US
#  define M05_RETURN_BUDGET_US 1000
#endif

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int drain_command_done(legato_player* player) {
    int got = 0;
    const legato_event* event;
    while ((event = legato_player_next_event(player)) != NULL) {
        if (event->type == LEGATO_EVENT_TYPE_COMMAND_DONE) {
            ++got;
        }
    }
    return got;
}

static int wait_commands(legato_player* player, int need) {
    int got = 0;
    for (int spin = 0; spin < 5000 && got < need; ++spin) {
        const legato_event* event;
        int progress = 0;
        while ((event = legato_player_next_event(player)) != NULL) {
            progress = 1;
            if (event->type == LEGATO_EVENT_TYPE_COMMAND_DONE) {
                ++got;
            }
        }
        if (!progress) {
            const struct timespec pause = {0, 2000000};
            nanosleep(&pause, NULL);
        }
    }
    return got;
}

static void test_open_does_not_block(void) {
    legato_test_set_open_delay_us(2000000);
    legato_engine* engine = open_engine();
    legato_player* first = legato_player_create(engine, "{\"preset\":\"vod\"}");
    legato_player* second = legato_player_create(engine, "{\"preset\":\"vod\"}");
    CHECK(engine != NULL && first != NULL && second != NULL);
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    const int64_t load_at = now_us();
    CHECK(legato_player_load(first, &item) == 0);
    CHECK(now_us() - load_at < M05_RETURN_BUDGET_US);
    const int64_t play_at = now_us();
    CHECK(legato_player_play(second) == 0);
    CHECK(now_us() - play_at < M05_RETURN_BUDGET_US);
    /* 1.5 秒时打开仍应堵在 Sink 上。钩子失效的话，这里已经能取到 command_done。 */
    const struct timespec mid = {1, 500000000};
    nanosleep(&mid, NULL);
    CHECK(drain_command_done(first) == 0);
    const int64_t release_at = now_us();
    legato_player_release(first);
    CHECK(now_us() - release_at < M05_RETURN_BUDGET_US);
    const struct timespec rest = {2, 0};
    nanosleep(&rest, NULL);
    legato_player_release(second);
    legato_engine_destroy(engine);
    legato_test_set_open_delay_us(0);
}

static const char* trace_of(const char* json) {
    const char* key = "\"trace_id\":\"";
    const char* found = json != NULL ? strstr(json, key) : NULL;
    return found != NULL ? found + strlen(key) : NULL;
}

static void test_trace_id_per_load(void) {
    legato_engine* engine = open_engine();
    legato_player* player = legato_player_create(engine, "{\"preset\":\"vod\"}");
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "http://example/a";
    CHECK(legato_player_load(player, &item) == 0);
    CHECK(wait_commands(player, 1) == 1);
    char* first = NULL;
    CHECK(legato_player_get(player, "last_error", &first) == 0);
    item.uri = "http://example/b";
    CHECK(legato_player_load(player, &item) == 0);
    CHECK(wait_commands(player, 1) == 1);
    char* second = NULL;
    CHECK(legato_player_get(player, "last_error", &second) == 0);
    const char* left = trace_of(first);
    const char* right = trace_of(second);
    CHECK(left != NULL && right != NULL);
    CHECK(left != NULL && right != NULL && strncmp(left, right, 18) != 0);
    legato_free(first);
    legato_free(second);
    legato_player_release(player);
    legato_engine_destroy(engine);
}

static void test_default_seek_mode(void) {
    legato_engine* engine = open_engine();
    legato_player* vod = legato_player_create(engine, "{\"preset\":\"vod\"}");
    CHECK(legato_player_command(vod, "{\"cmd\":\"seek\",\"position_us\":0}") != 0);
    CHECK(wait_commands(vod, 1) == 1);
    CHECK(legato_test_last_seek_mode() == LEGATO_SEEK_MODE_PRECISE);
    legato_player* feed = legato_player_create(engine, "{\"preset\":\"short_video\"}");
    CHECK(legato_player_command(feed, "{\"cmd\":\"seek\",\"position_us\":0}") != 0);
    CHECK(wait_commands(feed, 1) == 1);
    CHECK(legato_test_last_seek_mode() == LEGATO_SEEK_MODE_KEYFRAME);
    legato_player_release(vod);
    legato_player_release(feed);
    legato_engine_destroy(engine);
}

static void test_music_skips_video_decoder(void) {
    legato_test_reset_video_decoder_opens();
    legato_test_reset_video_packets();
    legato_engine* engine = open_engine();
    legato_player* player = legato_player_create(engine, "{\"preset\":\"music\"}");
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    CHECK(legato_player_load(player, &item) == 0);
    CHECK(legato_player_play(player) == 0);
    CHECK(wait_commands(player, 2) == 2);
    CHECK(legato_engine_advance_us(engine, 500000) == 0);
    CHECK(legato_test_video_decoder_opens() == 0);
    CHECK(legato_test_video_packets() == 0);
    legato_player_release(player);
    const struct timespec settle = {0, 200000000};
    nanosleep(&settle, NULL);

    legato_test_reset_video_packets();
    legato_player* vod = legato_player_create(engine, "{\"preset\":\"vod\"}");
    CHECK(legato_player_load(vod, &item) == 0);
    CHECK(legato_player_play(vod) == 0);
    CHECK(wait_commands(vod, 2) == 2);
    CHECK(legato_engine_advance_us(engine, 200000) == 0);
    CHECK(legato_test_video_packets() > 0);
    legato_player_release(vod);
    nanosleep(&settle, NULL);
    legato_engine_destroy(engine);
}

static int saw_ended(legato_player* player) {
    const legato_event* event;
    while ((event = legato_player_next_event(player)) != NULL) {
        if (event->type == LEGATO_EVENT_TYPE_ENDED) {
            return 1;
        }
    }
    return 0;
}

static void test_seek_does_not_drop_ended(void) {
    legato_engine* engine = open_engine();
    legato_player* player = legato_player_create(engine, "{\"preset\":\"vod\"}");
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    CHECK(legato_player_load(player, &item) == 0);
    CHECK(wait_commands(player, 1) == 1);
    char* duration_json = NULL;
    CHECK(legato_player_get(player, "duration_us", &duration_json) == 0);
    const long long duration = duration_json != NULL ? strtoll(duration_json, NULL, 10) : 0;
    legato_free(duration_json);
    CHECK(duration > 1000000);
    CHECK(legato_player_seek(player, duration - 200000, LEGATO_SEEK_MODE_PRECISE) == 0);
    CHECK(legato_player_play(player) == 0);
    CHECK(wait_commands(player, 2) == 2);
    CHECK(legato_engine_advance_us(engine, 1000000) == 0);
    char* state = NULL;
    CHECK(legato_player_get(player, "state", &state) == 0);
    CHECK(state != NULL && strcmp(state, "\"ended\"") == 0);
    legato_free(state);
    for (int i = 0; i < 100000; ++i) {
        legato_player_seek(player, duration - 100000, LEGATO_SEEK_MODE_KEYFRAME);
    }
    legato_session_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = (uint32_t)sizeof(stats);
    int stable = 0;
    int32_t previous = -1;
    for (int spin = 0; spin < 200; ++spin) {
        CHECK(legato_player_read_stats(player, &stats) == 0);
        if (stats.events_dropped > 0 && stats.events_dropped == previous) {
            ++stable;
            if (stable > 2) {
                break;
            }
        } else {
            stable = 0;
        }
        previous = stats.events_dropped;
        const struct timespec pause = {0, 20000000};
        nanosleep(&pause, NULL);
    }
    CHECK(stats.events_dropped > 0);
    CHECK(saw_ended(player) == 1);
    legato_player_release(player);
    const struct timespec settle = {1, 0};
    nanosleep(&settle, NULL);
    legato_engine_destroy(engine);
}

static int wait_until(int (*ready)(void)) {
    for (int spin = 0; spin < 1000; ++spin) {
        if (ready() != 0) {
            return 1;
        }
        const struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int pops_ready(void) { return legato_test_pcm_pops() >= 8; }

static int delay_started(void) { return legato_test_decode_delay_started() != 0; }

static int delay_applied(void) { return legato_test_decode_delay_applied() != 0; }

static void test_decode_delay_no_xrun(void) {
    legato_test_set_hold_demux(0);
    legato_test_set_pcm_output(1);
    legato_engine* engine = open_engine();
    legato_player* player = legato_player_create(engine, "{\"preset\":\"realtime\"}");
    CHECK(engine != NULL && player != NULL);
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    CHECK(legato_player_load(player, &item) == 0);
    CHECK(legato_player_play(player) == 0);
    CHECK(wait_commands(player, 2) == 2);
    CHECK(wait_until(pops_ready) == 1);
    legato_test_kick_decode_delay_us(30000);
    CHECK(wait_until(delay_started) == 1);
    const int pops = legato_test_pcm_pops();
    const struct timespec during = {0, 15000000};
    nanosleep(&during, NULL);
    CHECK(legato_test_pcm_pops() > pops);
    CHECK(wait_until(delay_applied) == 1);
    CHECK(legato_test_pcm_xruns() == 0);
    legato_player_release(player);
    const struct timespec settle = {0, 200000000};
    nanosleep(&settle, NULL);
    legato_engine_destroy(engine);
    legato_test_set_pcm_output(0);
}

static void test_queue_bottom_counts_stall(void) {
    legato_test_set_pcm_output(0);
    legato_test_set_hold_demux(0);
    legato_engine* engine = open_engine();
    legato_player* player = legato_player_create(engine, "{\"preset\":\"realtime\"}");
    CHECK(engine != NULL && player != NULL);
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    CHECK(legato_player_load(player, &item) == 0);
    CHECK(legato_player_play(player) == 0);
    CHECK(wait_commands(player, 2) == 2);
    CHECK(legato_engine_advance_us(engine, 200000) == 0);
    legato_session_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = (uint32_t)sizeof(stats);
    CHECK(legato_player_read_stats(player, &stats) == 0);
    CHECK(stats.stall_count == 0);
    legato_test_set_hold_demux(1);
    CHECK(legato_engine_advance_us(engine, 2000000) == 0);
    CHECK(legato_player_read_stats(player, &stats) == 0);
    CHECK(stats.stall_count >= 1);
    char* state = NULL;
    CHECK(legato_player_get(player, "state", &state) == 0);
    CHECK(state != NULL && strcmp(state, "\"buffering\"") == 0);
    legato_free(state);
    legato_test_set_hold_demux(0);
    legato_player_release(player);
    const struct timespec settle = {0, 200000000};
    nanosleep(&settle, NULL);
    legato_engine_destroy(engine);
}

static void test_missing_corpus_root(void) {
    legato_engine* engine = legato_engine_create("{\"mode\":\"headless\"}");
    legato_player* player = legato_player_create(engine, "{\"preset\":\"vod\"}");
    CHECK(engine != NULL && player != NULL);
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    CHECK(legato_player_load(player, &item) == 0);
    CHECK(wait_commands(player, 1) == 1);
    char* error = NULL;
    CHECK(legato_player_get(player, "last_error", &error) == 0);
    CHECK(error != NULL && strstr(error, "corpus 根目录未设置") != NULL);
    CHECK(error != NULL && strstr(error, "source_open_failed") != NULL);
    legato_free(error);
    legato_player_release(player);
    legato_engine_destroy(engine);
}

int main(void) {
    test_missing_corpus_root();
    test_open_does_not_block();
    test_trace_id_per_load();
    test_default_seek_mode();
    test_music_skips_video_decoder();
    test_seek_does_not_drop_ended();
    test_decode_delay_no_xrun();
    test_queue_bottom_counts_stall();
    if (g_failures != 0) {
        fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
