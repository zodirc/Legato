#define _POSIX_C_SOURCE 200809L

#include "legato/legato.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void) {
    legato_engine* engine = legato_engine_create("{\"mode\":\"headless\"}");
    legato_player* player = legato_player_create(engine, NULL);
    legato_media_item item;
    memset(&item, 0, sizeof(item));
    item.struct_size = (uint32_t)sizeof(item);
    item.uri = "corpus://startup/local-1080p-h264";
    if (legato_player_load(player, &item) != 0 || legato_player_play(player) != 0) {
        return 1;
    }
    int ready = 0;
    for (int spin = 0; spin < 3000 && ready < 2; ++spin) {
        const legato_event* event;
        int progress = 0;
        while ((event = legato_player_next_event(player)) != NULL) {
            progress = 1;
            if (event->type == LEGATO_EVENT_TYPE_COMMAND_DONE) {
                ++ready;
            }
        }
        if (!progress) {
            const struct timespec pause = {0, 2000000};
            nanosleep(&pause, NULL);
        }
    }
    if (ready < 2) {
        return 1;
    }
    if (legato_engine_advance_us(engine, 120000) != 0 || legato_engine_advance_us(engine, 5000000) != 0) {
        return 1;
    }
    const int64_t position = legato_player_position_us(player);
    legato_player_release(player);
    legato_engine_destroy(engine);
    return position == 5120000 ? 0 : 1;
}
