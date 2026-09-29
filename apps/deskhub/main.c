// main.c — Pomodoro Desk Hub per NucleoOS su display 7" (1024x600).
// Dashboard completa da scrivania: Cronometro radiale a 60 tacche, Selettore Task,
// Orologio digitale tempo reale, Calendario interattivo, Sintesi Vocale TTS e Night Mode.
#include "nucleo_sdk.h"

// ---- Lingua (segue la lingua di sistema) ---------------------------------------------------------
enum { L_IT = 0, L_EN = 1, L_COUNT };
static int g_lang = L_IT;

enum {
    T_NIGHT = 0, T_AUDIO_VOICE, T_AUDIO_BEEP, T_AUDIO_OFF, T_TASK_LABEL,
    T_TAB_FOCUS, T_TAB_BREAK, T_TAB_LONG, T_ST_FOCUS, T_ST_DONE, T_ST_BREAK,
    T_BTN_START, T_BTN_PAUSE, T_BTN_RESET, T_GOAL_TITLE, T_FOCUS_PREFIX, T_HOURS,
    T_MINUTES, T_TARGET_SUFFIX, T_CLEAR, T_WAKE_HINT, T_NIGHT_BREAK,
    T_TOAST_POMO_DONE, T_TOAST_BREAK_DONE, T_TOAST_CLEARED, T_UTC_LABEL,
    T_EXIT_TITLE, T_EXIT_BODY, T_EXIT_YES, T_EXIT_NO, T_COUNT
};

static const char *kStrings[T_COUNT][L_COUNT] = {
    [T_NIGHT]            = { "NOTTE",                    "NIGHT" },
    [T_AUDIO_VOICE]      = { "AUDIO: VOCE",               "AUDIO: VOICE" },
    [T_AUDIO_BEEP]       = { "AUDIO: BEEP",               "AUDIO: BEEP" },
    [T_AUDIO_OFF]        = { "AUDIO: OFF",                "AUDIO: OFF" },
    [T_TASK_LABEL]       = { "ATTIVITA':",                "TASK:" },
    [T_TAB_FOCUS]        = { "FOCUS (25M)",                "FOCUS (25M)" },
    [T_TAB_BREAK]        = { "PAUSA (5M)",                 "BREAK (5M)" },
    [T_TAB_LONG]         = { "LUNGA (15M)",                "LONG (15M)" },
    [T_ST_FOCUS]         = { "FOCUS IN CORSO",             "FOCUS IN PROGRESS" },
    [T_ST_DONE]          = { "SESSIONE COMPLETATA",        "SESSION COMPLETE" },
    [T_ST_BREAK]         = { "IN PAUSA",                   "ON BREAK" },
    [T_BTN_START]        = { "AVVIA",                      "START" },
    [T_BTN_PAUSE]        = { "PAUSA",                      "PAUSE" },
    [T_BTN_RESET]        = { "RESET",                      "RESET" },
    [T_GOAL_TITLE]       = { "OBIETTIVO GIORNALIERO",      "DAILY GOAL" },
    [T_FOCUS_PREFIX]     = { "FOCUS ACCUMULATO: ",         "TOTAL FOCUS: " },
    [T_HOURS]            = { " ORE ",                      " H " },
    [T_MINUTES]          = { " MIN",                       " MIN" },
    [T_TARGET_SUFFIX]    = { "% DEL TARGET RAGGIUNTO",     "% OF TARGET REACHED" },
    [T_CLEAR]            = { "AZZERA",                     "CLEAR" },
    [T_WAKE_HINT]        = { "TOCCA LO SCHERMO PER RISVEGLIARE LA DASHBOARD", "TAP SCREEN TO WAKE THE DASHBOARD" },
    [T_NIGHT_BREAK]      = { "PAUSA RELAX",                "RELAX BREAK" },
    [T_TOAST_POMO_DONE]  = { "Pomodoro completato! Fai una pausa.", "Pomodoro complete! Take a break." },
    [T_TOAST_BREAK_DONE] = { "Pausa finita. Pronto per un altro focus?", "Break over. Ready for another focus session?" },
    [T_TOAST_CLEARED]    = { "Statistiche giornaliere azzerate", "Daily stats cleared" },
    [T_UTC_LABEL]        = { "UTC",                        "UTC" },
    [T_EXIT_TITLE]       = { "USCIRE?",                    "EXIT?" },
    [T_EXIT_BODY]        = { "Il timer resta in pausa. Uscire dall'app?", "The timer stays paused. Exit the app?" },
    [T_EXIT_YES]         = { "ESCI",                       "EXIT" },
    [T_EXIT_NO]          = { "ANNULLA",                    "CANCEL" },
};

#define TR(id) (kStrings[id][g_lang])

static void detect_lang(void) {
    char buf[8] = { 0 };
    nv_lang(buf, sizeof buf);
    g_lang = (buf[0] == 'i' && buf[1] == 't') ? L_IT : L_EN;
}

// ---- Palette Colori (RGB565) --------------------------------------------------------------------
#define COLOR_BG            NV_RGB(10, 14, 20)      // Sfondo ardesia scuro
#define COLOR_PANEL_BG      NV_RGB(18, 24, 34)      // Card principale
#define COLOR_PANEL_BORDER  NV_RGB(36, 46, 62)      // Bordo card sottile
#define COLOR_HEADER        NV_RGB(14, 18, 26)      // Header bar
#define COLOR_SUBPANEL      NV_RGB(25, 33, 46)      // Box secondari
#define COLOR_SUB_BORDER    NV_RGB(45, 56, 75)      // Bordo box secondario

