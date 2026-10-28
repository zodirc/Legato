#define _POSIX_C_SOURCE 200809L

#include "legato/legato.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef LEGATO_HAS_EGL
#include <EGL/egl.h>
#include <GL/gl.h>
#endif

static int fail(const char* text) {
    fprintf(stderr, "FAIL av_sync_file: %s\n", text);
    return 1;
}

static int cmp_i64(const void* left, const void* right) {
    const int64_t a = *(const int64_t*)left;
    const int64_t b = *(const int64_t*)right;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static int64_t percentile_95(int64_t* values, int count) {
    qsort(values, (size_t)count, sizeof(int64_t), cmp_i64);
    const int index = (count - 1) * 95 / 100;
    return values[index];
}

#ifdef LEGATO_HAS_EGL
static int draw_once(legato_player* player) {
    setenv("EGL_PLATFORM", "surfaceless", 0);
    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
        return fail("eglBindAPI");
    }
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || eglInitialize(display, NULL, NULL) != EGL_TRUE) {
        return fail("eglInitialize");
    }
    const EGLint config_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                  EGL_RED_SIZE,    8,                 EGL_GREEN_SIZE,      8,
                                  EGL_BLUE_SIZE,   8,                 EGL_NONE};
    EGLConfig config;
    EGLint found = 0;
    if (eglChooseConfig(display, config_attr, &config, 1, &found) != EGL_TRUE || found < 1) {
        return fail("eglChooseConfig");
    }
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    if (context == EGL_NO_CONTEXT) {
        return fail("eglCreateContext");
    }
    const EGLint surface_attr[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attr);
    if (surface == EGL_NO_SURFACE || eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        return fail("eglMakeCurrent");
    }
    legato_render_params params;
    memset(&params, 0, sizeof(params));
    params.struct_size = (uint32_t)sizeof(params);
    params.api = LEGATO_RENDER_API_GL;
    legato_render_ctx* ctx = legato_render_create(player, &params);
    if (ctx == NULL) {
        return fail("legato_render_create");
    }
    legato_render_target target;
    memset(&target, 0, sizeof(target));
    target.struct_size = (uint32_t)sizeof(target);
    target.gl_fbo = 0;
    target.width = 64;
    target.height = 64;
    if (legato_render_frame(ctx, &target) != 0) {
        return fail("legato_render_frame");
    }
    unsigned char pixel[4] = {0, 0, 0, 0};
    glReadPixels(32, 32, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, pixel);
    if (pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0) {
        return fail("presented frame is black");
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return 0;
}
#endif

int main(int argc, char** argv) {
    if (argc != 3) {
        return fail("usage");
    }
    const int expect_offset = strcmp(argv[2], "offset") == 0;
    legato_engine* engine = legato_engine_create("{\"mode\":\"system\"}");
    legato_player* player = legato_player_create(engine, "{\"preset\":\"vod\"}");
    if (engine == NULL || player == NULL) {
        return fail("create");
    }
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = argv[1];
    if (legato_player_load(player, &item) != 0 || legato_player_play(player) != 0) {
        return fail("load");
    }

    int64_t samples[256];
    int count = 0;
    for (int spin = 0; spin < 400 && count < 12; ++spin) {
        const int got = legato_player_copy_av_sync_us(player, samples, 256);
        if (got > count) {
            count = got < 256 ? got : 256;
        }
        const struct timespec pause = {0, 50000000};
        nanosleep(&pause, NULL);
    }
    if (count < 12) {
        fprintf(stderr, "FAIL av_sync_file: only %d samples\n", count);
        return 1;
    }
#ifdef LEGATO_HAS_EGL
    if (!expect_offset && draw_once(player) != 0) {
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }
#else
    if (!expect_offset && legato_render_create(player, NULL) != NULL) {
        return fail("null params created a context");
    }
#endif
    legato_session_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = (uint32_t)sizeof(stats);
    if (legato_player_read_stats(player, &stats) != 0) {
        return fail("stats");
    }
    const int64_t p95 = percentile_95(samples, count);
    fprintf(stderr, "av_sync_file samples=%d p95_us=%lld dropped=%d\n", count, (long long)p95, stats.dropped_frames);
    legato_player_release(player);
    legato_engine_destroy(engine);
    if (expect_offset) {
        if (p95 < 40000) {
            return fail("offset did not move p95");
        }
        return 0;
    }
    if (p95 >= 40000) {
        return fail("p95 is not under 40ms");
    }
    return 0;
}
