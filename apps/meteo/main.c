// main.c — NucleoOS Meteo App (WASM host ABI v7).
// Standalone weather app fetching real-time data from Open-Meteo with geolocation,
// interactive city selection, settings persistence, and native vector weather graphics.
#include "nucleo_sdk.h"

// ---- Lingua (segue la lingua di sistema, non solo IT) ---------------------------------------------
enum { L_IT = 0, L_EN = 1, L_COUNT };
static int g_lang = L_IT;

enum {
    T_TITLE = 0, T_BTN_CITY, T_STATUS_UPDATING, T_STATUS_ONLINE, T_STATUS_OFFLINE,
    T_UPDATED_PREFIX, T_AGO_SUFFIX, T_LOADING, T_FEELS_LIKE, T_HUMIDITY, T_WIND,
    T_PROVIDER, T_IMMUTABLE, T_FORECAST_TITLE, T_DAY_TODAY, T_DAY_TOMORROW,
    T_DAY_AFTER, T_DAY_PLUS3, T_MODAL_TITLE, T_TOAST_OK, T_TOAST_OFFLINE, T_STATUS_NODATA,
    T_NO_DATA, T_NO_DATA_HINT, T_TOAST_NODATA, T_SAVED_DATA, T_COUNT
};

static const char *kStrings[T_COUNT][L_COUNT] = {
    [T_TITLE]           = { "NUCLEO METEO",             "NUCLEO WEATHER" },
    [T_BTN_CITY]        = { "CITTA'",                    "CITY" },
    [T_STATUS_UPDATING] = { "AGGIORNAMENTO...",           "UPDATING..." },
    [T_STATUS_ONLINE]   = { "IN LINEA (OPEN-METEO)",      "ONLINE (OPEN-METEO)" },
    [T_STATUS_OFFLINE]  = { "OFFLINE (DATI SALVATI)",     "OFFLINE (SAVED DATA)" },
    [T_STATUS_NODATA]   = { "OFFLINE",                    "OFFLINE" },
    [T_UPDATED_PREFIX]  = { "AGGIORNATO ",                "UPDATED " },
    [T_AGO_SUFFIX]      = { " FA",                        " AGO" },
    [T_LOADING]         = { "CARICAMENTO METEO...",       "LOADING WEATHER..." },
    [T_FEELS_LIKE]      = { "PERCEPITA",                  "FEELS LIKE" },
    [T_HUMIDITY]        = { "UMIDITA'",                   "HUMIDITY" },
    [T_WIND]            = { "VENTO",                      "WIND" },
    [T_PROVIDER]        = { "PROVIDER API",                "API PROVIDER" },
    [T_IMMUTABLE]       = { "IMMUTABILE / NO KEY",         "IMMUTABLE / NO KEY" },
    [T_FORECAST_TITLE]  = { "PREVISIONI PROSSIMI GIORNI",  "UPCOMING FORECAST" },
    [T_DAY_TODAY]       = { "OGGI",                        "TODAY" },
    [T_DAY_TOMORROW]    = { "DOMANI",                      "TOMORROW" },
    [T_DAY_AFTER]       = { "DOPODOMANI",                  "DAY AFTER" },
    [T_DAY_PLUS3]       = { "+3 GIORNI",                   "+3 DAYS" },
    [T_MODAL_TITLE]     = { "SELEZIONA CITTA' O GEOLOCALIZZAZIONE", "SELECT CITY OR GEOLOCATION" },
    [T_TOAST_OK]        = { "Dati meteo aggiornati",       "Weather data updated" },
    [T_TOAST_OFFLINE]   = { "Meteo offline: ultimi dati salvati", "Weather offline: last saved data" },
    [T_NO_DATA]         = { "NESSUN DATO",                 "NO DATA" },
    [T_NO_DATA_HINT]    = { "CONTROLLA IL WI-FI E TOCCA SYNC", "CHECK WI-FI AND TAP SYNC" },
    [T_TOAST_NODATA]    = { "Nessun dato: controlla il Wi-Fi", "No data: check Wi-Fi" },
    [T_SAVED_DATA]      = { "DATI SALVATI",                "SAVED DATA" },
};

#define TR(id) (kStrings[id][g_lang])

static void detect_lang(void) {
    char buf[8] = { 0 };
    nv_lang(buf, sizeof buf);
    g_lang = (buf[0] == 'i' && buf[1] == 't') ? L_IT : L_EN;
}

// ---- Palette colori (RGB565) --------------------------------------------------------------------
#define COLOR_BG            NV_RGB(10, 18, 36)      // Deep night navy
#define COLOR_CARD_BG       NV_RGB(20, 36, 68)      // Panel blue
#define COLOR_CARD_BORDER   NV_RGB(38, 64, 114)     // Lighter blue border
#define COLOR_HEADER        NV_RGB(15, 28, 54)      // Header bar
#define COLOR_TEXT_WHITE    NV_RGB(255, 255, 255)   // Primary ink
#define COLOR_TEXT_MUTED    NV_RGB(140, 175, 220)   // Secondary ink
#define COLOR_ACCENT_BLUE   NV_RGB(41, 128, 185)    // Active / Button
#define COLOR_ACCENT_HOVER  NV_RGB(52, 152, 219)    // Highlighted button
#define COLOR_SUN_GOLD      NV_RGB(255, 210, 30)    // Sunny yellow
#define COLOR_CLOUD_WHITE   NV_RGB(225, 235, 245)   // Fluffy clouds
#define COLOR_CLOUD_DARK    NV_RGB(130, 145, 170)   // Storm clouds
#define COLOR_RAIN_BLUE     NV_RGB(70, 170, 255)    // Raindrops
#define COLOR_LIGHTNING     NV_RGB(255, 240, 60)    // Thunder bolt
#define COLOR_SNOW_CYAN     NV_RGB(210, 245, 255)   // Snow
#define COLOR_STATUS_OK     NV_RGB(46, 204, 113)    // Green live status
#define COLOR_STATUS_WARN   NV_RGB(241, 196, 15)    // Yellow warning/refreshing
#define COLOR_STATUS_ERR    NV_RGB(231, 76, 60)     // Red offline

