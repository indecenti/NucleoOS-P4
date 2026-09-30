// Chess — you (white) against the computer (black), on the NucleoOS gfx surface.
//
// Engine: mcu-max (MIT, github.com/gissio/mcu-max), vendored as mcu-max.c/.h.
// Rendering: ABI v6 persist mode. Every square/panel element caches what it last drew and is
// redrawn only when that changes, so an idle board costs zero draw calls and a move repaints a few
// squares. Art is pre-rendered (gen_assets.py): each piece is pre-blended onto every square colour
// it can sit on, so a square is ONE anti-aliased image blit.
// While the computer thinks, the engine callback keeps presenting frames (animated dots, buttons,
// back gesture), so the app never looks frozen and the OS wedge watchdog stays fed.
#include "nucleo_sdk.h"
#include "mcu-max.h"

// ---- layout (keep in sync with gen_assets.py) ---------------------------------------------------
#define SQ   70
#define BX   40            // board top-left
#define BY   14
#define PX   624           // right panel
#define PW   380
#define Y_TITLE  18
#define Y_STATUS 86
#define Y_TRAY   166
#define Y_LVLBL  294
#define Y_LEVEL  320
#define Y_BTN    400
#define Y_HINT   492
#define SEG_W    ((PW - 16) / 3)
#define BTN_W    ((PW - 12) / 2)

#define MAXPLY 600

// ---- colours ------------------------------------------------------------------------------------
static int C_BG, C_CARD, C_LIGHT, C_DARK, C_LIGHT_HL, C_DARK_HL, C_MUTED, C_TEXT;
static int C_RING_L, C_RING_D, C_RING_LH, C_RING_DH;

// ---- tiny string helpers (freestanding) -----------------------------------------------------------
static char *sput(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static const char *g_lang = "en";
static char g_nb[32];
static const char *L(const char *base) {   // "<base>_<lang>"
    char *p = sput(g_nb, base); p = sput(p, "_"); sput(p, g_lang); return g_nb;
}

// ---- game state -----------------------------------------------------------------------------------
enum { ST_TURN, ST_THINK, ST_CHECK, ST_WIN, ST_LOSE, ST_STALE, ST_MATERIAL, ST_REPEAT, ST_FIFTY };

static uint8_t  hist[MAXPLY][2];
static uint8_t  hm_clock[MAXPLY + 1];     // half-move clock after each ply (50-move rule)
static uint32_t pos_hash[MAXPLY + 1];     // position after each ply (repetition)
static int      nply;
static uint8_t  bd[128];                  // mirror of the engine board (0x88), mcumax_piece values
static mcumax_move legal[160];
static int      nlegal;
static int      level = 1;                // 0 easy, 1 medium, 2 hard
static int      sel = -1;                 // selected square (0x88) or -1
static int      over;                     // game finished
static int      status_i = ST_TURN;
static int      check_sq = -1;            // king in check (side to move) or -1
static int      last_from = -1, last_to = -1;
static int      quit;

static int white_to_move(void) { return (nply & 1) == 0; }

// ---- chess helpers ----------------------------------------------------------------------------------
static int ptype(int p) { return p & 7; }
static int is_black(int p) { return (p & MCUMAX_BLACK) != 0; }

static void refresh_board(void) {
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int p = mcumax_get_piece((mcumax_square)(r * 16 + c));
            bd[r * 16 + c] = (uint8_t)((p & 7) ? p : 0);   // empty comes back as 0x08 (colour bit)
        }
}

