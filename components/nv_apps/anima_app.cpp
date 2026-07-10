// anima_app — native chat UI for ANIMA, the offline assistant (nv_anima engine).
// One scroll column of chat bubbles + an IME-bound input bar, plus a settings view
// (gear toggle): L1 serving policy, engine telemetry, online status, session reset.
// The engine cascade (L0 commands → L1 retrieval → HDC/KGE deduction → optional online
// tiers) runs on a persistent worker task so a query (SD reads, possibly a cloud fetch)
// never blocks LVGL; an lv_timer polls for the result. Engine init is lazy (first query,
// off the UI thread) per the no-boot-cost / no-static-bss rule. The worker outlives the
// app (idle on a queue, PSRAM stack, never writes internal flash) so reopen costs nothing.
//
// Glyph note: the system fonts cover ASCII + Latin-1 + bullet ONLY (gen_fonts.ps1). Card
// text carries typographic punctuation (' " – — …) that would render as boxes; latin1ize()
// maps it to Latin-1 equivalents before any text reaches a label.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"   // nv_kit_* + (transitively) nv_ime_hide
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
#include "nv_ui.h"       // nv_ui_toast
#include "nv_audio.h"    // voice input: nv_audio_rec_start/stop (mic -> WAV)
#include "cJSON.h"       // teacher.json read-modify-write (key manager) + chat log lines
#include "esp_attr.h"    // EXT_RAM_BSS_ATTR
#include "esp_heap_caps.h"

#include <sys/stat.h>    // mkdir for /sdcard/data/anima on a fresh card
#include <cctype>

#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr size_t kInputCap = 512;

// UI (valid only while the page lives; cleared on LV_EVENT_DELETE)
lv_obj_t   *s_chat     = nullptr;   // scroll column of bubbles
lv_obj_t   *s_bar      = nullptr;   // input bar (hidden while settings shown)
lv_obj_t   *s_input    = nullptr;
lv_obj_t   *s_pending  = nullptr;   // the "..." bubble label awaiting the current result
lv_obj_t   *s_settings = nullptr;   // settings view (hidden by default)
lv_obj_t   *s_stats    = nullptr;   // telemetry label inside settings
lv_obj_t   *s_gear     = nullptr;   // toggle button label swaps gear/close
lv_timer_t *s_poll     = nullptr;

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
struct anima_job { uint32_t gen; int kind; };
TaskHandle_t  s_worker = nullptr;
QueueHandle_t s_queue  = nullptr;  // depth 1, carries {generation, kind}
char          s_req[kInputCap];   // owned by the UI between submits, read by the worker
char          s_lang[4] = "it";
uint32_t      s_gen = 0;           // bumped per submit AND on teardown (stale results drop)
volatile uint32_t s_done_gen = 0;  // worker: generation of the finished result
volatile int   s_done_kind = JOB_QUERY;
// Cold-ish buffers -> PSRAM .bss (sequential copies only; internal SRAM stays for hot paths).
EXT_RAM_BSS_ATTR anima_result_t s_res;   // worker-filled, read by the poll timer after s_done_gen
EXT_RAM_BSS_ATTR char s_long[2048];      // worker copy of nucleo_anima_long_reply()
EXT_RAM_BSS_ATTR char s_san[2304];       // latin1ize scratch (LVGL thread only)

// Voice input (F4): mic -> WAV on SD -> cloud Whisper (engine) -> transcript -> normal query
bool s_recording = false;
lv_obj_t *s_mic = nullptr;                 // mic button label (icon swaps to stop)
char s_voice[kInputCap];                   // worker-filled transcript ("" = failed)
constexpr const char *kVoiceWav = "/sdcard/data/anima/voice.wav";
constexpr const char *kChatLog  = "/sdcard/data/anima/chatlog.ndjson";

bool lang_en(void) { return nv_i18n_get_lang() != NV_LANG_IT; }  // ANIMA speaks it/en; en fallback

// ---------------------------------------------------------------- glyph sanitizer

