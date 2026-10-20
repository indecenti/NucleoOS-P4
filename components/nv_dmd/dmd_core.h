// dmd_core — the pure part of the DMD app (no ESP-IDF, FreeRTOS or LVGL): everything a PC on the LAN
// can send goes through here, so it is unit-tested and fuzzed in tests/host (target `dmd`).
//
//   1. WebSocket server side (RFC 6455): the HTTP upgrade handshake and an incremental frame parser
//      that unmasks client frames and reassembles fragmented messages. dmdext's client
//      (websocket-sharp) splits every message longer than 1016 bytes into continuation frames.
//   2. The dmdext "networkstream" messages (LibDmd/Output/Network/WebsocketSerializer.cs, read at
//      freezy/dmd-extensions e54b239): "<name>\0<payload>", little-endian int32s, colours 0x00RRGGBB,
//      bit-planes LSB-first (plane k = bit k of the level, bit 0 of a byte = leftmost pixel), turned
//      into one 0x00RRGGBB value per DMD pixel.
//   3. The dot renderer: full panel or square dots, rotation, pre-computed dot masks (round or
//      square, optional glow), a cache of finished dots, and the RGB565 fill of a panel band.
//   4. A 5x7 text renderer (the idle clock and the test screen).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 1. WebSocket ----------------------------------------------------------------------------------

typedef enum {
    DMD_HS_INCOMPLETE = 0,   // the request headers are not complete yet: read more
    DMD_HS_OK,               // valid upgrade: send resp[0..resp_len), then WebSocket frames follow
    DMD_HS_BAD,              // not a WebSocket upgrade (or too long): send resp (a 400) and close
} dmd_hs_t;

// Parse the HTTP upgrade request in req[0..n). On DMD_HS_OK *consumed is the size of the request
// (bytes after it already belong to the WebSocket stream). Any path is accepted (dmdext's URL is the
// user's choice). resp gets the 101 (or 400) response, NUL-terminated, cap >= 256.
dmd_hs_t dmd_ws_handshake(const char *req, size_t n, size_t *consumed, char *resp, size_t cap,
                          size_t *resp_len);

// Sec-WebSocket-Accept for a key: base64(SHA-1(key + GUID)), 28 chars + NUL.
void dmd_ws_accept(const char *key, size_t key_len, char out[29]);

typedef enum {
    DMD_WS_MORE = 0,   // need more bytes (nothing consumed beyond *used)
    DMD_WS_MESSAGE,    // a complete data message is in ws->msg[0..ws->msg_len)
    DMD_WS_PING,       // ping: answer with a pong carrying ws->ctl[0..ws->ctl_len)
    DMD_WS_CLOSE,      // the client closes: answer with a close and drop the connection
    DMD_WS_CONTROL,    // a pong or other frame consumed with nothing to do
    DMD_WS_ERROR,      // protocol violation or a message over msg_cap: drop the connection
} dmd_ws_ev_t;

typedef struct {
    uint8_t *msg;       // reassembly buffer (caller-owned)
    size_t   msg_cap;
    size_t   msg_len;
    bool     in_msg;    // a fragmented message is being reassembled
    bool     done;      // msg holds a delivered message: the next data frame starts a new one
    uint8_t  ctl[125];  // payload of the last ping
    size_t   ctl_len;
} dmd_ws_t;

void dmd_ws_init(dmd_ws_t *ws, uint8_t *msg_buf, size_t cap);
// Parse at most one frame from buf[0..n). buf is modified (unmasked in place). *used = bytes
// consumed (0 with DMD_WS_MORE). Call again with the rest until DMD_WS_MORE.
dmd_ws_ev_t dmd_ws_next(dmd_ws_t *ws, uint8_t *buf, size_t n, size_t *used);
// Header of an unmasked server frame (FIN set). Returns its size (2, 4 or 10).
size_t dmd_ws_header(uint8_t opcode, size_t len, uint8_t out[10]);

// ---- 2. dmdext networkstream -------------------------------------------------------------------------