// Is square s attacked by the given side?
static int attacked(int s, int by_black) {
    static const int KN[8] = {14, 18, 31, 33, -14, -18, -31, -33};
    static const int KG[8] = {1, 15, 16, 17, -1, -15, -16, -17};
    for (int i = 0; i < 8; i++) {
        int t = s + KN[i];
        if (!(t & 0x88) && bd[t] && is_black(bd[t]) == by_black && ptype(bd[t]) == MCUMAX_KNIGHT) return 1;
        t = s + KG[i];
        if (!(t & 0x88) && bd[t] && is_black(bd[t]) == by_black && ptype(bd[t]) == MCUMAX_KING) return 1;
    }
    // pawns: a white pawn on p hits p-15/p-17, a black one p+15/p+17
    for (int k = 0; k < 2; k++) {
        int t = by_black ? s - (k ? 15 : 17) : s + (k ? 15 : 17);
        if (!(t & 0x88) && bd[t] && is_black(bd[t]) == by_black &&
            ptype(bd[t]) == (by_black ? MCUMAX_PAWN_DOWNSTREAM : MCUMAX_PAWN_UPSTREAM)) return 1;
    }
    for (int i = 0; i < 8; i++) {   // sliders: even i = orthogonal, odd = diagonal (via KG order)
        int d = KG[i];
        int diag = (d == 15 || d == 17 || d == -15 || d == -17);
        for (int t = s + d; !(t & 0x88); t += d) {
            int p = bd[t];
            if (!p) continue;
            if (is_black(p) == by_black) {
                int ty = ptype(p);
                if (ty == MCUMAX_QUEEN || (diag ? ty == MCUMAX_BISHOP : ty == MCUMAX_ROOK)) return 1;
            }
            break;
        }
    }
    return 0;
}

static uint32_t hash_pos(void) {
    uint32_t h = 2166136261u ^ (uint32_t)(nply & 1);
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) { h ^= bd[r * 16 + c]; h *= 16777619u; }
    return h;
}

static int insufficient(void) {
    int minors = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int t = ptype(bd[r * 16 + c]);
            if (!t || t == MCUMAX_KING) continue;
            if (t == MCUMAX_KNIGHT || t == MCUMAX_BISHOP) minors++;
            else return 0;
        }
    return minors <= 1;
}

static int is_legal(int from, int to) {
    for (int i = 0; i < nlegal; i++) if (legal[i].from == from && legal[i].to == to) return 1;
    return 0;
}

// Recompute everything that depends on the position: legal moves, check, end of game, status.
static void analyse(void) {
    refresh_board();
    int wtm = white_to_move();
    nlegal = (int)mcumax_search_valid_moves(legal, sizeof legal / sizeof legal[0]);
    if (nlegal > (int)(sizeof legal / sizeof legal[0])) nlegal = sizeof legal / sizeof legal[0];
    check_sq = -1;
    for (int s = 0; s < 128; s++) {
        if (s & 0x88) continue;
        if (ptype(bd[s]) == MCUMAX_KING && is_black(bd[s]) == !wtm) {
            if (attacked(s, wtm)) check_sq = s;
            break;
        }
    }
    pos_hash[nply] = hash_pos();
    int reps = 1;
    for (int i = nply - 2; i >= 0 && i >= nply - hm_clock[nply]; i -= 2)
        if (pos_hash[i] == pos_hash[nply]) reps++;
    over = 1;
    if (nlegal == 0) status_i = check_sq >= 0 ? (wtm ? ST_LOSE : ST_WIN) : ST_STALE;
    else if (insufficient()) status_i = ST_MATERIAL;
    else if (reps >= 3) status_i = ST_REPEAT;
    else if (hm_clock[nply] >= 100) status_i = ST_FIFTY;
    else { over = 0; status_i = wtm ? (check_sq >= 0 ? ST_CHECK : ST_TURN) : ST_THINK; }
    if (nply > 0) { last_from = hist[nply - 1][0]; last_to = hist[nply - 1][1]; }
    else last_from = last_to = -1;
}

// Play one move on the engine and the history. Returns 1 if the engine accepted it.
static int push_move(int from, int to) {
    if (nply >= MAXPLY) return 0;
    int irrev = bd[to] || ptype(bd[from]) <= MCUMAX_PAWN_DOWNSTREAM;
    if (!mcumax_play_move((mcumax_move){(mcumax_square)from, (mcumax_square)to})) return 0;
    hist[nply][0] = (uint8_t)from; hist[nply][1] = (uint8_t)to;
    hm_clock[nply + 1] = irrev ? 0 : (uint8_t)(hm_clock[nply] < 200 ? hm_clock[nply] + 1 : 200);
    nply++;
    return 1;
}

// Rebuild the engine from the start position + the first n plies of the history.
static void replay(int n) {
    mcumax_init();
    nply = 0;
    hm_clock[0] = 0;
    refresh_board();
    pos_hash[0] = hash_pos();
    for (int i = 0; i < n; i++) {
        refresh_board();
        if (!push_move(hist[i][0], hist[i][1])) break;
        refresh_board();
        pos_hash[nply] = hash_pos();
    }
    analyse();
}

