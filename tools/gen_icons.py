#!/usr/bin/env python3
# gen_icons.py — rasterize selected flat-color SVGs to LVGL 9 ARGB8888 icons.
# Output: components/nv_ui/generated/nv_icons.c + components/nv_ui/include/nv_icons.h
#
# The pixels are stored raw-deflate compressed (~24 KB for all icons instead of ~650 KB of
# ARGB8888) and inflated into one PSRAM block by nv_icons_init() at boot, with the ROM's tinfl.
#
#   python tools/gen_icons.py            # render the SVGs below (needs PyMuPDF: pip install pymupdf)
#   python tools/gen_icons.py --from-c   # re-emit the pixels already in nv_icons.c (no SVG render)
import os, re, sys, zlib

ROOT = r'D:\NucleoV2'
SVG_DIR = os.path.join(ROOT, 'system', 'icons', 'flat-color', 'svg')
MDI_DIR = os.path.join(ROOT, 'system', 'icons', 'mdi', 'svg')
OUT_C = os.path.join(ROOT, 'components', 'nv_ui', 'generated', 'nv_icons.c')
OUT_H = os.path.join(ROOT, 'components', 'nv_ui', 'include', 'nv_icons.h')
SIZE = 80

# (svg basename, C identifier[, opts]). opts: {'dir': 'mdi', 'tint': (r,g,b)} to render a
# monochrome MDI glyph tinted to a fixed color (keeps antialiased alpha).
REC_RED = (232, 65, 78)
ICONS = [
    ('settings', 'settings'),
    ('opened_folder', 'files'),
    ('assistant', 'anima'),
    ('combo_chart', 'diag'),
    ('command_line', 'terminal'),
    ('gallery', 'gallery'),
    ('music', 'music'),
    ('camera', 'camera'),
    ('calculator', 'calc'),
    ('todo_list', 'tasks'),
    ('document', 'notes'),   # Notes app (was reusing the todo/tasks icon)
    ('shop', 'apps'),        # Apps store (WASM app manager) — colorful storefront
    ('puzzle', 'wasm'),      # installed WASM app tiles
    ('multiple_devices', 'screen'),   # Second Screen (USB extended display) — laptop + 2nd device
    ('film', 'video'),       # Video player (MJPEG/H.264)
    ('microphone', 'recorder', {'dir': 'mdi', 'tint': REC_RED}),   # Voice Recorder (MDI, tinted)
    ('area_chart', 'sysmon'),   # System Monitor (task manager) — perf-graph look
    # ---- WASM app icons (mdi, tinted) — mapped to installed apps by id in apps_app.cpp ----
    ('timer',                'wtimer',  {'dir': 'mdi', 'tint': (84, 162, 255)}),   # Timer
    ('flashlight',           'wtorch',  {'dir': 'mdi', 'tint': (255, 200, 60)}),   # Torch
    ('piano',                'wpiano',  {'dir': 'mdi', 'tint': (150, 120, 255)}),  # Pianino
    ('target',               'wcannon', {'dir': 'mdi', 'tint': (240, 120, 60)}),   # Cannon
    ('tank',                 'wtank',   {'dir': 'mdi', 'tint': (120, 150, 110)}),  # Tanks
    ('alphabetical-variant', 'wabc',    {'dir': 'mdi', 'tint': (255, 193, 7)}),    # ABC 123
    ('code-braces',          'wcode',   {'dir': 'mdi', 'tint': (46, 196, 198)}),   # Ciao SDK
    ('gamepad-variant',      'wgame',   {'dir': 'mdi', 'tint': (232, 84, 140)}),   # generic game fallback
    ('bug',                  'wbug',    {'dir': 'mdi', 'tint': (150, 156, 172)}),  # Wedge (test)
]


def render_bgra(svg_path, size, tint=None):
    """Render an SVG centered on a size×size transparent canvas, return BGRA bytes.
    If `tint` (r,g,b) is given, force every opaque pixel to that color (for mono MDI glyphs),
    keeping the antialiased alpha so edges stay smooth."""
    import fitz  # PyMuPDF — only needed when rendering from SVG
    doc = fitz.open(svg_path)
    page = doc[0]
    zoom = size / max(page.rect.width, page.rect.height)
    pix = page.get_pixmap(matrix=fitz.Matrix(zoom, zoom), alpha=True)
    src = pix.samples  # RGBA, pix.width×pix.height
    sw, sh, n = pix.width, pix.height, pix.n
    ox, oy = (size - sw) // 2, (size - sh) // 2
    out = bytearray(size * size * 4)  # zero => transparent
    for y in range(sh):
        for x in range(sw):
            si = (y * sw + x) * n
            r, g, b, a = src[si], src[si + 1], src[si + 2], (src[si + 3] if n == 4 else 255)
            if tint and a > 0:
                r, g, b = tint
            di = ((oy + y) * size + (ox + x)) * 4
            out[di + 0] = b  # LVGL ARGB8888 memory order = B,G,R,A
            out[di + 1] = g
            out[di + 2] = r
            out[di + 3] = a
    return bytes(out)


def pixels_from_c():
    """Icon pixels already in OUT_C: raw arrays (pre-compression format) or raw-deflate blobs."""
    src = open(OUT_C, encoding='utf-8', errors='replace').read()
    nums = lambda body: bytes(int(x) for x in re.findall(r'\d+', body))
    px = {n: nums(b) for n, b in re.findall(r'nv_icon_(\w+)_map\[\] = \{(.*?)\};', src, re.S)}
    for n, b in re.findall(r'nv_icon_(\w+)_z\[\] = \{(.*?)\};', src, re.S):
        px[n] = zlib.decompress(nums(b), -15)
    return px


