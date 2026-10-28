#define _POSIX_C_SOURCE 200809L

#include "legato/legato.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef LEGATO_HAS_SDL2
#include <SDL.h>
#endif

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

static int preset_ok(const char* preset) {
    if (preset == NULL || preset[0] == '\0') {
        return 0;
    }
    for (const char* cursor = preset; *cursor != '\0'; ++cursor) {
        const int letter = (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= '0' && *cursor <= '9') || *cursor == '_';
        if (!letter) {
            return 0;
        }
    }
    return 1;
}

static void drain_events(legato_player* player);

#ifdef LEGATO_HAS_SDL2
/* 库不创建窗口。这里用 SDL2 准备 GL 上下文，再把当前帧画进它给的目标。没有显示时改用 offscreen。 */
static int play_window(legato_player* player, int64_t limit_ms, int64_t started) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    const char* display = getenv("DISPLAY");
    if (display == NULL || display[0] == '\0') {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            return -1;
        }
    }
    SDL_Window* window = SDL_CreateWindow("legato", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 960, 540,
                                          SDL_WINDOW_OPENGL);
    if (window == NULL) {
        SDL_Quit();
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            return -1;
        }
        window = SDL_CreateWindow("legato", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 960, 540, SDL_WINDOW_OPENGL);
    }
    if (window == NULL) {
        SDL_Quit();
        return -1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (gl == NULL) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    legato_render_params params;
    memset(&params, 0, sizeof(params));
    params.struct_size = (uint32_t)sizeof(params);
    params.api = LEGATO_RENDER_API_GL;
    legato_render_ctx* ctx = legato_render_create(player, &params);
    if (ctx == NULL) {
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event) == 1) {
            if (event.type == SDL_QUIT) {
                SDL_GL_DeleteContext(gl);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 3;
            }
        }
        int width = 0;
        int height = 0;
        SDL_GL_GetDrawableSize(window, &width, &height);
        if (width > 0 && height > 0) {
            legato_render_target target;
            memset(&target, 0, sizeof(target));
            target.struct_size = (uint32_t)sizeof(target);
            target.gl_fbo = 0;
            target.width = width;
            target.height = height;
            legato_render_frame(ctx, &target);
            SDL_GL_SwapWindow(window);
        }
        char* state = NULL;
        if (legato_player_get(player, "state", &state) != 0 || state == NULL) {
            SDL_GL_DeleteContext(gl);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 2;
        }
        drain_events(player);
        const int64_t position = legato_player_position_us(player);
        printf("\r%lld ms  %s", (long long)(position / 1000), state);
        fflush(stdout);
        const int ended = strcmp(state, "\"ended\"") == 0;
        const int failed = strcmp(state, "\"error\"") == 0;
        legato_free(state);
        if (ended || failed || now_ms() - started > limit_ms) {
            printf("\n%s\n", ended ? "结束" : (failed ? "失败" : "超时"));
            SDL_GL_DeleteContext(gl);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return ended ? 0 : 1;
        }
        const struct timespec pause = {0, 16000000};
        nanosleep(&pause, NULL);
    }
}
#endif

static void drain_events(legato_player* player) {
    for (;;) {
        const legato_event* event = legato_player_next_event(player);
        if (event == NULL) {
            break;
        }
        if (event->type == LEGATO_EVENT_TYPE_AUDIO_FALLBACK) {
            printf("audio_fallback\n");
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || !preset_ok(argc == 3 ? argv[2] : "music")) {
        fprintf(stderr, "用法: legato_play <本地文件> [预设]\n");
        return 2;
    }
    const char* preset = argc == 3 ? argv[2] : "music";
    char options[96];
    snprintf(options, sizeof(options), "{\"preset\":\"%s\"}", preset);

    legato_engine* engine = legato_engine_create(NULL);
    legato_player* player = legato_player_create(engine, options);
    if (engine == NULL || player == NULL) {
        fprintf(stderr, "创建播放器失败\n");
        return 1;
    }

    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = argv[1];
    /* load 返回 0 只表示命令被接受。打开在会话的 IO 线程上完成。 */
    if (legato_player_load(player, &item) != 0 || legato_player_play(player) != 0) {
        fprintf(stderr, "打开失败: %s\n", argv[1]);
        print_last_error(player);
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }
    int opened = 0;
    for (int spin = 0; spin < 500; ++spin) {
        if (state_is(player, "\"error\"")) {
            fprintf(stderr, "打开失败: %s\n", argv[1]);
            print_last_error(player);
            legato_player_release(player);
            legato_engine_destroy(engine);
            return 1;
        }
        if (state_is(player, "\"playing\"") || state_is(player, "\"ready\"") || state_is(player, "\"ended\"")) {
            opened = 1;
            break;
        }
        const struct timespec pause = {0, 20000000};
        nanosleep(&pause, NULL);
    }
    if (!opened) {
        fprintf(stderr, "打开超时: %s\n", argv[1]);
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
#ifdef LEGATO_HAS_SDL2
    const int window_rc = play_window(player, limit_ms, started);
    if (window_rc >= 0) {
        if (window_rc == 1) {
            print_last_error(player);
        }
        legato_player_release(player);
        legato_engine_destroy(engine);
        return window_rc == 0 || window_rc == 3 ? 0 : 1;
    }
#endif
    for (;;) {
        char* state = NULL;
        if (legato_player_get(player, "state", &state) != 0 || state == NULL) {
            fprintf(stderr, "读状态失败\n");
            legato_player_release(player);
            legato_engine_destroy(engine);
            return 1;
        }
        drain_events(player);
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