// ---- persistence ------------------------------------------------------------------------------------
#define SAVE_MAGIC 0x43485331u   // "CHS1"
static struct { uint32_t magic; int32_t level, nply; uint8_t mv[MAXPLY][2]; } g_save;

static void save_game(void) {
    g_save.magic = SAVE_MAGIC; g_save.level = level; g_save.nply = nply;
    memcpy(g_save.mv, hist, (size_t)nply * 2);
    nv_save("game.bin", &g_save, (int32_t)(12 + nply * 2));
}
static void load_game(void) {
    int n = nv_load("game.bin", &g_save, sizeof g_save);
    if (n >= 12 && g_save.magic == SAVE_MAGIC && g_save.nply >= 0 && g_save.nply <= MAXPLY &&
        n >= 12 + g_save.nply * 2) {
        if (g_save.level >= 0 && g_save.level <= 2) level = g_save.level;
        memcpy(hist, g_save.mv, (size_t)g_save.nply * 2);
        replay(g_save.nply);
    } else {
        replay(0);
    }
}

// ---- sounds -----------------------------------------------------------------------------------------
static void move_sound(int captured) {
    if (over) {
        nv_sound(status_i == ST_WIN ? "win" : status_i == ST_LOSE ? "lose" : "draw");
    } else if (check_sq >= 0) nv_sound("check");
    else nv_sound(captured ? "capture" : "move");
}

// ---- rendering --------------------------------------------------------------------------------------
static int sq_drawn[128];            // cached code per square (-1 = unknown)
static int ui_status = -1, ui_level = -1, ui_undo = -1, ui_new = -1, ui_hint = -1;
static uint32_t ui_tray = 0xFFFFFFFFu;
static int full_redraw = 1;
static int btn_down = -1;            // button being pressed (visual state)
static int new_confirm_until;        // "Confirm?" state deadline (ms), 0 = off
static int thinking;
static int think_phase = -1;

enum { B_NONE = -1, B_UNDO = 0, B_NEW = 1, B_LV0 = 2 };

static int sq_code(int s) {
    int p = bd[s] & 15;
    int hl = (s == sel || s == last_from || s == last_to) ? 1 : 0;
    int ck = (s == check_sq && !hl) ? 1 : 0;
    int mark = 0;
    if (sel >= 0 && is_legal(sel, s)) mark = p ? 2 : 1;
    return p | (hl << 4) | (ck << 5) | (mark << 6);
}

static void draw_square(int s, int code) {
    int r = s >> 4, c = s & 7;
    int x = BX + c * SQ, y = BY + r * SQ;
    int dark = (r + c) & 1;
    int p = code & 15, hl = (code >> 4) & 1, ck = (code >> 5) & 1, mark = (code >> 6) & 3;
    char name[12], *q = name;
    if (p) {
        static const char TY[8] = {'?', 'p', 'p', 'n', 'k', 'b', 'r', 'q'};
        *q++ = is_black(p) ? 'b' : 'w';
        *q++ = TY[ptype(p)];
        *q++ = '_';
        *q++ = dark ? 'd' : 'l';
        if (hl) *q++ = 'h';
        else if (ck) *q++ = 'c';
        *q = 0;
        nv_gfx_image(name, x, y, SQ, SQ);
    } else if (mark == 1) {
        q = sput(q, "dot_");
        *q++ = dark ? 'd' : 'l';
        if (hl) *q++ = 'h';
        *q = 0;
        nv_gfx_image(name, x, y, SQ, SQ);
    } else {
        nv_gfx_rect(x, y, SQ, SQ, hl ? (dark ? C_DARK_HL : C_LIGHT_HL) : (dark ? C_DARK : C_LIGHT));
    }
    if (mark == 2) {   // capture target: corner wedges
        int col = hl ? (dark ? C_RING_DH : C_RING_LH) : (dark ? C_RING_D : C_RING_L);
        const int k = 17, e = SQ - 1;
        nv_gfx_tri(x, y, x + k, y, x, y + k, col);
        nv_gfx_tri(x + e, y, x + e - k, y, x + e, y + k, col);
        nv_gfx_tri(x, y + e, x + k, y + e, x, y + e - k, col);
        nv_gfx_tri(x + e, y + e, x + e - k, y + e, x + e, y + e - k, col);
    }
}