#define DMD_MAX_W   512
#define DMD_MAX_H   256
#define DMD_MAX_PX  (256 * 128)   // 32768 dots: 4x the largest real DMD (256x64)
#define DMD_PAL_MAX 64

typedef enum {
    DMD_FMT_NONE = 0, DMD_FMT_GRAY2, DMD_FMT_GRAY4, DMD_FMT_CGRAY2, DMD_FMT_CGRAY4, DMD_FMT_RGB24,
} dmd_fmt_t;

typedef enum {
    DMD_MSG_IGNORED = 0,   // unknown name (a newer dmdext): skipped, never an error
    DMD_MSG_META,          // gameName / dimensions / color / palette / clear*: state updated
    DMD_MSG_FRAME,         // a frame was decoded into the output buffer
    DMD_MSG_BAD,           // malformed (too short, sizes out of range): dropped
} dmd_msg_t;

typedef struct {
    int       w, h;                  // current dimensions (0 until a valid "dimensions")
    uint32_t  color;                 // monochrome colour from the PC, 0x00RRGGBB (dmdext default OrangeRed)
    int32_t   color_override;        // -1: use the PC's colour; else 0x00RRGGBB for gray frames
    uint32_t  pal[DMD_PAL_MAX];      // palette from "palette" (applies to gray2/gray4 frames)
    int       pal_n;                 // 0 = none (monochrome)
    char      game[48];              // ROM name, printable ASCII
    dmd_fmt_t last_fmt;
} dmd_state_t;

void dmd_state_init(dmd_state_t *s);
// Decode one dmdext message m[0..n). A frame is written to out (out_cap pixels, 0x00RRGGBB, row-major
// from the top-left) and needs w*h <= out_cap.
dmd_msg_t dmd_proto_feed(dmd_state_t *s, const uint8_t *m, size_t n, uint32_t *out, size_t out_cap);
const char *dmd_fmt_name(dmd_fmt_t f);

// ---- 2b. ZeDMD over USB serial --------------------------------------------------------------------------
// The board answers as a ZeDMD (PPUC/ZeDMD firmware 5.1.8, as libzedmd 0.11.0 in dmdext 2.5 drives it), so
// dmdext's [zedmd] output finds it on its own on the USB port (Espressif USB-Serial-JTAG, 303A:1001):
//   handshake: "FRAME" "ZeDMD" 0x0C 00 00 00 -> 58-byte reply ("ZeDM", w, h, version, package size, ...,
//              [57] = 'R') + "ZeDMDA"
//   transfer:  "FRAME" then commands "ZeDMD" cmd size_hi size_lo compressed payload, cut by the PC into
//              chunks of the advertised package size (the last one zero-padded); every chunk is answered
//              "ZeDMDA" ("ZeDMD" 'F' = error: the PC resends a full frame next time)
//   0x05 RGB565 zones (zlib when flagged): [idx][zw*zh*2 bytes LE RGB565] or [idx+128] = black zone,
//        16 x 8 zones; 0x06 render; 0x0A clear; 0x0B keep-alive; 0x1F reset (the PC closes); anything
//        else ends the transfer with an ack, like the firmware's default.

#define DMD_Z_PAY_MAX 2048   // one command's payload (libzedmd sends <= 1040 bytes before compression)
#define DMD_Z_RAW_MAX 4096   // one zones packet once inflated

// Inflate zlib data (with its header) into out. Returns the bytes produced, or -1.
typedef int (*dmd_inflate_fn)(void *ctx, const uint8_t *in, size_t n, uint8_t *out, size_t cap);

enum {
    DMD_Z_EV_RENDER = 1,   // img holds a new picture
    DMD_Z_EV_HELLO  = 2,   // a PC completed the handshake
    DMD_Z_EV_BYE    = 4,   // the PC closed (reset command)
    DMD_Z_EV_ERROR  = 8,   // a malformed command was refused ('F')
};