// Map UTF-8 typographic punctuation to Latin-1 equivalents the fonts actually have.
// Latin-1 (2-byte C2/C3) and the bullet (U+2022) pass through; any other sequence
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
        // Any other multi-byte sequence: swallow it, emit one '?'.
        unsigned char lead = *p++;
        int ext = (lead >= 0xF0) ? 3 : (lead >= 0xE0) ? 2 : 1;
        while (ext-- && *p) p++;
        s_san[o++] = '?';
    }
    s_san[o] = '\0';
    return s_san;
}

// ---------------------------------------------------------------- worker

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
        s_res = nucleo_anima_query(s_req, s_lang);
        const char *lr = nucleo_anima_long_reply();
        if (lr && lr[0]) { strncpy(s_long, lr, sizeof s_long - 1); s_long[sizeof s_long - 1] = '\0'; }
        else s_long[0] = '\0';
        nucleo_anima_unlock();
        s_done_kind = JOB_QUERY;
        s_done_gen = job.gen;
    }
}

void worker_send(int kind) {
    anima_job job = { ++s_gen, kind };
    xQueueOverwrite(s_queue, &job);
}

void worker_ensure(void) {
    if (s_worker) return;
    s_queue = xQueueCreate(1, sizeof(anima_job));
    // PSRAM stack: session-persistent, SD-only I/O (pack reads, session/telemetry writes),
    // never internal flash/NVS — the profile the PSRAM-stack rule allows.
    xTaskCreateWithCaps(worker_task, "anima", 24 * 1024, nullptr, 4, &s_worker,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// ---------------------------------------------------------------- bubbles

// Full-width transparent row so the bubble can hug left (ANIMA) or right (user).
lv_obj_t *bubble_add(bool user, const char *text) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = lv_obj_create(s_chat);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, lv_pct(75), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, user ? th->primary : th->surface, 0);
    lv_obj_align(b, user ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lb = lv_label_create(b);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lb, 640, 0);
    lv_obj_set_style_text_font(lb, &nv_font_14, 0);
    lv_obj_set_style_text_color(lb, user ? th->on_primary : th->text, 0);
    lv_label_set_text(lb, latin1ize(text));
    return lb;   // caller may retext it (the pending "..." bubble)
}

// Muted one-liner under an ANIMA bubble (tier · confidence · trace, or a restored one).
void meta_label(const char *m) {
    lv_obj_t *lb = lv_label_create(s_chat);
    lv_obj_set_width(lb, lv_pct(100));
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lb, &nv_font_14, 0);
    lv_obj_set_style_text_color(lb, nv_theme_get()->text_dim, 0);
    lv_obj_set_style_pad_left(lb, 6, 0);
    lv_label_set_text(lb, latin1ize(m));
}

void meta_format(const anima_result_t &r, char *m, size_t cap) {
    const char *tier = r.tier == ANIMA_TIER_COMMAND ? "L0"
                     : r.tier == ANIMA_TIER_FACT    ? "L1/KGE"
                     : r.tier == ANIMA_TIER_STITCH  ? "L2"
                     : r.tier == ANIMA_TIER_REMOTE  ? "cloud" : "-";
    snprintf(m, cap, "%s \xC2\xB7 %d%%%s%.120s", tier, r.confidence,
             r.trace[0] ? " \xC2\xB7 " : "", r.trace);
}

// Full-width code panel (teacher replies fence code with ```). No 75% cap: code wants columns.
void code_add(const char *text) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *b = lv_obj_create(s_chat);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_all(b, 12, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, th->surface2, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lb = lv_label_create(b);
    lv_obj_set_width(lb, lv_pct(100));
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lb, &nv_font_14, 0);
    lv_obj_set_style_text_color(lb, th->text_strong, 0);
    lv_label_set_text(lb, latin1ize(text));
}