// Captured pieces + material balance, packed so the tray redraws only when it changes.
static const int VAL[8] = {0, 1, 1, 3, 0, 3, 5, 9};
static void material(int cnt[2][8], int *bal) {
    for (int i = 0; i < 8; i++) cnt[0][i] = cnt[1][i] = 0;
    *bal = 0;
    for (int s = 0; s < 128; s++) {
        if ((s & 0x88) || !bd[s]) continue;
        int b = is_black(bd[s]), t = ptype(bd[s]);
        if (t == MCUMAX_PAWN_DOWNSTREAM) t = MCUMAX_PAWN_UPSTREAM;
        cnt[b][t]++;
        *bal += b ? -VAL[t] : VAL[t];
    }
}
static const int START[8] = {0, 8, 0, 2, 1, 2, 2, 1};
static const int ORDER[5] = {MCUMAX_QUEEN, MCUMAX_ROOK, MCUMAX_BISHOP, MCUMAX_KNIGHT, MCUMAX_PAWN_UPSTREAM};
static uint32_t tray_sig(void) {
    int cnt[2][8], bal;
    material(cnt, &bal);
    uint32_t h = (uint32_t)(bal + 64);
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 5; i++) h = h * 31u + (uint32_t)(cnt[b][ORDER[i]] + 1);
    return h;
}
static void draw_number(int right_x, int y, int n) {   // "+N" right-aligned
    char dg[4]; int k = 0;
    do { dg[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < 3);
    int x = right_x - 12 * (k + 1);
    nv_gfx_image("nplus", x, y, 12, 26);
    for (int i = k - 1; i >= 0; i--) {
        char nm[4] = {'n', dg[i], 0, 0};
        x += 12;
        nv_gfx_image(nm, x, y, 12, 26);
    }
}
static void draw_tray(void) {
    int cnt[2][8], bal;
    material(cnt, &bal);
    nv_gfx_image("tray", PX, Y_TRAY, PW, 104);
    for (int row = 0; row < 2; row++) {   // row 0: you (captured black pieces), row 1: computer
        int y = Y_TRAY + (row ? 60 : 14);
        nv_gfx_image(L(row ? "lb_cpu" : "lb_you"), PX + 16, y, 100, 30);
        int victim = row ? 0 : 1;            // colour index of the captured pieces
        int n = 0;
        for (int i = 0; i < 5; i++) {
            int t = ORDER[i], m = START[t] - cnt[victim][t];
            if (m > 0) n += m;
        }
        int avail = PW - 116 - 60, step = n > 0 ? avail / n : 0;
        if (step > 24) step = 24;
        int x = PX + 116;
        for (int i = 0; i < 5; i++) {
            int t = ORDER[i], m = START[t] - cnt[victim][t];
            static const char TY[8] = {'?', 'p', 'p', 'n', 'k', 'b', 'r', 'q'};
            char nm[6] = {'s', '_', victim ? 'b' : 'w', TY[t], 0, 0};
            for (int j = 0; j < m; j++) { nv_gfx_image(nm, x, y, 30, 30); x += step; }
            if (m > 0) x += 6;
        }
        int adv = row ? -bal : bal;
        if (adv > 0) draw_number(PX + PW - 16, y + 2, adv);
    }
}

static int undo_enabled(void) { return nply > 0; }

static void render(void) {
    if (full_redraw) {
        nv_gfx_clear(C_BG);
        nv_gfx_image("ranks0", BX - 24, BY, 20, 4 * SQ);
        nv_gfx_image("ranks1", BX - 24, BY + 4 * SQ, 20, 4 * SQ);
        nv_gfx_image("files0", BX, BY + 8 * SQ + 2, 4 * SQ, 20);
        nv_gfx_image("files1", BX + 4 * SQ, BY + 8 * SQ + 2, 4 * SQ, 20);
        nv_gfx_image(L("title"), PX, Y_TITLE, PW, 52);
        nv_gfx_image(L("lb_level"), PX, Y_LVLBL, PW, 22);
        for (int s = 0; s < 128; s++) sq_drawn[s] = -1;
        ui_status = ui_level = ui_undo = ui_new = ui_hint = -1;
        ui_tray = 0xFFFFFFFFu;
        think_phase = -1;
        full_redraw = 0;
    }
    for (int s = 0; s < 128; s++) {
        if (s & 0x88) continue;
        int code = sq_code(s);
        if (code != sq_drawn[s]) { draw_square(s, code); sq_drawn[s] = code; }
    }
    int st = thinking ? ST_THINK : status_i;
    if (st != ui_status) {
        char nm[6] = {'s', 't', (char)('0' + st), 0, 0, 0};
        nv_gfx_image(L(nm), PX, Y_STATUS, PW, 64);
        ui_status = st;
        think_phase = -1;
    }
    uint32_t ts = tray_sig();
    if (ts != ui_tray) { draw_tray(); ui_tray = ts; }
    int lv = level | (btn_down >= B_LV0 ? (btn_down - B_LV0 + 1) << 4 : 0);
    if (lv != ui_level) {
        for (int i = 0; i < 3; i++) {
            char nm[8] = {'l', 'v', (char)('0' + i), 'o', 0, 0, 0, 0};
            if (i == level || btn_down == B_LV0 + i) { nm[4] = 'n'; }
            else { nm[4] = 'f'; nm[5] = 'f'; }
            nv_gfx_image(L(nm), PX + i * (SEG_W + 8), Y_LEVEL, SEG_W, 56);
        }
        ui_level = lv;
    }
    int u = !undo_enabled() ? 2 : (btn_down == B_UNDO ? 1 : 0);
    if (u != ui_undo) {
        nv_gfx_image(L(u == 2 ? "b_undo_x" : u == 1 ? "b_undo_p" : "b_undo_n"), PX, Y_BTN, BTN_W, 64);
        ui_undo = u;
    }
    int nw = new_confirm_until ? 2 : (btn_down == B_NEW ? 1 : 0);
    if (nw != ui_new) {
        nv_gfx_image(L(nw == 2 ? "b_new_c" : nw == 1 ? "b_new_p" : "b_new_n"), PX + BTN_W + 12, Y_BTN, BTN_W, 64);
        ui_new = nw;
    }
    int hi = over ? 1 : 0;
    if (hi != ui_hint) { nv_gfx_image(L(hi ? "hint_end" : "hint"), PX, Y_HINT, PW, 56); ui_hint = hi; }
}

// Three pulsing dots on the status card while the computer thinks (bounded: only while thinking).
static void draw_think_dots(int phase) {
    int x0 = PX + PW - 74, y = Y_STATUS + 32;
    nv_gfx_rect(x0 - 10, y - 10, 66, 20, C_CARD);
    for (int i = 0; i < 3; i++)
        nv_gfx_circle(x0 + i * 22, y, i == phase ? 6 : 4, i == phase ? C_TEXT : C_MUTED);
}

// ---- input ------------------------------------------------------------------------------------------
static int hit_button(int x, int y) {
    if (y >= Y_BTN && y < Y_BTN + 64) {
        if (x >= PX && x < PX + BTN_W) return B_UNDO;
        if (x >= PX + BTN_W + 12 && x < PX + PW) return B_NEW;
    }
    if (y >= Y_LEVEL && y < Y_LEVEL + 56)
        for (int i = 0; i < 3; i++)
            if (x >= PX + i * (SEG_W + 8) && x < PX + i * (SEG_W + 8) + SEG_W) return B_LV0 + i;
    return B_NONE;
}
static int hit_square(int x, int y) {
    if (x < BX || y < BY || x >= BX + 8 * SQ || y >= BY + 8 * SQ) return -1;
    return ((y - BY) / SQ) * 16 + (x - BX) / SQ;
}

enum { ACT_NONE, ACT_UNDO, ACT_NEW };
static int pending_act;
static int prev_down, press_sq = -1, press_btn = B_NONE, pre_sel = -1;
static int cpu_at;                 // ms when the computer may start (lets the human move render first)

static void new_game(void) {
    replay(0);
    sel = -1;
    new_confirm_until = 0;
    save_game();
}
static void do_undo(void) {
    if (nply == 0) return;
    int n = nply - (white_to_move() && nply >= 2 ? 2 : 1);
    replay(n);
    sel = -1;
    save_game();
}

static void human_move(int from, int to) {
    int cap = bd[to] != 0 || (ptype(bd[from]) == MCUMAX_PAWN_UPSTREAM && ((from ^ to) & 7));
    if (!push_move(from, to)) return;
    sel = -1;
    analyse();
    move_sound(cap);
    save_game();
    cpu_at = nv_millis() + 150;
}

// Button press/release is handled here (also while thinking); board input only when idle.
static void poll_input(void) {
    int x, y;
    int down = nv_touch(&x, &y);
    if (down && !prev_down) {                       // press
        press_btn = hit_button(x, y);
        press_sq = hit_square(x, y);
        btn_down = press_btn;
        if (press_btn == B_UNDO && !undo_enabled()) btn_down = B_NONE;
        pre_sel = sel;
        if (press_sq >= 0 && !thinking && !over && white_to_move() && bd[press_sq] && !is_black(bd[press_sq]))
            sel = press_sq;
    } else if (down && prev_down) {                 // drag: keep button highlight only while inside
        if (press_btn != B_NONE) {
            int b = hit_button(x, y) == press_btn ? press_btn : B_NONE;
            if (press_btn == B_UNDO && !undo_enabled()) b = B_NONE;
            btn_down = b;
        }
    } else if (!down && prev_down) {                // release
        int rb = hit_button(x, y), rs = hit_square(x, y);
        btn_down = B_NONE;
        if (press_btn != B_NONE && rb == press_btn) {
            if (press_btn == B_UNDO && undo_enabled()) pending_act = ACT_UNDO;
            else if (press_btn == B_NEW) {
                if (nply == 0 || over || new_confirm_until) pending_act = ACT_NEW;
                else new_confirm_until = nv_millis() + 3000;
            } else if (press_btn >= B_LV0) {
                level = press_btn - B_LV0;
                save_game();
            }
        } else if (press_sq >= 0 && !thinking && !over && white_to_move()) {
            if (sel >= 0 && rs >= 0 && rs != sel && is_legal(sel, rs)) human_move(sel, rs);
            else if (rs == press_sq) {
                if (bd[rs] && !is_black(bd[rs])) { if (pre_sel == rs) sel = -1; }
                else sel = -1;
            }
        }
        press_btn = B_NONE; press_sq = -1;
    }
    prev_down = down;
    if (new_confirm_until && nv_millis() > new_confirm_until && pending_act != ACT_NEW) new_confirm_until = 0;
    if (nv_gfx_back()) quit = 1;
}

// ---- computer move ----------------------------------------------------------------------------------
static const struct { int nodes, depth, soft_ms, hard_ms, random_pct; } LEVELS[3] = {
    {    800, 1,  300, 1200, 25 },   // Facile: shallow + an occasional casual move
    {  20000, 4,  450, 1500,  0 },   // Medio
    { 300000, 8,  800, 1900,  0 },   // Difficile
};
static int t0, soft_ms, hard_ms, soft_done, hard_stop, last_frame, cb_count;

static void pump_frame(void) {
    int now = nv_millis();
    poll_input();
    if (pending_act != ACT_NONE || quit) mcumax_stop_search();
    render();
    int ph = (now / 220) % 3;
    if (ph != think_phase) { draw_think_dots(ph); think_phase = ph; }
    if (!nv_gfx_present()) { quit = 1; mcumax_stop_search(); }
    last_frame = nv_millis();
}

static void think_cb(void *u) {
    (void)u;
    if (++cb_count & 31) return;
    int now = nv_millis();
    if (!soft_done && now - t0 > soft_ms) { soft_done = 1; mcumax_finish_search(); }
    if (!hard_stop && now - t0 > hard_ms) { hard_stop = 1; mcumax_stop_search(); }
    if (now - last_frame >= 70) pump_frame();
}

static void cpu_move(void) {
    thinking = 1;
    render();
    think_phase = -1;
    pump_frame();
    const int lv = level;
    mcumax_move mv = MCUMAX_MOVE_INVALID;
    t0 = nv_millis();
    if (LEVELS[lv].random_pct && (nv_rand() & 0x7fffffff) % 100 < LEVELS[lv].random_pct) {
        // a plausible casual move: random legal move, but prefer captures
        int pick = (nv_rand() & 0x7fffffff) % nlegal;
        for (int i = 0; i < nlegal; i++)
            if (bd[legal[i].to]) { if ((nv_rand() & 3) == 0) { pick = i; break; } }
        mv = legal[pick];
    } else {
        soft_ms = LEVELS[lv].soft_ms; hard_ms = LEVELS[lv].hard_ms;
        soft_done = hard_stop = 0; cb_count = 0; last_frame = t0;
        mcumax_set_callback(think_cb, 0);
        mv = mcumax_search_best_move((uint32_t)LEVELS[lv].nodes, (uint32_t)LEVELS[lv].depth);
        mcumax_set_callback(0, 0);
        if (quit || pending_act != ACT_NONE) { thinking = 0; return; }
        if (hard_stop || mv.from == MCUMAX_SQUARE_INVALID) {   // out of time: best seen so far
            mv = mcumax_get_root_best();
            if (mv.from == MCUMAX_SQUARE_INVALID || !is_legal(mv.from, mv.to))
                mv = mcumax_search_best_move(1, 0);   // last resort: a quick shallow answer
        }
    }
    int took = nv_millis() - t0;
    char msg[64], *p = sput(msg, "cpu lv");
    *p++ = (char)('0' + lv); p = sput(p, " ms=");
    { char d[8]; int k = 0, v = took; do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 7);
      while (k) *p++ = d[--k]; *p = 0; }
    nv_log(NV_LOG_INFO, msg);
    while (!quit && pending_act == ACT_NONE && nv_millis() - t0 < 450) pump_frame();   // never "instant"
    thinking = 0;
    if (quit) return;
    if (pending_act != ACT_NONE) return;
    // (a finished or stopped search leaves the engine board as it found it: no resync needed)
    if (mv.from == MCUMAX_SQUARE_INVALID || !is_legal(mv.from, mv.to)) {
        if (nlegal <= 0) return;
        mv = legal[0];
    }
    int cap = bd[mv.to] != 0 || (ptype(bd[mv.from]) == MCUMAX_PAWN_DOWNSTREAM && ((mv.from ^ mv.to) & 7));
    if (push_move(mv.from, mv.to)) {
        analyse();
        move_sound(cap);
        save_game();
    }
}

