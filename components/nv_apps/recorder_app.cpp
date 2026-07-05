// recorder_app — voice recorder with a two-column layout matching the Music player: a fixed
// left "stage" (timer, live waveform, morphing record button, now-playing transport with a big
// play/pause) and a scrollable right library list (rename + delete per row). Records the
// on-board mic (ES8311 ADC) to WAV on SD via nv_audio; plays back through nv_media (the Music
// engine). All heavy buffers are PSRAM + per-app.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_audio.h"
#include "nv_media.h"
#include "nv_notify.h"
#include "nv_time.h"
#include "nv_sd.h"

#include "lvgl.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

constexpr char kRecDir[]  = "/sdcard/Recordings";
constexpr int  kMaxFiles  = 256;
constexpr int  kNameLen   = 128;
constexpr int  kBars      = 30;     // waveform bars
constexpr int  kBarMax    = 46;     // px
constexpr int  kBarMin    = 4;

char (*s_files)[kNameLen] = nullptr;   // PSRAM, per-app
int  s_nfiles = 0;
int  s_cur    = -1;                    // selected/loaded recording (playing or paused); -1 = none

// stage
lv_obj_t  *s_status = nullptr;   // top-right chip: "REC" while recording, else "Ready"
lv_obj_t  *s_time    = nullptr;  // big MM:SS
lv_obj_t  *s_bars[kBars] = {nullptr};
lv_obj_t  *s_ring   = nullptr;   // record button ring (pulses while recording)
lv_obj_t  *s_core   = nullptr;   // red disc / square

// now-playing transport (mirrors the Music player's left panel)
lv_obj_t  *s_pv_title = nullptr;
lv_obj_t  *s_pv_sub   = nullptr;
lv_obj_t  *s_pv_pos   = nullptr;
lv_obj_t  *s_pv_dur   = nullptr;
lv_obj_t  *s_pv_bar   = nullptr;   // read-only progress (WAV isn't a seekable format)
lv_obj_t  *s_pv_play  = nullptr;   // big round play/pause button
lv_obj_t  *s_pv_next  = nullptr;   // "Up next" preview line

// library
lv_obj_t  *s_list  = nullptr;
lv_obj_t  *s_hdr   = nullptr;      // "Recordings (N · Xm)"
lv_obj_t  *s_sd    = nullptr;      // live SD free-space line
lv_obj_t  *s_eq[3] = {};           // animated mini-EQ on the active row (Music-app pattern)
bool       s_eq_running = false;

// rename modal
lv_obj_t *s_ren_modal = nullptr;
lv_obj_t *s_ren_ta    = nullptr;
int       s_ren_idx   = -1;
bool      s_ren_pending = false;

lv_timer_t *s_timer = nullptr;
int  s_wave[kBars] = {0};
bool s_was_rec = false;
int  s_pulse = 0;

void build_list(void);          // fwd
void update_now_playing(void);  // fwd
void open_rename(int i);        // fwd

// ---------------------------------------------------------------- helpers
int wav_dur_ms(const char *path){
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[44];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 44 || memcmp(h, "RIFF", 4) || memcmp(h+8, "WAVE", 4)) return 0;
    uint32_t byterate = (uint32_t)h[28] | (uint32_t)h[29]<<8 | (uint32_t)h[30]<<16 | (uint32_t)h[31]<<24;
    uint32_t datasize = (uint32_t)h[40] | (uint32_t)h[41]<<8 | (uint32_t)h[42]<<16 | (uint32_t)h[43]<<24;
    return byterate ? (int)((uint64_t)datasize * 1000 / byterate) : 0;
}
void fmt_ms(char *b, size_t n, int ms){ if (ms<0) ms=0; lv_snprintf(b, n, "%d:%02d", ms/60000, (ms/1000)%60); }

