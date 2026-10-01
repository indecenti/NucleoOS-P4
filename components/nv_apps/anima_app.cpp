// anima_app — native UI for ANIMA, the offline assistant (nv_anima engine).
//
// The look and the feel follow the Terminal app and an agentic CLI (Claude Code-style): edge to
// edge on the terminal's dark Tango palette, DejaVu Sans Mono, a transcript instead of chat bubbles:
//   > the question you typed                (dim, on a faint band)
//   ● ANIMA's answer, hanging-indented       (``` code fences become full-width panels)
//     └ L1/KGE · 87% · trace                 (how it got there: tier, confidence, tool, correction)
// A spinner row ("· Ragiono… (3s · esc per interrompere)") stands in while the worker thinks; Esc
// or ^C interrupts it. The input is a bordered "> " prompt box with a status footer, slash commands
// (/help /clear /status /config /model /l1 /voice /exit) with a live suggestion menu (Tab completes,
// ↑↓ pick), ↑↓ prompt history and the Terminal's extra-keys row for touch. The settings view
// (/config or the gear key): L1 serving policy, cloud teacher key manager, telemetry, session reset.
//
// The engine cascade (L0 commands → L1 retrieval → HDC/KGE deduction → optional online tiers)
// runs on a persistent worker task so a query (SD reads, possibly a cloud fetch) never blocks
// LVGL; an lv_timer polls for the result. Engine init is lazy (first query, off the UI thread)
// per the no-boot-cost / no-static-bss rule. The worker outlives the app (idle on a queue, PSRAM
// stack, never writes internal flash) so reopen costs nothing.
//
// Glyph note: the transcript font is nv_font_mono_17 (ASCII, Latin-1, typographic punctuation,
// arrows, box drawing, block elements, ● ▲ ▶ ▼ ◀) falling back to nv_font_14 (Latin-1 + LVGL
// symbols). Engine/teacher text can carry anything, so latin1ize() maps it to Latin-1 before it
// reaches a label; the UI's own strings only use glyphs the mono font has.
#include "apps_internal.h"
#include "nv_ui_host.h"   // nv_ui_set_back: Esc / Back closes sub-pages and modals

#include "nv_ui_focus.h"   // keyboard: first focus on the question field
#include "nv_app.h"
#include "nv_ui_kit.h"   // nv_kit_* + (transitively) nv_ime_*
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"

#include "nucleo_anima.h"
#include "nv_anima_system.h" // shared ANIMA_ACT_SYSTEM {value} resolver
#include "nv_config.h"   // persisted L1 serving mode ("anima.l1")
#include "nv_time.h"     // ANIMA_ACT_SYSTEM "time"
#include "nv_sd.h"       // ANIMA_ACT_SYSTEM "storage"
#include "nv_wifi.h"     // settings: online status line
#include "nv_ui.h"       // nv_ui_toast, nv_ui_close_app
#include "nv_ota.h"      // nv_ota_running_version (welcome header)
#include "nv_audio.h"    // voice input: nv_audio_rec_start/stop (mic -> WAV)
#include "nv_sealed.h"   // teacher.json holds the API keys: sealed to this chip
#include "cJSON.h"       // teacher.json read-modify-write (key manager) + chat log lines
#include "esp_attr.h"    // EXT_RAM_BSS_ATTR
#include "nv_mem_attr.h" // NV_PSRAM_BSS
#include "esp_heap_caps.h"

#include <sys/stat.h>    // mkdir for /sdcard/data/anima on a fresh card
#include <cctype>

#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kInputCap = 512;

// ---------------------------------------------------------------- look

// The Terminal's palette (terminal_app.cpp), so the two read as one family, plus the CLI accent.
constexpr uint32_t kBg      = 0x121417;   // background, edge to edge
constexpr uint32_t kFg      = 0xD3D7CF;   // text
constexpr uint32_t kFgBold  = 0xEEEEEC;   // ANIMA's bullet, command names
constexpr uint32_t kDim     = 0x7F8386;   // prompts, meta lines, hints
constexpr uint32_t kBand    = 0x1C1F23;   // the faint band behind a question
constexpr uint32_t kCodeBg  = 0x1A1D21;   // code panels
constexpr uint32_t kBorder  = 0x3A3F46;   // input box
constexpr uint32_t kAccent  = 0xD97757;   // spinner, welcome frame, selected command
constexpr uint32_t kGreen   = 0x8AE234;   // online
constexpr uint32_t kRed     = 0xEF2929;   // errors, interruptions
constexpr uint32_t kBlue    = 0x729FCF;   // command names in /help
constexpr uint32_t kKeyBg   = 0x202327;   // extra-keys row (as in the Terminal)
constexpr uint32_t kKey     = 0x2C3035;
constexpr uint32_t kKeyDown = 0x3A3F46;

constexpr int kIndent = 20;               // two mono cells: "● " / "└ " hang the text after them

// nv_font_mono_17 with nv_font_14 as fallback (LVGL symbols for the mic / gear keys). A RAM copy,
// since the fallback is a field of the font.
lv_font_t s_mono;
bool      s_mono_ok = false;

// UTF-8 for the few non-ASCII glyphs the UI draws itself (all in nv_font_mono_17).
#define G_DOT    "\xE2\x97\x8F"   // ● ANIMA speaks
#define G_ELBOW  "\xE2\x94\x94"   // └ meta / tool line
#define G_ARROW  "\xE2\x86\x92"   // → suggestion
#define G_UP     "\xE2\x86\x91"   // ↑
#define G_DOWN   "\xE2\x86\x93"   // ↓
#define G_ELL    "\xE2\x80\xA6"   // …
#define G_MID    "\xC2\xB7"       // ·

// ---------------------------------------------------------------- state

// UI (valid only while the page lives; cleared on LV_EVENT_DELETE)
lv_obj_t   *s_root     = nullptr;
lv_obj_t   *s_chat     = nullptr;   // the transcript (scroll column)
lv_obj_t   *s_menu     = nullptr;   // slash-command suggestions (above the input box)
lv_obj_t   *s_box      = nullptr;   // bordered "> " input box
lv_obj_t   *s_footer   = nullptr;   // hint + status line under the box
lv_obj_t   *s_status   = nullptr;   // footer: "● offline" / "● online · teacher ..."
lv_obj_t   *s_input    = nullptr;
lv_obj_t   *s_pending  = nullptr;   // the spinner row awaiting the current result
lv_obj_t   *s_spin     = nullptr;   // its label
uint32_t    s_spin_t0  = 0;         // when the current wait started (elapsed seconds)
bool        s_spin_voice = false;   // waiting for a transcript, not an answer
lv_obj_t   *s_settings = nullptr;   // settings view (hidden by default)
lv_obj_t   *s_stats    = nullptr;   // telemetry label inside settings
lv_obj_t   *s_gear     = nullptr;   // gear key label: swaps gear/close
lv_timer_t *s_poll     = nullptr;

// Prompt history (newest last), walked with ↑↓ / ^P ^N. PSRAM, allocated once.
constexpr int kHistMax = 32;
char (*s_hist)[kInputCap] = nullptr;
int s_hist_n = 0, s_hist_pos = 0;

// Slash-command menu: the matches shown for the typed prefix, and the highlighted one.
constexpr int kMenuMax = 8;
int s_menu_ids[kMenuMax];
int s_menu_n = 0, s_menu_sel = 0;

// Teacher key manager (settings view)
lv_obj_t *s_prov_dd  = nullptr;
lv_obj_t *s_key_ta   = nullptr;
lv_obj_t *s_model_ta = nullptr;
lv_obj_t *s_base_ta  = nullptr;
bool      s_key_dirty = false;      // user typed in the key field (else keep the stored key)

constexpr const char *kTeacherPath = "/sdcard/data/anima/teacher.json";
// Dropdown order. Base URLs mirror the engine's provider defaults (nucleo_anima_online.c).
enum { PROV_AUTO = 0, PROV_GROQ, PROV_ANTHROPIC, PROV_GEMINI, PROV_CUSTOM };
const char *kProvName[] = {"", "openai", "anthropic", "google", "openai"};
const char *kProvBase[] = {"", "https://api.groq.com/openai/v1", "https://api.anthropic.com",
                           "https://generativelanguage.googleapis.com/v1beta/openai", ""};