typedef struct {
    int       w, h, zw, zh;     // panel and zone size (16 x 8 zones)
    uint16_t  chunk;            // package size advertised in the handshake
    uint32_t *img;              // w*h 0x00RRGGBB: the picture, updated zone by zone (caller-owned)
    dmd_inflate_fn inflate;
    void     *inflate_ctx;
    // stream state
    int       st;               // 0: looking for "FRAME", 1: inside a transfer
    int       match;            // "FRAME" characters matched
    uint32_t  chunk_left;       // bytes left in the current chunk
    bool      done;             // the transfer is over: the rest of the chunk is padding
    bool      err;
    int       ctrl;             // "ZeDMD" characters matched
    int       hdr;              // header bytes after "ZeDMD" (cmd, size hi, size lo, flag)
    uint8_t   cmd;
    bool      comp;
    uint16_t  size, got;
    uint32_t  cmds;             // commands parsed so far, keep-alives included (a PC that is still there)
    uint8_t   pay[DMD_Z_PAY_MAX];
    uint8_t   raw[DMD_Z_RAW_MAX];
} dmd_zedmd_t;

// w x h: 128x32 or 256x64 (multiples of 16 x 8). chunk: 32..1920, a multiple of 32.
void dmd_zedmd_init(dmd_zedmd_t *z, int w, int h, uint16_t chunk, uint32_t *img, dmd_inflate_fn inflate, void *ctx);
// Back to looking for "FRAME" (the PC gave up on a transfer: call after a pause in the middle of one).
void dmd_zedmd_reset(dmd_zedmd_t *z);
// Feed received bytes. Replies (the handshake answer, chunk acks) are appended to out[*out_len..out_cap)
// and must be sent in order; out_cap >= 64 per chunk the input can complete. Returns DMD_Z_EV_* bits.
int dmd_zedmd_feed(dmd_zedmd_t *z, const uint8_t *in, size_t n, uint8_t *out, size_t out_cap, size_t *out_len);
// True while inside a transfer (a pause here means the PC gave up on it).
static inline bool dmd_zedmd_busy(const dmd_zedmd_t *z) { return z->st == 1; }

// ---- 2c. The menu link (board <-> our PC bridge for PinUP Popper, on the same USB port) ----------------
// While no table runs, the bridge (tools/popper_bridge) owns the USB port and talks to the board in
// messages: "NVDM" type len(u32 LE) payload sum(u16 LE: the 16-bit sum of type, len and payload bytes).
// Payloads are JSON, except type 'I' (an image): u16 LE json_len, the JSON header, then raw pixels.
// The same bytes also go through the ZeDMD parser (dmdext takes the port while a table runs): a stray
// "FRAME" there only costs an ack nobody reads; the bridge ignores everything that is not "NVDM".

#define DMD_LINK_MAX (1024 * 600 * 2 + 4096)   // the largest message: a full-panel RGB565 image + header

typedef void (*dmd_link_cb)(void *ctx, uint8_t type, const uint8_t *payload, uint32_t len);

typedef struct {
    uint8_t *buf;          // payload buffer (caller-owned, cap bytes)
    uint32_t cap;
    int      st;           // 0: magic, 1: type+len, 2: payload, 3: checksum
    int      n;            // bytes of the current field
    uint8_t  hdr[5];
    uint8_t  sumb[2];
    uint32_t len, got;
    uint16_t sum;
    uint32_t bad;          // frames dropped (checksum, too long, cut short)
    uint32_t ok;           // frames delivered
    uint8_t  bad_type;     // the last dropped frame: its type and length
    uint32_t bad_len;
    uint32_t resync;       // dropped frames that held the next frame's start (bytes lost): it was saved
    uint32_t lost;         // the last of those: how many bytes were missing
} dmd_link_t;

