// music_app — the NucleoOS music player. Split view on the 1024x600 panel: a Now-Playing
// panel (stylized vinyl, seek, transport with shuffle/repeat, volume + output route badge)
// and the library list. Swipe left/right on the vinyl panel changes track (nv_gesture_bind).
// Playback stops when the app closes (page teardown) — the player owns its audio session.
// Engine: nv_media (MP3/AAC/FLAC/WAV/M4A -> nv_audio PCM -> ES8311 or USB speaker).
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_media.h"
#include "nv_gesture.h"
#include "nv_config.h"
#include "nv_audio.h"
#include "nv_usb_audio.h"   // output route badge (USB soundbar vs JST speaker)

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dirent.h>

namespace {

constexpr char kMusicDir[] = "/sdcard/Music";
constexpr int  kMaxFiles   = 256;
constexpr int  kNameLen    = 128;

char (*s_files)[kNameLen] = nullptr;   // PSRAM, allocated on app open, freed on close (not .bss)
int *s_durs = nullptr;                 // header-probed durations (ms), same lifetime
int  s_nfiles = 0;
int  s_cur    = -1;                    // loaded track (session-persistent, playback is not)

// Play modes (session-persistent)
bool s_shuffle = false;
int  s_repeat  = 0;                    // 0 off · 1 all · 2 one

// UI handles (nulled on teardown)
lv_obj_t   *s_title    = nullptr;
lv_obj_t   *s_sub      = nullptr;      // format · rate · route
lv_obj_t   *s_play     = nullptr;      // play/pause round button
lv_obj_t   *s_pos      = nullptr;
lv_obj_t   *s_dur      = nullptr;
lv_obj_t   *s_seek     = nullptr;      // slider
lv_obj_t   *s_list     = nullptr;
lv_obj_t   *s_shuf_lb  = nullptr;      // shuffle icon label (recolored when active)
lv_obj_t   *s_rep_lb   = nullptr;      // repeat icon label
lv_obj_t   *s_disc_ic  = nullptr;      // center-of-vinyl icon (play state tint)
lv_obj_t   *s_vol_ic   = nullptr;      // volume icon (level/mute aware; tap = mute toggle)
lv_obj_t   *s_route    = nullptr;      // output chip on the art panel ("USB" / "JST")
lv_obj_t   *s_ring     = nullptr;      // progress ring around the vinyl
lv_obj_t   *s_orbit    = nullptr;      // orbiting "stylus" dot (runs while playing)
lv_obj_t   *s_next     = nullptr;      // "Up next" line under the transport
bool        s_orbit_on = false;
lv_obj_t   *s_eq[3]    = {};           // animated EQ bars on the active list row
lv_timer_t *s_timer    = nullptr;
bool        s_scrubbing = false;       // finger on the seek slider: freeze its auto-update
bool        s_remaining = false;       // right time label shows -remaining instead of duration
bool        s_eq_running = false;

void build_list(void);
void update_now_playing(void);

// ---------------------------------------------------------------- library

void fmt_ms(char *b, size_t n, int ms) {
    if (ms < 0) ms = 0;
    lv_snprintf(b, n, "%d:%02d", ms / 60000, (ms / 1000) % 60);
}

void scan_dir(void) {
    s_nfiles = 0;
    if (!s_files) return;
    DIR *d = opendir(kMusicDir);
    if (!d) return;
    struct dirent *e;
    while (s_nfiles < kMaxFiles && (e = readdir(d)) != nullptr) {
        if (e->d_type == DT_DIR) continue;
        if (!nv_media_is_audio(e->d_name)) continue;
        strncpy(s_files[s_nfiles], e->d_name, kNameLen - 1);
        s_files[s_nfiles][kNameLen - 1] = '\0';
        if (s_durs) {   // header-only probe: a few SD reads per file
            char full[300];
            lv_snprintf(full, sizeof full, "%s/%s", kMusicDir, s_files[s_nfiles]);
            s_durs[s_nfiles] = nv_media_probe_dur_ms(full);
        }
        s_nfiles++;
    }
    closedir(d);
}

// "01_Moody Gang.mp3" -> "01 Moody Gang" (display only).
void pretty_name(const char *file, char *out, size_t n) {
    snprintf(out, n, "%s", file);
    if (char *dot = strrchr(out, '.')) *dot = '\0';
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

// ---------------------------------------------------------------- transport

void play_index(int i) {
    if (i < 0 || i >= s_nfiles) return;
    s_cur = i;
    char full[300];
    lv_snprintf(full, sizeof full, "%s/%s", kMusicDir, s_files[i]);
    nv_media_play(full);
    update_now_playing();
    build_list();
}

int pick_next(void) {
    if (s_nfiles <= 0) return -1;
    if (s_shuffle) {
        if (s_nfiles == 1) return 0;
        int n;
        do { n = (int)(esp_random() % (uint32_t)s_nfiles); } while (n == s_cur);
        return n;
    }
    if (s_cur + 1 < s_nfiles) return s_cur + 1;
    return s_repeat == 1 ? 0 : -1;      // wrap only on repeat-all
}

int pick_prev(void) {
    if (s_nfiles <= 0) return -1;
    if (s_shuffle) return pick_next();  // shuffled "prev" = another random pick
    if (s_cur > 0) return s_cur - 1;
    return s_repeat == 1 ? s_nfiles - 1 : -1;
}

void next_cb(lv_event_t *) { int n = pick_next(); if (n >= 0) play_index(n); }
void prev_cb(lv_event_t *) { int p = pick_prev(); if (p >= 0) play_index(p); }
void gesture_next(void *)  { int n = pick_next(); if (n >= 0) play_index(n); }
void gesture_prev(void *)  { int p = pick_prev(); if (p >= 0) play_index(p); }

void playpause_cb(lv_event_t *) {
    switch (nv_media_state()) {
        case NV_MEDIA_PLAYING: nv_media_pause(true);  break;
        case NV_MEDIA_PAUSED:  nv_media_pause(false); break;
        default:               if (s_cur >= 0) play_index(s_cur);
                               else if (s_nfiles) play_index(0);
                               break;
    }
}

void mode_paint(void) {
    const NvTheme *th = nv_theme_get();
    if (s_shuf_lb) lv_obj_set_style_text_color(s_shuf_lb, s_shuffle ? th->accent : th->text_dim, 0);
    if (s_rep_lb) {
        lv_obj_set_style_text_color(s_rep_lb, s_repeat ? th->accent : th->text_dim, 0);
        lv_label_set_text(s_rep_lb, s_repeat == 2 ? "1" LV_SYMBOL_LOOP : LV_SYMBOL_LOOP);
    }
}

void shuffle_cb(lv_event_t *) { s_shuffle = !s_shuffle; mode_paint(); update_now_playing(); }
void repeat_cb(lv_event_t *)  { s_repeat = (s_repeat + 1) % 3; mode_paint(); update_now_playing(); }

void row_cb(lv_event_t *e) { play_index((int)(intptr_t)lv_event_get_user_data(e)); }

// ---------------------------------------------------------------- seek + volume

void seek_pressed_cb(lv_event_t *)  { s_scrubbing = true; }
void seek_released_cb(lv_event_t *) {
    s_scrubbing = false;
    if (s_seek && nv_media_seekable()) nv_media_seek((int)lv_slider_get_value(s_seek));
}
void seek_scrub_cb(lv_event_t *) {     // live time preview under the finger
    if (!s_scrubbing || !s_pos || !s_seek) return;
    const int dur = nv_media_dur_ms();
    if (dur <= 0) return;
    char b[16];
    fmt_ms(b, sizeof b, (int)((int64_t)dur * lv_slider_get_value(s_seek) / 1000));
    lv_label_set_text(s_pos, b);
}

void time_mode_cb(lv_event_t *) { s_remaining = !s_remaining; }   // tap the right label

void vol_icon_paint(int vol) {
    if (!s_vol_ic) return;
    const bool muted = nv_config_get_bool("mute", false);
    lv_label_set_text(s_vol_ic, muted || vol == 0 ? LV_SYMBOL_MUTE
                              : vol < 55          ? LV_SYMBOL_VOLUME_MID
                                                  : LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(s_vol_ic, muted ? nv_theme_get()->danger : nv_theme_get()->text_dim, 0);
}

void vol_changed_cb(lv_event_t *e) {   // live while dragging
    const int v = (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    nv_audio_set_volume(v);
    vol_icon_paint(v);
}
void vol_released_cb(lv_event_t *e) {  // persist once, on release (settings-app pattern)
    nv_config_set_int("volume", (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
}
void mute_toggle_cb(lv_event_t *) {    // tap the volume icon
    const bool m = !nv_config_get_bool("mute", false);
    nv_config_set_bool("mute", m);
    nv_audio_set_mute(m);
    vol_icon_paint(nv_config_get_int("volume", 60));
}

// ---------------------------------------------------------------- now-playing panel

void update_now_playing(void) {
    const NvTheme *th = nv_theme_get();
    const bool it = (nv_i18n_get_lang() == NV_LANG_IT);
    if (s_title) {
        if (s_cur >= 0 && s_cur < s_nfiles) {
            char nm[kNameLen];
            pretty_name(s_files[s_cur], nm, sizeof nm);
            lv_label_set_text(s_title, nm);
        } else if (nv_media_state() == NV_MEDIA_PLAYING || nv_media_state() == NV_MEDIA_PAUSED) {
            // Engine is playing something the app didn't start (remote/web control).
            lv_label_set_text(s_title, it ? "In riproduzione" : "Now playing");
        } else {
            lv_label_set_text(s_title, it ? "Scegli un brano" : "Pick a track");
        }
    }
    if (s_next) {
        if (s_shuffle) {
            lv_label_set_text(s_next, it ? "Prossimo: casuale" : "Up next: shuffle");
        } else {
            const int n = (s_cur >= 0 && s_cur + 1 < s_nfiles) ? s_cur + 1
                        : (s_repeat == 1 && s_nfiles > 0) ? 0 : -1;
            if (n >= 0) {
                char nm[kNameLen], b[kNameLen + 16];
                pretty_name(s_files[n], nm, sizeof nm);
                lv_snprintf(b, sizeof b, "%s %s", it ? "Prossimo:" : "Up next:", nm);
                lv_label_set_text(s_next, b);
            } else {
                lv_label_set_text(s_next, it ? "Fine coda" : "End of queue");
            }
        }
    }
    if (s_sub) {
        const char *ext = "";
        if (s_cur >= 0) { ext = strrchr(s_files[s_cur], '.'); ext = ext ? ext + 1 : ""; }
        int rate = 0, ch = 0;
        nv_media_track_info(&rate, &ch, nullptr);
        char up[8] = {0};
        for (int i = 0; ext[i] && i < 7; i++) up[i] = (char)((ext[i] >= 'a' && ext[i] <= 'z') ? ext[i] - 32 : ext[i]);
        char b[80];
        if (rate && up[0])
            lv_snprintf(b, sizeof b, "%s \xC2\xB7 %d.%d kHz \xC2\xB7 %s \xC2\xB7 %s",
                        up, rate / 1000, (rate % 1000) / 100, ch == 2 ? "stereo" : "mono",
                        nv_usb_audio_present() ? "USB" : "JST");
        else if (rate)
            lv_snprintf(b, sizeof b, "%d.%d kHz \xC2\xB7 %s \xC2\xB7 %s",
                        rate / 1000, (rate % 1000) / 100, ch == 2 ? "stereo" : "mono",
                        nv_usb_audio_present() ? "USB" : "JST");
        else
            lv_snprintf(b, sizeof b, "%s", nv_usb_audio_present() ? "USB" : "JST");
        lv_label_set_text(s_sub, b);
    }
    (void)th;
}

void orbit_anim_cb(void *o, int32_t v);   // fwd decl: used by the now-playing view below, defined later

// Animated EQ bars on the active row: alive while playing, flat while paused/stopped.
void eq_anim_cb(void *o, int32_t v) { lv_obj_set_height((lv_obj_t *)o, v); }
void eq_set(bool run) {
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

// Start/stop the orbiting stylus with playback (smooth 4 s revolution via lv_anim).
void orbit_set(bool run) {
    if (!s_orbit || run == s_orbit_on) return;
    s_orbit_on = run;
    lv_anim_delete(s_orbit, nullptr);
    if (run) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_orbit);
        lv_anim_set_exec_cb(&a, orbit_anim_cb);
        lv_anim_set_values(&a, 0, 3599);
        lv_anim_set_duration(&a, 4000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

void tick(lv_timer_t *) {
    const nv_media_state_t st = nv_media_state();
    const NvTheme *th = nv_theme_get();
    if (s_play)
        lv_label_set_text(lv_obj_get_child(s_play, 0),
                          st == NV_MEDIA_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if (s_disc_ic)
        lv_obj_set_style_text_color(s_disc_ic, st == NV_MEDIA_PLAYING ? th->accent : th->text_dim, 0);
    eq_set(st == NV_MEDIA_PLAYING);
    orbit_set(st == NV_MEDIA_PLAYING);

    const int pos = nv_media_pos_ms();
    int dur = nv_media_dur_ms();
    if (dur <= 0 && s_cur >= 0 && s_durs) dur = s_durs[s_cur];   // probed header duration
    char b[16];
    if (s_pos && !s_scrubbing) { fmt_ms(b, sizeof b, pos); lv_label_set_text(s_pos, b); }
    if (s_dur) {
        if (s_remaining && dur > 0) { b[0] = '-'; fmt_ms(b + 1, sizeof b - 1, dur - pos); }
        else fmt_ms(b, sizeof b, dur);
        lv_label_set_text(s_dur, b);
    }
    if (s_seek && !s_scrubbing) {
        int pct = (dur > 0) ? (int)((int64_t)pos * 1000 / dur) : 0;
        if (pct > 1000) pct = 1000;
        lv_slider_set_value(s_seek, pct, LV_ANIM_OFF);
        if (s_ring) lv_arc_set_value(s_ring, pct);   // the vinyl's progress ring tracks too
    }

    // Subtitle refreshes lazily (rate appears shortly after play; route can hot-swap).
    static int      last_rate = -1;
    static bool     last_usb  = false;
    int rate = 0;
    nv_media_track_info(&rate, nullptr, nullptr);
    const bool usb = nv_usb_audio_present();
    if (rate != last_rate || usb != last_usb) {
        last_rate = rate; last_usb = usb;
        update_now_playing();
        if (s_route) {
            lv_label_set_text(s_route, usb ? LV_SYMBOL_USB " USB" : LV_SYMBOL_VOLUME_MAX " JST");
            lv_obj_set_style_bg_color(s_route, usb ? th->primary : th->surface3, 0);
            lv_obj_set_style_text_color(s_route, usb ? th->on_primary : th->text_dim, 0);
        }
    }

    if (nv_media_took_eot()) {
        const int n = (s_repeat == 2) ? s_cur : pick_next();
        if (n >= 0) play_index(n);
        else update_now_playing();      // end of queue: leave the title, transport shows Play
    }
}

// Stylized vinyl inside a 240px holder: a thin accent PROGRESS RING around the disc, an
// ORBITING dot that circles the rim while playing (fixed-point trig, tiny invalidation area),
// concentric grooves, tinted hub. All SW-renderer-safe: no shadows, no transforms, no layers.
constexpr int kVinyl = 200;   // space-evenly flex absorbs the rest of the column height

void orbit_anim_cb(void *o, int32_t v) {          // v = angle in 0.1deg (0..3599)
    const int deg = v / 10;
    const int r = kVinyl / 2 - 14;
    const int x = kVinyl / 2 + ((lv_trigo_sin((int16_t)(deg + 90)) * r) >> 15);   // cos
    const int y = kVinyl / 2 + ((lv_trigo_sin((int16_t)deg) * r) >> 15);
    lv_obj_set_pos((lv_obj_t *)o, x - 5, y - 5);
}

lv_obj_t *vinyl_make(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *holder = lv_obj_create(parent);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, kVinyl, kVinyl);
    lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

    // progress ring: full-circle arc, no knob, not touchable (the linear seek does the input)
    s_ring = lv_arc_create(holder);
    lv_obj_set_size(s_ring, kVinyl, kVinyl);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, th->surface2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, th->accent, LV_PART_INDICATOR);

    lv_obj_t *disc = lv_obj_create(holder);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, kVinyl - 34, kVinyl - 34);
    lv_obj_center(disc);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(disc, th->scrim, 0);
    lv_obj_set_style_border_width(disc, 2, 0);
    lv_obj_set_style_border_color(disc, th->surface3, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {                 // grooves
        lv_obj_t *ring = lv_obj_create(disc);
        lv_obj_remove_style_all(ring);
        const int d = kVinyl - 80 - i * 40;
        lv_obj_set_size(ring, d, d);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, th->surface2, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *hub = lv_obj_create(disc);          // label area
    lv_obj_remove_style_all(hub);
    lv_obj_set_size(hub, 74, 74);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hub, th->surface2, 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);

    s_disc_ic = lv_label_create(hub);
    lv_label_set_text(s_disc_ic, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(s_disc_ic, &nv_font_28, 0);
    lv_obj_set_style_text_color(s_disc_ic, th->text_dim, 0);
    lv_obj_center(s_disc_ic);

    // orbiting "stylus" dot — animated only while playing (see play_motion_set)
    s_orbit = lv_obj_create(holder);
    lv_obj_remove_style_all(s_orbit);
    lv_obj_set_size(s_orbit, 10, 10);
    lv_obj_set_style_radius(s_orbit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_orbit, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_orbit, th->accent, 0);
    orbit_anim_cb(s_orbit, 0);
    return holder;
}

lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, bool primary, int size) {
    lv_obj_t *b = nv_kit_button(parent, sym, primary);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

// ---------------------------------------------------------------- list panel

void build_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    s_eq[0] = s_eq[1] = s_eq[2] = nullptr;   // rows are gone; tick re-arms on the new bars
    s_eq_running = false;
    const NvTheme *th = nv_theme_get();
    if (s_nfiles == 0) {
        lv_obj_t *box = lv_obj_create(s_list);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(box, 24, 0);
        lv_obj_set_style_pad_row(box, 10, 0);
        lv_obj_t *ic = lv_label_create(box);
        lv_label_set_text(ic, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(ic, &nv_font_28, 0);
        lv_obj_set_style_text_color(ic, th->text_dim, 0);
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_text(l, nv_tr(NV_STR_NO_MUSIC));
        lv_obj_set_style_text_color(l, th->text_dim, 0);
        lv_obj_t *hint = lv_label_create(box);
        lv_label_set_text(hint, nv_i18n_get_lang() == NV_LANG_IT
                                ? "Copia i brani in /Music sulla microSD"
                                : "Copy tracks to /Music on the microSD");
        lv_obj_set_style_text_font(hint, &nv_font_14, 0);
        lv_obj_set_style_text_color(hint, th->text_dim, 0);
        return;
    }
    for (int i = 0; i < s_nfiles; i++) {
        const bool active = (i == s_cur);
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, active ? th->surface2 : th->surface, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, th->surface3, LV_STATE_PRESSED);   // touch feedback
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 15, 0);
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
        if (active) lv_obj_scroll_to_view(row, LV_ANIM_ON);   // keep the playing row on screen

        if (active) {
            // Animated mini-EQ instead of the track number: the "now playing" landmark.
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
            s_eq_running = false;   // tick restarts the animation on the fresh bars
        } else {
            lv_obj_t *nr = lv_label_create(row);
            char nb[8];
            lv_snprintf(nb, sizeof nb, "%02d", i + 1);
            lv_label_set_text(nr, nb);
            lv_obj_set_style_text_color(nr, th->text_dim, 0);
            lv_obj_set_width(nr, 34);
        }

        char nm[kNameLen];
        pretty_name(s_files[i], nm, sizeof nm);
        lv_obj_t *lb = lv_label_create(row);
        lv_label_set_text(lb, nm);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(lb, 1);
        lv_obj_set_style_text_color(lb, active ? th->text_strong : th->text, 0);

        if (s_durs && s_durs[i] > 0) {
            char db[12];
            fmt_ms(db, sizeof db, s_durs[i]);
            lv_obj_t *dl = lv_label_create(row);
            lv_label_set_text(dl, db);
            lv_obj_set_style_text_color(dl, th->text_dim, 0);
        }
    }
}

// ---------------------------------------------------------------- build / teardown

void page_deleted(lv_event_t *) {
    nv_media_stop();   // the player owns its audio session: leaving the app stops the music
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_files) { heap_caps_free(s_files); s_files = nullptr; s_nfiles = 0; }
    if (s_durs)  { heap_caps_free(s_durs);  s_durs = nullptr; }
    s_title = s_sub = s_play = s_pos = s_dur = s_seek = s_list = nullptr;
    s_shuf_lb = s_rep_lb = s_disc_ic = s_vol_ic = s_route = nullptr;
    s_ring = s_orbit = s_next = nullptr;
    s_eq[0] = s_eq[1] = s_eq[2] = nullptr;
    s_eq_running = false;
    s_orbit_on = false;
    s_scrubbing = false;
}

void music_build(lv_obj_t *content) {
    nv_media_init();
    if (!s_files) s_files = (char (*)[kNameLen])heap_caps_malloc((size_t)kMaxFiles * kNameLen,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_durs) s_durs = (int *)heap_caps_malloc(kMaxFiles * sizeof(int),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    scan_dir();
    if (s_cur >= s_nfiles) s_cur = -1;
    const NvTheme *th = nv_theme_get();
    const bool it = (nv_i18n_get_lang() == NV_LANG_IT);

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 14, 0);
    lv_obj_set_style_pad_column(root, 14, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    // ================= left: Now Playing =================
    lv_obj_t *np = lv_obj_create(root);
    lv_obj_remove_style_all(np);
    lv_obj_set_size(np, 400, lv_pct(100));
    lv_obj_set_style_bg_color(np, th->surface, 0);
    lv_obj_set_style_bg_grad_color(np, th->bg, 0);      // subtle vertical fade toward the canvas
    lv_obj_set_style_bg_grad_dir(np, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(np, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(np, 18, 0);
    lv_obj_set_style_pad_all(np, 12, 0);
    lv_obj_set_style_pad_row(np, 4, 0);
    lv_obj_set_flex_flow(np, LV_FLEX_FLOW_COLUMN);
    // space-evenly on the main axis: the column BREATHES across the whole 600px instead of
    // huddling at the top with a dead zone below (leftover of the anti-clip compaction).
    lv_obj_set_flex_align(np, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(np, LV_OBJ_FLAG_SCROLLABLE);

    // swipe on the whole panel: left = next, right = previous
    nv_gesture_bind(np, LV_DIR_LEFT,  gesture_next, nullptr);
    nv_gesture_bind(np, LV_DIR_RIGHT, gesture_prev, nullptr);

    // Output route chip, pinned to the panel's top-right corner (repainted live in tick()).
    s_route = lv_label_create(np);
    lv_obj_set_style_bg_opa(s_route, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_route, th->surface3, 0);
    lv_obj_set_style_text_color(s_route, th->text_dim, 0);
    lv_obj_set_style_text_font(s_route, &nv_font_14, 0);
    lv_obj_set_style_radius(s_route, 10, 0);
    lv_obj_set_style_pad_hor(s_route, 10, 0);
    lv_obj_set_style_pad_ver(s_route, 4, 0);
    lv_label_set_text(s_route, LV_SYMBOL_VOLUME_MAX " JST");
    lv_obj_add_flag(s_route, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s_route, LV_ALIGN_TOP_RIGHT, 0, 0);

    vinyl_make(np);

    s_title = lv_label_create(np);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);   // long titles marquee
    lv_obj_set_width(s_title, lv_pct(100));
    lv_obj_set_style_text_font(s_title, &nv_font_20, 0);
    lv_obj_set_style_text_color(s_title, th->text_strong, 0);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_anim_duration(s_title, 12000, 0);                // slow, readable sweep

    s_sub = lv_label_create(np);
    lv_obj_set_width(s_sub, lv_pct(100));
    lv_obj_set_style_text_font(s_sub, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_sub, th->text_dim, 0);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);

    // seek row: pos [slider] dur
    lv_obj_t *sr = lv_obj_create(np);
    lv_obj_remove_style_all(sr);
    lv_obj_set_size(sr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sr, 10, 0);
    lv_obj_set_style_pad_ver(sr, 3, 0);
    lv_obj_clear_flag(sr, LV_OBJ_FLAG_SCROLLABLE);

    s_pos = lv_label_create(sr);
    lv_label_set_text(s_pos, "0:00");
    lv_obj_set_style_text_font(s_pos, &nv_font_20, 0);
    lv_obj_set_style_text_color(s_pos, th->text, 0);

    s_seek = lv_slider_create(sr);
    lv_obj_set_flex_grow(s_seek, 1);
    lv_obj_set_height(s_seek, 10);
    lv_slider_set_range(s_seek, 0, 1000);
    lv_obj_set_style_bg_color(s_seek, th->surface3, 0);
    lv_obj_set_style_bg_color(s_seek, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_seek, th->accent, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_seek, 8, LV_PART_KNOB);   // fat knob: grabbable with a thumb
    lv_obj_set_ext_click_area(s_seek, 16);               // forgiving vertical hit box
    lv_obj_add_event_cb(s_seek, seek_pressed_cb,  LV_EVENT_PRESSED,  nullptr);
    lv_obj_add_event_cb(s_seek, seek_released_cb, LV_EVENT_RELEASED, nullptr);

    s_dur = lv_label_create(sr);
    lv_label_set_text(s_dur, "0:00");
    lv_obj_set_style_text_font(s_dur, &nv_font_20, 0);
    lv_obj_set_style_text_color(s_dur, th->text_dim, 0);
    lv_obj_add_flag(s_dur, LV_OBJ_FLAG_CLICKABLE);       // tap: duration <-> -remaining
    lv_obj_add_event_cb(s_dur, time_mode_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_seek, seek_scrub_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // transport: shuffle · prev · play · next · repeat
    lv_obj_t *tr = lv_obj_create(np);
    lv_obj_remove_style_all(tr);
    lv_obj_set_size(tr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tr, 16, 0);
    lv_obj_clear_flag(tr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *shuf = round_btn(tr, LV_SYMBOL_SHUFFLE, shuffle_cb, false, 48);
    s_shuf_lb = lv_obj_get_child(shuf, 0);
    round_btn(tr, LV_SYMBOL_PREV, prev_cb, false, 56);
    s_play = round_btn(tr, LV_SYMBOL_PLAY, playpause_cb, true, 64);
    round_btn(tr, LV_SYMBOL_NEXT, next_cb, false, 56);
    lv_obj_t *rep = round_btn(tr, LV_SYMBOL_LOOP, repeat_cb, false, 48);
    s_rep_lb = lv_obj_get_child(rep, 0);
    mode_paint();

    // "Up next" — one glance tells where the queue goes (shuffle/repeat aware).
    s_next = lv_label_create(np);
    lv_obj_set_width(s_next, lv_pct(100));
    lv_label_set_long_mode(s_next, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_next, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_next, th->text_dim, 0);
    lv_obj_set_style_text_align(s_next, LV_TEXT_ALIGN_CENTER, 0);

    // volume row
    lv_obj_t *vr = lv_obj_create(np);
    lv_obj_remove_style_all(vr);
    lv_obj_set_size(vr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vr, 10, 0);
    lv_obj_set_style_pad_top(vr, 6, 0);
    lv_obj_clear_flag(vr, LV_OBJ_FLAG_SCROLLABLE);

    s_vol_ic = lv_label_create(vr);
    lv_obj_add_flag(s_vol_ic, LV_OBJ_FLAG_CLICKABLE);    // tap: mute toggle
    lv_obj_set_style_pad_all(s_vol_ic, 6, 0);            // finger-sized target
    lv_obj_add_event_cb(s_vol_ic, mute_toggle_cb, LV_EVENT_CLICKED, nullptr);
    vol_icon_paint(nv_config_get_int("volume", 60));

    lv_obj_t *vol = lv_slider_create(vr);
    lv_obj_set_flex_grow(vol, 1);
    lv_obj_set_height(vol, 10);
    lv_slider_set_range(vol, 0, 100);
    lv_slider_set_value(vol, nv_config_get_int("volume", 60), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vol, th->surface3, 0);
    lv_obj_set_style_bg_color(vol, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol, th->accent, LV_PART_KNOB);
    lv_obj_set_style_pad_all(vol, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(vol, 16);
    lv_obj_add_event_cb(vol, vol_changed_cb,  LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(vol, vol_released_cb, LV_EVENT_RELEASED,      nullptr);

    // ================= right: library =================
    lv_obj_t *lib = lv_obj_create(root);
    lv_obj_remove_style_all(lib);
    lv_obj_set_height(lib, lv_pct(100));
    lv_obj_set_flex_grow(lib, 1);
    lv_obj_set_flex_flow(lib, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lib, 8, 0);
    lv_obj_clear_flag(lib, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(lib);
    int total_ms = 0;
    if (s_durs) for (int i = 0; i < s_nfiles; i++) total_ms += s_durs[i] > 0 ? s_durs[i] : 0;
    char hb[64];
    if (total_ms > 0)
        lv_snprintf(hb, sizeof hb, "%s (%d \xC2\xB7 %d min)", it ? "Libreria" : "Library",
                    s_nfiles, (total_ms + 30000) / 60000);
    else
        lv_snprintf(hb, sizeof hb, "%s (%d)", it ? "Libreria" : "Library", s_nfiles);
    lv_label_set_text(hdr, hb);
    lv_obj_set_style_text_font(hdr, &nv_font_20, 0);
    lv_obj_set_style_text_color(hdr, th->text_strong, 0);

    s_list = nv_kit_scroll_column(lib);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    build_list();

    update_now_playing();
    s_timer = lv_timer_create(tick, 300, nullptr);
    tick(nullptr);
}

const NvApp kMusicApp = {"music", "Music", &nv_icon_music, 2u << 20, music_build,
                         NV_STR_APP_MUSIC, nullptr};

}  // namespace

void music_app_register(void) { nv_app_register(&kMusicApp); }