#define COLOR_TEXT_WHITE    NV_RGB(245, 248, 255)   // Testo principale
#define COLOR_TEXT_MUTED    NV_RGB(130, 145, 170)   // Testo secondario
#define COLOR_TEXT_DIM      NV_RGB(75, 88, 108)     // Testo terziario

#define COLOR_ACCENT_WORK   NV_RGB(235, 75, 60)     // Rosso corallo Pomodoro
#define COLOR_ACCENT_SHORT  NV_RGB(46, 204, 140)    // Menta Pausa Breve
#define COLOR_ACCENT_LONG   NV_RGB(88, 166, 255)    // Zaffiro Pausa Lunga
#define COLOR_BUTTON_START  NV_RGB(35, 145, 65)     // Verde Start
#define COLOR_BUTTON_PAUSE  NV_RGB(210, 135, 15)    // Ambra Pausa
#define COLOR_TICK_ACTIVE   NV_RGB(255, 255, 255)   // Tacca corrente

// Colori Modalità Notte (Comodino)
#define COLOR_NIGHT_BG      NV_RGB(0, 0, 0)         // Nero puro
#define COLOR_NIGHT_RED     NV_RGB(175, 20, 20)     // Rosso sveglia
#define COLOR_NIGHT_MUTED   NV_RGB(65, 10, 10)      // Rosso attenuato

#define CANVAS_W 1024
#define CANVAS_H 600

#define TASK_COUNT 6

// Tabella precalcolata seno e coseno per le 60 tacche della corona radiale (* 1000)
static const short kSin60[60] = {
    0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995,
    1000, 995, 978, 951, 914, 866, 809, 743, 669, 588, 500, 407, 309, 208, 105,
    0, -105, -208, -309, -407, -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105
};

static const short kCos60[60] = {
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105,
    0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995,
    1000, 995, 978, 951, 914, 866, 809, 743, 669, 588, 500, 407, 309, 208, 105,
    0, -105, -208, -309, -407, -500, -588, -669, -743, -809, -866, -914, -951, -978, -995
};

enum {
    MODE_WORK = 0,
    MODE_SHORT_BREAK,
    MODE_LONG_BREAK
};

enum {
    AUDIO_VOICE = 0,    // Sintesi vocale italiana offline
    AUDIO_BEEP,         // Toni armonici
    AUDIO_MUTE          // Silenzioso
};

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    int pomodoros_today;
    int focus_minutes_total;
    int audio_mode;         // AUDIO_VOICE, AUDIO_BEEP, AUDIO_MUTE
    int target_pomodoros;   // Target giornaliero (default: 8)
    int task_idx;           // Indice del task corrente (0..5)
    int utc_offset_h;       // Scostamento manuale da UTC (nessun accesso al fuso di sistema da WASM)
} HubConfig;

static HubConfig g_cfg;

static const char *kTaskNames[TASK_COUNT][L_COUNT] = {
    { "1. SVILUPPO & CODING",  "1. DEV & CODING" },
    { "2. STUDIO & RICERCA",   "2. STUDY & RESEARCH" },
    { "3. SCRITTURA & EMAIL",  "3. WRITING & EMAIL" },
    { "4. PROGETTAZIONE HW",   "4. HW DESIGN" },
    { "5. DEBUGGING & TEST",   "5. DEBUGGING & TEST" },
    { "6. LETTURA & REVIEW",   "6. READING & REVIEW" },
};
static const char *task_name(int i) { return kTaskNames[i][g_lang]; }

// Stato del Timer
static int g_mode = MODE_WORK;
static int g_timer_running = 0;
static int g_total_duration_s = 25 * 60;
static int g_remaining_s = 25 * 60;
static int g_last_tick_ms = 0;

static int g_night_mode = 0;

// Calendario: mese/anno visualizzati (navigabili) vs. la data reale odierna (fissa, da orologio)
static int g_cal_year = 2026;
static int g_cal_month = 9;
static int g_today_year, g_today_month, g_today_day;

static int g_prev_touch = 0;
static int g_confirm_exit = 0;   // modale "uscire?" armata dal gesto Back
static int g_want_exit = 0;      // confermato: il loop principale esce al prossimo giro

// ---- Funzioni Ausiliarie Grafiche e Numeriche ----------------------------------------------------

static int point_in_rect(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void draw_panel(int x, int y, int w, int h, int fill, int border) {
    nv_gfx_rect(x, y, w, h, fill);
    nv_gfx_line(x, y, x + w, y, border);
    nv_gfx_line(x, y + h, x + w, y + h, border);
    nv_gfx_line(x, y, x, y + h, border);
    nv_gfx_line(x + w, y, x + w, y + h, border);
}

// Scurisce un colore RGB565 di 'amt' sedicesimi (0..16) per il bordo/lip dei bottoni: niente
// primitiva "rounded rect" nella gfx ABI, ma un bordo scuro + un piccolo lip inferiore danno
// comunque un rilievo 3D leggero invece del rettangolo piatto.
static int darken(int c565, int amt) {
    int r = (c565 >> 11) & 0x1F, g = (c565 >> 5) & 0x3F, b = c565 & 0x1F;
    r = (r * (16 - amt)) / 16; g = (g * (16 - amt)) / 16; b = (b * (16 - amt)) / 16;
    return (r << 11) | (g << 5) | b;
}

// Bottone con lip 3D: corpo pieno, bordo scuro sottile e una banda scura in basso a simulare
// un rilievo pressabile. Tocco pensato per essere comodo su schermo capacitivo (min. ~44 px).
static void draw_button(Rect r, const char *label, int fill, int ink, int scale) {
    int lip = r.h >= 40 ? 4 : 3;
    nv_gfx_rect(r.x, r.y, r.w, r.h - lip, fill);
    nv_gfx_rect(r.x, r.y + r.h - lip, r.w, lip, darken(fill, 7));
    int border = darken(fill, 10);
    nv_gfx_line(r.x, r.y, r.x + r.w, r.y, border);
    nv_gfx_line(r.x, r.y + r.h, r.x + r.w, r.y + r.h, border);
    nv_gfx_line(r.x, r.y, r.x, r.y + r.h, border);
    nv_gfx_line(r.x + r.w, r.y, r.x + r.w, r.y + r.h, border);

    int tw = nv_gfx_text_width(label, scale);
    // Il lip sposta il centro ottico di qualche px verso l'alto: ricompensato sull'asse Y.
    nv_gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - lip - 7 * scale) / 2, label, ink, scale);
}