// Worker plumbing (session-lifetime; survives app close)
enum { JOB_QUERY = 0, JOB_VOICE = 1 };
// The query text travels WITH the job: the engine uses the caller's pointer through the whole
// cascade (teacher call, ring push, telemetry), so a shared buffer rewritten by the next submit
// while an orphaned query was still running sent torn text to the cloud and stored it as topic.
struct anima_job { uint32_t gen; int kind; char text[kInputCap]; };
TaskHandle_t  s_worker = nullptr;
QueueHandle_t s_queue  = nullptr;  // depth 1, carries {generation, kind}
NV_PSRAM_BSS char s_req[kInputCap];   // owned by the UI (the worker gets a copy in its job)
char          s_lang[4] = "it";
lv_timer_t   *s_launch_timer = nullptr;   // agentic "open app" one-shot; deleted on teardown
uint32_t      s_gen = 0;           // bumped per submit AND on teardown / interrupt (stale results drop)
volatile uint32_t s_done_gen = 0;  // worker: generation of the finished result
volatile int   s_done_kind = JOB_QUERY;
// Cold-ish buffers -> PSRAM .bss (sequential copies only; internal SRAM stays for hot paths).
EXT_RAM_BSS_ATTR anima_result_t s_res;   // worker-filled, read by the poll timer after s_done_gen
EXT_RAM_BSS_ATTR char s_long[2048];      // worker copy of nucleo_anima_long_reply()
EXT_RAM_BSS_ATTR char s_san[2304];       // latin1ize scratch (LVGL thread only)
EXT_RAM_BSS_ATTR char s_md[2304];        // md_lite scratch (LVGL thread only)
// The teacher as the worker last saw it (footer, /model). nucleo_anima_teacher_info reads the SD
// vault and, without a key, may probe the LAN over mDNS for 2 s: never on the LVGL thread per
// reply, so the worker refreshes this after each turn. Byte 48 stays 0: a torn read still ends.
EXT_RAM_BSS_ATTR char s_teach_prov[25];
EXT_RAM_BSS_ATTR char s_teach_model[49];
volatile int s_teach_state = -1;           // -1 not looked yet, 0 no teacher, 1 configured

// Voice input (F4): mic -> WAV on SD -> cloud Whisper (engine) -> transcript -> normal query
bool s_recording = false;
bool s_voice_wait = false;                 // stop requested: dispatch JOB_VOICE once the WAV is finalized
lv_obj_t *s_mic = nullptr;                 // mic key label (icon swaps to stop)
NV_PSRAM_BSS char s_voice[kInputCap];      // worker-filled transcript ("" = failed)
constexpr const char *kVoiceWav = "/sdcard/data/anima/voice.wav";
constexpr const char *kChatLog  = "/sdcard/data/anima/chatlog.ndjson";

bool lang_en(void) { return nv_i18n_get_lang() != NV_LANG_IT; }  // ANIMA speaks it/en; en fallback
// A literal pair, so printf-style formats stay checkable by the compiler.
#define T(it, en) (lang_en() ? (en) : (it))

// ---------------------------------------------------------------- glyph sanitizer

// Map UTF-8 typographic punctuation to Latin-1 equivalents the fonts actually have.
// Latin-1 (2-byte C2/C3), the bullet (U+2022) and what nv_font_mono_17 adds (arrows U+2190..2195,
// box drawing and blocks U+2500..259F, ● U+25CF, € U+20AC) pass through; any other sequence
// outside the font range degrades to '?' instead of a missing-glyph box.
const char *latin1ize(const char *src) {
    size_t o = 0;
    const size_t cap = sizeof s_san - 4;
    for (const unsigned char *p = (const unsigned char *)src; *p && o < cap;) {
        if (p[0] < 0x80) { s_san[o++] = (char)*p++; continue; }              // ASCII
        if (p[0] == 0xC2 || p[0] == 0xC3) {                                   // Latin-1: in the fonts
            if (!p[1]) break;
            s_san[o++] = (char)p[0]; s_san[o++] = (char)p[1]; p += 2; continue;
        }
        if (p[0] == 0xE2 && p[1] == 0x80 && p[2]) {                           // General Punctuation
            unsigned char c = p[2]; p += 3;
            switch (c) {
                case 0x98: case 0x99: s_san[o++] = '\''; break;               // ' '
                case 0x9C: case 0x9D: s_san[o++] = '"';  break;               // " "
                case 0x93: case 0x94: s_san[o++] = '-';  break;               // – —
                case 0xA6: if (o + 3 < cap) { memcpy(s_san + o, "...", 3); o += 3; } break;  // …
                case 0xA2: if (o + 3 < cap) { memcpy(s_san + o, "\xE2\x80\xA2", 3); o += 3; } break;  // • kept
                default:   s_san[o++] = '?'; break;
            }
            continue;
        }
        const bool mono_glyph =
            p[0] == 0xE2 && p[1] && p[2] &&
            ((p[1] == 0x86 && p[2] >= 0x90 && p[2] <= 0x95) ||                 // ← ↑ → ↓ ↔ ↕
             (p[1] >= 0x94 && p[1] <= 0x95) || (p[1] == 0x96 && p[2] <= 0x9F) ||  // box drawing, blocks
             (p[1] == 0x97 && p[2] == 0x8F) || (p[1] == 0x82 && p[2] == 0xAC));   // ● €
        if (mono_glyph) {
            if (o + 3 >= cap) break;
            memcpy(s_san + o, p, 3); o += 3; p += 3;
            continue;
        }
        // Any other multi-byte sequence: swallow it, emit one '?'.
        unsigned char lead = *p++;
        int ext = (lead >= 0xF0) ? 3 : (lead >= 0xE0) ? 2 : 1;
        while (ext-- && *p) p++;
        s_san[o++] = '?';
    }
    s_san[o] = '\0';
    return s_san;
}

// Light markdown for prose (teacher replies are markdown): "# " heading marks, ** / __ / *word*
// emphasis and inline `code` ticks are dropped, "- " / "* " list items become "• ". Code fences
// are split off before this runs (reply_render). A lone '*' between spaces or digits ("2 * 3",
// "2*3") is arithmetic and stays.
const char *md_lite(const char *src) {
    size_t o = 0;
    const size_t cap = sizeof s_md - 4;
    bool bol = true;   // at the beginning of a line
    for (const char *p = src; *p && o < cap;) {
        if (bol) {
            const char *q = p;
            while (*q == ' ' && q - p < 6) q++;
            if (*q == '#') {                                  // heading: drop the hashes
                const char *h = q;
                while (*h == '#') h++;
                if (*h == ' ') { p = h + 1; bol = false; continue; }
            }
            if ((*q == '-' || *q == '*') && q[1] == ' ') {     // list item -> bullet
                size_t ind = (size_t)(q - p);
                if (o + ind + 4 >= cap) break;
                memset(s_md + o, ' ', ind); o += ind;
                memcpy(s_md + o, "\xE2\x80\xA2 ", 4); o += 4;
                p = q + 2; bol = false; continue;
            }
            bol = false;
        }
        if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_')) { p += 2; continue; }
        if (*p == '*') {
            const unsigned char prev = o ? (unsigned char)s_md[o - 1] : ' ';
            const unsigned char next = (unsigned char)p[1];
            const bool prev_sp = isspace(prev) || prev == '(';
            const bool next_sp = !next || isspace(next) || ispunct(next);
            if ((prev_sp && !isspace(next) && next) || (!prev_sp && next_sp)) { p++; continue; }
        }
        if (*p == '`') { p++; continue; }
        if (*p == '\n') bol = true;
        s_md[o++] = *p++;
    }
    s_md[o] = '\0';
    return s_md;
}

// ---------------------------------------------------------------- worker

void teacher_snapshot(void) {
    char prov[24] = "", model[40] = "";
    const bool ok = nucleo_anima_teacher_info(prov, sizeof prov, model, sizeof model);
    snprintf(s_teach_prov, sizeof s_teach_prov - 1, "%s", prov);
    snprintf(s_teach_model, sizeof s_teach_model - 1, "%s", model);
    s_teach_state = ok ? 1 : 0;
}

