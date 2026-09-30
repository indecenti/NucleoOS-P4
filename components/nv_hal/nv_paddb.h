// nv_paddb — the SDL_GameControllerDB mappings compiled into the firmware (nv_paddb.c, generated
// by tools/gen_paddb.py). Private to nv_pad.c. Each map row is nv_pad_map_t's btn[] then axis[].
#pragma once

#include <stdint.h>

typedef struct {
    uint16_t vid, pid;
    uint16_t map;          // row in maps
} nv_paddb_row_t;

typedef struct {
    const uint8_t        *maps;       // n x width bytes
    uint8_t               width;
    const nv_paddb_row_t *usb;        // sorted by (vid, pid)
    uint16_t              n_usb;
    const nv_paddb_row_t *bt;
    uint16_t              n_bt;
} nv_paddb_t;

extern const nv_paddb_t nv_paddb;
