#define _POSIX_C_SOURCE 200809L

#include "legato/legato.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fail(const char* text) {
    fprintf(stderr, "FAIL http_playback: %s\n", text);
    return 1;
}

static void pause_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
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

static int wait_open(legato_player* player, legato_session_stats* stats) {
    for (int spin = 0; spin < 300; ++spin) {
        if (state_is(player, "\"error\"")) {
            char* err = NULL;
            if (legato_player_get(player, "last_error", &err) == 0 && err != NULL) {
                fprintf(stderr, "FAIL http_playback: %s\n", err);
                legato_free(err);
            }
            return -1;
        }
        memset(stats, 0, sizeof(*stats));
        stats->struct_size = (uint32_t)sizeof(*stats);
        if (legato_player_read_stats(player, stats) == 0 && stats->first_frame_us >= 0 && stats->stage_dns_us >= 0) {
            return 0;
        }
        pause_ms(50);
    }
    return fail("timed out before first frame");
}

static int run_http(const char* url, int plain) {
    legato_engine* engine = legato_engine_create("{\"mode\":\"system\"}");
    legato_player* player = legato_player_create(engine, "{\"preset\":\"vod\"}");
    if (engine == NULL || player == NULL) {
        return fail("create");
    }
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = url;
    if (legato_player_load(player, &item) != 0 || legato_player_play(player) != 0) {
        return fail("load");
    }
    legato_session_stats stats;
    if (wait_open(player, &stats) != 0) {
        legato_player_release(player);
        legato_engine_destroy(engine);
        return 1;
    }
    fprintf(stderr, "http_playback dns=%lld connect=%lld tls=%lld first_byte=%lld seekable=%d\n",
            (long long)stats.stage_dns_us, (long long)stats.stage_connect_us, (long long)stats.stage_tls_us,
            (long long)stats.stage_first_byte_us, stats.source_seekable);
    if (stats.stage_connect_us <= 0 || stats.stage_first_byte_us <= 0 || stats.stage_tls_us != 0) {
        return fail("network stages");
    }
    if (plain) {
        if (stats.source_seekable != 0) {
            return fail("plain server was marked seekable");
        }
        legato_player_command(player, "{\"cmd\":\"seek\",\"position_us\":1000000}");
        pause_ms(300);
        if (state_is(player, "\"error\"")) {
            return fail("seek entered error");
        }
        char* err = NULL;
        if (legato_player_get(player, "last_error", &err) != 0 || err == NULL || strcmp(err, "null") != 0) {
            legato_free(err);
            return fail("seek stored an error");
        }
        legato_free(err);
    } else if (stats.source_seekable != 1) {
        return fail("range server was not seekable");
    }
    legato_player_release(player);
    legato_engine_destroy(engine);
    return 0;
}

struct MemFile {
    uint8_t* data;
    int64_t size;
    int64_t pos;
};

static int mem_read(void* userdata, uint8_t* dst, int size) {
    struct MemFile* file = userdata;
    if (file->pos >= file->size) {
        return 0;
    }
    int64_t left = file->size - file->pos;
    int got = left < size ? (int)left : size;
    memcpy(dst, file->data + file->pos, (size_t)got);
    file->pos += got;
    return got;
}

static int64_t mem_seek(void* userdata, int64_t offset, int whence) {
    struct MemFile* file = userdata;
    int64_t next = file->pos;
    if (whence == 0) {
        next = offset;
    } else if (whence == 1) {
        next = file->pos + offset;
    } else if (whence == 2) {
        next = file->size + offset;
    } else {
        return -1;
    }
    if (next < 0 || next > file->size) {
        return -1;
    }
    file->pos = next;
    return file->pos;
}

static int64_t mem_length(void* userdata) {
    return ((struct MemFile*)userdata)->size;
}

static void write_u16(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static int run_callback(void) {
    const int rate = 48000;
    const int frames = rate / 5;
    const uint32_t pcm_bytes = (uint32_t)frames * 4;
    struct MemFile file;
    memset(&file, 0, sizeof(file));
    file.size = 44 + (int64_t)pcm_bytes;
    file.data = calloc(1, (size_t)file.size);
    if (file.data == NULL) {
        return fail("alloc");
    }
    memcpy(file.data, "RIFF", 4);
    write_u32(file.data + 4, 36 + pcm_bytes);
    memcpy(file.data + 8, "WAVE", 4);
    memcpy(file.data + 12, "fmt ", 4);
    write_u32(file.data + 16, 16);
    write_u16(file.data + 20, 1);
    write_u16(file.data + 22, 2);
    write_u32(file.data + 24, (uint32_t)rate);
    write_u32(file.data + 28, (uint32_t)rate * 4);
    write_u16(file.data + 32, 4);
    write_u16(file.data + 34, 16);
    memcpy(file.data + 36, "data", 4);
    write_u32(file.data + 40, pcm_bytes);

    legato_engine* engine = legato_engine_create("{\"mode\":\"system\"}");
    legato_source_vtable vtable;
    memset(&vtable, 0, sizeof(vtable));
    vtable.struct_size = (uint32_t)sizeof(vtable);
    vtable.read = mem_read;
    vtable.seek = mem_seek;
    vtable.length = mem_length;
    if (engine == NULL || legato_register_source(engine, "mem", &vtable, &file) != 0) {
        free(file.data);
        return fail("register");
    }
    legato_player* player = legato_player_create(engine, "{\"preset\":\"vod\"}");
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "mem://tone.wav";
    if (player == NULL || legato_player_load(player, &item) != 0 || legato_player_play(player) != 0) {
        free(file.data);
        return fail("callback load");
    }
    int ended = 0;
    for (int spin = 0; spin < 200; ++spin) {
        if (state_is(player, "\"error\"")) {
            free(file.data);
            return fail("callback entered error");
        }
        if (state_is(player, "\"ended\"")) {
            ended = 1;
            break;
        }
        pause_ms(50);
    }
    legato_player_release(player);
    legato_engine_destroy(engine);
    free(file.data);
    if (!ended) {
        return fail("callback did not reach ended");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "callback") == 0) {
        return run_callback();
    }
    if (argc == 3 && (strcmp(argv[2], "range") == 0 || strcmp(argv[2], "plain") == 0)) {
        return run_http(argv[1], strcmp(argv[2], "plain") == 0);
    }
    return fail("usage");
}
