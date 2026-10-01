// ss_assets — the NucleoCast page and PC script, stored raw-deflated in flash (CMakeLists packs
// them at build time) and inflated once into PSRAM by the ROM's tinfl on first use: ~40 KB of
// flash saved, no decompressor linked.
#include "ss_assets.h"

#include "esp_heap_caps.h"
#include "miniz.h"
#include "nv_mem_attr.h"

#include <string.h>

extern const uint8_t cast_z_start[] asm("_binary_cast_html_z_start");
extern const uint8_t cast_z_end[] asm("_binary_cast_html_z_end");
extern const uint8_t py_z_start[] asm("_binary_nucleocast_py_z_start");
extern const uint8_t py_z_end[] asm("_binary_nucleocast_py_z_end");

namespace {

struct Asset { char *data; size_t len; };   // inflated copy, PSRAM (internal RAM is full)
NV_PSRAM_BSS Asset s_cast;
NV_PSRAM_BSS Asset s_py;

// Packed format: 4-byte little-endian plain length, then the raw deflate stream.
const char *get(Asset &a, const uint8_t *z, const uint8_t *z_end, size_t *len) {
    if (!a.data) {
        const size_t zl = (size_t)(z_end - z);
        if (zl < 4) return nullptr;
        const size_t plain = (size_t)z[0] | (size_t)z[1] << 8 | (size_t)z[2] << 16 | (size_t)z[3] << 24;
        char *out = (char *)heap_caps_malloc(plain + 1, MALLOC_CAP_SPIRAM);
        tinfl_decompressor *d = (tinfl_decompressor *)heap_caps_malloc(sizeof *d, MALLOC_CAP_SPIRAM);
        if (!out || !d) { heap_caps_free(out); heap_caps_free(d); return nullptr; }
        size_t in_len = zl - 4, out_len = plain;
        tinfl_init(d);
        const tinfl_status st = tinfl_decompress(d, z + 4, &in_len, (uint8_t *)out, (uint8_t *)out, &out_len,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        heap_caps_free(d);
        if (st != TINFL_STATUS_DONE || out_len != plain) { heap_caps_free(out); return nullptr; }
        out[plain] = 0;
        a.data = out;
        a.len = plain;
    }
    if (len) *len = a.len;
    return a.data;
}

}  // namespace

const char *ss_asset_cast_html(size_t *len) { return get(s_cast, cast_z_start, cast_z_end, len); }
const char *ss_asset_nucleocast_py(size_t *len) { return get(s_py, py_z_start, py_z_end, len); }