// Dimensioni schermo e layout
#define CANVAS_W 1024
#define CANVAS_H 600

#define CITY_COUNT 8
#define FORECAST_DAYS 4
#define HTTP_BUF_SIZE 4096
#define NAME_MAX_LEN 32

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    const char name[NAME_MAX_LEN];
    const char name_en[NAME_MAX_LEN];
    const char country[8];
    int lat_x1000;   // Latitudine * 1000 per evitare dipendenze float su string
    int lon_x1000;   // Longitudine * 1000
    int is_auto;     // 1 per auto-IP
} CityInfo;

static const CityInfo kCities[CITY_COUNT] = {
    { "AUTO (IP)", "AUTO (IP)", "LOC",       0,       0, 1 },
    { "ROMA",      "ROME",      "IT",    41892,   12511, 0 },
    { "MILANO",    "MILAN",     "IT",    45464,    9190, 0 },
    { "NAPOLI",    "NAPLES",    "IT",    40852,   14268, 0 },
    { "LONDRA",    "LONDON",    "GB",    51507,    -128, 0 },
    { "PARIGI",    "PARIS",     "FR",    48857,    2352, 0 },
    { "NEW YORK",  "NEW YORK",  "US",    40713,  -74006, 0 },
    { "TOKYO",     "TOKYO",     "JP",    35689,  139692, 0 }
};

static const char *city_display_name(int i) {
    return g_lang == L_EN ? kCities[i].name_en : kCities[i].name;
}

typedef struct {
    int valid;
    int current_temp;     // gradi C interi
    int apparent_temp;
    int humidity;
    int wind_speed;
    int weather_code;
    int is_day;
    int daily_code[FORECAST_DAYS];
    int daily_max[FORECAST_DAYS];
    int daily_min[FORECAST_DAYS];
    int daily_mday[FORECAST_DAYS];   // giorno del mese di ogni previsione (da "daily.time")
    int daily_mon[FORECAST_DAYS];
    int utc_offset_s;                // fuso della località (per contare i giorni trascorsi)
} WeatherData;

// Ultimo dato buono, uno per città, nella cartella dell'app su SD (nv_save "cacheN.bin").
#define CACHE_MAGIC 0x4D455432  // "MET2"
typedef struct {
    int magic;
    int city_idx;
    int lat_x1000, lon_x1000;
    char city_name[NAME_MAX_LEN];
    int64_t fetched_unix;            // 0 se l'orologio non era sincronizzato
    WeatherData w;
} MeteoCache;

typedef struct {
    int city_idx;
    int use_fahrenheit;
} MeteoConfig;

// Stato applicazione
static MeteoConfig g_config;
static WeatherData g_weather;
static char g_active_city_name[NAME_MAX_LEN] = "ROMA";
static int g_active_lat_x1000 = 41892;
static int g_active_lon_x1000 = 12511;

static int g_is_fetching = 0;
static int g_fetch_status = 0; // 0=ok, 1=fetching, -1=error
static int g_last_fetch_ms = 0;   // ultimo TENTATIVO (per l'auto-refresh)
static int g_data_ms = 0;          // quando g_weather è stato scaricato in questa sessione (0 = da cache)
static int64_t g_data_unix = 0;    // epoch UTC del dato mostrato (0 = sconosciuto)
static int g_data_city = -1;       // città a cui appartiene g_weather
static int g_show_city_modal = 0;
static int g_prev_touch = 0;

static char g_http_buf[HTTP_BUF_SIZE];

// ---- Funzioni ausiliarie per stringhe e numeri ---------------------------------------------------


static void str_copy(char *dst, const char *src, int maxlen) {
    int i = 0;
    while (src[i] && i < maxlen - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int str_find_substr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return -1;
    int hlen = (int)strlen(haystack);
    int nlen = (int)strlen(needle);
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (haystack[i + j] != needle[j]) { match = 0; break; }
        }
        if (match) return i;
    }
    return -1;
}

static int parse_int(const char *p, int *out_val) {
    while (*p == ' ' || *p == ':' || *p == '"') p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }
    int val = 0;
    int found = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
        found = 1;
    }
    if (!found) return 0;
    *out_val = val * sign;
    return 1;
}

// Numero decimale JSON -> valore * 1000, arrotondato (es. "41.8919" -> 41892).
static int parse_fixed3(const char *p, int *out_x1000) {
    while (*p == ' ' || *p == ':' || *p == '"') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }
    if (*p < '0' || *p > '9') return 0;
    int ip = 0;
    while (*p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); p++; }
    int frac = 0, digits = 0, round_up = 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (digits < 3) { frac = frac * 10 + (*p - '0'); digits++; }
            else if (digits == 3) { round_up = (*p >= '5'); digits++; }
            p++;
        }
    }
    while (digits < 3) { frac *= 10; digits++; }
    int v = ip * 1000 + frac + round_up;
    *out_x1000 = neg ? -v : v;
    return 1;
}