// "Recording 2026-07-04 143210" -> "Recording 2026-07-04 143210" (display only; strips .wav).
void pretty_name(const char *file, char *out, size_t n){
    lv_snprintf(out, n, "%s", file);
    if (char *dot = strrchr(out, '.')) *dot = '\0';
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

void scan_dir(void){
    s_nfiles = 0;
    if (!s_files) return;
    DIR *d = opendir(kRecDir);
    if (!d) return;
    struct dirent *e;
    while (s_nfiles < kMaxFiles && (e = readdir(d)) != nullptr) {
        if (e->d_type == DT_DIR) continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".wav") != 0) continue;
        lv_snprintf(s_files[s_nfiles], kNameLen, "%s", e->d_name);   // snprintf: no -Wstringop-truncation
        s_nfiles++;
    }
    closedir(d);
}

// ---------------------------------------------------------------- record control
void toggle_rec(lv_event_t *){
    if (!nv_audio_mic_ready()) return;
    if (nv_audio_rec_active()) {
        nv_audio_rec_stop();
    } else {
        if (s_cur >= 0) { nv_media_stop(); s_cur = -1; update_now_playing(); }   // don't record over playback
        mkdir(kRecDir, 0777);
        struct tm tmv;
        nv_time_now(&tmv);
        char stamp[48];
        strftime(stamp, sizeof stamp, "Recording %Y-%m-%d %H%M%S", &tmv);   // human-readable by default
        char path[220];
        lv_snprintf(path, sizeof path, "%s/%s.wav", kRecDir, stamp);
        nv_audio_rec_start(path);
    }
}

// ---------------------------------------------------------------- playback / transport
void play_index(int i){
    if (i < 0 || i >= s_nfiles) return;
    if (nv_audio_rec_active()) return;   // mic busy
    char full[300];
    lv_snprintf(full, sizeof full, "%s/%s", kRecDir, s_files[i]);
    nv_media_play(full);
    s_cur = i;
    update_now_playing();
    build_list();
}

int pick_next(void){ if (s_nfiles <= 0) return -1; return (s_cur + 1 < s_nfiles) ? s_cur + 1 : -1; }
int pick_prev(void){ if (s_nfiles <= 0) return -1; return (s_cur > 0) ? s_cur - 1 : -1; }
void next_cb(lv_event_t *){ int n = pick_next(); if (n >= 0) play_index(n); }
void prev_cb(lv_event_t *){ int p = pick_prev(); if (p >= 0) play_index(p); }

void pv_playpause_cb(lv_event_t *){
    switch (nv_media_state()) {
        case NV_MEDIA_PLAYING: nv_media_pause(true);  break;
        case NV_MEDIA_PAUSED:  nv_media_pause(false); break;
        default:
            if (nv_audio_rec_active()) return;
            if (s_cur >= 0) play_index(s_cur);
            else if (s_nfiles > 0) play_index(0);
            break;
    }
}

void row_cb(lv_event_t *e){
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == s_cur) {                              // tapping the active row toggles pause/resume
        if (nv_media_state() == NV_MEDIA_PLAYING) nv_media_pause(true);
        else                                      nv_media_pause(false);
    } else {
        play_index(i);
    }
}
void del_cb(lv_event_t *e){
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nfiles) return;
    if (i == s_cur) { nv_media_stop(); s_cur = -1; }
    char full[300];
    lv_snprintf(full, sizeof full, "%s/%s", kRecDir, s_files[i]);
    remove(full);
    if (s_cur > i) s_cur--;              // list shifts
    scan_dir();
    build_list();
    update_now_playing();
}

// ---------------------------------------------------------------- rename modal
void ren_close_async(void *){
    s_ren_pending = false;
    nv_ime_set_submit_cb(nullptr, nullptr);
    nv_ime_hide();
    if (s_ren_modal) { lv_obj_delete(s_ren_modal); s_ren_modal = nullptr; s_ren_ta = nullptr; }
    s_ren_idx = -1;
}
void ren_close_deferred(void){   // Cancel/Rename are children of the modal — defer its deletion
    if (s_ren_pending) return;
    if (lv_async_call(ren_close_async, nullptr) == LV_RESULT_OK) s_ren_pending = true;
}

