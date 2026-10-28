#define _POSIX_C_SOURCE 200809L

#include "legato/legato.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef LEGATO_SOURCE_DIR
#define LEGATO_SOURCE_DIR "."
#endif

static void sleep_ms(int ms) {
    struct timespec pause;
    pause.tv_sec = ms / 1000;
    pause.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&pause, NULL);
}

static int wait_commands(legato_player* player, int need) {
    int got = 0;
    for (int spin = 0; spin < 3000 && got < need; ++spin) {
        const legato_event* event;
        int progress = 0;
        while ((event = legato_player_next_event(player)) != NULL) {
            progress = 1;
            if (event->type == LEGATO_EVENT_TYPE_COMMAND_DONE) {
                ++got;
            }
        }
        if (!progress) {
            sleep_ms(2);
        }
    }
    return got;
}

static int run_pause(void) {
    char config[4096];
    snprintf(config, sizeof(config), "{\"mode\":\"headless\",\"corpus_root\":\"%s\"}", LEGATO_SOURCE_DIR);
    legato_engine* engine = legato_engine_create(config);
    if (engine == NULL) {
        fprintf(stderr, "engine create failed\n");
        return 1;
    }
    legato_player* player = legato_player_create(engine, "{\"preset\":\"vod\"}");
    if (player == NULL) {
        legato_engine_destroy(engine);
        return 1;
    }
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    if (legato_player_load(player, &item) != 0 || wait_commands(player, 1) != 1) {
        fprintf(stderr, "load failed\n");
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }
    if (legato_player_play(player) != 0 || wait_commands(player, 1) != 1) {
        fprintf(stderr, "play failed\n");
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }
    if (legato_engine_advance_us(engine, 500000) != 0) {
        fprintf(stderr, "advance failed\n");
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }
    if (legato_player_pause(player) != 0 || wait_commands(player, 1) != 1) {
        fprintf(stderr, "pause failed\n");
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }
    sleep_ms(1500);
    legato_player_release(player);
    legato_engine_destroy(engine);
    return 0;
}

static int run_idle(void) {
    legato_engine* engine = legato_engine_create("{\"mode\":\"headless\"}");
    if (engine == NULL) {
        fprintf(stderr, "engine create failed\n");
        return 1;
    }
    sleep_ms(1500);
    legato_engine_destroy(engine);
    return 0;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "idle";
    setenv("LEGATO_TEST_QUIET", mode, 1);
    if (strcmp(mode, "pause") == 0) {
        setenv("LEGATO_TEST_FRAME_HOLD", "1", 1);
        return run_pause();
    }
    return run_idle();
}