// Come parse_int ma arrotonda all'intero più vicino invece di troncare (21.7 -> 22).
static int parse_round(const char *p, int *out_val) {
    int v;
    if (!parse_fixed3(p, &v)) return 0;
    *out_val = v >= 0 ? (v + 500) / 1000 : -((-v + 500) / 1000);
    return 1;
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

// valore*1000 -> "41.892" / "-0.128" (3 decimali, nessuna perdita di precisione).
static void fixed3_to_str(int v, char *buf) {
    int i = 0;
    if (v < 0) { buf[i++] = '-'; v = -v; }
    int_to_str(v / 1000, buf + i);
    while (buf[i]) i++;
    int f = v % 1000;
    buf[i++] = '.';
    buf[i++] = (char)('0' + f / 100);
    buf[i++] = (char)('0' + (f / 10) % 10);
    buf[i++] = (char)('0' + f % 10);
    buf[i] = '\0';
}

static int to_unit(int temp_c) {
    if (g_config.use_fahrenheit) {
        return (temp_c * 9 / 5) + 32;
    }
    return temp_c;
}

static const char *unit_symbol(void) {
    return g_config.use_fahrenheit ? "F" : "C";
}

static int point_in_rect(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// ---- Descrizione codici WMO Open-Meteo -----------------------------------------------------------

static const char *weather_desc(int code) {
    if (g_lang == L_EN) {
        switch (code) {
            case 0: return "CLEAR SKY";
            case 1: return "MOSTLY CLEAR";
            case 2: return "PARTLY CLOUDY";
            case 3: return "OVERCAST";
            case 45: case 48: return "FOG";
            case 51: case 53: case 55: return "DRIZZLE";
            case 61: case 63: case 65: return "RAIN";
            case 66: case 67: return "FREEZING RAIN";
            case 71: case 73: case 75: case 77: return "SNOW";
            case 80: case 81: case 82: return "SHOWERS";
            case 85: case 86: return "SNOW SHOWERS";
            case 95: case 96: case 99: return "THUNDERSTORM";
            default: return "VARIABLE";
        }
    }
    switch (code) {
        case 0: return "CIELO SERENO";
        case 1: return "PREVALENZA SERENO";
        case 2: return "PARZIALMENTE NUVOLOSO";
        case 3: return "COPERTO / NUBI";
        case 45: case 48: return "NEBBIA / FOSCHIA";
        case 51: case 53: case 55: return "PIOGGERELLA";
        case 61: case 63: case 65: return "PIOGGIA";
        case 66: case 67: return "PIOGGIA GELATA";
        case 71: case 73: case 75: case 77: return "NEVE";
        case 80: case 81: case 82: return "ROVESCI";
        case 85: case 86: return "TEMPESTA DI NEVE";
        case 95: case 96: case 99: return "TEMPORALE";
        default: return "VARIABILE";
    }
}

// ---- Parsing della risposta Open-Meteo -----------------------------------------------------------

// Legge un array numerico "key":[a,b,...] (FORECAST_DAYS valori) arrotondando.
static int parse_daily_array(const char *json, const char *key, int *out) {
    int idx = str_find_substr(json, key);
    if (idx < 0) return 0;
    const char *p = json + idx + (int)strlen(key);
    for (int d = 0; d < FORECAST_DAYS; d++) {
        while (*p == ' ' || *p == ',') p++;
        if (!parse_round(p, &out[d])) return 0;
        while (*p && *p != ',' && *p != ']') p++;
    }
    return 1;
}

// Riempie *w dalla risposta Open-Meteo. Ritorna 1 solo se i campi essenziali ci sono: una
// risposta tronca o un errore non devono mai diventare "dati meteo".
static int parse_open_meteo_json(const char *json, WeatherData *w) {
    int idx;
    memset(w, 0, sizeof *w);

    // Il blocco "current_units" precede "current" e ripete GLI STESSI nomi di campo con
    // valori stringa (es. "temperature_2m":"C deg"): cercare quei nomi nell'intero payload
    // trova prima quel blocco e legge sempre 0. Ancoriamo la ricerca dentro "current":{...}.
    idx = str_find_substr(json, "\"current\":{");
    if (idx < 0) return 0;
    const char *cur = json + idx + 11;

    idx = str_find_substr(cur, "\"temperature_2m\":");
    if (idx < 0 || !parse_round(cur + idx + 17, &w->current_temp)) return 0;
    idx = str_find_substr(cur, "\"weather_code\":");
    if (idx < 0 || !parse_round(cur + idx + 15, &w->weather_code)) return 0;

    idx = str_find_substr(cur, "\"apparent_temperature\":");
    if (idx >= 0) parse_round(cur + idx + 23, &w->apparent_temp);
    idx = str_find_substr(cur, "\"relative_humidity_2m\":");
    if (idx >= 0) parse_round(cur + idx + 23, &w->humidity);
    idx = str_find_substr(cur, "\"wind_speed_10m\":");
    if (idx >= 0) parse_round(cur + idx + 17, &w->wind_speed);
    w->is_day = 1;
    idx = str_find_substr(cur, "\"is_day\":");
    if (idx >= 0) parse_round(cur + idx + 9, &w->is_day);

    idx = str_find_substr(json, "\"utc_offset_seconds\":");
    if (idx >= 0) parse_int(json + idx + 21, &w->utc_offset_s);

    if (!parse_daily_array(json, "\"weather_code\":[", w->daily_code)) return 0;
    if (!parse_daily_array(json, "\"temperature_2m_max\":[", w->daily_max)) return 0;
    if (!parse_daily_array(json, "\"temperature_2m_min\":[", w->daily_min)) return 0;

    // Date delle previsioni: "daily":{"time":["2026-09-30",...]}
    idx = str_find_substr(json, "\"daily\":{");
    if (idx >= 0) {
        const char *d0 = json + idx;
        idx = str_find_substr(d0, "\"time\":[");
        if (idx >= 0) {
            const char *p = d0 + idx + 8;
            for (int d = 0; d < FORECAST_DAYS; d++) {
                while (*p == ' ' || *p == ',' || *p == '"') p++;
                if (p[0] && p[1] && p[2] && p[3] && p[4] == '-' && p[5] && p[6] &&
                    p[7] == '-' && p[8] && p[9]) {   // YYYY-MM-DD
                    w->daily_mon[d] = (p[5] - '0') * 10 + (p[6] - '0');
                    w->daily_mday[d] = (p[8] - '0') * 10 + (p[9] - '0');
                }
                while (*p && *p != ',' && *p != ']') p++;
            }
        }
    }

    w->valid = 1;
    return 1;
}

// ---- Cache su SD dell'ultimo dato buono -----------------------------------------------------------

static int64_t clock_unix(void) {
    int64_t t = nv_time_unix();
    return t > 1600000000LL ? t : 0;   // orologio non sincronizzato -> sconosciuto
}

static void cache_name(int city_idx, char *out) {
    const char *b = "cache";
    int i = 0;
    while (*b) out[i++] = *b++;
    out[i++] = (char)('0' + city_idx);
    out[i++] = '.'; out[i++] = 'b'; out[i++] = 'i'; out[i++] = 'n';
    out[i] = '\0';
}

static MeteoCache g_cache_io;

static void cache_save(void) {
    char name[16];
    cache_name(g_config.city_idx, name);
    memset(&g_cache_io, 0, sizeof g_cache_io);
    g_cache_io.magic = CACHE_MAGIC;
    g_cache_io.city_idx = g_config.city_idx;
    g_cache_io.lat_x1000 = g_active_lat_x1000;
    g_cache_io.lon_x1000 = g_active_lon_x1000;
    str_copy(g_cache_io.city_name, g_active_city_name, NAME_MAX_LEN);
    g_cache_io.fetched_unix = g_data_unix;
    g_cache_io.w = g_weather;
    nv_save(name, &g_cache_io, sizeof g_cache_io);
}

// Carica l'ultimo dato buono della città selezionata. 1 = trovato (g_weather, nome e coordinate
// aggiornati), 0 = nessuna cache valida (niente viene toccato).
static int cache_load(void) {
    char name[16];
    cache_name(g_config.city_idx, name);
    if (nv_load(name, &g_cache_io, sizeof g_cache_io) != (int)sizeof g_cache_io) return 0;
    if (g_cache_io.magic != CACHE_MAGIC || g_cache_io.city_idx != g_config.city_idx ||
        !g_cache_io.w.valid) return 0;
    g_weather = g_cache_io.w;
    g_data_unix = g_cache_io.fetched_unix;
    g_data_ms = 0;
    g_data_city = g_config.city_idx;
    g_cache_io.city_name[NAME_MAX_LEN - 1] = '\0';
    str_copy(g_active_city_name, g_cache_io.city_name, NAME_MAX_LEN);
    g_active_lat_x1000 = g_cache_io.lat_x1000;
    g_active_lon_x1000 = g_cache_io.lon_x1000;
    return 1;
}

// Geolocation via ip-api. Aggiorna nome/coordinate SOLO se la risposta ha lat e lon validi.
static int fetch_geolocation(void) {
    const char *geo_url = "http://ip-api.com/json/?fields=status,lat,lon,city,countryCode";
    int res = nv_http_get(geo_url, g_http_buf, sizeof(g_http_buf));
    if (res <= 0) return 0;

    int lat = 0, lon = 0;
    int idx = str_find_substr(g_http_buf, "\"lat\":");
    if (idx < 0 || !parse_fixed3(g_http_buf + idx + 6, &lat)) return 0;
    idx = str_find_substr(g_http_buf, "\"lon\":");
    if (idx < 0 || !parse_fixed3(g_http_buf + idx + 6, &lon)) return 0;
    g_active_lat_x1000 = lat;
    g_active_lon_x1000 = lon;

    idx = str_find_substr(g_http_buf, "\"city\":\"");
    if (idx >= 0) {
        const char *start = g_http_buf + idx + 8;
        int len = 0;
        while (start[len] && start[len] != '"' && len < NAME_MAX_LEN - 1) {
            g_active_city_name[len] = start[len];
            len++;
        }
        g_active_city_name[len] = '\0';
    } else {
        str_copy(g_active_city_name, city_display_name(0), NAME_MAX_LEN);
    }
    return 1;
}

static void draw_main_screen(void);
static void draw_city_modal(void);

// Disegna e mostra subito un frame "in aggiornamento" prima della chiamata di rete bloccante,
// altrimenti lo schermo resta congelato senza feedback finché l'HTTP non risponde.
static void present_fetching_frame(void) {
    draw_main_screen();
    if (g_show_city_modal) draw_city_modal();
    nv_gfx_present();
}

static WeatherData g_parsed;

// Fetch fallito: mai numeri inventati. Si tiene il dato reale già a schermo se è della città
// selezionata, altrimenti l'ultimo dato salvato su SD, altrimenti lo stato vuoto "nessun dato".
static void fetch_failed(void) {
    g_fetch_status = -1;
    if (!(g_weather.valid && g_data_city == g_config.city_idx) && !cache_load()) {
        memset(&g_weather, 0, sizeof g_weather);
        g_data_city = -1;
        g_data_ms = 0;
        g_data_unix = 0;
        if (!kCities[g_config.city_idx].is_auto)
            str_copy(g_active_city_name, city_display_name(g_config.city_idx), NAME_MAX_LEN);
    }
    nv_toast(NV_TOAST_WARN, g_weather.valid ? TR(T_TOAST_OFFLINE) : TR(T_TOAST_NODATA));
}

static void trigger_fetch(void) {
    g_is_fetching = 1;
    g_fetch_status = 1;
    present_fetching_frame();

    // Se la città è impostata su Auto, proviamo la geolocalizzazione. Senza una posizione AUTO
    // (nuova o già nota) non si chiede il meteo di coordinate a caso: si considera fallito.
    int have_pos = 1;
    if (kCities[g_config.city_idx].is_auto) {
        if (!fetch_geolocation())
            have_pos = (g_weather.valid && g_data_city == g_config.city_idx) || cache_load();
    } else {
        str_copy(g_active_city_name, city_display_name(g_config.city_idx), NAME_MAX_LEN);
        g_active_lat_x1000 = kCities[g_config.city_idx].lat_x1000;
        g_active_lon_x1000 = kCities[g_config.city_idx].lon_x1000;
    }

    int ok = 0;
    if (have_pos) {
        char url[320];
        char lat_str[16];
        char lon_str[16];
        fixed3_to_str(g_active_lat_x1000, lat_str);
        fixed3_to_str(g_active_lon_x1000, lon_str);

        // Costruiamo URL per Open-Meteo
        char *u = url;
        const char *base = "http://api.open-meteo.com/v1/forecast?latitude=";
        while (*base) *u++ = *base++;
        char *s = lat_str; while (*s) *u++ = *s++;
        const char *p1 = "&longitude="; while (*p1) *u++ = *p1++;
        s = lon_str; while (*s) *u++ = *s++;
        const char *tail = "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
                           "is_day,precipitation,weather_code,wind_speed_10m"
                           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                           "&timezone=auto&forecast_days=4";
        while (*tail) *u++ = *tail++;
        *u = '\0';

        int res = nv_http_get(url, g_http_buf, sizeof(g_http_buf));
        ok = res > 0 && parse_open_meteo_json(g_http_buf, &g_parsed);
    }

    if (ok) {
        g_weather = g_parsed;
        g_data_city = g_config.city_idx;
        g_data_ms = nv_millis();
        if (g_data_ms == 0) g_data_ms = 1;
        g_data_unix = clock_unix();
        g_fetch_status = 0;
        cache_save();
        nv_toast(NV_TOAST_OK, TR(T_TOAST_OK));
    } else {
        fetch_failed();
    }

    g_last_fetch_ms = nv_millis();
    g_is_fetching = 0;
}

// ---- Disegno grafica meteo vettoriale ------------------------------------------------------------

static void draw_weather_icon(int cx, int cy, int code, int is_day, int scale) {
    int r = 16 * scale;
    // Sole
    if (code == 0 || code == 1) {
        if (!is_day && code == 0) {
            // Luna
            nv_gfx_circle(cx, cy, r, COLOR_CLOUD_WHITE);
            nv_gfx_circle(cx + r / 2, cy - r / 4, r * 4 / 5, COLOR_BG);
            return;
        }
        nv_gfx_circle(cx, cy, r, COLOR_SUN_GOLD);
        // 8 raggi solari
        int ray_len = 8 * scale;
        nv_gfx_line(cx, cy - r - 2, cx, cy - r - ray_len, COLOR_SUN_GOLD);
        nv_gfx_line(cx, cy + r + 2, cx, cy + r + ray_len, COLOR_SUN_GOLD);
        nv_gfx_line(cx - r - 2, cy, cx - r - ray_len, cy, COLOR_SUN_GOLD);
        nv_gfx_line(cx + r + 2, cy, cx + r + ray_len, cy, COLOR_SUN_GOLD);
        nv_gfx_line(cx - r * 7 / 10, cy - r * 7 / 10, cx - (r + ray_len) * 7 / 10,
                    cy - (r + ray_len) * 7 / 10, COLOR_SUN_GOLD);
        nv_gfx_line(cx + r * 7 / 10, cy - r * 7 / 10, cx + (r + ray_len) * 7 / 10,
                    cy - (r + ray_len) * 7 / 10, COLOR_SUN_GOLD);
        nv_gfx_line(cx - r * 7 / 10, cy + r * 7 / 10, cx - (r + ray_len) * 7 / 10,
                    cy + (r + ray_len) * 7 / 10, COLOR_SUN_GOLD);
        nv_gfx_line(cx + r * 7 / 10, cy + r * 7 / 10, cx + (r + ray_len) * 7 / 10,
                    cy + (r + ray_len) * 7 / 10, COLOR_SUN_GOLD);
        return;
    }

    // Parzialmente nuvoloso
    if (code == 2) {
        nv_gfx_circle(cx - 10 * scale, cy - 8 * scale, r * 3 / 4, COLOR_SUN_GOLD);
    }

    // Nuvola base
    int ccolor = (code >= 95) ? COLOR_CLOUD_DARK : COLOR_CLOUD_WHITE;
    nv_gfx_circle(cx - 8 * scale, cy, 10 * scale, ccolor);
    nv_gfx_circle(cx + 8 * scale, cy, 8 * scale, ccolor);
    nv_gfx_circle(cx, cy - 6 * scale, 12 * scale, ccolor);
    nv_gfx_rect(cx - 8 * scale, cy, 16 * scale, 8 * scale, ccolor);

    // Dettagli precipitazioni
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        // Pioggia: gocce azzurre
        for (int i = -10; i <= 10; i += 10) {
            nv_gfx_line(cx + i * scale, cy + 10 * scale,
                        cx + (i - 4) * scale, cy + 18 * scale, COLOR_RAIN_BLUE);
        }
    } else if (code >= 95) {
        // Fulmine giallo a saetta
        nv_gfx_line(cx, cy + 8 * scale, cx - 4 * scale, cy + 16 * scale, COLOR_LIGHTNING);
        nv_gfx_line(cx - 4 * scale, cy + 16 * scale, cx + 2 * scale, cy + 16 * scale,
                    COLOR_LIGHTNING);
        nv_gfx_line(cx + 2 * scale, cy + 16 * scale, cx - 2 * scale, cy + 24 * scale,
                    COLOR_LIGHTNING);
    } else if (code >= 71 && code <= 77) {
        // Neve
        nv_gfx_circle(cx - 6 * scale, cy + 14 * scale, 2 * scale, COLOR_SNOW_CYAN);
        nv_gfx_circle(cx + 6 * scale, cy + 14 * scale, 2 * scale, COLOR_SNOW_CYAN);
    }
}

// Disegna un pannello arrotondato/bordato
static void draw_card(int x, int y, int w, int h, int fill, int border) {
    nv_gfx_rect(x, y, w, h, fill);
    nv_gfx_line(x, y, x + w, y, border);
    nv_gfx_line(x, y + h, x + w, y + h, border);
    nv_gfx_line(x, y, x, y + h, border);
    nv_gfx_line(x + w, y, x + w, y + h, border);
}

// Disegna un pulsante interattivo
static void draw_button(Rect r, const char *text, int is_active) {
    int bg = is_active ? COLOR_ACCENT_HOVER : COLOR_ACCENT_BLUE;
    draw_card(r.x, r.y, r.w, r.h, bg, COLOR_CARD_BORDER);
    int tw = nv_gfx_text_width(text, 2);
    nv_gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - 14) / 2, text, COLOR_TEXT_WHITE, 2);
}