// Render a reply that may contain ``` fences: prose -> bubbles, code -> full-width panels.
// The first prose segment lands in the pending "..." bubble; extra segments append below.
void reply_render(const char *text) {
    const char *p = text;
    bool used_pending = false, code = false;
    while (*p) {
        const char *f = strstr(p, "```");
        size_t n = f ? (size_t)(f - p) : strlen(p);
        if (n) {
            char seg[2048];
            if (n >= sizeof seg) n = sizeof seg - 1;
            memcpy(seg, p, n);
            seg[n] = '\0';
            const char *s = seg;
            if (code) {                       // drop the ```lang tag line
                const char *nl = strchr(seg, '\n');
                if (nl && (nl - seg) < 24) s = nl + 1;
            }
            bool empty = true;
            for (const char *q = s; *q; q++) if (!isspace((unsigned char)*q)) { empty = false; break; }
            if (!empty) {
                if (code) code_add(s);
                else if (!used_pending && s_pending) { lv_label_set_text(s_pending, latin1ize(s)); used_pending = true; }
                else bubble_add(false, s);
            }
        }
        if (!f) break;
        p = f + 3;
        code = !code;
    }
    if (!used_pending && s_pending)           // all-code reply: give the bubble a short lead-in
        lv_label_set_text(s_pending, lang_en() ? "Here you go:" : "Ecco:");
    s_pending = nullptr;
}

// ---------------------------------------------------------------- chat history (SD)

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
            bubble_add(r->valuestring[0] == 'u', t->valuestring);
            if (cJSON_IsString(m) && m->valuestring[0]) meta_label(m->valuestring);
            any = true;
        }
        cJSON_Delete(o);
    }
    heap_caps_free(line);
    fclose(f);
    return any;
}

void chat_scroll_bottom(void) {
    if (!s_chat) return;
    lv_obj_update_layout(s_chat);
    int32_t bottom = lv_obj_get_scroll_bottom(s_chat);
    if (bottom > 0) lv_obj_scroll_by(s_chat, 0, -bottom, LV_ANIM_OFF);
}

void submit_cb(lv_event_t *);   // defined in the input section below

void chip_cb(lv_event_t *e) {
    if (!s_input) return;
    const char *prompt = (const char *)lv_event_get_user_data(e);
    lv_textarea_set_text(s_input, prompt);
    submit_cb(nullptr);
}

void welcome_add(void) {
    const bool en = lang_en();
    bubble_add(false, en
        ? "Hi! I'm ANIMA, your offline assistant. Ask me a question, some math, or tell me "
          "to open an app. If I don't know, I say so - I never make things up."
        : "Ciao! Sono ANIMA, la tua assistente offline. Fammi una domanda, un calcolo, o dimmi "
          "di aprire un'app. Se non so una cosa, te lo dico - non invento mai.");

    // Quick-start chips: one tap = one query (they scroll away with the history).
    static const char *kIt[] = {"Quanto fa 128 per 46?", "Chi era Alan Turing?", "Apri musica"};
    static const char *kEn[] = {"What is 128 times 46?", "Who was Alan Turing?", "Open music"};
    const char **chips = en ? kEn : kIt;
    lv_obj_t *row = lv_obj_create(s_chat);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_row(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *c = nv_kit_button(row, chips[i], false);
        lv_obj_add_event_cb(c, chip_cb, LV_EVENT_CLICKED, (void *)chips[i]);
    }
}

// ---------------------------------------------------------------- result handling

// Live ANIMA_ACT_SYSTEM answers now come from the shared resolver (nv_anima_system.cpp): one
// implementation for the native chat AND the web REST path, covering every SYSTEM key the
// engine emits (time/date/season/storage/capabilities/network/ram/version/uptime/...). The old
// local version knew only time+storage, so every other key leaked the raw "{value}" template.