void worker_task(void *) {
    bool mode_applied = false;
    for (;;) {
        anima_job job;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        nucleo_anima_init(s_lang);   // idempotent; first call loads the L0 pack from SD
        if (!mode_applied) {         // restore the persisted L1 serving policy once per boot
            nucleo_anima_l1_set_mode(nv_config_get_int("anima.l1", ANIMA_L1_AUTO));
            mode_applied = true;
        }
        if (job.kind == JOB_VOICE) {
            // Cloud Whisper on the recorded WAV; "" on failure (no key / offline / API error).
            char lang_out[8] = "";
            s_voice[0] = '\0';
            int n = nucleo_anima_transcribe(kVoiceWav, "auto", s_voice, sizeof s_voice,
                                            lang_out, sizeof lang_out);
            if (n <= 0) s_voice[0] = '\0';
            remove(kVoiceWav);
            s_done_kind = JOB_VOICE;
            s_done_gen = job.gen;
            continue;
        }
        // Spine gate: briefly poll, then answer busy (the web handler may own the cascade).
        bool locked = false;
        for (int i = 0; i < 20 && !(locked = nucleo_anima_try_lock()); i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (!locked) {
            memset(&s_res, 0, sizeof s_res);
            snprintf(s_res.reply, sizeof s_res.reply, "%s",
                     s_lang[0] == 'e' ? "I'm busy with another request, try again."
                                      : "Sono occupata con un'altra richiesta, riprova.");
            s_long[0] = '\0';
            s_done_kind = JOB_QUERY;
            s_done_gen = job.gen;
            continue;
        }
        s_res = nucleo_anima_query(job.text, s_lang);
        const char *lr = nucleo_anima_long_reply();
        if (lr && lr[0]) { strncpy(s_long, lr, sizeof s_long - 1); s_long[sizeof s_long - 1] = '\0'; }
        else s_long[0] = '\0';
        teacher_snapshot();          // under the spine gate, like every other engine call
        nucleo_anima_unlock();
        s_done_kind = JOB_QUERY;
        s_done_gen = job.gen;
    }
}

void worker_send(int kind, const char *text) {
    if (!s_queue) return;
    anima_job job;
    job.gen = ++s_gen;
    job.kind = kind;
    snprintf(job.text, sizeof job.text, "%s", text ? text : "");
    xQueueOverwrite(s_queue, &job);
}

void worker_ensure(void) {
    if (s_worker) return;
    if (!s_queue) s_queue = xQueueCreate(1, sizeof(anima_job));
    if (!s_queue) { nv_ui_toast("ANIMA: out of memory"); return; }   // xQueueOverwrite(NULL) asserts
    // PSRAM stack: session-persistent, SD-only I/O (pack reads, session/telemetry writes).
    // The one flash touch on this path — nv_config_* from the executor — is proxied by nv_config
    // to an internal-stack helper, so the PSRAM-stack rule holds.
    if (xTaskCreateWithCaps(worker_task, "anima", 24 * 1024, nullptr, 4, &s_worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_worker = nullptr;   // queue kept for the next attempt (no per-retry leak)
        nv_ui_toast("ANIMA: worker start failed");
    }
}

// ---------------------------------------------------------------- transcript

lv_obj_t *mono_label(lv_obj_t *parent, const char *text, uint32_t color) {
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_set_style_text_font(lb, &s_mono, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(color), 0);
    lv_label_set_text(lb, text);
    return lb;
}

// A full-width transcript row: a fixed two-cell prefix ("> ", "● ", "└ ", "  ") and the text
// wrapped after it, so continuation lines hang under the first word like in a terminal CLI.
// Returns the row; the text label is its child 1.
lv_obj_t *row_add(const char *prefix, uint32_t pcolor, const char *text, uint32_t tcolor) {
    lv_obj_t *row = lv_obj_create(s_chat);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *p = mono_label(row, prefix, pcolor);
    lv_obj_set_width(p, kIndent);

    lv_obj_t *lb = mono_label(row, text, tcolor);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lb, 1);
    return row;
}
lv_obj_t *row_text(lv_obj_t *row) { return lv_obj_get_child(row, 1); }

void user_add(const char *text) {
    lv_obj_t *row = row_add(">", kDim, latin1ize(text), kFg);
    lv_obj_set_style_bg_color(row, lv_color_hex(kBand), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_margin_top(row, 6, 0);
}

// ANIMA's prose: "● " on the first segment of a reply, a plain indent on the following ones.
void prose_add(const char *text, bool first) {
    row_add(first ? G_DOT : "", kFgBold, latin1ize(md_lite(text)), kFg);
}

// Dim "└ ..." line under an answer (tier · confidence · trace, a tool call, a correction) or the
// output of a slash command (color = kFg).
void meta_add(const char *m, uint32_t color = kDim) {
    lv_obj_t *row = row_add(G_ELBOW, kDim, latin1ize(m), color);
    lv_obj_set_style_pad_left(row, kIndent, 0);
}

// Full-width code panel (teacher replies fence code with ```), indented under the bullet.
void code_add(const char *text) {
    lv_obj_t *b = lv_obj_create(s_chat);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_margin_left(b, kIndent, 0);
    lv_obj_set_style_pad_all(b, 8, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(kCodeBg), 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(kBorder), 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lb = mono_label(b, latin1ize(text), kFgBold);
    lv_obj_set_width(lb, lv_pct(100));
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
}

// Render a reply that may contain ``` fences: prose -> "●" rows, code -> panels.
void reply_render(const char *text) {
    const char *p = text;
    bool first = true, code = false;
    while (*p) {
        const char *f = strstr(p, "```");
        size_t n = f ? (size_t)(f - p) : strlen(p);
        if (n) {
            char seg[2048];
            if (n >= sizeof seg) n = sizeof seg - 1;
            memcpy(seg, p, n);
            seg[n] = '\0';
            char *s = seg;
            if (code) {                       // drop the ```lang tag line
                const char *nl = strchr(seg, '\n');
                if (nl && (nl - seg) < 24) s = seg + (nl - seg) + 1;
            }
            // Trim blank lines around the segment (fences leave them behind).
            while (*s == '\n' || *s == '\r') s++;
            size_t len = strlen(s);
            while (len && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
            if (len) {
                if (code) {
                    if (first) { prose_add(T("Ecco:", "Here you go:"), true); first = false; }
                    code_add(s);
                } else {
                    prose_add(s, first);
                    first = false;
                }
            }
        }
        if (!f) break;
        p = f + 3;
        code = !code;
    }
    if (first) prose_add(T("Non lo so.", "I don't know."), true);
}

void meta_format(const anima_result_t &r, char *m, size_t cap) {
    const char *tier = r.tier == ANIMA_TIER_COMMAND ? "L0"
                     : r.tier == ANIMA_TIER_FACT    ? "L1/KGE"
                     : r.tier == ANIMA_TIER_STITCH  ? "L2"
                     : r.tier == ANIMA_TIER_REMOTE  ? "cloud" : "-";
    snprintf(m, cap, "%s \xC2\xB7 %d%%%s%.120s", tier, r.confidence,
             r.trace[0] ? " \xC2\xB7 " : "", r.trace);
}

void chat_scroll_bottom(void) {
    if (!s_chat) return;
    lv_obj_update_layout(s_chat);
    int32_t bottom = lv_obj_get_scroll_bottom(s_chat);
    if (bottom > 0) lv_obj_scroll_by(s_chat, 0, -bottom, LV_ANIM_OFF);
}

// ---------------------------------------------------------------- chat history (SD)

void hist_push(const char *line);

// One NDJSON line per turn: {"r":"u"|"a","t":text,"m":meta?}. Bounded crudely: past 24 KB the
// log restarts (old turns drop; the ENGINE's own session memory on SD is separate and untouched).
void history_append(char role, const char *text, const char *meta) {
    struct stat st;
    if (stat(kChatLog, &st) == 0 && st.st_size > 24 * 1024) remove(kChatLog);
    FILE *f = fopen(kChatLog, "a");
    if (!f) return;
    cJSON *o = cJSON_CreateObject();
    char r[2] = {role, 0};
    cJSON_AddStringToObject(o, "r", r);
    cJSON_AddStringToObject(o, "t", text);
    if (meta && meta[0]) cJSON_AddStringToObject(o, "m", meta);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (txt) { fputs(txt, f); fputc('\n', f); cJSON_free(txt); }
    fclose(f);
}

// Replays the last conversation into the transcript; the questions also refill ↑ history.
bool history_load(void) {
    FILE *f = fopen(kChatLog, "r");
    if (!f) return false;
    char *line = (char *)heap_caps_malloc(2304, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) { fclose(f); return false; }
    bool any = false;
    while (fgets(line, 2304, f)) {
        cJSON *o = cJSON_Parse(line);
        if (!o) continue;
        cJSON *r = cJSON_GetObjectItem(o, "r"), *t = cJSON_GetObjectItem(o, "t"),
              *m = cJSON_GetObjectItem(o, "m");
        if (cJSON_IsString(r) && cJSON_IsString(t) && t->valuestring[0]) {
            if (r->valuestring[0] == 'u') { user_add(t->valuestring); hist_push(t->valuestring); }
            else reply_render(t->valuestring);
            if (cJSON_IsString(m) && m->valuestring[0]) meta_add(m->valuestring);
            any = true;
        }
        cJSON_Delete(o);
    }
    heap_caps_free(line);
    fclose(f);
    return any;
}

// ---------------------------------------------------------------- status footer

void status_refresh(void) {
    if (!s_status) return;
    char b[96];
    uint32_t c = kDim;
    if (!nucleo_anima_online_available()) {
        snprintf(b, sizeof b, G_DOT " offline");
    } else if (s_teach_state == 1) {
        snprintf(b, sizeof b, G_DOT " online " G_MID " %s", s_teach_model[0] ? s_teach_model : s_teach_prov);
        c = kGreen;
    } else if (s_teach_state == 0) {
        snprintf(b, sizeof b, G_DOT " online " G_MID " %s", T("cervello offline", "offline brain"));
        c = kBlue;
    } else {
        snprintf(b, sizeof b, G_DOT " online");
        c = kBlue;
    }
    nv_kit_label_set(s_status, b);
    nv_kit_text_color(s_status, lv_color_hex(c));
}

// ---------------------------------------------------------------- spinner

// The CLI-style "working" row: a pulsing dot, a rotating verb, elapsed seconds and the way out.
lv_obj_t *spinner_add(bool voice) {
    s_spin_voice = voice;
    s_spin_t0 = lv_tick_get();
    lv_obj_t *row = row_add(G_MID, kAccent, "", kAccent);
    s_spin = row_text(row);
    return row;
}

void spinner_tick(void) {
    if (!s_pending || !s_spin) return;
    static const char *kFrames[] = {G_MID, "\xE2\x80\xA2", G_DOT, "\xE2\x80\xA2"};   // · • ● •
    static const char *kVerbIt[] = {"Penso", "Ragiono", "Cerco", "Consulto la memoria", "Collego i fatti"};
    static const char *kVerbEn[] = {"Thinking", "Reasoning", "Searching", "Recalling", "Connecting facts"};
    const uint32_t ms = lv_tick_elaps(s_spin_t0);
    lv_label_set_text(lv_obj_get_child(s_pending, 0), kFrames[(ms / 180) % 4]);
    const char *verb = s_spin_voice ? T("Trascrivo", "Transcribing")
                                    : (lang_en() ? kVerbEn : kVerbIt)[(ms / 2500) % 5];
    char b[96];
    snprintf(b, sizeof b, "%s" G_ELL " (%us " G_MID " %s)", verb, (unsigned)(ms / 1000),
             T("esc per interrompere", "esc to interrupt"));
    nv_kit_label_set(s_spin, b);
}

void spinner_drop(void) {
    if (s_pending) lv_obj_delete(s_pending);
    s_pending = nullptr;
    s_spin = nullptr;
}

// Esc / ^C while ANIMA works: drop the turn (the worker finishes it unseen; the result is stale).
bool interrupt(void) {
    if (!s_pending && !s_voice_wait) return false;
    s_gen++;
    s_voice_wait = false;
    spinner_drop();
    meta_add(T("Interrotto " G_MID " cosa faccio invece?", "Interrupted " G_MID " what should I do instead?"), kRed);
    chat_scroll_bottom();
    return true;
}

// ---------------------------------------------------------------- welcome

void submit_cb(lv_event_t *);   // defined in the input section below

void tip_cb(lv_event_t *e) {
    if (!s_input || s_pending) return;
    lv_textarea_set_text(s_input, (const char *)lv_event_get_user_data(e));
    submit_cb(nullptr);
}

// The CLI welcome card: framed title, version, where to start; tappable example prompts.
void welcome_add(void) {
    lv_obj_t *card = lv_obj_create(s_chat);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kAccent), 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_hor(card, 12, 0);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = mono_label(card, "", kFgBold);
    char b[160];
    snprintf(b, sizeof b, G_DOT " ANIMA  " G_MID "  %s " G_MID " NucleoOS %s",
             T("assistente offline", "offline assistant"), nv_ota_running_version());
    lv_label_set_text(title, b);
    lv_obj_t *sub = mono_label(card, T("  /help per comandi e tasti " G_MID " /status per lo stato " G_MID " /config per le impostazioni",
                                       "  /help for commands and keys " G_MID " /status for state " G_MID " /config for settings"), kDim);
    lv_obj_set_width(sub, lv_pct(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);

    lv_obj_t *hdr = mono_label(s_chat, T("Prova a chiedere:", "Try asking:"), kDim);
    lv_obj_set_style_margin_top(hdr, 4, 0);
    static const char *kIt[] = {"Quanto fa 128 per 46?", "Chi era Alan Turing?", "Che ore sono?", "Apri musica"};
    static const char *kEn[] = {"What is 128 times 46?", "Who was Alan Turing?", "What time is it?", "Open music"};
    const char **tips = lang_en() ? kEn : kIt;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = row_add(G_ARROW, kAccent, tips[i], kFg);
        lv_obj_set_style_pad_left(row, kIndent / 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(kBand), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, tip_cb, LV_EVENT_CLICKED, (void *)tips[i]);
    }
}

// ---------------------------------------------------------------- result handling

// Live ANIMA_ACT_SYSTEM answers now come from the shared resolver (nv_anima_system.cpp): one
// implementation for the native chat AND the web REST path, covering every SYSTEM key the
// engine emits (time/date/season/storage/capabilities/network/ram/version/uptime/...). The old
// local version knew only time+storage, so every other key leaked the raw "{value}" template.

void poll_cb(lv_timer_t *) {
    if (s_voice_wait && nv_audio_mic_state() == NV_MIC_IDLE) {   // WAV finalized -> transcribe now
        s_voice_wait = false;
        worker_ensure();
        worker_send(JOB_VOICE, "");
    }
    // Still recording or waiting for the WAV: the last finished generation is the PREVIOUS turn's.
    if (s_voice_wait || s_done_gen != s_gen || !s_pending) { spinner_tick(); return; }
    spinner_drop();

    if (s_done_kind == JOB_VOICE) {
        // The transcript runs through the normal path, as if typed.
        if (!s_voice[0]) {
            meta_add(T("Trascrizione fallita (chiave? rete?)", "Transcription failed (key? network?)"), kRed);
            chat_scroll_bottom();
            return;
        }
        if (s_input) { lv_textarea_set_text(s_input, s_voice); submit_cb(nullptr); }
        return;
    }

    const anima_result_t &r = s_res;

    const char *text = s_long[0] ? s_long : r.reply;
    char live[640];   // capabilities lists the app registry — far longer than a clock reading
    if (r.action == ANIMA_ACT_SYSTEM) {
        nv_anima_system_reply(r.arg, text, lang_en(), live, sizeof live);
        text = live;
    } else if (r.action == ANIMA_ACT_LAUNCH && r.arg[0]) {
        strlcpy(live, text, sizeof live);   // launch replies are one short sentence
        nv_anima_pretty_launch(live, sizeof live, r.arg);   // "Apro calc." -> "Apro Calcolatrice."
        text = live;
    }
    if (!text[0]) text = T("Non lo so.", "I don't know.");

    // Typed tool proposals (set_volume/set_brightness) really happen — the engine only proposes,
    // the OS layer executes (same contract as the web handler).
    bool tool_ok = true;
    if (r.action == ANIMA_ACT_TOOL) tool_ok = nv_anima_os_exec(r.intent, r.arg);

    reply_render(text);
    // The turn's working, CLI-style: what was understood, which tool ran, how it was answered.
    char line[160];
    if (r.corrected[0]) {
        snprintf(line, sizeof line, "%s \"%.60s\"", T("ho capito", "understood"), r.corrected);
        meta_add(line);
    }
    if (r.action == ANIMA_ACT_LAUNCH && r.arg[0]) {
        snprintf(line, sizeof line, "open_app(%.60s)", r.arg);
        meta_add(line, kBlue);
    } else if (r.action == ANIMA_ACT_TOOL && r.intent[0]) {
        snprintf(line, sizeof line, "%.24s(%.60s)%s", r.intent, r.arg, tool_ok ? "" : T("  fallito", "  failed"));
        meta_add(line, tool_ok ? kBlue : kRed);
    }
    char meta[196];
    meta_format(r, meta, sizeof meta);
    const bool has_meta = r.tier != ANIMA_TIER_NONE || r.trace[0];
    if (has_meta) meta_add(meta);
    history_append('a', text, has_meta ? meta : nullptr);
    status_refresh();
    chat_scroll_bottom();

    // Agentic launch: the reply lands, then the requested app takes over
    // (nv_ui_open_app tears this page down — do it last, via a one-shot timer so
    // this callback unwinds cleanly first).
    if (r.action == ANIMA_ACT_LAUNCH && r.arg[0]) {
        // Tracked timer (deleted in page_deleted): a swipe home within the 700 ms used to leave it
        // armed and it opened the app over the launcher. The id travels as a heap copy.
        if (s_launch_timer) { lv_timer_delete(s_launch_timer); s_launch_timer = nullptr; }
        char *app_id = strdup(r.arg);
        if (app_id) {
            s_launch_timer = lv_timer_create([](lv_timer_t *tm) {
                char *id = static_cast<char *>(lv_timer_get_user_data(tm));
                s_launch_timer = nullptr;
                lv_timer_delete(tm);
                const NvApp *a = nv_ui_find_app(id);
                free(id);
                if (a) nv_ui_open_app(a);
            }, 700, app_id);
            lv_timer_set_repeat_count(s_launch_timer, 1);
        }
    }
}

// ---------------------------------------------------------------- prompt history

void hist_push(const char *line) {
    if (!line || !line[0]) return;
    if (!s_hist) s_hist = (char (*)[kInputCap])heap_caps_calloc(kHistMax, kInputCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hist) return;
    if (s_hist_n && !strcmp(s_hist[s_hist_n - 1], line)) { s_hist_pos = s_hist_n; return; }
    if (s_hist_n == kHistMax) {
        memmove(s_hist[0], s_hist[1], (size_t)(kHistMax - 1) * kInputCap);
        s_hist_n--;
    }
    strlcpy(s_hist[s_hist_n++], line, kInputCap);
    s_hist_pos = s_hist_n;
}

void hist_nav(int dir) {
    if (!s_input || !s_hist || !s_hist_n) return;
    int pos = s_hist_pos + dir;
    if (pos < 0) pos = 0;
    if (pos > s_hist_n) pos = s_hist_n;
    s_hist_pos = pos;
    lv_textarea_set_text(s_input, pos == s_hist_n ? "" : s_hist[pos]);
    lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST);
}

// ---------------------------------------------------------------- slash commands

void settings_toggle_cb(lv_event_t *);
void mic_cb(lv_event_t *);
void session_clear(void);

void cmd_help(const char *);
void cmd_clear(const char *) { session_clear(); }
void cmd_status(const char *);
void cmd_config(const char *) { settings_toggle_cb(nullptr); }
void cmd_model(const char *);
void cmd_l1(const char *);
void cmd_voice(const char *) { mic_cb(nullptr); }
void cmd_exit(const char *) { lv_async_call([](void *) { nv_ui_close_app(); }, nullptr); }

struct Cmd {
    const char *name, *arg, *it, *en;
    void (*fn)(const char *arg);
};
const Cmd kCmds[] = {
    {"help",   "",               "comandi e tasti",                       "commands and keys",                   cmd_help},
    {"clear",  "",               "nuova conversazione (cancella tutto)",  "new conversation (forget it all)",    cmd_clear},
    {"status", "",               "motore, rete, teacher, statistiche",    "engine, network, teacher, stats",     cmd_status},
    {"config", "",               "impostazioni: teacher cloud, indice L1", "settings: cloud teacher, L1 index",  cmd_config},
    {"model",  "",               "il teacher cloud in uso",               "the cloud teacher in use",            cmd_model},
    {"l1",     "[auto|on|off]",  "politica del cervello offline",         "offline brain policy",                cmd_l1},
    {"voice",  "",               "fai una domanda a voce",                "ask by voice",                        cmd_voice},
    {"exit",   "",               "chiudi ANIMA",                          "close ANIMA",                         cmd_exit},
};
constexpr int kNumCmds = (int)(sizeof kCmds / sizeof kCmds[0]);

// Echo a command line the way the shell does, without sending it to the engine or the chat log.
void cmd_echo(const char *line) { user_add(line); }

void cmd_help(const char *) {
    for (const Cmd &c : kCmds) {
        char b[128];
        snprintf(b, sizeof b, "/%s%s%s  %s", c.name, c.arg[0] ? " " : "", c.arg, lang_en() ? c.en : c.it);
        meta_add(b, kFg);
    }
    meta_add(T("Invio invia " G_MID " " G_UP G_DOWN " cronologia " G_MID " Tab completa " G_MID
               " Esc/^C interrompe " G_MID " ^L pulisce lo schermo",
               "Enter sends " G_MID " " G_UP G_DOWN " history " G_MID " Tab completes " G_MID
               " Esc/^C interrupts " G_MID " ^L clears the screen"));
}

void cmd_model(const char *) {
    // Explicit ask: look now if the worker hasn't yet (a one-off, like the settings statistics).
    if (s_teach_state < 0) teacher_snapshot();
    char b[128];
    if (s_teach_state == 1) {
        snprintf(b, sizeof b, "teacher: %s " G_MID " %s%s", s_teach_prov, s_teach_model[0] ? s_teach_model : "default",
                 nucleo_anima_online_available() ? "" : T("  (ora offline)", "  (offline now)"));
        meta_add(b, kFg);
    } else {
        meta_add(T("Nessun teacher cloud: rispondo col cervello offline. /config per aggiungere una chiave.",
                   "No cloud teacher: I answer with the offline brain. /config to add a key."), kFg);
    }
}

const char *l1_name(int mode) {
    return mode == ANIMA_L1_ON ? "on" : mode == ANIMA_L1_OFF ? "off" : "auto";
}

void cmd_l1(const char *arg) {
    int mode = -1;
    if (!strcmp(arg, "auto")) mode = ANIMA_L1_AUTO;
    else if (!strcmp(arg, "on") || !strcmp(arg, "sempre")) mode = ANIMA_L1_ON;
    else if (!strcmp(arg, "off") || !strcmp(arg, "spento")) mode = ANIMA_L1_OFF;
    char b[96];
    if (mode < 0) {
        if (arg[0]) { meta_add(T("Uso: /l1 auto|on|off", "Usage: /l1 auto|on|off"), kRed); return; }
        snprintf(b, sizeof b, "L1: %s " G_MID " %u KB RAM", l1_name(nucleo_anima_l1_get_mode()),
                 (unsigned)(nucleo_anima_l1_heap_bytes() / 1024));
        meta_add(b, kFg);
        return;
    }
    nucleo_anima_l1_set_mode(mode);
    nv_config_set_int("anima.l1", mode);
    snprintf(b, sizeof b, "L1 " G_ARROW " %s", l1_name(mode));
    meta_add(b, kGreen);
}

void cmd_status(const char *) {
    anima_diag_t d;
    nucleo_anima_diag(&d);
    char b[160];
    snprintf(b, sizeof b, T("Domande %u " G_MID " L0 %u " G_MID " L1/KGE %u " G_MID " L2 %u " G_MID " cloud %u " G_MID " \"non lo so\" %u",
                            "Queries %u " G_MID " L0 %u " G_MID " L1/KGE %u " G_MID " L2 %u " G_MID " cloud %u " G_MID " \"don't know\" %u"),
             (unsigned)d.queries, (unsigned)d.t_command, (unsigned)d.t_fact, (unsigned)d.t_stitch,
             (unsigned)d.t_remote, (unsigned)d.t_none);
    meta_add(b, kFg);
    snprintf(b, sizeof b, T("Ultima confidenza %d%% " G_MID " indice L1 %s, %u KB RAM",
                            "Last confidence %d%% " G_MID " L1 index %s, %u KB RAM"),
             d.last_conf, l1_name(nucleo_anima_l1_get_mode()), (unsigned)(nucleo_anima_l1_heap_bytes() / 1024));
    meta_add(b, kFg);
    snprintf(b, sizeof b, "%s: %s", T("Rete", "Network"), nucleo_anima_online_available() ? "online" : "offline");
    meta_add(b, kFg);
    cmd_model(nullptr);
}

const Cmd *cmd_find(const char *name, size_t n) {
    for (const Cmd &c : kCmds)
        if (strlen(c.name) == n && !strncmp(c.name, name, n)) return &c;
    return nullptr;
}

// ---------------------------------------------------------------- slash-command menu

void menu_hide(void) {
    s_menu_n = 0;
    if (s_menu) { lv_obj_clean(s_menu); lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN); }
}
bool menu_open(void) { return s_menu_n > 0; }

void menu_pick_cb(lv_event_t *e);

void menu_render(void) {
    if (!s_menu) return;
    lv_obj_clean(s_menu);
    for (int i = 0; i < s_menu_n; i++) {
        const Cmd &c = kCmds[s_menu_ids[i]];
        const bool sel = i == s_menu_sel;
        lv_obj_t *row = lv_obj_create(s_menu);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);   // keep the keyboard on the prompt
        lv_obj_add_event_cb(row, menu_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)s_menu_ids[i]);
        char name[40];
        snprintf(name, sizeof name, "%s/%s%s%s", sel ? G_ARROW " " : "  ", c.name, c.arg[0] ? " " : "", c.arg);
        lv_obj_t *n = mono_label(row, name, sel ? kAccent : kBlue);
        lv_obj_set_width(n, 230);
        lv_obj_t *dsc = mono_label(row, lang_en() ? c.en : c.it, sel ? kFg : kDim);
        lv_obj_set_flex_grow(dsc, 1);
        lv_label_set_long_mode(dsc, LV_LABEL_LONG_DOT);
    }
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
}