// ---- Schermata Principale ------------------------------------------------------------------------

// Età in secondi del dato mostrato, -1 se non si può sapere (salvato con orologio non sincronizzato).
static int data_age_s(void) {
    if (g_data_ms > 0) return (nv_millis() - g_data_ms) / 1000;
    int64_t now = clock_unix();
    if (g_data_unix > 0 && now >= g_data_unix) {
        int64_t age = now - g_data_unix;
        return age > 0x7fffffff ? 0x7fffffff : (int)age;
    }
    return -1;
}

// Giorni di calendario (nel fuso della località) passati dal download: 0 = scaricato oggi.
// -1 = sconosciuto.
static int data_day_shift(void) {
    int64_t now = clock_unix();
    if (g_data_unix > 0 && now > 0) {
        int64_t off = g_weather.utc_offset_s;
        int64_t k = (now + off) / 86400 - (g_data_unix + off) / 86400;
        return k < 0 ? 0 : (int)k;
    }
    return g_data_ms > 0 ? 0 : -1;
}

static void draw_main_screen(void) {
    // Sfondo completo
    nv_gfx_clear(COLOR_BG);

    // 1. Header superiore
    draw_card(0, 0, CANVAS_W, 64, COLOR_HEADER, COLOR_CARD_BORDER);
    nv_gfx_text(24, 20, TR(T_TITLE), COLOR_TEXT_WHITE, 3);

    // Nome città attiva
    char loc_title[64];
    loc_title[0] = '[';
    loc_title[1] = ' ';
    int li = 2;
    const char *cn = g_active_city_name;
    while (*cn && li < 48) loc_title[li++] = *cn++;
    loc_title[li++] = ' ';
    loc_title[li++] = ']';
    loc_title[li] = '\0';
    nv_gfx_text(300, 24, loc_title, COLOR_TEXT_MUTED, 2);

    // Indicatore status
    if (g_fetch_status == 1) {
        nv_gfx_circle(520, 32, 6, COLOR_STATUS_WARN);
        nv_gfx_text(535, 26, TR(T_STATUS_UPDATING), COLOR_STATUS_WARN, 1);
    } else if (g_fetch_status == 0) {
        nv_gfx_circle(520, 32, 6, COLOR_STATUS_OK);
        nv_gfx_text(535, 26, TR(T_STATUS_ONLINE), COLOR_STATUS_OK, 1);
    } else {
        nv_gfx_circle(520, 32, 6, COLOR_STATUS_ERR);
        nv_gfx_text(535, 26, TR(g_weather.valid ? T_STATUS_OFFLINE : T_STATUS_NODATA),
                    COLOR_STATUS_ERR, 1);
    }

    // Freschezza del dato mostrato: "aggiornato Xm fa" (anche per i dati salvati su SD).
    if (g_weather.valid) {
        int age_s = data_age_s();
        char age_str[40];
        if (age_s < 0) {
            str_copy(age_str, TR(T_SAVED_DATA), sizeof age_str);
        } else {
            int ai = 0;
            const char *pre = TR(T_UPDATED_PREFIX);
            while (*pre) age_str[ai++] = *pre++;
            char num[16];
            char unit;
            if (age_s < 60)         { int_to_str(age_s, num); unit = 'S'; }
            else if (age_s < 3600)  { int_to_str(age_s / 60, num); unit = 'M'; }
            else if (age_s < 86400) { int_to_str(age_s / 3600, num); unit = 'H'; }
            else { int_to_str(age_s / 86400, num); unit = g_lang == L_IT ? 'G' : 'D'; }
            char *n = num; while (*n) age_str[ai++] = *n++;
            age_str[ai++] = unit;
            const char *post = TR(T_AGO_SUFFIX);
            while (*post) age_str[ai++] = *post++;
            age_str[ai] = '\0';
        }
        nv_gfx_text(535, 42, age_str, g_fetch_status == -1 ? COLOR_STATUS_ERR : COLOR_TEXT_MUTED, 1);
    }

    // Bottoni Header
    Rect btn_citta = { 700, 12, 130, 40 };
    Rect btn_refresh = { 840, 12, 110, 40 };
    Rect btn_unit = { 960, 12, 50, 40 };

    draw_button(btn_citta, TR(T_BTN_CITY), g_show_city_modal);
    draw_button(btn_refresh, "SYNC", g_is_fetching);
    char u_str[4];
    u_str[0] = g_config.use_fahrenheit ? 'F' : 'C';
    u_str[1] = '\0';
    draw_button(btn_unit, u_str, 0);

    // 2. Pannello Sinistro: Meteo Corrente (X: 24..540, Y: 80..580)
    draw_card(24, 80, 520, 496, COLOR_CARD_BG, COLOR_CARD_BORDER);

    if (!g_weather.valid) {
        // Nessun dato reale (né scaricato né salvato): stato vuoto, mai numeri inventati.
        if (g_fetch_status == 1) {
            const char *t = TR(T_LOADING);
            nv_gfx_text(24 + (520 - nv_gfx_text_width(t, 2)) / 2, 300, t, COLOR_TEXT_MUTED, 2);
        } else {
            const char *t = TR(T_NO_DATA);
            nv_gfx_text(24 + (520 - nv_gfx_text_width(t, 4)) / 2, 270, t, COLOR_STATUS_ERR, 4);
            t = TR(T_NO_DATA_HINT);
            nv_gfx_text(24 + (520 - nv_gfx_text_width(t, 2)) / 2, 330, t, COLOR_TEXT_MUTED, 2);
        }
        goto forecast_panel;
    }

    // Icona grande meteo corrente
    draw_weather_icon(140, 210, g_weather.weather_code, g_weather.is_day, 3);

    // Temperatura principale in grandissimo
    char temp_str[16];
    int_to_str(to_unit(g_weather.current_temp), temp_str);
    int tlen = (int)strlen(temp_str);
    temp_str[tlen++] = ' ';
    temp_str[tlen++] = *unit_symbol();
    temp_str[tlen] = '\0';
    nv_gfx_text(260, 150, temp_str, COLOR_TEXT_WHITE, 6);

    // Descrizione testuale condizione
    nv_gfx_text(260, 230, weather_desc(g_weather.weather_code), COLOR_TEXT_MUTED, 2);

    // Sotto-card dettagli (Umidità, Percepita, Vento, ecc.)
    int sub_y = 320;
    // Percepita
    draw_card(48, sub_y, 220, 100, COLOR_HEADER, COLOR_CARD_BORDER);
    nv_gfx_text(64, sub_y + 16, TR(T_FEELS_LIKE), COLOR_TEXT_MUTED, 1);
    char perc_str[16];
    int_to_str(to_unit(g_weather.apparent_temp), perc_str);
    int plen = (int)strlen(perc_str);
    perc_str[plen++] = ' ';
    perc_str[plen++] = *unit_symbol();
    perc_str[plen] = '\0';
    nv_gfx_text(64, sub_y + 44, perc_str, COLOR_TEXT_WHITE, 3);

    // Umidità
    draw_card(288, sub_y, 220, 100, COLOR_HEADER, COLOR_CARD_BORDER);
    nv_gfx_text(304, sub_y + 16, TR(T_HUMIDITY), COLOR_TEXT_MUTED, 1);
    char hum_str[16];
    int_to_str(g_weather.humidity, hum_str);
    int hlen = (int)strlen(hum_str);
    hum_str[hlen++] = '%';
    hum_str[hlen] = '\0';
    nv_gfx_text(304, sub_y + 44, hum_str, COLOR_TEXT_WHITE, 3);

    // Vento
    draw_card(48, sub_y + 116, 220, 100, COLOR_HEADER, COLOR_CARD_BORDER);
    nv_gfx_text(64, sub_y + 132, TR(T_WIND), COLOR_TEXT_MUTED, 1);
    char wind_str[24];
    int_to_str(g_weather.wind_speed, wind_str);
    int wlen = (int)strlen(wind_str);
    const char *wunit = " KM/H";
    while (*wunit) wind_str[wlen++] = *wunit++;
    wind_str[wlen] = '\0';
    nv_gfx_text(64, sub_y + 160, wind_str, COLOR_TEXT_WHITE, 3);

    // Sorgente
    draw_card(288, sub_y + 116, 220, 100, COLOR_HEADER, COLOR_CARD_BORDER);
    nv_gfx_text(304, sub_y + 132, TR(T_PROVIDER), COLOR_TEXT_MUTED, 1);
    nv_gfx_text(304, sub_y + 160, "OPEN-METEO", COLOR_TEXT_WHITE, 2);
    nv_gfx_text(304, sub_y + 184, TR(T_IMMUTABLE), COLOR_STATUS_OK, 1);

forecast_panel:
    // 3. Pannello Destro: Previsioni Prossimi 4 Giorni (X: 560..1000, Y: 80..580)
    draw_card(560, 80, 440, 496, COLOR_CARD_BG, COLOR_CARD_BORDER);
    nv_gfx_text(584, 100, TR(T_FORECAST_TITLE), COLOR_TEXT_WHITE, 2);

    if (!g_weather.valid) return;

    // Open-Meteo "daily" array: indice 0 = il giorno del download, non per forza oggi (dati
    // salvati di ieri): si sposta l'etichetta dei giorni trascorsi, o si mostra la data.
    const char *day_labels[FORECAST_DAYS] = { TR(T_DAY_TODAY), TR(T_DAY_TOMORROW), TR(T_DAY_AFTER), TR(T_DAY_PLUS3) };
    int shift = data_day_shift();

    for (int d = 0; d < FORECAST_DAYS; d++) {
        int card_y = 144 + d * 102;
        draw_card(580, card_y, 400, 88, COLOR_HEADER, COLOR_CARD_BORDER);

        // Nome giorno
        int rel = d - shift;
        char date_str[8];
        const char *label = date_str;
        if (shift >= 0 && rel >= 0 && rel < FORECAST_DAYS) {
            label = day_labels[rel];
        } else if (g_weather.daily_mday[d] > 0) {
            date_str[0] = (char)('0' + g_weather.daily_mday[d] / 10);
            date_str[1] = (char)('0' + g_weather.daily_mday[d] % 10);
            date_str[2] = '/';
            date_str[3] = (char)('0' + g_weather.daily_mon[d] / 10);
            date_str[4] = (char)('0' + g_weather.daily_mon[d] % 10);
            date_str[5] = '\0';
        } else {
            label = "-";
        }
        nv_gfx_text(596, card_y + 18, label, COLOR_TEXT_MUTED, 2);
        nv_gfx_text(596, card_y + 48, weather_desc(g_weather.daily_code[d]), COLOR_TEXT_WHITE, 1);

        // Icona meteo previsione
        draw_weather_icon(780, card_y + 44, g_weather.daily_code[d], 1, 1);

        // Temperature Min / Max
        char mm_str[32];
        char min_s[8], max_s[8];
        int_to_str(to_unit(g_weather.daily_min[d]), min_s);
        int_to_str(to_unit(g_weather.daily_max[d]), max_s);
        int mmi = 0;
        char *ms = min_s; while (*ms) mm_str[mmi++] = *ms++;
        mm_str[mmi++] = '/';
        ms = max_s; while (*ms) mm_str[mmi++] = *ms++;
        mm_str[mmi++] = *unit_symbol();
        mm_str[mmi] = '\0';

        nv_gfx_text(850, card_y + 34, mm_str, COLOR_TEXT_WHITE, 2);
    }
}