static void int_to_2digits(int v, char *out) {
    out[0] = (char)('0' + ((v / 10) % 10));
    out[1] = (char)('0' + (v % 10));
    out[2] = '\0';
}

static void int_to_str(int val, char *buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16];
    int i = 0;
    int sign = 1;
    if (val < 0) { sign = -1; val = -val; }
    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int j = 0;
    if (sign < 0) buf[j++] = '-';
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

// Data e Ora da Epoch Unix. Il firmware non espone il fuso configurato in Impostazioni alla
// WASM ABI attuale (solo nv_time_unix, UTC puro): l'offset è quindi regolabile a mano qui
// (persistito) invece di essere fissato a un valore che sarebbe sbagliato meta' dell'anno
// (CEST e' UTC+2 solo in ora legale, CET e' UTC+1 il resto dell'anno).
static int64_t local_epoch_s(void) {
    return nv_time_unix() + (int64_t)g_cfg.utc_offset_h * 3600;
}

static void get_current_time(int *hh, int *mm, int *ss) {
    int64_t now_s = local_epoch_s();
    int s_day = (int)(now_s % 86400);
    if (s_day < 0) s_day += 86400;
    *hh = s_day / 3600;
    *mm = (s_day % 3600) / 60;
    *ss = s_day % 60;
}

// Howard Hinnant's civil_from_days: giorni interi da epoch (1970-01-01) -> (anno, mese, giorno).
static void civil_from_days(long z, int *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;                                   // [0, 146096]
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    long yr  = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);             // [0, 365]
    long mp  = (5 * doy + 2) / 153;                                 // [0, 11]
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);                       // [1, 31]
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);                          // [1, 12]
    *y = (int)(yr + (*m <= 2));
}

static void get_current_date(int *y, int *m, int *d) {
    long days = (long)(local_epoch_s() / 86400);
    civil_from_days(days, y, m, d);
}

static int is_leap_year(int y) {
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

static int days_in_month(int y, int m) {
    static const int kDays[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && is_leap_year(y)) return 29;
    if (m >= 1 && m <= 12) return kDays[m];
    return 30;
}

static int day_of_week_first(int y, int m) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int ty = y - (m < 3);
    int d = (ty + ty/4 - ty/100 + ty/400 + t[m-1] + 1) % 7;
    return (d == 0) ? 6 : (d - 1);
}

static const char *month_name(int m) {
    static const char *kNamesIT[13] = {
        "", "GENNAIO", "FEBBRAIO", "MARZO", "APRILE", "MAGGIO", "GIUGNO",
        "LUGLIO", "AGOSTO", "SETTEMBRE", "OTTOBRE", "NOVEMBRE", "DICEMBRE"
    };
    static const char *kNamesEN[13] = {
        "", "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
        "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
    };
    if (m < 1 || m > 12) return "";
    return g_lang == L_EN ? kNamesEN[m] : kNamesIT[m];
}

// Feedback Audio & Sintesi Vocale Offline
static void notify_event(int is_completion) {
    if (g_cfg.audio_mode == AUDIO_MUTE) return;

    if (g_cfg.audio_mode == AUDIO_VOICE) {
        // La voce offline copre solo il corpus IT verificato per queste parole; per le altre
        // lingue non tentiamo parole non testate (rischio di corpus mancante) e usiamo il tono.
        if (is_completion && g_lang == L_IT) {
            nv_speak((g_mode == MODE_WORK) ? "OTTIMO" : "VIA", "it");
        } else {
            nv_gfx_tone(is_completion ? 1046 : 1100, is_completion ? 180 : 25);
        }
    } else {
        // AUDIO_BEEP
        if (is_completion) {
            nv_gfx_tone(523, 100); nv_sleep_ms(110);
            nv_gfx_tone(659, 100); nv_sleep_ms(110);
            nv_gfx_tone(784, 100); nv_sleep_ms(110);
            nv_gfx_tone(1046, 220);
        } else {
            nv_gfx_tone(1100, 25);
        }
    }
}

// ---- Logica Timer -------------------------------------------------------------------------------

static void set_timer_mode(int mode) {
    g_mode = mode;
    g_timer_running = 0;
    if (mode == MODE_WORK) {
        g_total_duration_s = 25 * 60;
    } else if (mode == MODE_SHORT_BREAK) {
        g_total_duration_s = 5 * 60;
    } else if (mode == MODE_LONG_BREAK) {
        g_total_duration_s = 15 * 60;
    }
    g_remaining_s = g_total_duration_s;
}

static void on_timer_finished(void) {
    g_timer_running = 0;
    if (g_mode == MODE_WORK) {
        g_cfg.pomodoros_today++;
        g_cfg.focus_minutes_total += (g_total_duration_s / 60);
        nv_save("deskhub.cfg", &g_cfg, sizeof(g_cfg));
        notify_event(1);
        nv_toast(NV_TOAST_OK, TR(T_TOAST_POMO_DONE));
        set_timer_mode(MODE_SHORT_BREAK);
    } else {
        notify_event(1);
        nv_toast(NV_TOAST_INFO, TR(T_TOAST_BREAK_DONE));
        set_timer_mode(MODE_WORK);
    }
}