void do_rename(void){
    const bool it = (nv_i18n_get_lang() == NV_LANG_IT);
    if (s_ren_idx < 0 || s_ren_idx >= s_nfiles || !s_ren_ta) { ren_close_deferred(); return; }
    const char *txt = lv_textarea_get_text(s_ren_ta);
    if (!txt || !txt[0] || strchr(txt, '/')) {
        nv_toast(NV_NOTE_ERROR, it ? "Nome non valido" : "Invalid name");
        return;
    }
    char newname[kNameLen];
    const char *dot = strrchr(txt, '.');
    if (dot && strcasecmp(dot, ".wav") == 0) lv_snprintf(newname, sizeof newname, "%s", txt);
    else                                     lv_snprintf(newname, sizeof newname, "%s.wav", txt);

    char oldp[300], newp[300];
    lv_snprintf(oldp, sizeof oldp, "%s/%s", kRecDir, s_files[s_ren_idx]);
    lv_snprintf(newp, sizeof newp, "%s/%s", kRecDir, newname);
    if (strcmp(oldp, newp) == 0) { ren_close_deferred(); return; }

    if (rename(oldp, newp) == 0) {
        if (s_ren_idx == s_cur) { nv_media_stop(); s_cur = -1; }   // index may shift after rescan
        scan_dir();
        build_list();
        update_now_playing();
        nv_toast(NV_NOTE_OK, nv_tr(NV_STR_SAVED));
        ren_close_deferred();
    } else {
        nv_toast(NV_NOTE_ERROR, it ? "Rinomina non riuscita" : "Rename failed");
    }
}
void ren_confirm_cb(lv_event_t *){ do_rename(); }
void ren_submit_cb(lv_obj_t *, void *){ do_rename(); }   // keyboard "Go" return key
void ren_cancel_cb(lv_event_t *){ ren_close_deferred(); }
void ren_row_cb(lv_event_t *e){ open_rename((int)(intptr_t)lv_event_get_user_data(e)); }

void open_rename(int i){
    if (i < 0 || i >= s_nfiles || s_ren_modal) return;
    s_ren_idx = i;
    const NvTheme *th = nv_theme_get();
    const bool it = (nv_i18n_get_lang() == NV_LANG_IT);

    // Parent on the active screen (NOT lv_layer_top): the shared IME keyboard is a screen child
    // that raises itself with move_foreground, so a top-layer sheet would sit above the keyboard.
    s_ren_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_ren_modal);
    lv_obj_set_size(s_ren_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ren_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ren_modal, LV_OPA_50, 0);
    lv_obj_clear_flag(s_ren_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_ren_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(88));
    lv_obj_set_style_max_width(card, 460, 0);
    lv_obj_set_height(card, 220);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, th->surface3, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text(ttl, it ? "Rinomina registrazione" : "Rename recording");
    lv_obj_set_style_text_font(ttl, &nv_font_20, 0);
    lv_obj_set_style_text_color(ttl, th->text_strong, 0);

    char base[kNameLen];
    pretty_name(s_files[i], base, sizeof base);

    s_ren_ta = nv_kit_textarea_ex(card, it ? "Nome" : "Name", true, NV_IME_TEXT, NV_IME_RET_GO);
    lv_textarea_set_text(s_ren_ta, base);
    lv_obj_set_width(s_ren_ta, lv_pct(100));
    nv_ime_set_submit_cb(ren_submit_cb, nullptr);

    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_height(btns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 12, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_button_create(btns);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, 46);
    lv_obj_set_style_bg_color(cancel, th->surface3, 0);
    lv_obj_add_event_cb(cancel, ren_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, nv_tr(NV_STR_CANCEL));
    lv_obj_set_style_text_color(cl, th->text_strong, 0);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_button_create(btns);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_height(ok, 46);
    lv_obj_set_style_bg_color(ok, th->primary, 0);
    lv_obj_add_event_cb(ok, ren_confirm_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, nv_tr(NV_STR_RENAME));
    lv_obj_set_style_text_color(ol, th->on_primary, 0);
    lv_obj_center(ol);
}