// ---- Modale di Selezione Città -------------------------------------------------------------------

static void draw_city_modal(void) {
    // Sfondo overlay oscurato
    nv_gfx_rect(100, 60, 824, 480, COLOR_HEADER);
    draw_card(100, 60, 824, 480, COLOR_CARD_BG, COLOR_ACCENT_HOVER);

    nv_gfx_text(130, 84, TR(T_MODAL_TITLE), COLOR_TEXT_WHITE, 2);
    Rect btn_close = { 850, 76, 54, 40 };
    draw_button(btn_close, "X", 1);

    // Griglia 2 colonne x 4 righe per le 8 città
    for (int i = 0; i < CITY_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;
        int cx = 130 + col * 390;
        int cy = 150 + row * 84;

        Rect cr = { cx, cy, 370, 68 };
        int is_sel = (g_config.city_idx == i);
        draw_card(cr.x, cr.y, cr.w, cr.h, is_sel ? COLOR_ACCENT_BLUE : COLOR_HEADER,
                  COLOR_CARD_BORDER);

        nv_gfx_text(cx + 20, cy + 22, city_display_name(i), COLOR_TEXT_WHITE, 2);
        nv_gfx_text(cx + 260, cy + 26, kCities[i].country, COLOR_TEXT_MUTED, 1);
    }
}