def emit(from_c=False):
    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
    old = pixels_from_c() if from_c else None
    nbytes = SIZE * SIZE * 4
    names, raw_total, z_total = [], 0, 0

    body = []
    for entry in ICONS:
        svg, name = entry[0], entry[1]
        opts = entry[2] if len(entry) > 2 else {}
        if from_c:
            if name not in old:
                sys.exit(f'--from-c: nv_icon_{name} not found in {OUT_C}')
            data = old[name]
        else:
            base = MDI_DIR if opts.get('dir') == 'mdi' else SVG_DIR
            data = render_bgra(os.path.join(base, svg + '.svg'), SIZE, opts.get('tint'))
        assert len(data) == nbytes, f'{name}: {len(data)} bytes, expected {nbytes}'
        co = zlib.compressobj(9, zlib.DEFLATED, -15)   # raw deflate: what tinfl reads without a zlib header
        z = co.compress(data) + co.flush()
        body.append(f'static const uint8_t nv_icon_{name}_z[] = {{\n')
        for i in range(0, len(z), 24):
            body.append('  ' + ','.join(str(b) for b in z[i:i + 24]) + ',\n')
        body.append('};\n')
        names.append(name)
        raw_total += len(data)
        z_total += len(z)
        print(f'  nv_icon_{name}: {len(data)} -> {len(z)} bytes')

    with open(OUT_C, 'w', newline='\n') as c:
        c.write('// AUTO-GENERATED by tools/gen_icons.py — do not edit.\n')
        c.write('// Launcher/app icons (icons8 flat-color + tinted MDI, MIT), 80x80 LVGL 9 ARGB8888. The pixels\n')
        c.write('// are raw-deflate compressed here and inflated into one PSRAM block by nv_icons_init(), using\n')
        c.write("// the ROM's tinfl, so no decompressor costs flash.\n")
        c.write('#include "nv_icons.h"\n#include "esp_attr.h"\n#include "esp_heap_caps.h"\n#include "miniz.h"\n\n')
        c.write(f'#define NV_ICON_SIZE  {SIZE}\n#define NV_ICON_BYTES (NV_ICON_SIZE * NV_ICON_SIZE * 4)\n\n')
        c.write(''.join(body))
        c.write('\n// Descriptors in PSRAM (zeroed at boot); nv_icons_init() fills them in.\n')
        for n in names:
            c.write(f'EXT_RAM_BSS_ATTR lv_image_dsc_t nv_icon_{n};\n')
        c.write('\nstatic const struct { lv_image_dsc_t *dsc; const uint8_t *z; uint32_t z_len; } s_pack[] = {\n')
        for n in names:
            c.write(f'    {{ &nv_icon_{n}, nv_icon_{n}_z, sizeof(nv_icon_{n}_z) }},\n')
        c.write('};\n\n')
        c.write('''bool nv_icons_init(void) {
    static bool s_done = false;
    if (s_done) return true;
    const size_t n = sizeof(s_pack) / sizeof(s_pack[0]);
    // One 64-byte-aligned PSRAM block (cache-line aligned, like the old per-icon mirror).
    uint8_t *px = heap_caps_aligned_alloc(64, n * NV_ICON_BYTES, MALLOC_CAP_SPIRAM);
    tinfl_decompressor *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
    if (!px || !d) {
        heap_caps_free(px);
        heap_caps_free(d);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        uint8_t *out = px + i * NV_ICON_BYTES;
        size_t in_len = s_pack[i].z_len, out_len = NV_ICON_BYTES;
        tinfl_init(d);
        const tinfl_status st = tinfl_decompress(d, s_pack[i].z, &in_len, out, out, &out_len,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (st != TINFL_STATUS_DONE || out_len != NV_ICON_BYTES) {
            ok = false;   // leave this descriptor zeroed: LVGL skips it instead of drawing garbage
            continue;
        }
        lv_image_dsc_t *dsc = s_pack[i].dsc;
        dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
        dsc->header.w = NV_ICON_SIZE;
        dsc->header.h = NV_ICON_SIZE;
        dsc->header.stride = NV_ICON_SIZE * 4;
        dsc->data_size = NV_ICON_BYTES;
        dsc->data = out;
    }
    heap_caps_free(d);
    s_done = true;
    return ok;
}
''')

    with open(OUT_H, 'w', newline='\n') as h:
        h.write('// AUTO-GENERATED by tools/gen_icons.py — do not edit.\n')
        h.write('// Launcher/app icons, 80x80 LVGL 9 ARGB8888. The descriptors live in PSRAM and are empty until\n')
        h.write('// nv_icons_init() inflates them: call it once at boot, before any UI uses an icon.\n')
        h.write('#pragma once\n#include <stdbool.h>\n#include "lvgl.h"\n\n#ifdef __cplusplus\nextern "C" {\n#endif\n\n')
        for n in names:
            h.write(f'extern lv_image_dsc_t nv_icon_{n};\n')
        h.write('\n// Inflate every icon into PSRAM (idempotent). False if memory was short or an icon failed\n')
        h.write('// to decode; the icons that did decode are usable either way.\n')
        h.write('bool nv_icons_init(void);\n')
        h.write('\n#ifdef __cplusplus\n}\n#endif\n')
    print(f'{len(names)} icons: {raw_total} -> {z_total} bytes compressed')
    print(f'wrote {OUT_C}\nwrote {OUT_H}')


if __name__ == '__main__':
    emit(from_c='--from-c' in sys.argv)