static void update_timer(void) {
    if (!g_timer_running) {
        g_last_tick_ms = nv_millis();
        return;
    }

    int now = nv_millis();
    if (now - g_last_tick_ms >= 1000) {
        int elapsed = (now - g_last_tick_ms) / 1000;
        g_last_tick_ms += elapsed * 1000;
        g_remaining_s -= elapsed;
        if (g_remaining_s <= 0) {
            g_remaining_s = 0;
            on_timer_finished();
        }
    }
}

// Disegno corona radiale a 60 tacche
static void draw_radial_dial(int cx, int cy, int accent_color) {
    int elapsed = g_total_duration_s - g_remaining_s;
    int active_ticks = 0;
    if (g_total_duration_s > 0) {
        active_ticks = (elapsed * 60) / g_total_duration_s;
    }
    if (active_ticks > 60) active_ticks = 60;

    for (int i = 0; i < 60; i++) {
        int r_in = (i % 5 == 0) ? 78 : 84;
        int r_out = 96;

        int x1 = cx + (r_in * kSin60[i]) / 1000;
        int y1 = cy + (r_in * kCos60[i]) / 1000;
        int x2 = cx + (r_out * kSin60[i]) / 1000;
        int y2 = cy + (r_out * kCos60[i]) / 1000;

        int col;
        if (i < active_ticks) {
            col = accent_color;
        } else if (i == active_ticks && g_timer_running) {
            col = COLOR_TICK_ACTIVE;
        } else {
            col = COLOR_PANEL_BORDER;
        }

        nv_gfx_line(x1, y1, x2, y2, col);
    }
}

// ---- Render Schermata Principale -----------------------------------------------------------------

static void draw_header_bar(void) {
    draw_panel(0, 0, CANVAS_W, 64, COLOR_HEADER, COLOR_PANEL_BORDER);

    // Titolo con badge colorato
    nv_gfx_rect(24, 18, 6, 28, COLOR_ACCENT_WORK);
    nv_gfx_text(40, 20, "POMODORO DESK HUB", COLOR_TEXT_WHITE, 3);

    // Orologio da scrivania continuo
    int ch, cm, cs;
    get_current_time(&ch, &cm, &cs);
    char clock_str[16];
    int_to_2digits(ch, clock_str);
    clock_str[2] = ':';
    int_to_2digits(cm, clock_str + 3);
    clock_str[5] = ':';
    int_to_2digits(cs, clock_str + 6);

    int c_tw = nv_gfx_text_width(clock_str, 3);
    nv_gfx_text((CANVAS_W - c_tw) / 2, 20, clock_str, COLOR_TEXT_MUTED, 3);

    // Scostamento UTC manuale (l'ABI WASM non espone il fuso di sistema): tap per +1h, wrap a -12.
    char utc_lbl[16];
    {
        int i = 0;
        const char *u = TR(T_UTC_LABEL);
        while (*u) utc_lbl[i++] = *u++;
        utc_lbl[i++] = g_cfg.utc_offset_h < 0 ? '-' : '+';
        char os[8];
        int_to_str(g_cfg.utc_offset_h < 0 ? -g_cfg.utc_offset_h : g_cfg.utc_offset_h, os);
        char *o = os; while (*o) utc_lbl[i++] = *o++;
        utc_lbl[i] = '\0';
    }
    Rect btn_utc = { 616, 10, 100, 44 };
    draw_button(btn_utc, utc_lbl, COLOR_PANEL_BG, COLOR_TEXT_MUTED, 2);

    // Pulsanti Header: Notte, Audio a 3 stati
    Rect btn_night = { 728, 10, 110, 44 };
    Rect btn_audio = { 850, 10, 150, 44 };

    draw_button(btn_night, TR(T_NIGHT), COLOR_PANEL_BG, COLOR_TEXT_WHITE, 2);

    const char *audio_lbl = (g_cfg.audio_mode == AUDIO_VOICE) ? TR(T_AUDIO_VOICE) :
                           ((g_cfg.audio_mode == AUDIO_BEEP)  ? TR(T_AUDIO_BEEP) : TR(T_AUDIO_OFF));
    int audio_fill = (g_cfg.audio_mode != AUDIO_MUTE) ? COLOR_BUTTON_START : COLOR_PANEL_BG;
    draw_button(btn_audio, audio_lbl, audio_fill, COLOR_TEXT_WHITE, 2);
}