// The menu follows the prompt: "/" plus a command prefix (no argument typed yet) lists matches.
void menu_update(void) {
    const char *t = s_input ? lv_textarea_get_text(s_input) : "";
    if (!t || t[0] != '/' || strchr(t, ' ')) { menu_hide(); return; }
    const size_t n = strlen(t + 1);
    int prev = s_menu_n ? s_menu_ids[s_menu_sel] : -1;
    s_menu_n = 0;
    s_menu_sel = 0;
    for (int i = 0; i < kNumCmds && s_menu_n < kMenuMax; i++) {
        if (strncmp(kCmds[i].name, t + 1, n)) continue;
        if (i == prev) s_menu_sel = s_menu_n;
        s_menu_ids[s_menu_n++] = i;
    }
    if (!s_menu_n) { menu_hide(); return; }
    menu_render();
}

void menu_move(int dir) {
    if (!s_menu_n) return;
    s_menu_sel = (s_menu_sel + dir + s_menu_n) % s_menu_n;
    menu_render();
}

// Tab: the prompt becomes the highlighted command (plus a space when it takes an argument).
void menu_complete(void) {
    if (!s_menu_n || !s_input) return;
    const Cmd &c = kCmds[s_menu_ids[s_menu_sel]];
    char b[40];
    snprintf(b, sizeof b, "/%s%s", c.name, c.arg[0] ? " " : "");
    lv_textarea_set_text(s_input, b);
    lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST);
}

