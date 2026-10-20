#define _POSIX_C_SOURCE 200809L

#include "legato/legato.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int state_is(legato_player* player, const char* quoted) {
    char* state = NULL;
    if (legato_player_get(player, "state", &state) != 0 || state == NULL) {
        return 0;
    }
    const int same = strcmp(state, quoted) == 0;
    legato_free(state);
    return same;
}

static void print_last_error(legato_player* player) {
    char* err = NULL;
    if (legato_player_get(player, "last_error", &err) == 0 && err != NULL) {
        fprintf(stderr, "%s\n", err);
        legato_free(err);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "用法: legato_play <本地文件>\n");
        return 2;
    }

    legato_engine* engine = legato_engine_create(NULL);
    legato_player* player = legato_player_create(engine, "{\"preset\":\"music\"}");
    if (engine == NULL || player == NULL) {
        fprintf(stderr, "创建播放器失败\n");
        return 1;
    }

    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = argv[1];
    /* load 返回 0 只表示命令被接受。打不开文件时状态是 "error"，原因在 last_error。 */
    if (legato_player_load(player, &item) != 0 || state_is(player, "\"error\"")) {
        fprintf(stderr, "打开失败: %s\n", argv[1]);
        print_last_error(player);
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }
    if (legato_player_play(player) != 0) {
        fprintf(stderr, "起播失败: %s\n", argv[1]);
        print_last_error(player);
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }

    char* duration = NULL;
    legato_player_get(player, "duration_us", &duration);
    printf("打开 %s\n时长 %s us\n", argv[1], duration != NULL ? duration : "?");
    long long limit_ms = 30000;
    if (duration != NULL) {
        const long long duration_us = strtoll(duration, NULL, 10);
        if (duration_us > 0) {
            const long long padded = duration_us / 1000 + 10000;
            if (padded > limit_ms) {
                limit_ms = padded;
            }
        }
    }
    legato_free(duration);

    const int64_t started = now_ms();
    for (;;) {
        char* state = NULL;
        if (legato_player_get(player, "state", &state) != 0 || state == NULL) {
            fprintf(stderr, "读状态失败\n");
            legato_player_release(player);
            legato_engine_destroy(engine);
            return 1;
        }
        const int64_t position = legato_player_position_us(player);
        printf("\r%lld ms  %s", (long long)(position / 1000), state);
        fflush(stdout);
        const int ended = strcmp(state, "\"ended\"") == 0;
        const int failed = strcmp(state, "\"error\"") == 0;
        legato_free(state);
        if (ended || failed) {
            printf("\n%s\n", ended ? "结束" : "失败");
            if (failed) {
                print_last_error(player);
            }
            legato_player_release(player);
            legato_engine_destroy(engine);
            return ended ? 0 : 1;
        }
        if (now_ms() - started > limit_ms) {
            printf("\n超时\n");
            legato_player_release(player);
            legato_engine_destroy(engine);
            return 1;
        }
        const struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);
    }
}