// ---- entry ------------------------------------------------------------------------------------------
static void init_colors(void) {
    C_BG = NV_RGB(21, 24, 28);      C_CARD = NV_RGB(33, 37, 43);
    C_LIGHT = NV_RGB(238, 238, 210); C_DARK = NV_RGB(118, 150, 86);
    C_LIGHT_HL = NV_RGB(246, 246, 130); C_DARK_HL = NV_RGB(186, 202, 68);
    C_MUTED = NV_RGB(140, 148, 158); C_TEXT = NV_RGB(236, 238, 240);
    C_RING_L = NV_RGB(190, 190, 168); C_RING_D = NV_RGB(94, 120, 69);
    C_RING_LH = NV_RGB(197, 197, 104); C_RING_DH = NV_RGB(149, 162, 54);
}

NV_EXPORT("run")
void run(void) {
    char lang[8] = {0};
    nv_lang(lang, sizeof lang);
    g_lang = (lang[0] == 'i' && lang[1] == 't') ? "it" : "en";
    init_colors();
    nv_gfx_persist(1);
    load_game();
    full_redraw = 1;
    while (!quit) {
        poll_input();
        if (quit) break;
        if (pending_act == ACT_UNDO) { pending_act = ACT_NONE; do_undo(); }
        else if (pending_act == ACT_NEW) { pending_act = ACT_NONE; new_game(); }
        render();
        if (!nv_gfx_present()) break;
        if (!over && !white_to_move() && nv_millis() >= cpu_at) {
            cpu_move();
            if (pending_act == ACT_UNDO) { pending_act = ACT_NONE; do_undo(); }
            else if (pending_act == ACT_NEW) { pending_act = ACT_NONE; new_game(); }
        }
    }
    save_game();
}