void cmd_run(const Cmd &c, const char *line, const char *arg) {
    hist_push(line);
    cmd_echo(line);
    if (s_input) lv_textarea_set_text(s_input, "");
    menu_hide();
    c.fn(arg);
    chat_scroll_bottom();
}

void menu_pick_cb(lv_event_t *e) {
    const Cmd &c = kCmds[(int)(intptr_t)lv_event_get_user_data(e)];
    char line[40];
    snprintf(line, sizeof line, "/%s", c.name);
    cmd_run(c, line, "");
}

// A "/..." line: the exact command, else the highlighted menu entry for a bare prefix.
void slash_submit(const char *txt) {
    char line[kInputCap];
    strlcpy(line, txt, sizeof line);
    const char *name = line + 1;
    const char *sp = strchr(name, ' ');
    const size_t n = sp ? (size_t)(sp - name) : strlen(name);
    const char *arg = sp ? sp + 1 : "";
    while (*arg == ' ') arg++;
    const Cmd *c = cmd_find(name, n);
    if (!c && !sp && s_menu_n) c = &kCmds[s_menu_ids[s_menu_sel]];
    if (c) { cmd_run(*c, line, arg); return; }
    hist_push(line);
    cmd_echo(line);
    if (s_input) lv_textarea_set_text(s_input, "");
    menu_hide();
    char b[96];
    snprintf(b, sizeof b, T("Comando sconosciuto: %.40s " G_MID " /help per l'elenco",
                            "Unknown command: %.40s " G_MID " /help for the list"), line);
    meta_add(b, kRed);
    chat_scroll_bottom();
}

