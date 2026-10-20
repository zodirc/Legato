#include "legato/legato.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int cond, const char* expr, int line) {
    if (!cond) {
        fprintf(stderr, "FAIL smoke:%d %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

static int dummy_read(void* userdata, uint8_t* dst, int size) {
    (void)userdata;
    (void)dst;
    (void)size;
    return 0;
}

static int count_events(legato_player* player, int type) {
    int count = 0;
    const legato_event* event;
    while ((event = legato_player_next_event(player)) != NULL) {
        if (event->type == type) {
            ++count;
        }
    }
    return count;
}

int main(void) {
    CHECK(legato_abi_version() == 1u);
    CHECK(strstr(legato_capabilities(), "\"skeleton\":true") != NULL);

    CHECK(legato_player_create(NULL, "{\"preset\":\"nope\"}") == NULL);

    legato_engine* engine = legato_engine_create("{\"mode\":\"headless\"}");
    CHECK(engine != NULL);
    legato_warmup(engine);

    legato_player* player = legato_player_create(engine, "{\"preset\":\"vod\"}");
    CHECK(player != NULL);

    legato_media_item bad;
    memset(&bad, 0, sizeof(bad));
    CHECK(legato_player_load(player, &bad) == LEGATO_ERR_INVALID_ARGUMENT);

    CHECK(legato_player_seek(player, 3000000, LEGATO_SEEK_MODE_KEYFRAME) == 0);
    CHECK(legato_player_seek(player, 10000000, LEGATO_SEEK_MODE_PRECISE) == 0);
    CHECK(count_events(player, LEGATO_EVENT_TYPE_SEEK) == 1);

    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    CHECK(legato_player_load(player, &item) == 0);
    CHECK(legato_player_play(player) == 0);
    CHECK(legato_engine_advance_us(engine, 120000) == 0);
    CHECK(legato_player_position_us(player) == 10000000);
    CHECK(legato_engine_advance_us(engine, 1000000) == 0);
    CHECK(legato_player_position_us(player) == 11000000);

    char* state = NULL;
    CHECK(legato_player_get(player, "state", &state) == 0);
    CHECK(state != NULL && strcmp(state, "\"playing\"") == 0);
    legato_free(state);

    char* path = NULL;
    CHECK(legato_player_get(player, "video.output_path", &path) == 0);
    CHECK(path != NULL && strcmp(path, "\"none\"") == 0);
    legato_free(path);

    legato_session_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = (uint32_t)sizeof(stats);
    CHECK(legato_player_read_stats(player, &stats) == 0);
    CHECK(stats.first_frame_us == 120000);
    CHECK(stats.alloc_steady == 0);
    CHECK(stats.presented == 1);

    /* The coalesced seek was drained above. Load and the first frame are still queued. */
    CHECK(count_events(player, LEGATO_EVENT_TYPE_FIRST_FRAME) == 1);

    uint64_t id = legato_player_command(player, "{\"cmd\":\"pause\"}");
    CHECK(id != 0);
    state = NULL;
    CHECK(legato_player_get(player, "state", &state) == 0);
    CHECK(state != NULL && strcmp(state, "\"ready\"") == 0);
    legato_free(state);

    int marker = 1;
    legato_surface surface;
    memset(&surface, 0, sizeof(surface));
    surface.struct_size = (uint32_t)sizeof(surface);
    surface.platform = "linux";
    surface.handle = &marker;
    CHECK(legato_player_attach_surface(player, &surface) == 0);
    path = NULL;
    CHECK(legato_player_get(player, "video.output_path", &path) == 0);
    CHECK(path != NULL && strcmp(path, "\"direct\"") == 0);
    legato_free(path);

    CHECK(legato_player_observe(player, "state", 0.5, 7) == 0);
    CHECK(legato_render_create(player, NULL) == NULL);
    CHECK(legato_render_frame(NULL, NULL) == LEGATO_ERR_UNSUPPORTED);
    CHECK(legato_player_tap(player, "{}", NULL, NULL) == 0);

    legato_source_vtable vtable;
    memset(&vtable, 0, sizeof(vtable));
    vtable.struct_size = (uint32_t)sizeof(vtable);
    vtable.read = NULL;
    CHECK(legato_register_source(engine, "p2p", &vtable, NULL) == LEGATO_ERR_INVALID_ARGUMENT);
    vtable.read = dummy_read;
    CHECK(legato_register_source(engine, "p2p", &vtable, NULL) == 0);

    legato_player_release(player);
    CHECK(legato_player_play(player) == 0);
    CHECK(legato_player_position_us(player) == 0);
    state = NULL;
    CHECK(legato_player_get(player, "state", &state) == 0);
    CHECK(state != NULL && strcmp(state, "{\"released\":true}") == 0);
    legato_free(state);

    legato_engine_destroy(engine);
    if (g_failures != 0) {
        fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