static void draw_pomodoro_section(void) {
    int px = 24, py = 80, pw = 476, ph = 496;
    draw_panel(px, py, pw, ph, COLOR_PANEL_BG, COLOR_PANEL_BORDER);

    // 1. Selettore Task Attuale (Pulsante largo touch)
    Rect r_task = { px + 16, py + 14, pw - 32, 34 };
    draw_panel(r_task.x, r_task.y, r_task.w, r_task.h, COLOR_SUBPANEL, COLOR_SUB_BORDER);
    nv_gfx_text(r_task.x + 12, r_task.y + 9, TR(T_TASK_LABEL), COLOR_TEXT_MUTED, 2);
    nv_gfx_text(r_task.x + 115, r_task.y + 9, task_name(g_cfg.task_idx), COLOR_ACCENT_LONG, 2);

    // 2. Pill Tabs per le Modalità
    Rect tab_w = { px + 16, py + 56, 142, 44 };
    Rect tab_s = { px + 166, py + 56, 142, 44 };
    Rect tab_l = { px + 316, py + 56, 142, 44 };

    int col_act = (g_mode == MODE_WORK) ? COLOR_ACCENT_WORK :
                  (g_mode == MODE_SHORT_BREAK ? COLOR_ACCENT_SHORT : COLOR_ACCENT_LONG);

    draw_button(tab_w, TR(T_TAB_FOCUS), (g_mode == MODE_WORK) ? COLOR_ACCENT_WORK : COLOR_SUBPANEL,
                COLOR_TEXT_WHITE, 2);
    draw_button(tab_s, TR(T_TAB_BREAK), (g_mode == MODE_SHORT_BREAK) ? COLOR_ACCENT_SHORT : COLOR_SUBPANEL,
                COLOR_TEXT_WHITE, 2);
    draw_button(tab_l, TR(T_TAB_LONG), (g_mode == MODE_LONG_BREAK) ? COLOR_ACCENT_LONG : COLOR_SUBPANEL,
                COLOR_TEXT_WHITE, 2);

    // 3. Cronometro Radiale a 60 Tacche
    int dial_cx = px + pw / 2;
    int dial_cy = py + 205;
    draw_radial_dial(dial_cx, dial_cy, col_act);

    // Tempo Digitale centrale (MM:SS)
    int mins = g_remaining_s / 60;
    int secs = g_remaining_s % 60;
    char timer_str[16];
    int_to_2digits(mins, timer_str);
    int blink = (g_timer_running && (nv_millis() % 1000 > 500));
    timer_str[2] = blink ? ' ' : ':';
    int_to_2digits(secs, timer_str + 3);

    int d_tw = nv_gfx_text_width(timer_str, 5);
    nv_gfx_text(dial_cx - d_tw / 2, dial_cy - 18, timer_str, COLOR_TEXT_WHITE, 5);

    // Sottotitolo
    const char *st_text = g_timer_running ? TR(T_ST_FOCUS) :
                          (g_remaining_s == 0 ? TR(T_ST_DONE) : TR(T_ST_BREAK));
    int st_w = nv_gfx_text_width(st_text, 1);
    nv_gfx_text(dial_cx - st_w / 2, dial_cy + 24, st_text, col_act, 1);

    // 4. Controlli Touch Principali
    Rect btn_play  = { px + 28, py + 330, 260, 76 };
    Rect btn_reset = { px + 300, py + 330, 148, 76 };

    int play_fill = g_timer_running ? COLOR_BUTTON_PAUSE : COLOR_BUTTON_START;
    const char *play_lbl = g_timer_running ? TR(T_BTN_PAUSE) : TR(T_BTN_START);
    draw_button(btn_play, play_lbl, play_fill, COLOR_TEXT_WHITE, 3);
    draw_button(btn_reset, TR(T_BTN_RESET), COLOR_SUBPANEL, COLOR_TEXT_MUTED, 3);

    // 5. Regolazioni Rapide Tempo
    Rect btn_m1 = { px + 28, py + 418, 134, 56 };
    Rect btn_p1 = { px + 172, py + 418, 134, 56 };
    Rect btn_p5 = { px + 316, py + 418, 132, 56 };

    draw_button(btn_m1, "-1 MIN", COLOR_SUBPANEL, COLOR_TEXT_WHITE, 2);
    draw_button(btn_p1, "+1 MIN", COLOR_SUBPANEL, COLOR_TEXT_WHITE, 2);
    draw_button(btn_p5, "+5 MIN", COLOR_SUBPANEL, COLOR_TEXT_WHITE, 2);
}