// ---------------------------------------------------------------- input

bool settings_showing(void) { return s_settings && !lv_obj_has_flag(s_settings, LV_OBJ_FLAG_HIDDEN); }

void submit_cb(lv_event_t *) {
    if (!s_input || settings_showing()) return;
    const char *raw = lv_textarea_get_text(s_input);
    if (!raw) return;
    while (*raw == ' ') raw++;
    if (!raw[0]) return;
    if (raw[0] == '/') { slash_submit(raw); return; }
    if (s_pending) return;   // one in-flight query at a time (Esc interrupts it)

    strncpy(s_req, raw, sizeof s_req - 1);
    s_req[sizeof s_req - 1] = '\0';
    user_add(s_req);
    hist_push(s_req);
    history_append('u', s_req, nullptr);
    lv_textarea_set_text(s_input, "");
    menu_hide();

    s_pending = spinner_add(false);
    spinner_tick();
    chat_scroll_bottom();

    snprintf(s_lang, sizeof s_lang, "%s", lang_en() ? "en" : "it");
    worker_ensure();
    worker_send(JOB_QUERY, s_req);
}

void input_changed_cb(lv_event_t *) { menu_update(); }

// ^L: a clean screen; the conversation (engine session, chat log) goes on.
void screen_clear(void) {
    if (!s_chat || s_pending) return;
    lv_obj_clean(s_chat);
}

// Esc: interrupt ANIMA, else drop the half-typed command. False = let the system have it.
bool key_esc(void) {
    if (interrupt()) return true;
    if (s_input && lv_textarea_get_text(s_input)[0] == '/') { lv_textarea_set_text(s_input, ""); menu_hide(); return true; }
    return false;
}

// Hardware / remote keys (nv_ime key hook): the shell's line-editing keys.
bool input_key_hook(lv_obj_t *, int key, char ctrl) {
    if (ctrl) {
        switch (ctrl) {
            case 'c': if (!interrupt() && s_input) lv_textarea_set_text(s_input, ""); return true;
            case 'u': if (s_input) lv_textarea_set_text(s_input, ""); return true;
            case 'l': screen_clear(); return true;
            case 'p': hist_nav(-1); return true;
            case 'n': hist_nav(+1); return true;
            default:  return false;
        }
    }
    switch (key) {
        case NV_IME_RK_UP:   if (menu_open()) menu_move(-1); else hist_nav(-1); return true;
        case NV_IME_RK_DOWN: if (menu_open()) menu_move(+1); else hist_nav(+1); return true;
        case NV_IME_RK_TAB:  menu_complete(); return true;
        case NV_IME_RK_ESC:  return key_esc();
        default:             return false;
    }
}

// Mic toggle: first tap records (icon -> stop), second tap stops and hands the WAV to the
// worker for cloud transcription; the transcript then goes through the normal submit path.
void mic_cb(lv_event_t *) {
    const bool en = lang_en();
    if (!s_recording) {
        if (s_pending) return;                     // one thing at a time
        mkdir("/sdcard/data", 0775);
        mkdir("/sdcard/data/anima", 0775);
        if (!nv_audio_rec_start(kVoiceWav)) {
            meta_add(en ? "Microphone unavailable" : "Microfono non disponibile", kRed);
            chat_scroll_bottom();
            return;
        }
        s_recording = true;
        if (s_mic) lv_label_set_text(s_mic, "Stop");
        nv_ui_toast(en ? "Listening... tap to stop" : "Ti ascolto... tocca per fermare");
        return;
    }
    nv_audio_rec_stop();   // asynchronous: the mic task patches the WAV header + closes the file
    s_recording = false;
    if (s_mic) lv_label_set_text(s_mic, "Mic");
    s_pending = spinner_add(true);
    spinner_tick();
    chat_scroll_bottom();
    snprintf(s_lang, sizeof s_lang, "%s", lang_en() ? "en" : "it");
    // Dispatch from poll_cb once the mic is IDLE: sending now uploaded a WAV whose data-size was
    // still 0 / whose tail was unflushed -> empty or garbage transcript.
    s_voice_wait = true;
}

// ---------------------------------------------------------------- extra keys (as in the Terminal)

enum : uint8_t { K_ESC, K_TAB, K_SLASH, K_UP, K_DOWN, K_MIC, K_GEAR };
struct ExtraKey { const char *label; uint8_t action; };
const ExtraKey kKeys[] = {
    {"Esc", K_ESC}, {"Tab", K_TAB}, {"/", K_SLASH}, {G_UP, K_UP}, {G_DOWN, K_DOWN},
    {"Mic", K_MIC}, {LV_SYMBOL_SETTINGS, K_GEAR},
};

void extra_key_cb(lv_event_t *e) {
    const ExtraKey *k = (const ExtraKey *)lv_event_get_user_data(e);
    if (!s_input) return;
    switch (k->action) {
        case K_ESC:   if (!key_esc() && settings_showing()) settings_toggle_cb(nullptr); break;
        case K_TAB:   menu_complete(); break;
        case K_SLASH:
            if (settings_showing()) break;
            if (!lv_textarea_get_text(s_input)[0]) lv_textarea_set_text(s_input, "/");
            else lv_textarea_add_text(s_input, "/");
            nv_ime_focus(s_input);
            break;
        case K_UP:    if (menu_open()) menu_move(-1); else hist_nav(-1); break;
        case K_DOWN:  if (menu_open()) menu_move(+1); else hist_nav(+1); break;
        case K_MIC:   mic_cb(nullptr); break;
        case K_GEAR:  settings_toggle_cb(nullptr); break;
        default: break;
    }
}

void build_keys(lv_obj_t *root) {
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kKeyBg), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (const ExtraKey &k : kKeys) {
        lv_obj_t *b = lv_obj_create(bar);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, 40);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(kKey), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(kKeyDown), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        // Pressing a key must not take focus from the prompt (that would drop the keyboard).
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, extra_key_cb, LV_EVENT_CLICKED, (void *)&k);
        lv_obj_t *l = mono_label(b, k.label, kFg);
        lv_obj_center(l);
        if (k.action == K_MIC) s_mic = l;
        if (k.action == K_GEAR) s_gear = l;
    }
}

// The prompt field: no card of its own, the bordered box around it is the frame.
void style_prompt_input(lv_obj_t *ta) {
    static const lv_style_selector_t kStates[] = {
        LV_STATE_DEFAULT, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY, LV_STATE_EDITED, LV_STATE_PRESSED,
    };
    for (lv_style_selector_t s : kStates) {
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, s);
        lv_obj_set_style_border_width(ta, 0, s);
        lv_obj_set_style_outline_width(ta, 0, s);
        lv_obj_set_style_shadow_width(ta, 0, s);
    }
    lv_obj_set_style_pad_all(ta, 0, 0);
    lv_obj_set_style_text_font(ta, &s_mono, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kFgBold), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kDim), LV_PART_TEXTAREA_PLACEHOLDER);
    const lv_style_selector_t cur = (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED;
    lv_obj_set_style_border_color(ta, lv_color_hex(kFg), cur);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
}