// ---- Gestione Input Touch -----------------------------------------------------------------------

static void handle_touch(void) {
    int tx = 0, ty = 0;
    int is_down = nv_touch(&tx, &ty);

    // Trigger su tap release (edge down -> up)
    if (g_prev_touch && !is_down) {
        if (g_show_city_modal) {
            // Controlla pulsante chiudi
            Rect btn_close = { 850, 76, 54, 40 };
            if (point_in_rect(btn_close, tx, ty)) {
                g_show_city_modal = 0;
                g_prev_touch = is_down;
                return;
            }

            // Selezione città nella griglia
            for (int i = 0; i < CITY_COUNT; i++) {
                int col = i % 2;
                int row = i / 2;
                Rect cr = { 130 + col * 390, 150 + row * 84, 370, 68 };
                if (point_in_rect(cr, tx, ty)) {
                    g_config.city_idx = i;
                    nv_save("meteo.cfg", &g_config, sizeof(g_config));
                    g_show_city_modal = 0;
                    // Mai il meteo della città precedente sotto il nome nuovo: ultimo dato
                    // salvato della nuova città, oppure vuoto finché il fetch non risponde.
                    if (g_data_city != i && !cache_load()) {
                        memset(&g_weather, 0, sizeof g_weather);
                        g_data_city = -1;
                        g_data_ms = 0;
                        g_data_unix = 0;
                        str_copy(g_active_city_name, city_display_name(i), NAME_MAX_LEN);
                    }
                    trigger_fetch();
                    break;
                }
            }
        } else {
            // Pulsanti Header
            Rect btn_citta = { 700, 12, 130, 40 };
            Rect btn_refresh = { 840, 12, 110, 40 };
            Rect btn_unit = { 960, 12, 50, 40 };

            if (point_in_rect(btn_citta, tx, ty)) {
                g_show_city_modal = 1;
            } else if (point_in_rect(btn_refresh, tx, ty)) {
                trigger_fetch();
            } else if (point_in_rect(btn_unit, tx, ty)) {
                g_config.use_fahrenheit = !g_config.use_fahrenheit;
                nv_save("meteo.cfg", &g_config, sizeof(g_config));
            }
        }
    }

    g_prev_touch = is_down;
}