static void draw_calendar_and_goals_section(void) {
    int px = 524, py = 80, pw = 476, ph = 496;
    draw_panel(px, py, pw, ph, COLOR_PANEL_BG, COLOR_PANEL_BORDER);

    // 1. Calendario Header
    nv_gfx_text(px + 28, py + 20, month_name(g_cal_month), COLOR_TEXT_WHITE, 3);
    char yr_str[8];
    int_to_str(g_cal_year, yr_str);
    nv_gfx_text(px + 220, py + 20, yr_str, COLOR_TEXT_MUTED, 3);

    Rect btn_m_prev = { px + 348, py + 12, 48, 44 };
    Rect btn_m_next = { px + 404, py + 12, 48, 44 };
    draw_button(btn_m_prev, "<", COLOR_SUBPANEL, COLOR_TEXT_WHITE, 2);
    draw_button(btn_m_next, ">", COLOR_SUBPANEL, COLOR_TEXT_WHITE, 2);

    // 2. Griglia Giorni Settimana
    static const char *kDaysHeadersIT[7] = { "LUN", "MAR", "MER", "GIO", "VEN", "SAB", "DOM" };
    static const char *kDaysHeadersEN[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };
    const char **kDaysHeaders = g_lang == L_EN ? kDaysHeadersEN : kDaysHeadersIT;
    int col_w = 60;
    int grid_x = px + 28;
    int grid_y = py + 68;

    for (int d = 0; d < 7; d++) {
        int color = (d >= 5) ? COLOR_ACCENT_WORK : COLOR_TEXT_MUTED;
        nv_gfx_text(grid_x + d * col_w + 10, grid_y, kDaysHeaders[d], color, 2);
    }
    nv_gfx_line(grid_x, grid_y + 24, grid_x + 7 * col_w, grid_y + 24, COLOR_PANEL_BORDER);

    // 3. Render Giorni del Mese
    int first_dow = day_of_week_first(g_cal_year, g_cal_month);
    int total_days = days_in_month(g_cal_year, g_cal_month);
    int row = 0;
    int col = first_dow;

    int is_cur_month = (g_cal_year == g_today_year && g_cal_month == g_today_month);
    int today_num = g_today_day;

    for (int day = 1; day <= total_days; day++) {
        int cx = grid_x + col * col_w;
        int cy = grid_y + 34 + row * 34;

        if (is_cur_month && day == today_num) {
            nv_gfx_rect(cx + 4, cy - 2, 34, 24, COLOR_ACCENT_WORK);
            char num_s[4];
            int_to_str(day, num_s);
            int ntw = nv_gfx_text_width(num_s, 2);
            nv_gfx_text(cx + 21 - ntw / 2, cy + 2, num_s, COLOR_TEXT_WHITE, 2);
        } else {
            char num_s[4];
            int_to_str(day, num_s);
            int ntw = nv_gfx_text_width(num_s, 2);
            int ink = (col >= 5) ? COLOR_BUTTON_PAUSE : COLOR_TEXT_WHITE;
            nv_gfx_text(cx + 21 - ntw / 2, cy + 2, num_s, ink, 2);
        }

        col++;
        if (col == 7) {
            col = 0;
            row++;
        }
    }

    // 4. Sezione Obiettivi e Statistiche Giornaliere
    int stat_y = py + 310;
    draw_panel(px + 20, stat_y, pw - 40, 168, COLOR_SUBPANEL, COLOR_PANEL_BORDER);

    // Intestazione con pulsanti [-] e [+] per regolare il target
    nv_gfx_text(px + 36, stat_y + 14, TR(T_GOAL_TITLE), COLOR_TEXT_WHITE, 2);

    Rect btn_dec_tgt = { px + 318, stat_y + 6, 44, 36 };
    Rect btn_inc_tgt = { px + 370, stat_y + 6, 44, 36 };
    draw_button(btn_dec_tgt, "-", COLOR_HEADER, COLOR_TEXT_WHITE, 2);
    draw_button(btn_inc_tgt, "+", COLOR_HEADER, COLOR_TEXT_WHITE, 2);

    // Slot numerati delle sessioni target
    int blk_x = px + 36;
    int blk_y = stat_y + 46;
    int num_slots = g_cfg.target_pomodoros > 8 ? 8 : g_cfg.target_pomodoros;
    for (int i = 0; i < num_slots; i++) {
        int bx = blk_x + i * 50;
        int fill = (i < g_cfg.pomodoros_today) ? COLOR_ACCENT_WORK : COLOR_HEADER;
        draw_panel(bx, blk_y, 40, 28, fill, COLOR_PANEL_BORDER);
        char idx_s[4];
        int_to_str(i + 1, idx_s);
        int itw = nv_gfx_text_width(idx_s, 2);
        nv_gfx_text(bx + (40 - itw) / 2, blk_y + 7, idx_s, COLOR_TEXT_WHITE, 2);
    }

    // Tempo focus totale
    int hours = g_cfg.focus_minutes_total / 60;
    int rem_mins = g_cfg.focus_minutes_total % 60;

    char focus_summary[64];
    char hs[8], ms[8];
    int_to_str(hours, hs);
    int_to_str(rem_mins, ms);

    int idx = 0;
    const char *lbl_t = TR(T_FOCUS_PREFIX);
    while (*lbl_t) focus_summary[idx++] = *lbl_t++;
    char *p = hs; while (*p) focus_summary[idx++] = *p++;
    const char *lbl_h = TR(T_HOURS);
    while (*lbl_h) focus_summary[idx++] = *lbl_h++;
    p = ms; while (*p) focus_summary[idx++] = *p++;
    const char *lbl_m = TR(T_MINUTES);
    while (*lbl_m) focus_summary[idx++] = *lbl_m++;
    focus_summary[idx] = '\0';

    nv_gfx_text(px + 36, stat_y + 92, focus_summary, COLOR_ACCENT_SHORT, 2);

    // Percentuale progresso
    int pct = 0;
    if (g_cfg.target_pomodoros > 0) {
        pct = (g_cfg.pomodoros_today * 100) / g_cfg.target_pomodoros;
    }
    char pct_str[32];
    int_to_str(pct, pct_str);
    int plen = (int)strlen(pct_str);
    const char *pct_tail = TR(T_TARGET_SUFFIX);
    while (*pct_tail) pct_str[plen++] = *pct_tail++;
    pct_str[plen] = '\0';
    nv_gfx_text(px + 36, stat_y + 118, pct_str, COLOR_TEXT_MUTED, 1);

    Rect btn_clear = { px + 36, stat_y + 136, 120, 24 };
    draw_button(btn_clear, TR(T_CLEAR), COLOR_HEADER, COLOR_TEXT_MUTED, 1);
}

// ---- Modalità Notte (Night Stand) ----------------------------------------------------------------

static void draw_night_mode_screen(void) {
    nv_gfx_clear(COLOR_NIGHT_BG);

    int mins = g_remaining_s / 60;
    int secs = g_remaining_s % 60;
    char n_str[16];
    int_to_2digits(mins, n_str);
    int blink = (g_timer_running && (nv_millis() % 1000 > 500));
    n_str[2] = blink ? ' ' : ':';
    int_to_2digits(secs, n_str + 3);

    int n_tw = nv_gfx_text_width(n_str, 12);
    nv_gfx_text((CANVAS_W - n_tw) / 2, 160, n_str, COLOR_NIGHT_RED, 12);

    const char *n_lbl = (g_mode == MODE_WORK) ? task_name(g_cfg.task_idx) : TR(T_NIGHT_BREAK);
    int n_lw = nv_gfx_text_width(n_lbl, 2);
    nv_gfx_text((CANVAS_W - n_lw) / 2, 310, n_lbl, COLOR_NIGHT_MUTED, 2);

    int ch, cm, cs;
    get_current_time(&ch, &cm, &cs);
    char wall_s[16];
    int_to_2digits(ch, wall_s);
    wall_s[2] = ':';
    int_to_2digits(cm, wall_s + 3);

    int wall_w = nv_gfx_text_width(wall_s, 4);
    nv_gfx_text((CANVAS_W - wall_w) / 2, 420, wall_s, COLOR_NIGHT_RED, 4);

    const char *hint = TR(T_WAKE_HINT);
    int hw = nv_gfx_text_width(hint, 2);
    nv_gfx_text((CANVAS_W - hw) / 2, 530, hint, COLOR_NIGHT_MUTED, 2);
}