// ---------------------------------------------------------------- settings view

void stats_refresh(void) {
    if (!s_stats) return;
    anima_diag_t d;
    nucleo_anima_diag(&d);
    const bool en = lang_en();

    char online[96];
    if (nucleo_anima_online_available()) {
        if (s_teach_state < 0) teacher_snapshot();   // before the first turn only (see s_teach_state)
        if (s_teach_state == 1)
            snprintf(online, sizeof online, "online, teacher %.24s (%.48s)", s_teach_prov, s_teach_model);
        else
            snprintf(online, sizeof online, "%s", en ? "online, no teacher key" : "online, nessuna chiave teacher");
    } else {
        snprintf(online, sizeof online, "%s", "offline");
    }

    char b[420];
    snprintf(b, sizeof b,
             en ? "Queries: %u\nL0 commands: %u\nL1/KGE facts: %u\nL2 stitch: %u\nCloud: %u\n"
                  "Honest \"I don't know\": %u\nLast confidence: %d%%\n\nNetwork: %s\nL1 index RAM: %u KB"
                : "Domande: %u\nComandi L0: %u\nFatti L1/KGE: %u\nStitch L2: %u\nCloud: %u\n"
                  "\"Non lo so\" onesti: %u\nUltima confidenza: %d%%\n\nRete: %s\nRAM indice L1: %u KB",
             (unsigned)d.queries, (unsigned)d.t_command, (unsigned)d.t_fact, (unsigned)d.t_stitch,
             (unsigned)d.t_remote, (unsigned)d.t_none, d.last_conf, online,
             (unsigned)(nucleo_anima_l1_heap_bytes() / 1024));
    lv_label_set_text(s_stats, b);
}

void l1_mode_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    int mode = (int)lv_dropdown_get_selected(dd);   // 0 AUTO, 1 ON, 2 OFF — matches the enum
    nucleo_anima_l1_set_mode(mode);
    nv_config_set_int("anima.l1", mode);
    stats_refresh();
}

// /clear and the settings button: the engine forgets the session, the transcript starts over.
void session_clear(void) {
    nucleo_anima_reset_session();
    remove(kChatLog);
    if (s_chat) {
        s_gen++;                 // orphan any in-flight result
        s_voice_wait = false;
        s_pending = nullptr;     // its row goes with the clean below
        s_spin = nullptr;
        lv_obj_clean(s_chat);
        welcome_add();
    }
    stats_refresh();
}

void reset_session_cb(lv_event_t *) { session_clear(); }

// ---------------------------------------------------------------- teacher key manager

// Read teacher.json (may be absent). Caller owns the returned cJSON object (never NULL).
// Returns nullptr ONLY when the file exists but does not parse (truncated / corrupt): a caller that
// is about to WRITE must then refuse, or it overwrites every other provider's key with the single
// edited one — which is exactly what a 1.5 KB stack read of a grown vault used to cause.
cJSON *teacher_json_load(void) {
    struct stat st;
    if (stat(kTeacherPath, &st) != 0 || st.st_size <= 0) return cJSON_CreateObject();   // absent: start empty
    char *buf = nv_sealed_read(kTeacherPath, 32 * 1024, nullptr);   // sealed to this chip (API keys)
    if (!buf) return nullptr;                     // too big, or sealed by another device / damaged
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    return o;                                     // nullptr = unparsable: do NOT save over it
}

// Map the stored config to a dropdown slot (for prefill).
int teacher_provider_slot(cJSON *o) {
    cJSON *k = cJSON_GetObjectItem(o, "key");
    if (!cJSON_IsString(k) || !k->valuestring[0]) return PROV_AUTO;
    cJSON *p = cJSON_GetObjectItem(o, "provider");
    cJSON *b = cJSON_GetObjectItem(o, "base");
    const char *prov = cJSON_IsString(p) ? p->valuestring : "";
    const char *base = cJSON_IsString(b) ? b->valuestring : "";
    if (!strcmp(prov, "anthropic")) return PROV_ANTHROPIC;
    if (!strcmp(prov, "google"))    return PROV_GEMINI;
    if (strstr(base, "groq.com") || !base[0]) return PROV_GROQ;
    return PROV_CUSTOM;
}

void teacher_prefill(void) {
    cJSON *o = teacher_json_load();
    if (!o) o = cJSON_CreateObject();     // unreadable vault: prefill empty (saving is refused separately)
    lv_dropdown_set_selected(s_prov_dd, (uint32_t)teacher_provider_slot(o));
    cJSON *m = cJSON_GetObjectItem(o, "model"), *b = cJSON_GetObjectItem(o, "base"),
          *k = cJSON_GetObjectItem(o, "key");
    if (cJSON_IsString(m)) lv_textarea_set_text(s_model_ta, m->valuestring);
    if (cJSON_IsString(b)) lv_textarea_set_text(s_base_ta, b->valuestring);
    if (cJSON_IsString(k) && k->valuestring[0]) {
        // Never echo the stored key: show a masked hint with the tail, keep the field empty.
        size_t n = strlen(k->valuestring);
        char hint[32];
        snprintf(hint, sizeof hint, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2%s",
                 n > 4 ? k->valuestring + n - 4 : "");
        lv_textarea_set_placeholder_text(s_key_ta, hint);
    }
    s_key_dirty = false;
    cJSON_Delete(o);
}

void key_edited_cb(lv_event_t *) { s_key_dirty = true; }

// Replace (or drop, when value is empty) one string field on the config object.
void json_put(cJSON *o, const char *name, const char *value) {
    cJSON_DeleteItemFromObject(o, name);
    if (value && value[0]) cJSON_AddStringToObject(o, name, value);
}

void teacher_save_cb(lv_event_t *) {
    const bool en = lang_en();
    const int slot = (int)lv_dropdown_get_selected(s_prov_dd);
    const char *key   = lv_textarea_get_text(s_key_ta);
    const char *model = lv_textarea_get_text(s_model_ta);
    const char *base  = (slot == PROV_CUSTOM) ? lv_textarea_get_text(s_base_ta) : kProvBase[slot];

    if (slot == PROV_CUSTOM && (!base || !base[0])) {
        nv_ui_toast(en ? "Custom provider needs a base URL" : "Il provider custom richiede una base URL");
        return;
    }

    cJSON *o = teacher_json_load();       // read-modify-write: whisper/profile fields survive
    if (!o) {                             // vault exists but is unreadable: never overwrite it blindly
        nv_ui_toast(en ? "teacher.json is unreadable — not saved" : "teacher.json illeggibile — non salvato");
        return;
    }
    if (slot == PROV_AUTO) {
        // No cloud key: the teacher tier stands down (a nucleomind on the LAN still auto-serves).
        cJSON_DeleteItemFromObject(o, "key");
    } else {
        if (s_key_dirty && key[0]) json_put(o, "key", key);
        cJSON *have = cJSON_GetObjectItem(o, "key");
        if (!cJSON_IsString(have) || !have->valuestring[0]) {
            cJSON_Delete(o);
            nv_ui_toast(en ? "Paste an API key first" : "Prima incolla una chiave API");
            return;
        }
        json_put(o, "provider", kProvName[slot]);
        json_put(o, "base", base);
        json_put(o, "model", model);      // empty -> engine default for the provider
        cJSON_DeleteItemFromObject(o, "version");   // engine default anthropic-version
    }

    mkdir("/sdcard/data", 0775);          // fresh card without the knowledge pack
    mkdir("/sdcard/data/anima", 0775);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    bool ok = false;
    if (txt) {
        // Write-then-rename inside nv_sealed_write: a worker reading the vault mid-turn (or a power
        // cut) must never see a truncated/empty file — "wb" on the live path erased every key.
        ok = nv_sealed_write(kTeacherPath, txt, strlen(txt));
        memset(txt, 0, strlen(txt));
        cJSON_free(txt);
    }
    if (ok) {
        lv_textarea_set_text(s_key_ta, "");
        teacher_prefill();                // refresh the masked hint
        s_teach_state = -1;               // footer: re-read by the worker on the next turn
        stats_refresh();
    }
    nv_ui_toast(ok ? (en ? "Teacher saved" : "Teacher salvato")
                   : (en ? "SD write failed" : "Scrittura SD fallita"));
}