void dmd_link_init(dmd_link_t *l, uint8_t *buf, uint32_t cap);
// Feed received bytes; cb runs once per complete, valid message. A frame whose checksum fails because bytes
// went missing has run into the next frame: that frame's start is looked for in its payload and trailer
// (or, at the very end, the first bytes of one) and fed again. One level: a frame that fails among those
// bytes is only dropped.
void dmd_link_feed(dmd_link_t *l, const uint8_t *in, size_t n, dmd_link_cb cb, void *ctx);
// Back to looking for "NVDM", counters kept: call when a message stops arriving half-way (the sender gave up
// or bytes were lost in its length). True when one was cut short: counted in bad (bad_type / bad_len: 0 when
// its header had not come yet).
bool dmd_link_reset(dmd_link_t *l);
// Inside a message (its header or payload has started).
static inline bool dmd_link_busy(const dmd_link_t *l) { return l->st != 0; }
// The 9-byte header and the 2-byte trailer of a message (write header, payload, trailer).
void dmd_link_header(uint8_t type, uint32_t len, uint8_t out[9]);
void dmd_link_trailer(uint8_t type, uint32_t len, const uint8_t *payload, uint8_t out[2]);

// ---- 3. Renderer ---------------------------------------------------------------------------------------
//
// Two ways to fit the DMD on the panel:
//   DMD_FIT_FILL    the whole panel: cells are rectangles (128x32 -> 8 x 18.75 px) holding the same round
//                   dot (never stretched: more dark between the rows), and so that every panel pixel is
//                   used, columns / rows differ by at most one pixel (128x32 -> 24 rows of 19 px, 8 of 18);
//   DMD_FIT_SQUARE  square dots, the largest integer size that fits, centred (128x32 -> 8 px, a
//                   1024x256 band).
// Cell c spans x = ox + dmd_cell_x(g, c) .. ox + dmd_cell_x(g, c + 1) - 1 (rows alike), so a dot is pw or
// pw + 1 wide and ph or ph + 1 tall: four dot shapes, one mask each.

enum { DMD_FIT_FILL = 0, DMD_FIT_SQUARE = 1 };

#define DMD_MAX_SIDE (DMD_MAX_W > DMD_MAX_H ? DMD_MAX_W : DMD_MAX_H)

typedef struct {
    int pw, ph;        // the smaller dot size (some dots are one pixel wider / taller in FILL)
    int cols, rows;    // dots across / down on the panel (after rotation)
    int gw, gh;        // grid size in pixels
    int ox, oy;        // top-left of the grid on the panel
    // Where each column / row starts, relative to ox / oy (entry cols / rows = gw / gh): computed once
    // here, because the renderer needs them for every dot of every panel row (a 64-bit division each
    // was 30 ms a frame on the RV32 core).
    int16_t x[DMD_MAX_SIDE + 1];
    int16_t y[DMD_MAX_SIDE + 1];
} dmd_geom_t;   // ~2 KB

#define DMD_PITCH_MAX   64   // largest dot side, mask storage

// rot: 0..3 = 0/90/180/270 degrees clockwise. False when the DMD does not fit (a dot under 1 px) or w/h
// are out of range.
bool dmd_geom(int w, int h, int rot, int fit, int scr_w, int scr_h, dmd_geom_t *g);
static inline int dmd_cell_x(const dmd_geom_t *g, int c) { return g->x[c]; }   // c in 0..cols
static inline int dmd_cell_y(const dmd_geom_t *g, int r) { return g->y[r]; }   // r in 0..rows

#define DMD_MASK_LEVELS 16   // mask values 0..15 = coverage / 15
// 16..23: inside a dot, its hot core: full coverage and a step towards white (a lit LED's bright centre),
// as much as the dot itself is lit (an unlit dot stays dark). The renderers' tables span all of them.
#define DMD_CORE_LEVELS 8
#define DMD_MASK_VALUES (DMD_MASK_LEVELS + DMD_CORE_LEVELS)
#define DMD_CORE_WHITE 102   // the centre's step towards white, of 256 (40%) for a dot at full colour

enum { DMD_SHAPE_ROUND = 0, DMD_SHAPE_SQUARE = 1 };

// Fill mask[w*h] (a cell w x h) with the dot's coverage (0..15) and its hot core (16..23): an anti-aliased
// round dot 90% of `dot` px wide (square: 84%), centred, never stretched (a rectangular cell only leaves more
// dark around it); glow adds a halo in the gap. w, h 1..DMD_PITCH_MAX; dot <= min(w, h) (0 or larger:
// min(w, h)).
void dmd_mask_build(uint8_t *mask, int w, int h, int dot, int shape, bool glow);