// ---------------------------------------------------------------- now-playing panel
void update_now_playing(void){
    const bool it = (nv_i18n_get_lang() == NV_LANG_IT);
    if (s_pv_title) {
        if (s_cur >= 0 && s_cur < s_nfiles) {
            char nm[kNameLen];
            pretty_name(s_files[s_cur], nm, sizeof nm);
            lv_label_set_text(s_pv_title, nm);
        } else {
            lv_label_set_text(s_pv_title, it ? "Scegli una registrazione" : "Pick a recording");
        }
    }
    if (s_pv_sub) {
        if (s_cur >= 0 && s_cur < s_nfiles) {
            char full[300];
            lv_snprintf(full, sizeof full, "%s/%s", kRecDir, s_files[s_cur]);
            char db[12];
            fmt_ms(db, sizeof db, wav_dur_ms(full));
            char b[48];
            lv_snprintf(b, sizeof b, "%s \xC2\xB7 WAV", db);
            lv_label_set_text(s_pv_sub, b);
        } else {
            lv_label_set_text(s_pv_sub, "");
        }
    }
    if (s_pv_next) {
        const int n = pick_next();
        if (n >= 0) {
            char nm[kNameLen], b[kNameLen + 16];
            pretty_name(s_files[n], nm, sizeof nm);
            lv_snprintf(b, sizeof b, "%s %s", it ? "Prossimo:" : "Up next:", nm);
            lv_label_set_text(s_pv_next, b);
        } else {
            lv_label_set_text(s_pv_next, it ? "Fine elenco" : "End of list");
        }
    }
}

// ---------------------------------------------------------------- waveform + EQ + tick
void wave_push(int level){
    for (int i = 0; i < kBars-1; i++) s_wave[i] = s_wave[i+1];
    s_wave[kBars-1] = level;
    for (int i = 0; i < kBars; i++) {
        if (!s_bars[i]) continue;
        lv_obj_set_height(s_bars[i], kBarMin + s_wave[i] * (kBarMax - kBarMin) / 100);
    }
}

// Animated mini-EQ on the active list row: alive while playing, flat while paused/stopped.
// Same look as the Music player's active-row indicator.
void eq_anim_cb(void *o, int32_t v){ lv_obj_set_height((lv_obj_t *)o, v); }
void eq_set(bool run){
    if (run == s_eq_running) return;
    s_eq_running = run;
    static const uint32_t kPeriod[3] = {420, 560, 340};
    for (int i = 0; i < 3; i++) {
        if (!s_eq[i]) return;
        lv_anim_delete(s_eq[i], nullptr);
        if (run) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_eq[i]);
            lv_anim_set_exec_cb(&a, eq_anim_cb);
            lv_anim_set_values(&a, 6, 18);
            lv_anim_set_duration(&a, kPeriod[i]);
            lv_anim_set_playback_duration(&a, kPeriod[i]);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        } else {
            lv_obj_set_height(s_eq[i], 8);
        }
    }
}