void settings_toggle_cb(lv_event_t *) {
    if (!s_settings) return;
    // Settings replace the transcript, the prompt and its footer; the extra keys stay (the gear
    // key, now a close key, and Esc lead back, as do the system Back / Esc).
    const bool showing = settings_showing();
    lv_obj_t *convo[] = {s_chat, s_box ? lv_obj_get_parent(s_box) : nullptr, s_footer};
    if (showing) {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t *o : convo) if (o) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        if (s_gear) lv_label_set_text(s_gear, LV_SYMBOL_SETTINGS);
        nv_ui_set_back(nullptr);
        status_refresh();
        chat_scroll_bottom();
    } else {
        nv_ui_set_back([] { settings_toggle_cb(nullptr); });   // Back / Esc closes the settings view
        nv_ime_hide();
        menu_hide();
        stats_refresh();
        for (lv_obj_t *o : convo) if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        if (s_gear) lv_label_set_text(s_gear, LV_SYMBOL_CLOSE);
    }
}

lv_obj_t *settings_heading(const char *text) {
    lv_obj_t *l = mono_label(s_settings, text, kAccent);
    return l;
}

lv_obj_t *settings_hint(const char *text) {
    lv_obj_t *l = lv_label_create(s_settings);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(kDim), 0);
    lv_label_set_text(l, text);
    return l;
}

void settings_build(lv_obj_t *root) {
    const bool en = lang_en();

    s_settings = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_settings, 1);
    lv_obj_set_style_pad_row(s_settings, 12, 0);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);

    // L1 serving policy
    settings_heading(en ? "# Offline brain (L1 index)" : "# Cervello offline (indice L1)");

    lv_obj_t *dd = lv_dropdown_create(s_settings);
    lv_obj_set_width(dd, 360);
    lv_dropdown_set_options(dd, en ? "Auto (stand down when online)\nAlways on\nOff"
                                   : "Auto (si ritira se online)\nSempre attivo\nSpento");
    lv_dropdown_set_selected(dd, (uint32_t)nucleo_anima_l1_get_mode());
    lv_obj_add_event_cb(dd, l1_mode_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    settings_hint(en
        ? "Auto frees the semantic index while a cloud teacher answers; Always on forces the offline brain. Also: /l1 auto|on|off"
        : "Auto libera l'indice semantico quando risponde un teacher cloud; Sempre attivo forza il cervello offline. Anche: /l1 auto|on|off");

    // Teacher cloud (API key manager) — writes /sdcard/data/anima/teacher.json, same file the
    // engine and the web companion read. Read-modify-write keeps unrelated fields (whisper, ...).
    settings_heading(en ? "# Cloud teacher (API key)" : "# Teacher cloud (chiave API)");

    s_prov_dd = lv_dropdown_create(s_settings);
    lv_obj_set_width(s_prov_dd, 360);
    lv_dropdown_set_options(s_prov_dd, en
        ? "Auto (LAN phone / none)\nGroq\nClaude (Anthropic)\nGemini\nOpenAI-compatible (custom)"
        : "Auto (telefono LAN / nessuno)\nGroq\nClaude (Anthropic)\nGemini\nOpenAI-compatibile (custom)");

    s_key_ta = nv_kit_textarea_ex(s_settings, en ? "API key" : "Chiave API", true,
                                  NV_IME_PASSWORD, NV_IME_RET_DONE);
    lv_obj_set_width(s_key_ta, lv_pct(100));
    lv_obj_add_event_cb(s_key_ta, key_edited_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_model_ta = nv_kit_textarea_ex(s_settings, en ? "Model (empty = default)" : "Modello (vuoto = default)",
                                    true, NV_IME_EMAIL, NV_IME_RET_DONE);
    lv_obj_set_width(s_model_ta, lv_pct(100));

    s_base_ta = nv_kit_textarea_ex(s_settings, en ? "Base URL (custom only)" : "Base URL (solo custom)",
                                   true, NV_IME_URL, NV_IME_RET_DONE);
    lv_obj_set_width(s_base_ta, lv_pct(100));

    lv_obj_t *save = nv_kit_button(s_settings, en ? "Save teacher" : "Salva teacher", true);
    lv_obj_add_event_cb(save, teacher_save_cb, LV_EVENT_CLICKED, nullptr);

    teacher_prefill();

    // Telemetry
    settings_heading(en ? "# Statistics (since boot)" : "# Statistiche (da avvio)");

    s_stats = mono_label(s_settings, "", kFg);
    lv_obj_set_width(s_stats, lv_pct(100));
    lv_label_set_long_mode(s_stats, LV_LABEL_LONG_WRAP);

    // Session reset
    lv_obj_t *btn = nv_kit_button(s_settings, en ? "Clear conversation (/clear)" : "Pulisci conversazione (/clear)", false);
    lv_obj_add_event_cb(btn, reset_session_cb, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------- build / teardown

void page_deleted(lv_event_t *) {
    nv_ime_hide();
    if (s_recording) { nv_audio_rec_stop(); s_recording = false; }
    s_voice_wait = false;
    s_mic = nullptr;
    s_gen++;             // orphan any in-flight result (worker keeps running, result drops)
    if (s_poll) { lv_timer_delete(s_poll); s_poll = nullptr; }
    if (s_launch_timer) {   // armed "open app" shot: drop it (and its heap id) with the page
        free(lv_timer_get_user_data(s_launch_timer));
        lv_timer_delete(s_launch_timer);
        s_launch_timer = nullptr;
    }
    s_root = nullptr;
    s_chat = nullptr;
    s_menu = nullptr;
    s_menu_n = 0;
    s_box = nullptr;
    s_footer = nullptr;
    s_status = nullptr;
    s_input = nullptr;
    s_pending = nullptr;
    s_spin = nullptr;
    s_settings = nullptr;
    s_stats = nullptr;
    s_gear = nullptr;
    s_prov_dd = nullptr;
    s_key_ta = nullptr;
    s_model_ta = nullptr;
    s_base_ta = nullptr;
}

void anima_build(lv_obj_t *content) {
    if (!s_mono_ok) {
        s_mono = nv_font_mono_17;
        s_mono.fallback = &nv_font_14;
        s_mono_ok = true;
    }
    s_hist_pos = s_hist_n;
    s_menu_n = 0;

    // Edge to edge on the terminal background, like the Terminal app.
    s_root = lv_obj_create(content);
    lv_obj_t *root = s_root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root, lv_color_hex(kFg), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    s_chat = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_chat, 1);
    lv_obj_set_style_pad_hor(s_chat, 12, 0);
    lv_obj_set_style_pad_ver(s_chat, 10, 0);
    lv_obj_set_style_pad_row(s_chat, 4, 0);

    settings_build(root);   // hidden sibling of the transcript

    // Slash-command suggestions, right above the prompt (hidden until "/" is typed).
    s_menu = lv_obj_create(root);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_menu, 14, 0);
    lv_obj_set_style_pad_bottom(s_menu, 4, 0);
    lv_obj_set_style_pad_row(s_menu, 2, 0);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);

    // The prompt box: "> " and the field, framed like the CLI's input (inset by its wrapper).
    lv_obj_t *wrap = lv_obj_create(root);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(wrap, 8, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    s_box = lv_obj_create(wrap);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(s_box, 1, 0);
    lv_obj_set_style_border_color(s_box, lv_color_hex(kBorder), 0);
    lv_obj_set_style_radius(s_box, 6, 0);
    lv_obj_set_style_pad_hor(s_box, 10, 0);
    lv_obj_set_style_pad_ver(s_box, 8, 0);
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *gt = mono_label(s_box, ">", kFgBold);
    lv_obj_set_width(gt, kIndent);

    s_input = nv_kit_textarea_ex(s_box, T("Chiedimi qualcosa, o / per i comandi", "Ask me anything, or / for commands"),
                                 true, NV_IME_TEXT, NV_IME_RET_SEND);
    style_prompt_input(s_input);
    lv_obj_set_flex_grow(s_input, 1);
    lv_obj_add_event_cb(s_input, submit_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_input, input_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    nv_ime_set_key_hook(s_input, input_key_hook);
    nv_focus_prefer(s_input);   // keyboard: the first key goes to the question, not a sample tip

    // Footer: how to drive it on the left, where answers come from on the right.
    s_footer = lv_obj_create(root);
    lv_obj_remove_style_all(s_footer);
    lv_obj_set_size(s_footer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_footer, 14, 0);
    lv_obj_set_style_pad_ver(s_footer, 4, 0);
    lv_obj_set_flex_flow(s_footer, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hint = mono_label(s_footer, T("/ comandi " G_MID " " G_UP G_DOWN " cronologia " G_MID " esc interrompe",
                                            "/ commands " G_MID " " G_UP G_DOWN " history " G_MID " esc interrupts"), kDim);
    lv_obj_set_flex_grow(hint, 1);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    s_status = mono_label(s_footer, "", kDim);

    build_keys(root);

    welcome_add();
    history_load();           // the last conversation follows the welcome card
    status_refresh();
    chat_scroll_bottom();

    s_poll = lv_timer_create(poll_cb, 120, nullptr);
}

const NvApp kAnimaApp = {"anima", "Anima", &nv_icon_anima, 2u << 20, anima_build,
                         NV_STR_APP_ANIMA, nullptr};

}  // namespace

void anima_app_register(void) { nv_app_register(&kAnimaApp); }