// The four masks of a geometry: m[v], v = (wider ? 1 : 0) | (taller ? 2 : 0), each (pw + (v & 1)) wide,
// all with the same dot (min(pw, ph)): round dots stay round on the whole-panel fit.
typedef struct {
    int     pw, ph;
    uint8_t m[4][DMD_PITCH_MAX * DMD_PITCH_MAX];
} dmd_masks_t;
void dmd_masks_build(dmd_masks_t *mk, const dmd_geom_t *g, int shape, bool glow);

// Draw frame px (w x h, 0x00RRGGBB) as dots into dst, a panel band whose row 0 is panel row g->oy:
// rows 0..g->gh-1, stride pixels per row, columns g->ox..g->ox+g->gw-1 (the rest of each row is
// cleared to black when clear_margins). bright 1..100 (%).
void dmd_render(uint16_t *dst, int stride, const dmd_geom_t *g, const dmd_masks_t *mk, const uint32_t *px,
                int w, int h, int rot, int bright, bool clear_margins, int scr_w);

// A cache of finished dots ("sprites", one per colour and dot shape) so a frame costs a few word copies
// per dot row instead of a table look-up per pixel. Valid for one set of masks and one brightness:
// dmd_sprites_reset() when the masks change (dot size and brightness are checked by the renderer). Dots
// up to DMD_SPRITE_AREA px; bigger ones are drawn pixel by pixel. ~90 KB: allocate it (PSRAM is fine).
#define DMD_SPRITE_AREA   1024
#define DMD_SPRITE_PX     32768      // sprite storage: 512 dots of 8x8, 128 of 9x19
#define DMD_SPRITE_SLOTS  2048
typedef struct {
    int      pw, ph, bright;
    int      shift, nslots;         // slots = DMD_SPRITE_PX / ((pw+1)*(ph+1)) (a power of two), hash shift
    uint32_t row_id;                // the dot row being drawn: a slot it uses is not given away
    uint32_t key[DMD_SPRITE_SLOTS]; // colour | shape << 24 | 1 << 26 (0 = empty)
    uint32_t stamp[DMD_SPRITE_SLOTS];
    uint32_t col[DMD_MAX_W];        // scratch for one dot row: colours and their sprites (NULL = none)
    const uint16_t *row[DMD_MAX_W];
    uint16_t px[DMD_SPRITE_PX];
} dmd_sprites_t;
void dmd_sprites_reset(dmd_sprites_t *sc);

// Same for dot rows [cy0, cy1) only: dst row 0 is band row dmd_cell_y(g, cy0) (a strip the caller then
// copies into place). Rows outside 0..g->rows are clamped. sc may be NULL (no cache: slower, same pixels).
void dmd_render_rows(uint16_t *dst, int stride, const dmd_geom_t *g, const dmd_masks_t *mk, const uint32_t *px,
                     int w, int h, int rot, int bright, bool clear_margins, int scr_w, int cy0, int cy1,
                     dmd_sprites_t *sc);

// A monochrome frame (every lit pixel a darker copy of the brightest one, as dmdext sends a ZeDMD a plain
// orange DMD) redrawn in `to`, each pixel keeping its level; false, `dst` untouched, for a frame in colours
// (ColorDMD, Serum, a coloured palette) or an empty one. `dst` may be `px`.
bool dmd_mono_recolor(const uint32_t *px, size_t n, uint32_t to, uint32_t *dst);