void tick(lv_timer_t *){
    const NvTheme *th = nv_theme_get();
    const bool rec = nv_audio_rec_active();
    const bool it = (nv_i18n_get_lang() == NV_LANG_IT);

    // waveform: live level while recording, else scroll down to rest
    wave_push(rec ? nv_audio_mic_level() : 0);
    if (rec != s_was_rec) {                        // recolor bars on state change
        for (int i = 0; i < kBars; i++) if (s_bars[i]) lv_obj_set_style_bg_color(s_bars[i], rec ? th->danger : th->surface3, 0);
    }

    // timer + record button + status chip
    if (s_time) {
        char b[12]; fmt_ms(b, sizeof b, (int)(nv_audio_rec_secs() * 1000));
        lv_label_set_text(s_time, b);
        lv_obj_set_style_text_color(s_time, rec ? th->danger : th->text_strong, 0);
    }
    if (s_core) lv_obj_set_style_radius(s_core, rec ? NV_RAD_SM : LV_RADIUS_CIRCLE, 0);
    if (s_ring) {                                   // pulse the ring ~2 Hz while recording
        const bool on = rec && ((s_pulse / 7) & 1);
        lv_obj_set_style_border_color(s_ring, on ? th->danger : th->surface3, 0);
    }
    if (s_status) {
        lv_label_set_text(s_status, rec ? LV_SYMBOL_STOP " REC" : (it ? "Pronto" : "Ready"));
        lv_obj_set_style_bg_color(s_status, rec ? th->danger : th->surface3, 0);
        lv_obj_set_style_text_color(s_status, rec ? lv_color_white() : th->text_dim, 0);
    }
    if (s_sd && s_pulse % 14 == 0) {   // ~1s at 70ms tick: cheap, but no need to hammer the FS
        uint64_t total = 0, freeb = 0;
        if (nv_sd_info(&total, &freeb)) {
            char b[48];
            lv_snprintf(b, sizeof b, "%s %.1f GB", it ? "Libera:" : "Free:", freeb / 1073741824.0);
            lv_label_set_text(s_sd, b);
        } else {
            lv_label_set_text(s_sd, it ? "SD non trovata" : "No SD card");
        }
    }
    s_pulse++;

    // now-playing transport
    const nv_media_state_t st = nv_media_state();
    if (s_pv_play) lv_label_set_text(lv_obj_get_child(s_pv_play, 0), st == NV_MEDIA_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    eq_set(st == NV_MEDIA_PLAYING);

    const int pos = nv_media_pos_ms();
    const int dur = nv_media_dur_ms();
    char b[16];
    if (s_pv_pos) { fmt_ms(b, sizeof b, s_cur >= 0 ? pos : 0); lv_label_set_text(s_pv_pos, b); }
    if (s_pv_dur) { fmt_ms(b, sizeof b, s_cur >= 0 ? dur : 0); lv_label_set_text(s_pv_dur, b); }
    if (s_pv_bar) lv_bar_set_value(s_pv_bar, (s_cur >= 0 && dur > 0) ? (int)((int64_t)pos*1000/dur) : 0, LV_ANIM_OFF);

    if (s_cur >= 0 && (nv_media_took_eot() || st == NV_MEDIA_ERROR)) {
        const int n = pick_next();
        if (n >= 0) play_index(n);
        else { s_cur = -1; build_list(); update_now_playing(); }
    }

    if (rec != s_was_rec) {
        if (!rec) { scan_dir(); build_list(); update_now_playing(); }   // a recording just finished -> refresh
        s_was_rec = rec;
    }
}

// ---------------------------------------------------------------- list
void build_list(void){
    if (!s_list) return;
    lv_obj_clean(s_list);
    s_eq[0] = s_eq[1] = s_eq[2] = nullptr;   // rows are gone; tick re-arms on the fresh bars
    s_eq_running = false;
    const NvTheme *th = nv_theme_get();
    const bool it = (nv_i18n_get_lang() == NV_LANG_IT);

    if (s_nfiles == 0) {
        lv_obj_t *box = lv_obj_create(s_list);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(box, 24, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_text(l, nv_tr(NV_STR_REC_EMPTY));
        lv_obj_set_style_text_color(l, th->text_dim, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (s_hdr) lv_label_set_text(s_hdr, it ? "Registrazioni (0)" : "Recordings (0)");
        return;
    }

    char full[300], nm[kNameLen], db[12];
    int total_ms = 0;
    for (int i = 0; i < s_nfiles; i++) {
        const bool active = (i == s_cur);
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, active ? th->surface2 : th->surface, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, th->surface3, LV_STATE_PRESSED);   // touch feedback
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 16, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (active) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(row, 3, 0);
            lv_obj_set_style_border_color(row, th->accent, 0);
        }
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (active) lv_obj_scroll_to_view(row, LV_ANIM_ON);

        if (active) {
            // Animated mini-EQ instead of the track number: the "now playing" landmark
            // (same widget the Music player uses on its active row).
            lv_obj_t *eq = lv_obj_create(row);
            lv_obj_remove_style_all(eq);
            lv_obj_set_size(eq, 34, 20);
            lv_obj_set_flex_flow(eq, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(eq, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
            lv_obj_set_style_pad_column(eq, 3, 0);
            lv_obj_clear_flag(eq, LV_OBJ_FLAG_SCROLLABLE);
            for (int k = 0; k < 3; k++) {
                s_eq[k] = lv_obj_create(eq);
                lv_obj_remove_style_all(s_eq[k]);
                lv_obj_set_size(s_eq[k], 5, 8);
                lv_obj_set_style_radius(s_eq[k], 2, 0);
                lv_obj_set_style_bg_opa(s_eq[k], LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(s_eq[k], th->accent, 0);
            }
        } else {
            lv_obj_t *nr = lv_label_create(row);
            char nb[8];
            lv_snprintf(nb, sizeof nb, "%02d", i + 1);
            lv_label_set_text(nr, nb);
            lv_obj_set_style_text_color(nr, th->text_dim, 0);
            lv_obj_set_width(nr, 34);
        }

        pretty_name(s_files[i], nm, sizeof nm);
        lv_obj_t *lb = lv_label_create(row);
        lv_label_set_text(lb, nm);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(lb, 1);
        lv_obj_set_style_text_color(lb, active ? th->text_strong : th->text, 0);

        lv_snprintf(full, sizeof full, "%s/%s", kRecDir, s_files[i]);
        const int ms = wav_dur_ms(full);
        total_ms += ms;
        lv_obj_t *dur = lv_label_create(row);
        fmt_ms(db, sizeof db, ms);
        lv_label_set_text(dur, db);
        lv_obj_set_style_text_color(dur, th->text_dim, 0);
        lv_obj_set_style_text_font(dur, &nv_font_14, 0);

        lv_obj_t *ren = lv_button_create(row);
        lv_obj_set_size(ren, NV_TOUCH_MIN, NV_TOUCH_MIN);
        lv_obj_set_style_radius(ren, NV_TOUCH_MIN / 2, 0);
        lv_obj_set_style_shadow_width(ren, 0, 0);
        lv_obj_set_style_bg_opa(ren, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(ren, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_add_event_cb(ren, ren_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *rl = lv_label_create(ren);
        lv_label_set_text(rl, LV_SYMBOL_EDIT);
        lv_obj_set_style_text_color(rl, th->text_dim, 0);
        lv_obj_center(rl);

        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, NV_TOUCH_MIN, NV_TOUCH_MIN);
        lv_obj_set_style_radius(del, NV_TOUCH_MIN / 2, 0);
        lv_obj_set_style_shadow_width(del, 0, 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_add_event_cb(del, del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dl = lv_label_create(del);
        lv_label_set_text(dl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(dl, th->danger, 0);
        lv_obj_center(dl);
    }

    if (s_hdr) {
        char hb[64];
        if (total_ms > 0)
            lv_snprintf(hb, sizeof hb, "%s (%d \xC2\xB7 %d min)", it ? "Registrazioni" : "Recordings",
                        s_nfiles, (total_ms + 30000) / 60000);
        else
            lv_snprintf(hb, sizeof hb, "%s (%d)", it ? "Registrazioni" : "Recordings", s_nfiles);
        lv_label_set_text(s_hdr, hb);
    }
}

void page_deleted(lv_event_t *){
    if (nv_audio_rec_active()) nv_audio_rec_stop();
    if (s_cur >= 0) { nv_media_stop(); s_cur = -1; }
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_ren_modal) {   // not a child of root: must be torn down explicitly
        nv_ime_set_submit_cb(nullptr, nullptr);
        nv_ime_hide();
        lv_obj_delete(s_ren_modal);
        s_ren_modal = nullptr;
        s_ren_ta = nullptr;
    }
    s_ren_idx = -1; s_ren_pending = false;
    if (s_files) { heap_caps_free(s_files); s_files = nullptr; s_nfiles = 0; }
    for (int i = 0; i < kBars; i++) s_bars[i] = nullptr;
    s_time = s_ring = s_core = s_list = s_hdr = s_status = s_sd = nullptr;
    s_pv_title = s_pv_sub = s_pv_pos = s_pv_dur = s_pv_bar = s_pv_play = s_pv_next = nullptr;
    s_eq[0] = s_eq[1] = s_eq[2] = nullptr;
    s_eq_running = false;
}

lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, bool primary, int size){
    lv_obj_t *b = nv_kit_button(parent, sym, primary);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

void rec_build(lv_obj_t *content){
    nv_media_init();
    if (!s_files) s_files = (char (*)[kNameLen])heap_caps_malloc((size_t)kMaxFiles * kNameLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    scan_dir();
    if (s_cur >= s_nfiles) s_cur = -1;
    const NvTheme *th = nv_theme_get();
    for (int i = 0; i < kBars; i++) s_wave[i] = 0;
    s_was_rec = false; s_pulse = 0;

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 14, 0);
    lv_obj_set_style_pad_column(root, 14, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    // ================= left: stage + now-playing transport =================
    lv_obj_t *left = lv_obj_create(root);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 380, lv_pct(100));
    lv_obj_set_style_bg_color(left, th->surface, 0);
    lv_obj_set_style_bg_grad_color(left, th->bg, 0);
    lv_obj_set_style_bg_grad_dir(left, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(left, 18, 0);
    lv_obj_set_style_pad_all(left, 14, 0);
    lv_obj_set_style_pad_row(left, 6, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    // status chip, pinned to the panel's top-right corner (repainted live in tick())
    s_status = lv_label_create(left);
    lv_obj_set_style_bg_opa(s_status, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_status, th->surface3, 0);
    lv_obj_set_style_text_color(s_status, th->text_dim, 0);
    lv_obj_set_style_text_font(s_status, &nv_font_14, 0);
    lv_obj_set_style_radius(s_status, 10, 0);
    lv_obj_set_style_pad_hor(s_status, 10, 0);
    lv_obj_set_style_pad_ver(s_status, 4, 0);
    lv_label_set_text(s_status, nv_i18n_get_lang() == NV_LANG_IT ? "Pronto" : "Ready");
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, 0, 0);

    if (!nv_audio_mic_ready()) {
        lv_obj_t *l = lv_label_create(left);
        lv_label_set_text(l, nv_tr(NV_STR_NO_MIC));
        lv_obj_set_style_text_color(l, th->text_dim, 0);
    } else {
        s_time = lv_label_create(left);
        lv_label_set_text(s_time, "0:00");
        lv_obj_set_style_text_font(s_time, &nv_font_28, 0);
        lv_obj_set_style_text_color(s_time, th->text_strong, 0);

        // waveform row — stretched full width (space-between) instead of hugging the center,
        // so the panel's whole width is used instead of leaving dead margins either side.
        lv_obj_t *wave = lv_obj_create(left);
        lv_obj_remove_style_all(wave);
        lv_obj_set_size(wave, lv_pct(100), kBarMax + 8);
        lv_obj_set_flex_flow(wave, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wave, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(wave, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < kBars; i++) {
            lv_obj_t *b = lv_obj_create(wave);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, 5, kBarMin);
            lv_obj_set_style_radius(b, 2, 0);
            lv_obj_set_style_bg_color(b, th->surface3, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            s_bars[i] = b;
        }

        // record button: ring + morphing red core (bigger — the app's primary action)
        s_ring = lv_obj_create(left);
        lv_obj_remove_style_all(s_ring);
        lv_obj_set_size(s_ring, 96, 96);
        lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_ring, th->surface3, 0);
        lv_obj_set_style_bg_opa(s_ring, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_ring, 3, 0);
        lv_obj_set_style_border_color(s_ring, th->surface3, 0);
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_ext_click_area(s_ring, NV_SP_2);
        lv_obj_add_event_cb(s_ring, toggle_rec, LV_EVENT_CLICKED, nullptr);
        s_core = lv_obj_create(s_ring);
        lv_obj_remove_style_all(s_core);
        lv_obj_set_size(s_core, 46, 46);
        lv_obj_center(s_core);
        lv_obj_set_style_radius(s_core, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_core, th->danger, 0);
        lv_obj_set_style_bg_opa(s_core, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_core, LV_OBJ_FLAG_CLICKABLE);
    }

    // divider: separates the record stage from the now-playing/transport section below
    lv_obj_t *div = lv_obj_create(left);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, lv_pct(88), 2);
    lv_obj_set_style_bg_color(div, th->surface3, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    // now-playing: title + subtitle
    s_pv_title = lv_label_create(left);
    lv_label_set_long_mode(s_pv_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_pv_title, lv_pct(100));
    lv_obj_set_style_text_font(s_pv_title, &nv_font_20, 0);
    lv_obj_set_style_text_color(s_pv_title, th->text_strong, 0);
    lv_obj_set_style_text_align(s_pv_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_anim_duration(s_pv_title, 12000, 0);

    s_pv_sub = lv_label_create(left);
    lv_obj_set_width(s_pv_sub, lv_pct(100));
    lv_obj_set_style_text_font(s_pv_sub, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_pv_sub, th->text_dim, 0);
    lv_obj_set_style_text_align(s_pv_sub, LV_TEXT_ALIGN_CENTER, 0);

    // seek row: pos [progress] dur (read-only — WAV isn't a seekable format)
    lv_obj_t *sr = lv_obj_create(left);
    lv_obj_remove_style_all(sr);
    lv_obj_set_size(sr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sr, 10, 0);
    lv_obj_clear_flag(sr, LV_OBJ_FLAG_SCROLLABLE);

    s_pv_pos = lv_label_create(sr);
    lv_label_set_text(s_pv_pos, "0:00");
    lv_obj_set_style_text_font(s_pv_pos, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_pv_pos, th->text, 0);

    s_pv_bar = lv_bar_create(sr);
    lv_obj_set_flex_grow(s_pv_bar, 1);
    lv_obj_set_height(s_pv_bar, 8);
    lv_bar_set_range(s_pv_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_pv_bar, th->surface3, 0);
    lv_obj_set_style_bg_color(s_pv_bar, th->accent, LV_PART_INDICATOR);

    s_pv_dur = lv_label_create(sr);
    lv_label_set_text(s_pv_dur, "0:00");
    lv_obj_set_style_text_font(s_pv_dur, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_pv_dur, th->text_dim, 0);

    // transport: prev · BIG play/pause · next
    lv_obj_t *tr = lv_obj_create(left);
    lv_obj_remove_style_all(tr);
    lv_obj_set_size(tr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tr, 20, 0);
    lv_obj_clear_flag(tr, LV_OBJ_FLAG_SCROLLABLE);

    round_btn(tr, LV_SYMBOL_PREV, prev_cb, false, 56);
    s_pv_play = round_btn(tr, LV_SYMBOL_PLAY, pv_playpause_cb, true, 80);   // bigger than Music's 64px
    round_btn(tr, LV_SYMBOL_NEXT, next_cb, false, 56);

    s_pv_next = lv_label_create(left);
    lv_obj_set_width(s_pv_next, lv_pct(100));
    lv_label_set_long_mode(s_pv_next, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_pv_next, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_pv_next, th->text_dim, 0);
    lv_obj_set_style_text_align(s_pv_next, LV_TEXT_ALIGN_CENTER, 0);

    // ================= right: recordings library =================
    lv_obj_t *lib = lv_obj_create(root);
    lv_obj_remove_style_all(lib);
    lv_obj_set_height(lib, lv_pct(100));
    lv_obj_set_flex_grow(lib, 1);
    lv_obj_set_flex_flow(lib, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lib, 8, 0);
    lv_obj_clear_flag(lib, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr_row = lv_obj_create(lib);
    lv_obj_remove_style_all(hdr_row);
    lv_obj_set_size(hdr_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(hdr_row, LV_OBJ_FLAG_SCROLLABLE);

    s_hdr = lv_label_create(hdr_row);
    lv_obj_set_style_text_font(s_hdr, &nv_font_20, 0);
    lv_obj_set_style_text_color(s_hdr, th->text_strong, 0);

    s_sd = lv_label_create(hdr_row);
    lv_obj_set_style_text_font(s_sd, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_sd, th->text_dim, 0);
    {   // seed immediately so the header isn't blank until the first tick fires
        uint64_t total = 0, freeb = 0;
        const bool it = (nv_i18n_get_lang() == NV_LANG_IT);
        char b[48];
        if (nv_sd_info(&total, &freeb)) lv_snprintf(b, sizeof b, "%s %.1f GB", it ? "Libera:" : "Free:", freeb / 1073741824.0);
        else                            lv_snprintf(b, sizeof b, "%s", it ? "SD non trovata" : "No SD card");
        lv_label_set_text(s_sd, b);
    }

    s_list = nv_kit_scroll_column(lib);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_row(s_list, 6, 0);

    build_list();
    update_now_playing();
    s_timer = lv_timer_create(tick, 70, nullptr);   // fast for a lively waveform
}

const NvApp kRecApp = {"recorder", "Recorder", &nv_icon_recorder, 512u << 10, rec_build,
                       NV_STR_APP_RECORDER, nullptr};

}  // namespace

void recorder_app_register(void) { nv_app_register(&kRecApp); }