// ---- Punto d'ingresso principale dell'app WASM --------------------------------------------------

NV_EXPORT("run")
void run(void) {
    detect_lang();

    // Carica configurazione salvata se esistente
    if (nv_load("meteo.cfg", &g_config, sizeof(g_config)) != sizeof(g_config)) {
        g_config.city_idx = 0; // Default: AUTO (IP)
        g_config.use_fahrenheit = 0; // Default: Celsius
    }

    if (g_config.city_idx < 0 || g_config.city_idx >= CITY_COUNT) g_config.city_idx = 0;

    // Ultimo dato buono salvato su SD: visibile subito (con la sua età) mentre si aggiorna.
    cache_load();

    // Primo fetch dei dati meteo
    trigger_fetch();

    // Loop principale di rendering e interazione
    while (nv_gfx_present()) {
        // Gestisci gesto Back di sistema per chiudere modale o uscire dall'app
        if (nv_gfx_back()) {
            if (g_show_city_modal) {
                g_show_city_modal = 0;
            } else {
                break; // Ritorna da run() per uscire dall'app
            }
        }

        handle_touch();

        // Auto-refresh ogni 15 minuti (900.000 ms)
        if (nv_millis() - g_last_fetch_ms > 900000) {
            trigger_fetch();
        }

        // Render interfaccia
        draw_main_screen();
        if (g_show_city_modal) {
            draw_city_modal();
        }

        nv_sleep_ms(20); // ~50 fps per un touch responsivo e bassi consumi
    }
}