// ---- Rebuilt DMD: twice as many rows
// The frame px (w x h, 0x00RRGGBB) with twice its rows, into dst (w x 2h): the rows in between are made
// with Scale2x's rules (each dot becomes 2x2 following the outlines of what it belongs to, so diagonals
// and curves stay smooth instead of turning into steps) except at a filled corner (right angles stay
// square: the letters' L, E, T...), and the two halves of each new dot merged, the lit one winning. False when 2h is over DMD_MAX_H or w*2h over DMD_MAX_PX (dst untouched).
bool dmd_scale2x_rows(const uint32_t *px, int w, int h, uint32_t *dst);
// The rebuilt picture (w x 2h) of rows [r0, r0 + n) of px: they are doubled with dmd_scale2x_rows as many
// times as fit in 2h rows (32 of 32 -> 64; the 16 lit rows of a 128x16 DMD that dmdext centres in 128x32
// -> 64, two passes), centred, black above and below. tmp: w*2h values of scratch. False on bad input
// (dst untouched) or when 2h / w*2h are over DMD_MAX_H / DMD_MAX_PX.
bool dmd_rebuild(const uint32_t *px, int w, int h, int r0, int n, uint32_t *dst, uint32_t *tmp);
// The same into out_rows rows (dst and tmp: w*out_rows values; h <= out_rows <= 2h): doubled while it fits
// in them. With out_rows = h a 128x16 DMD inside 128x32 gets its own 32 rows back (one pass), so every fit
// shows it whole, not as a strip between empty rows.
bool dmd_rebuild_to(const uint32_t *px, int w, int h, int r0, int n, uint32_t *dst, uint32_t *tmp, int out_rows);
// Which rows to rebuild, from the rows lit so far (lit_lo..lit_hi, -1 = none yet): the middle half when
// nothing outside it has ever been lit (a 128x16 DMD inside 128x32), else all of them. Fixed bounds, not
// the lit ones, so the picture never jumps while a game runs.
void dmd_rebuild_rows(int h, int lit_lo, int lit_hi, int *r0, int *n);
// Lit rows of px (w x h): the first and the last row with any dot on; false when it is all black.
bool dmd_lit_rows(const uint32_t *px, int w, int h, int *lo, int *hi);

// ---- Ambient bands: the light the DMD throws above and below it
// For the fits that leave black bands ("DMD only", "Tall DMD"): per panel column (after rotation: w columns,
// or h when rot is odd), the colour the DMD throws on the band above (from its upper half) and below (its
// lower half). Rows nearer the edge weigh more; it is spread sideways over +-columns/16 so it reads as
// light, not as a copy; it is as bright as that area is lit (a few letters throw little light), and white
// or grey throws half (text must not wash the bands out). scratch: as many values as columns.
void dmd_ambient(const uint32_t *px, int w, int h, int rot, uint32_t *top, uint32_t *bot, uint32_t *scratch);
// A band's picture (cols x rows, panel orientation) from those colours: the `gap` rows next to the DMD
// are unlit dots (a margin), then each dot is amb[c] dimmed to 55% and 0.84x more per row further away,
// never darker than floor_rgb (an unlit dot). above: the band over the DMD (its last row touches it), else
// the one under it (its first row does).
void dmd_ambient_band(const uint32_t *amb, int cols, int rows, bool above, int gap, uint32_t floor_rgb, uint32_t *out);

// ---- FullDMD art: the window left for the DMD
// A "FullDMD" picture (made for a big DMD screen: the table's art with a dark window where the DMD goes).
// Finds that window: the largest dark rectangle at least 2:1 wide in px (w x h, RGB565), searched on 4x4
// blocks, then its edges refined pixel by pixel. False when there is none worth using (narrower than a
// third of the picture, lower than a tenth, wider than 6:1, or touching the picture's edge: a dark
// background, not a window). w 16..DMD_WIN_MAX_W, h 16 or more.
#define DMD_WIN_MAX_W 2048
typedef struct {
    int x, y, w, h;
} dmd_rect_t;
bool dmd_find_window(const uint16_t *px, int w, int h, dmd_rect_t *out);
// The DMD's housing in a backglass's grill (px: the grill alone, w x h, RGB565). The backglass is stored
// unlit and housings are often dark grey, not black: the lowest of a few brightness thresholds that gives a
// rectangle at least 2:1, a third of the grill wide or more, centred across it (it may reach the grill's top
// or bottom, not its sides, where the speakers are). False when there is none.
bool dmd_find_housing(const uint16_t *px, int w, int h, dmd_rect_t *out);