void poll_cb(lv_timer_t *) {
    if (s_done_gen != s_gen || !s_pending) {
        // Thinking indicator: cycle . .. ... on the pending bubble (~every 450 ms).
        if (s_pending) {
            static uint8_t tick;
            if ((++tick % 3) == 0) {
                static const char *dots[] = {".", "..", "..."};
                lv_label_set_text(s_pending, dots[(tick / 3) % 3]);
            }
        }
        return;
    }
    if (s_done_kind == JOB_VOICE) {
        // Drop the "(transcribing...)" bubble, then run the transcript through the normal path.
        lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(s_pending));
        s_pending = nullptr;
        lv_obj_delete(row);
        if (!s_voice[0]) {
            nv_ui_toast(lang_en() ? "Transcription failed (key? network?)"
                                  : "Trascrizione fallita (chiave? rete?)");
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
    if (!text[0]) text = lang_en() ? "I don't know." : "Non lo so.";

    // Typed tool proposals (set_volume/set_brightness) really happen — the engine only proposes,
    // the OS layer executes (same contract as the web handler).
    if (r.action == ANIMA_ACT_TOOL) nv_anima_os_exec(r.intent, r.arg);

    char meta[196];
    meta_format(r, meta, sizeof meta);
    reply_render(text);                       // consumes s_pending (prose + ``` code panels)
    if (r.tier != ANIMA_TIER_NONE || r.trace[0]) meta_label(meta);
    history_append('a', text, (r.tier != ANIMA_TIER_NONE || r.trace[0]) ? meta : nullptr);
    chat_scroll_bottom();

    // Agentic launch: the reply bubble lands, then the requested app takes over
    // (nv_ui_open_app tears this page down — do it last, via a one-shot timer so
    // this callback unwinds cleanly first).
    if (r.action == ANIMA_ACT_LAUNCH && r.arg[0]) {
        static char app_id[64];
        strncpy(app_id, r.arg, sizeof app_id - 1);
        app_id[sizeof app_id - 1] = '\0';
        lv_timer_t *t = lv_timer_create([](lv_timer_t *tm) {
            lv_timer_delete(tm);
            const NvApp *a = nv_ui_find_app(app_id);
            if (a) nv_ui_open_app(a);
        }, 700, nullptr);
        lv_timer_set_repeat_count(t, 1);
    }
}

// ---------------------------------------------------------------- input

void submit_cb(lv_event_t *) {
    if (!s_input || s_pending) return;   // one in-flight query at a time
    if (s_settings && !lv_obj_has_flag(s_settings, LV_OBJ_FLAG_HIDDEN)) return;  // settings view up
    const char *txt = lv_textarea_get_text(s_input);
    if (!txt || !txt[0]) return;

    bubble_add(true, txt);
    strncpy(s_req, txt, sizeof s_req - 1);
    s_req[sizeof s_req - 1] = '\0';
    history_append('u', s_req, nullptr);
    lv_textarea_set_text(s_input, "");

    s_pending = bubble_add(false, "...");
    chat_scroll_bottom();

    snprintf(s_lang, sizeof s_lang, "%s", lang_en() ? "en" : "it");
    worker_ensure();
    worker_send(JOB_QUERY);
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
            nv_ui_toast(en ? "Microphone unavailable" : "Microfono non disponibile");
            return;
        }
        s_recording = true;
        if (s_mic) lv_label_set_text(s_mic, LV_SYMBOL_STOP);
        nv_ui_toast(en ? "Listening... tap to stop" : "Ti ascolto... tocca per fermare");
        return;
    }
    nv_audio_rec_stop();
    s_recording = false;
    if (s_mic) lv_label_set_text(s_mic, LV_SYMBOL_AUDIO);
    s_pending = bubble_add(false, en ? "(transcribing...)" : "(trascrivo...)");
    chat_scroll_bottom();
    snprintf(s_lang, sizeof s_lang, "%s", lang_en() ? "en" : "it");
    worker_ensure();
    worker_send(JOB_VOICE);
}

// ---------------------------------------------------------------- settings view