// ---- Modale di conferma uscita -------------------------------------------------------------------

static Rect exit_modal_rect(void) { return (Rect){ CANVAS_W / 2 - 220, 190, 440, 220 }; }
static Rect exit_btn_no_rect(void)  { Rect m = exit_modal_rect(); return (Rect){ m.x + 24, m.y + m.h - 76, (m.w - 64) / 2, 56 }; }
static Rect exit_btn_yes_rect(void) { Rect no = exit_btn_no_rect();
                                       return (Rect){ no.x + no.w + 16, no.y, no.w, no.h }; }

static void draw_confirm_exit_modal(void) {
    // Niente scrim semi-trasparente (la gfx ABI non ha alpha blending a basso costo): un
    // cartellino pieno con bordo acceso sopra la dashboard basta a segnalare il popup modale,
    // stesso trattamento del selettore citta' in meteo.
    Rect m = exit_modal_rect();
    draw_panel(m.x, m.y, m.w, m.h, COLOR_PANEL_BG, COLOR_ACCENT_WORK);

    const char *title = TR(T_EXIT_TITLE);
    int tw = nv_gfx_text_width(title, 3);
    nv_gfx_text(m.x + (m.w - tw) / 2, m.y + 26, title, COLOR_TEXT_WHITE, 3);

    const char *body = TR(T_EXIT_BODY);
    int bw = nv_gfx_text_width(body, 1);
    nv_gfx_text(m.x + (m.w - bw) / 2, m.y + 72, body, COLOR_TEXT_MUTED, 1);

    draw_button(exit_btn_no_rect(),  TR(T_EXIT_NO),  COLOR_SUBPANEL, COLOR_TEXT_WHITE, 2);
    draw_button(exit_btn_yes_rect(), TR(T_EXIT_YES), COLOR_ACCENT_WORK, COLOR_TEXT_WHITE, 2);
}

// ---- Touch Input --------------------------------------------------------------------------------

