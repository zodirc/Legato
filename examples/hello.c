#include "legato/legato.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
    if (legato_engine_advance_us(engine, 120000) != 0 || legato_engine_advance_us(engine, 5000000) != 0) {
        return 1;
    }
    const int64_t position = legato_player_position_us(player);
    legato_player_release(player);
    legato_engine_destroy(engine);
    return position == 5000000 ? 0 : 1;
}