void stats_refresh(void) {
    if (!s_stats) return;
    anima_diag_t d;
    nucleo_anima_diag(&d);
    const bool en = lang_en();

    char online[96];
    if (nucleo_anima_online_available()) {
        char prov[24] = "", model[40] = "";
        if (nucleo_anima_teacher_info(prov, sizeof prov, model, sizeof model))
            snprintf(online, sizeof online, en ? "online, teacher %s (%s)" : "online, teacher %s (%s)", prov, model);
        else
            snprintf(online, sizeof online, "%s", en ? "online, no teacher key" : "online, nessuna chiave teacher");
    } else {
        snprintf(online, sizeof online, "%s", en ? "offline" : "offline");
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

void reset_session_cb(lv_event_t *) {
    nucleo_anima_reset_session();
    remove(kChatLog);
    if (s_chat) {
        s_gen++;                 // orphan any in-flight result
        s_pending = nullptr;
        lv_obj_clean(s_chat);
        welcome_add();
    }
    stats_refresh();
}

// ---------------------------------------------------------------- teacher key manager

// Read teacher.json (may be absent). Caller owns the returned cJSON object (never NULL).
cJSON *teacher_json_load(void) {
    cJSON *o = nullptr;
    if (FILE *f = fopen(kTeacherPath, "rb")) {
        char buf[1536];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';
        o = cJSON_Parse(buf);
    }
    return o ? o : cJSON_CreateObject();
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
        if (FILE *f = fopen(kTeacherPath, "wb")) {
            ok = fwrite(txt, 1, strlen(txt), f) == strlen(txt);
            fclose(f);
        }
        cJSON_free(txt);
    }
    if (ok) {
        lv_textarea_set_text(s_key_ta, "");
        teacher_prefill();                // refresh the masked hint
        stats_refresh();
    }
    nv_ui_toast(ok ? (en ? "Teacher saved" : "Teacher salvato")
                   : (en ? "SD write failed" : "Scrittura SD fallita"));
}

void settings_toggle_cb(lv_event_t *) {
    if (!s_settings) return;
    // The gear lives in the input bar, so the bar stays visible in both views
    // (it is the only way back); submit_cb refuses while settings are showing.
    const bool showing = !lv_obj_has_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    if (showing) {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_chat, LV_OBJ_FLAG_HIDDEN);
        if (s_gear) lv_label_set_text(s_gear, LV_SYMBOL_SETTINGS);
    } else {
        nv_ime_hide();
        stats_refresh();
        lv_obj_add_flag(s_chat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        if (s_gear) lv_label_set_text(s_gear, LV_SYMBOL_CLOSE);
    }
}

void settings_build(lv_obj_t *root) {
    const NvTheme *th = nv_theme_get();
    const bool en = lang_en();

    s_settings = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_settings, 1);
    lv_obj_set_style_pad_row(s_settings, 14, 0);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);

    // L1 serving policy
    lv_obj_t *lt = lv_label_create(s_settings);
    lv_obj_set_style_text_font(lt, &nv_font_14, 0);
    lv_obj_set_style_text_color(lt, th->text_strong, 0);
    lv_label_set_text(lt, en ? "Offline brain (L1 index)" : "Cervello offline (indice L1)");

    lv_obj_t *dd = lv_dropdown_create(s_settings);
    lv_obj_set_width(dd, 360);
    lv_dropdown_set_options(dd, en ? "Auto (stand down when online)\nAlways on\nOff"
                                   : "Auto (si ritira se online)\nSempre attivo\nSpento");
    lv_dropdown_set_selected(dd, (uint32_t)nucleo_anima_l1_get_mode());
    lv_obj_add_event_cb(dd, l1_mode_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *hint = lv_label_create(s_settings);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, &nv_font_14, 0);
    lv_obj_set_style_text_color(hint, th->text_dim, 0);
    lv_label_set_text(hint, en
        ? "Auto frees the semantic index while a cloud teacher answers; Always on forces the offline brain."
        : "Auto libera l'indice semantico quando risponde un teacher cloud; Sempre attivo forza il cervello offline.");

    // Teacher cloud (API key manager) — writes /sdcard/data/anima/teacher.json, same file the
    // engine and the web companion read. Read-modify-write keeps unrelated fields (whisper, ...).
    lv_obj_t *kt = lv_label_create(s_settings);
    lv_obj_set_style_text_font(kt, &nv_font_14, 0);
    lv_obj_set_style_text_color(kt, th->text_strong, 0);
    lv_label_set_text(kt, en ? "Cloud teacher (API key)" : "Teacher cloud (chiave API)");

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
    lv_obj_t *st = lv_label_create(s_settings);
    lv_obj_set_style_text_font(st, &nv_font_14, 0);
    lv_obj_set_style_text_color(st, th->text_strong, 0);
    lv_label_set_text(st, en ? "Statistics (since boot)" : "Statistiche (da avvio)");

    s_stats = lv_label_create(s_settings);
    lv_obj_set_width(s_stats, lv_pct(100));
    lv_label_set_long_mode(s_stats, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_stats, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_stats, th->text, 0);

    // Session reset
    lv_obj_t *btn = nv_kit_button(s_settings, en ? "Clear conversation" : "Pulisci conversazione", false);
    lv_obj_add_event_cb(btn, reset_session_cb, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------- build / teardown

void page_deleted(lv_event_t *) {
    nv_ime_hide();
    if (s_recording) { nv_audio_rec_stop(); s_recording = false; }
    s_mic = nullptr;
    s_gen++;             // orphan any in-flight result (worker keeps running, result drops)
    if (s_poll) { lv_timer_delete(s_poll); s_poll = nullptr; }
    s_chat = nullptr;
    s_bar = nullptr;
    s_input = nullptr;
    s_pending = nullptr;
    s_settings = nullptr;
    s_stats = nullptr;
    s_gear = nullptr;
    s_prov_dd = nullptr;
    s_key_ta = nullptr;
    s_model_ta = nullptr;
    s_base_ta = nullptr;
}

void anima_build(lv_obj_t *content) {
    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 12, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    s_chat = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_chat, 1);
    lv_obj_set_style_pad_row(s_chat, 8, 0);

    settings_build(root);   // hidden sibling of the chat column

    // Input bar: settings toggle + text entry (IME return = SEND) + send button.
    s_bar = lv_obj_create(root);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_bar, 10, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *gear_btn = nv_kit_button(s_bar, LV_SYMBOL_SETTINGS, false);
    lv_obj_add_event_cb(gear_btn, settings_toggle_cb, LV_EVENT_CLICKED, nullptr);
    s_gear = lv_obj_get_child(gear_btn, 0);   // the button's label, retexted on toggle

    s_input = nv_kit_textarea_ex(s_bar, lang_en() ? "Ask me anything..." : "Chiedimi qualcosa...",
                                 true, NV_IME_TEXT, NV_IME_RET_SEND);
    lv_obj_set_flex_grow(s_input, 1);
    lv_obj_add_event_cb(s_input, submit_cb, LV_EVENT_READY, nullptr);

    lv_obj_t *mic_btn = nv_kit_button(s_bar, LV_SYMBOL_AUDIO, false);
    lv_obj_add_event_cb(mic_btn, mic_cb, LV_EVENT_CLICKED, nullptr);
    s_mic = lv_obj_get_child(mic_btn, 0);

    lv_obj_t *send = nv_kit_button(s_bar, LV_SYMBOL_UP, true);
    lv_obj_add_event_cb(send, submit_cb, LV_EVENT_CLICKED, nullptr);

    if (!history_load()) welcome_add();       // restore the last conversation, or greet
    chat_scroll_bottom();

    s_poll = lv_timer_create(poll_cb, 150, nullptr);
}

const NvApp kAnimaApp = {"anima", "Anima", &nv_icon_anima, 2u << 20, anima_build,
                         NV_STR_APP_ANIMA, nullptr};

}  // namespace

void anima_app_register(void) { nv_app_register(&kAnimaApp); }