// ---- 3b. The second screen and previews ----------------------------------------------------------------
// A cabinet's second screen (the backglass monitor) per table: what B2S shows there (the backglass, with or
// without its grill, and its DMD window with the FullDMD art) and where dmdext puts its virtual DMD. The
// board computes the rectangles from the pictures it has; the bridge writes them where B2S and dmdext read
// them when the table starts.
enum {
    DMD_S2_KEEP = 0,    // as the table came: nothing written (the bridge puts back what it had changed)
    DMD_S2_BG,          // the backglass alone (its grill hidden), the whole screen
    DMD_S2_BG_DMD,      // the backglass above, the DMD below it, the whole width
    DMD_S2_HOUSING,     // the backglass with its grill, the DMD in the grill's housing
    DMD_S2_ART,         // the FullDMD art (B2S's DMD window), the DMD in its window
    DMD_S2_BG_ART,      // the backglass above, the FullDMD art below it with the DMD in its window
    DMD_S2_N
};
typedef struct {
    int sw, sh;            // the second screen
    int bw, bh, grill;     // the backglass picture, whole (any scale; bw 0: none) and its grill's rows (0: none)
    dmd_rect_t housing;    // the DMD's housing in that picture (same pixels; w 0: none)
    int aw, ah;            // the FullDMD art's true shape (any scale; 0: none)
    dmd_rect_t window;     // the DMD's window in it (in aw x ah pixels; w 0: none)
    int dw, dh;            // the DMD's dots (its shape)
} dmd_s2_in_t;
typedef struct {
    dmd_rect_t bg;         // the backglass on the screen
    bool grill;            // shown with its grill
    dmd_rect_t art;        // B2S's DMD window, the FullDMD art (w 0: hidden)
    dmd_rect_t dmd;        // dmdext's virtual DMD (w 0: off)
} dmd_s2_out_t;
// The table has what the composition needs.
bool dmd_s2_avail(int mode, const dmd_s2_in_t *in);
// The composition's rectangles in the second screen's pixels. False for DMD_S2_KEEP or when not available.
// A picture keeps its shape; the backglass alone may fill the screen when that stretches it by DMD_S2_STRETCH %
// or less (the grill never: its speakers stay round).
#define DMD_S2_STRETCH 16
// The DMD in a window or a housing keeps its shape, stretched by this % at most (centred in the rest).
#define DMD_S2_DMD_STRETCH 20
bool dmd_s2_layout(int mode, const dmd_s2_in_t *in, dmd_s2_out_t *out);
// The DMD inside a window or a housing: a thin even margin to its frame (3% of its height, at least 2 px).
dmd_rect_t dmd_inset(dmd_rect_t r);
// Previews (nearest neighbour, clipped to dst): src's rectangle sr (RGB565, sstride pixels a row) into dst's
// rectangle dr; a DMD frame (0x00RRGGBB, w x h) into dr as dots (lit: their colour, unlit: dim; a gap between
// dots once they are 3 px or more); a plain rectangle.
void dmd_blit565(uint16_t *dst, int dw, int dh, dmd_rect_t dr, const uint16_t *src, int sstride, dmd_rect_t sr);
void dmd_blit_frame(uint16_t *dst, int dw, int dh, dmd_rect_t dr, const uint32_t *px, int w, int h);
void dmd_fill565(uint16_t *dst, int dw, int dh, dmd_rect_t r, uint16_t c);

// ---- 4. Text ---------------------------------------------------------------------------------------------
// Draw text into px (w x h, 0x00RRGGBB, cleared first) with a 5x7 font (A-Z, 0-9, space and . , : ! ? - ' /
// + %; lower case is drawn as capitals, anything else as a space), '\n' between lines, each line centred,
// at the largest integer scale that fits. Returns the scale (0: nothing fits).
int dmd_text(uint32_t *px, int w, int h, const char *text, uint32_t color);

#ifdef __cplusplus
}
#endif