static void handle_touch_events(void) {
    int tx = 0, ty = 0;
    int is_down = nv_touch(&tx, &ty);

    if (g_prev_touch && !is_down) {
        if (g_night_mode) {
            g_night_mode = 0;
            nv_backlight(100);
            notify_event(0);
            g_prev_touch = is_down;
            return;
        }

        if (g_confirm_exit) {
            if (point_in_rect(exit_btn_yes_rect(), tx, ty)) {
                g_want_exit = 1;
            } else if (point_in_rect(exit_btn_no_rect(), tx, ty)) {
                g_confirm_exit = 0;
            }
            // un tap fuori dai due bottoni non fa nulla: resta aperta finche' non si sceglie.
            g_prev_touch = is_down;
            return;
        }

        // Header: UTC offset, Notte, Audio
        Rect btn_utc   = { 616, 10, 100, 44 };
        Rect btn_night = { 728, 10, 110, 44 };
        Rect btn_audio = { 850, 10, 150, 44 };

        if (point_in_rect(btn_utc, tx, ty)) {
            g_cfg.utc_offset_h++;
            if (g_cfg.utc_offset_h > 14) g_cfg.utc_offset_h = -12;
            nv_save("deskhub.cfg", &g_cfg, sizeof(g_cfg));
            g_prev_touch = is_down;
            return;
        }

        if (point_in_rect(btn_night, tx, ty)) {
            g_night_mode = 1;
            nv_backlight(10);
            g_prev_touch = is_down;
            return;
        } else if (point_in_rect(btn_audio, tx, ty)) {
            g_cfg.audio_mode = (g_cfg.audio_mode + 1) % 3; // VOCE -> BEEP -> MUTE
            nv_save("deskhub.cfg", &g_cfg, sizeof(g_cfg));
            notify_event(0);
            g_prev_touch = is_down;
            return;
        }

        // Selettore Task
        Rect r_task = { 40, 94, 444, 34 };
        if (point_in_rect(r_task, tx, ty)) {
            g_cfg.task_idx = (g_cfg.task_idx + 1) % TASK_COUNT;
            nv_save("deskhub.cfg", &g_cfg, sizeof(g_cfg));
            notify_event(0);
            g_prev_touch = is_down;
            return;
        }

        // Tab Pomodoro
        Rect tab_w = { 40, 136, 142, 44 };
        Rect tab_s = { 190, 136, 142, 44 };
        Rect tab_l = { 340, 136, 142, 44 };

        if (point_in_rect(tab_w, tx, ty)) {
            set_timer_mode(MODE_WORK);
            notify_event(0);
        } else if (point_in_rect(tab_s, tx, ty)) {
            set_timer_mode(MODE_SHORT_BREAK);
            notify_event(0);
        } else if (point_in_rect(tab_l, tx, ty)) {
            set_timer_mode(MODE_LONG_BREAK);
            notify_event(0);
        }

        // Controlli Timer: AVVIA/PAUSA, RESET
        Rect btn_play  = { 52, 410, 260, 76 };
        Rect btn_reset = { 324, 410, 148, 76 };

        if (point_in_rect(btn_play, tx, ty)) {
            g_timer_running = !g_timer_running;
            g_last_tick_ms = nv_millis();
            notify_event(0);
        } else if (point_in_rect(btn_reset, tx, ty)) {
            set_timer_mode(g_mode);
            notify_event(0);
        }

        // Regolazioni tempo: -1m, +1m, +5m
        Rect btn_m1 = { 52, 498, 134, 56 };
        Rect btn_p1 = { 196, 498, 134, 56 };
        Rect btn_p5 = { 340, 498, 132, 56 };

        if (point_in_rect(btn_m1, tx, ty)) {
            if (g_remaining_s > 60) g_remaining_s -= 60;
            notify_event(0);
        } else if (point_in_rect(btn_p1, tx, ty)) {
            g_remaining_s += 60;
            g_total_duration_s += 60;
            notify_event(0);
        } else if (point_in_rect(btn_p5, tx, ty)) {
            g_remaining_s += 300;
            g_total_duration_s += 300;
            notify_event(0);
        }

        // Frecce Calendario
        Rect btn_m_prev = { 524 + 348, 92, 48, 44 };
        Rect btn_m_next = { 524 + 404, 92, 48, 44 };

        if (point_in_rect(btn_m_prev, tx, ty)) {
            g_cal_month--;
            if (g_cal_month < 1) { g_cal_month = 12; g_cal_year--; }
            notify_event(0);
        } else if (point_in_rect(btn_m_next, tx, ty)) {
            g_cal_month++;
            if (g_cal_month > 12) { g_cal_month = 1; g_cal_year++; }
            notify_event(0);
        }

        // Modifica Target Giornaliero [-] e [+]
        Rect btn_dec_tgt = { 524 + 318, 80 + 310 + 6, 44, 36 };
        Rect btn_inc_tgt = { 524 + 370, 80 + 310 + 6, 44, 36 };

        if (point_in_rect(btn_dec_tgt, tx, ty)) {
            if (g_cfg.target_pomodoros > 2) g_cfg.target_pomodoros--;
            nv_save("deskhub.cfg", &g_cfg, sizeof(g_cfg));
            notify_event(0);
        } else if (point_in_rect(btn_inc_tgt, tx, ty)) {
            if (g_cfg.target_pomodoros < 12) g_cfg.target_pomodoros++;
            nv_save("deskhub.cfg", &g_cfg, sizeof(g_cfg));
            notify_event(0);
        }

        // Azzera Statistiche
        Rect btn_clear = { 524 + 36, 80 + 310 + 136, 120, 24 };
        if (point_in_rect(btn_clear, tx, ty)) {
            g_cfg.pomodoros_today = 0;
            g_cfg.focus_minutes_total = 0;
            nv_save("deskhub.cfg", &g_cfg, sizeof(g_cfg));
            notify_event(0);
            nv_toast(NV_TOAST_INFO, TR(T_TOAST_CLEARED));
        }
    }

    g_prev_touch = is_down;
}

// ---- Entry Point --------------------------------------------------------------------------------

NV_EXPORT("run")
void run(void) {
    detect_lang();

    if (nv_load("deskhub.cfg", &g_cfg, sizeof(g_cfg)) != sizeof(g_cfg)) {
        g_cfg.pomodoros_today = 0;
        g_cfg.focus_minutes_total = 0;
        g_cfg.audio_mode = AUDIO_VOICE;
        g_cfg.target_pomodoros = 8;
        g_cfg.task_idx = 0;
        g_cfg.utc_offset_h = 1; // CET; l'utente lo regola dall'header se e' ora legale (+2)
    }

    set_timer_mode(MODE_WORK);
    get_current_date(&g_today_year, &g_today_month, &g_today_day);
    g_cal_year = g_today_year;
    g_cal_month = g_today_month;

    int last_date_check_ms = nv_millis();
    int last_draw_ms = -1000;

    while (nv_gfx_present()) {
        // Ricalcola la data odierna una volta al minuto (persa solo se l'app resta aperta oltre
        // mezzanotte) senza pagare civil_from_days ad ogni frame.
        int now_ms = nv_millis();
        if (now_ms - last_date_check_ms > 60000) {
            last_date_check_ms = now_ms;
            get_current_date(&g_today_year, &g_today_month, &g_today_day);
        }

        if (nv_gfx_back()) {
            if (g_night_mode) {
                g_night_mode = 0;
                nv_backlight(100);
            } else if (g_confirm_exit) {
                g_confirm_exit = 0;   // il Back stesso funge da "Annulla"
            } else {
                g_confirm_exit = 1;   // primo Back: chiede conferma invece di uscire subito
            }
        }

        if (g_want_exit) break;

        handle_touch_events();
        update_timer();

        // Il pannello e' pesante (corona radiale a 60 linee + calendario + testo): ridisegnare
        // l'intera scena ogni 25 ms (40 Hz) costa host-call per nulla, il contenuto cambia solo
        // al secondo (o due volte al secondo per il lampeggio dei due punti). 8 Hz e' fluido
        // per un timer da scrivania e taglia il costo di rendering di 5x.
        if (now_ms - last_draw_ms >= 125) {
            last_draw_ms = now_ms;
            if (g_night_mode) {
                draw_night_mode_screen();
            } else {
                nv_gfx_clear(COLOR_BG);
                draw_header_bar();
                draw_pomodoro_section();
                draw_calendar_and_goals_section();
                if (g_confirm_exit) draw_confirm_exit_modal();
            }
        }

        nv_sleep_ms(25);
    }

    nv_backlight(100);
}
