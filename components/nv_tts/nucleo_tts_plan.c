// nucleo_tts_plan — il "cervello" del TTS concatenativo: testo italiano -> sequenza di token
// CLIP/PAUSE. Puro (string.h/ctype.h), allocation-free, host-compilabile. Logica documentata in
// nucleo_tts.h.
#include "nucleo_tts.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// RAM-safe: il testo e' limitato a 95 char (Utt.text[96] in nv_tts.c), quindi non puo'
// produrre piu' di ~160 unita'/token. u[MAX_UNITS] vive sullo stack del voice_task (16KB): 160 e' il
// ceiling sicuro (coesiste con norm[1024]). Tenuto == TOK_MAX in nucleo_tts.c.
#define MAX_UNITS   160
#define MAX_PHRASE   6     // parole massime in un match a frase (MOSAICO-style)

// Cardinali italiani round, pre-renderizzati come clip atomiche (pacchetto numeri obbligatorio).
static const char *HUNDREDS[10] = {
    "", "cento", "duecento", "trecento", "quattrocento", "cinquecento",
    "seicento", "settecento", "ottocento", "novecento"
};
static const char *THOUSANDS[10] = {
    "", "mille", "duemila", "tremila", "quattromila", "cinquemila",
    "seimila", "settemila", "ottomila", "novemila"
};

// ---- normalizzazione: UTF-8 -> ASCII, fold accenti italiani -----------------------------------
// PRESERVA il case delle lettere ASCII (à->a, À->A): serve a riconoscere gli acronimi
// tutto-maiuscolo (USB/GPS) da sillabare. Lo slug poi e' sempre minuscolo (il lex fa tolower).
static void normalize_keepcase(const char *in, char *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC3 && in[i + 1]) {               // accenti latin-1: 0x80-0x9F maiuscole, 0xA0-0xBF minuscole
            unsigned char b = (unsigned char)in[i + 1];
            char f = 0; int up = 0;
            if      (b >= 0x80 && b <= 0x85) { f = 'a'; up = 1; } else if (b >= 0xA0 && b <= 0xA5) f = 'a';
            else if (b >= 0x88 && b <= 0x8B) { f = 'e'; up = 1; } else if (b >= 0xA8 && b <= 0xAB) f = 'e';
            else if (b >= 0x8C && b <= 0x8F) { f = 'i'; up = 1; } else if (b >= 0xAC && b <= 0xAF) f = 'i';
            else if (b >= 0x92 && b <= 0x96) { f = 'o'; up = 1; } else if (b >= 0xB2 && b <= 0xB6) f = 'o';
            else if (b >= 0x99 && b <= 0x9C) { f = 'u'; up = 1; } else if (b >= 0xB9 && b <= 0xBC) f = 'u';
            else if (b == 0x87)              { f = 'c'; up = 1; } else if (b == 0xA7)              f = 'c';
            else if (b == 0x91)              { f = 'n'; up = 1; } else if (b == 0xB1)              f = 'n';
            if (f) out[o++] = up ? (char)(f - 32) : f;
            i += 2;
        } else if (c < 0x80) {
            out[o++] = (char)c;                      // PRESERVA il case (niente tolower qui)
            i += 1;
        } else {
            i += 1;
        }
    }
    out[o] = 0;
}

// ---- unita' intermedie (parola / numero / pausa) ----------------------------------------------
typedef enum { U_WORD = 0, U_NUMBER, U_PAUSE } unit_kind_t;
typedef struct { unit_kind_t kind; char s[48]; int ms; bool allcaps; } unit_t;

static bool is_decimal_dot(const char *a, int len, const char *p)
{
    // ',' o '.' e' separatore decimale solo se sta TRA due cifre.
    return len > 0 && isdigit((unsigned char)a[len - 1]) && isdigit((unsigned char)p[1]);
}

// Spezza la stringa ASCII normalizzata in unita'. La punteggiatura di fine frase/inciso diventa
// PAUSE; un numero (con segno e/o decimale) resta una sola unita' NUMBER; il resto sono WORD.
// Spezza la stringa NORMALIZZATA-keepcase (case preservato sulle lettere ASCII) in unita'. Salva lo slug
// SEMPRE minuscolo (tolower) e marca `allcaps` se la parola e' fatta di >=2 lettere TUTTE maiuscole
// (acronimo: USB/GPS; le cifre interne non contano, cosi' "MP3" resta allcaps).
static int lex_units(const char *a, unit_t *u, int max)
{
    int n = 0, ci = 0, al = 0, up = 0;
    char cur[48];
    char curkind = 0;   // 0 = vuoto, 'w' = parola, 'd' = numerico
    #define FLUSH() do { if (ci) { cur[ci] = 0; if (n < max) { \
        u[n].kind = (curkind == 'd') ? U_NUMBER : U_WORD; \
        strncpy(u[n].s, cur, sizeof(u[n].s) - 1); u[n].s[sizeof(u[n].s) - 1] = 0; u[n].ms = 0; \
        u[n].allcaps = (curkind == 'w' && al >= 2 && up == al); n++; } \
        ci = 0; curkind = 0; al = 0; up = 0; } } while (0)
    #define PAUSE(d) do { if (n < max) { u[n].kind = U_PAUSE; u[n].s[0] = 0; u[n].ms = (d); u[n].allcaps = false; n++; } } while (0)

    for (int i = 0; a[i]; i++) {
        char c = a[i];
        if (isdigit((unsigned char)c)) {
            if (ci == 0) curkind = 'd';
            if (ci < (int)sizeof(cur) - 1) cur[ci++] = c;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if (ci == 0) curkind = 'w';
            al++; if (c >= 'A' && c <= 'Z') up++;        // conteggio per il flag acronimo
            if (ci < (int)sizeof(cur) - 1) cur[ci++] = (char)tolower((unsigned char)c);
        } else if (c == '-') {
            // segno negativo solo all'inizio di un numero (cur vuoto e cifra subito dopo)
            if (ci == 0 && isdigit((unsigned char)a[i + 1])) { curkind = 'd'; cur[ci++] = '-'; }
            else FLUSH();
        } else if (c == ',' || c == '.') {
            if (curkind == 'd' && is_decimal_dot(cur, ci, &a[i])) {
                if (ci < (int)sizeof(cur) - 1) cur[ci++] = ',';   // canonicalizza il decimale a ','
            } else {
                FLUSH();
                PAUSE(c == '.' ? 320 : 160);
            }
        } else if (c == '!' || c == '?') {
            FLUSH(); PAUSE(360);
        } else if (c == ';' || c == ':') {
            FLUSH(); PAUSE(220);
        } else {
            FLUSH();   // spazio o altra punteggiatura: separatore senza pausa
        }
    }
    FLUSH();
    #undef FLUSH
    #undef PAUSE
    return n;
}

// ---- emissione token --------------------------------------------------------------------------
typedef struct { tts_token_t *out; int max; int n; bool en; } sink_t;

static void push_clip(sink_t *s, const char *slug, bool fb)
{
    if (s->n >= s->max || !slug || !slug[0]) return;
    s->out[s->n].kind = TTS_TOK_CLIP;
    snprintf(s->out[s->n].slug, sizeof(s->out[s->n].slug), "%s", slug);  // NUL-safe, no trunc warning
    s->out[s->n].ms = 0;
    s->out[s->n].fallback = fb;
    s->n++;
}
static void push_pause(sink_t *s, int ms)
{
    if (s->n >= s->max || ms <= 0) return;
    s->out[s->n].kind = TTS_TOK_PAUSE;
    s->out[s->n].slug[0] = 0;
    s->out[s->n].ms = ms;
    s->out[s->n].fallback = false;
    s->n++;
}
static void push_num(sink_t *s, int v, bool fb)   // clip "nV" (atomici 0..99)
{
    char b[8]; int o = 0; b[o++] = 'n';
    if (v >= 10) b[o++] = (char)('0' + v / 10);
    b[o++] = (char)('0' + v % 10);
    b[o] = 0; push_clip(s, b, fb);
}

// > 999999 (o overflow): lettura cifra-per-cifra (fallback grazioso), dalla piu' significativa.
static void emit_big_digits(sink_t *s, long v)
{
    if (v < 0) v = -v;
    char rev[24]; int rn = 0; long t = v;
    while (t > 0 && rn < (int)sizeof(rev)) { rev[rn++] = (char)('0' + t % 10); t /= 10; }
    if (rn == 0) { push_num(s, 0, true); return; }
    for (int i = rn - 1; i >= 0; i--) push_num(s, rev[i] - '0', true);
}

// Intero -> cardinale ITALIANO. 0..99 atomico (risolve l'agglutinazione: ventuno/ventotto);
// 101..999 = "duecento.." + resto; 1000..999999 = "mille"/"duemila.." + resto (1984 -> "mille"
// "novecento" "ottantaquattro", segmentazione naturale per gli anni). Niente "e" interno.
static void emit_int_it(sink_t *s, long v)
{
    if (v < 0) v = -v;
    if (v < 100)     { push_num(s, (int)v, false); return; }   // 0..99 atomici; 100 -> "cento"
    if (v < 1000)    { int h = (int)(v / 100), r = (int)(v % 100);
                       push_clip(s, HUNDREDS[h], false); if (r) emit_int_it(s, r); return; }
    if (v < 1000000) { int th = (int)(v / 1000), r = (int)(v % 1000);
                       if (th == 1)      push_clip(s, "mille", false);
                       else if (th <= 9) push_clip(s, THOUSANDS[th], false);
                       else { emit_int_it(s, th); push_clip(s, "mila", false); }
                       if (r) emit_int_it(s, r);
                       return; }
    emit_big_digits(s, v);
}

// Intero -> cardinale INGLESE. 0..99 atomico ("twenty-three"); poi composizione "two hundred
// thirty-four", "one thousand nine hundred eighty-four". Connettori "hundred"/"thousand".
static void emit_int_en(sink_t *s, long v)
{
    if (v < 0) v = -v;
    if (v < 100)     { push_num(s, (int)v, false); return; }
    if (v < 1000)    { int h = (int)(v / 100), r = (int)(v % 100);
                       push_num(s, h, false); push_clip(s, "hundred", false); if (r) emit_int_en(s, r); return; }
    if (v < 1000000) { int th = (int)(v / 1000), r = (int)(v % 1000);
                       emit_int_en(s, th); push_clip(s, "thousand", false); if (r) emit_int_en(s, r); return; }
    emit_big_digits(s, v);
}

// Unita' NUMBER ("-?digits(,digits)?") -> clip. Segno -> "meno"/"minus"; decimale -> "virgola"/"point"
// + cifre. La lingua (s->en) sceglie cardinali e connettori.
static void emit_number(sink_t *s, const char *str)
{
    const char *p = str;
    if (*p == '-') { push_clip(s, s->en ? "minus" : "meno", false); p++; }
    long ip = 0; int ndig = 0;
    while (*p && *p != ',' && ndig < 12) { if (*p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); ndig++; } p++; }
    if (ndig > 6) {                                  // troppo grande per il valore: cifra-per-cifra
        const char *q = str; if (*q == '-') q++;
        for (; *q && *q != ','; q++) if (*q >= '0' && *q <= '9') push_num(s, *q - '0', true);
    } else if (s->en) emit_int_en(s, ip);
    else              emit_int_it(s, ip);
    if (*p == ',') {                                 // parte decimale: "virgola"/"point" poi cifre
        push_clip(s, s->en ? "point" : "virgola", false);
        for (p++; *p; p++) if (*p >= '0' && *p <= '9') push_num(s, *p - '0', false);
    }
}

// Parola ignota (nessuna clip): emette un token UNKNOWN, conservando la PAROLA (per diagnostica:
// dice esattamente cosa manca). NON si prova lo spelling; decide nv_tts.c (micro-pausa o silenzio).
static void push_unknown(sink_t *s, const char *word)
{
    if (s->n >= s->max) return;
    s->out[s->n].kind = TTS_TOK_UNKNOWN;
    snprintf(s->out[s->n].slug, sizeof s->out[s->n].slug, "%s", word ? word : "");
    s->out[s->n].ms = 0;
    s->out[s->n].fallback = true;
    s->n++;
}

// ---- sillabazione ACRONIMI: USB/GPS/MP3 (tutto-maiuscolo, non coperti) -> lettera-per-lettera ----
// Una sigla tutto-maiuscolo che NON ha una clip propria si dice compitando: ogni lettera con la clip
// lett_<x> ("u esse bi"), ogni cifra col cardinale nN. Tutto-o-niente: emette SOLO se OGNI lettera ha
// la sua clip (altrimenti -> UNKNOWN -> "leggila"), cosi' prima del deploy delle clip lettera il
// comportamento resta invariato. Solo 2..6 caratteri (le sigle reali; oltre, compitare suona peggio
// di "leggila"). `w` e' gia' minuscolo (lo slug); il flag allcaps sul case originale l'ha gia' deciso.
#define TTS_ACRO_MIN 2
#define TTS_ACRO_MAX 6
static bool spell_acronym(sink_t *s, const char *w, tts_has_clip_fn has, void *ud)
{
    int len = (int)strlen(w);
    if (len < TTS_ACRO_MIN || len > TTS_ACRO_MAX || !has) return false;
    bool any_letter = false;
    for (int k = 0; k < len; k++) {                  // copertura PRIMA di emettere (niente sillabe parziali)
        char c = w[k];
        if (c >= '0' && c <= '9') continue;          // cifra -> nN (sempre nel pacchetto obbligatorio)
        if (c < 'a' || c > 'z') return false;        // carattere non sillababile
        char slug[7] = { 'l', 'e', 't', 't', '_', c, 0 };
        if (!has(slug, ud)) return false;            // manca la clip lettera -> niente spelling
        any_letter = true;
    }
    if (!any_letter) return false;                   // solo cifre (es. "00"): lascia la via numerica
    for (int k = 0; k < len; k++) {
        char c = w[k];
        if (c >= '0' && c <= '9') push_num(s, c - '0', true);
        else { char slug[7] = { 'l', 'e', 't', 't', '_', c, 0 }; push_clip(s, slug, true); }
    }
    return true;
}

// ---- planner COMPOSIZIONALE: scala la copertura SENZA nuove clip ------------------------------
// Una parola non coperta (composti/derivati: "portacenere", "buonasera", "watercolor", "autostrada")
// viene SCOMPOSTA in 2..3 sotto-parole gia' coperte (prefisso PIU' LUNGO prima + backtracking; ogni
// parte >= 3 char per evitare frammentazione assurda). Cosi' migliaia di parole composte/derivate si
// pronunciano concatenando clip esistenti, a costo ZERO file. Sicuro: scatta SOLO su parole non coperte
// e SOLO se l'INTERA parola e' fatta di clip reali (>=3 char) -> niente spelling casuale, o UNKNOWN.
#define TTS_DECOMP_MINSUB   3
#define TTS_DECOMP_MAXPARTS 3
static int tts_decompose(const char *w, int len, tts_has_clip_fn has, void *ud, char parts[][48], int maxp)
{
    if (len == 0) return 0;                                  // consumata tutta -> successo
    if (maxp <= 0) return -1;                                // troppe parti
    for (int L = (len < 47 ? len : 47); L >= TTS_DECOMP_MINSUB; L--) {   // prefisso piu' lungo prima
        char pre[48];
        for (int k = 0; k < L; k++) pre[k] = w[k];
        pre[L] = 0;
        if (has && has(pre, ud)) {
            int rest = tts_decompose(w + L, len - L, has, ud, parts + 1, maxp - 1);
            if (rest >= 0) { for (int k = 0; k <= L; k++) parts[0][k] = pre[k]; return rest + 1; }
        }
    }
    return -1;
}

// ---- velocita' di lettura (rate dell'header WAV) ---------------------------------------------
int nucleo_tts_speed_clamp(int pct)
{
    if (pct < TTS_SPEED_MIN) return TTS_SPEED_MIN;
    if (pct > TTS_SPEED_MAX) return TTS_SPEED_MAX;
    return pct;
}

// ---- planner pubblico -------------------------------------------------------------------------
int nucleo_tts_plan(const char *text, const char *lang,
                    tts_has_clip_fn has_clip, void *ud,
                    tts_token_t *out, int max)
{
    if (!text || !out || max <= 0) return 0;
    bool en = lang && lang[0] == 'e' && lang[1] == 'n';   // "en" -> inglese, altrimenti italiano

    char norm[1024];
    normalize_keepcase(text, norm, sizeof(norm));   // case preservato: lex marca gli acronimi (allcaps)

    unit_t u[MAX_UNITS];
    int nu = lex_units(norm, u, MAX_UNITS);

    sink_t s = { out, max, 0, en };
    for (int i = 0; i < nu && s.n < max; ) {
        if (u[i].kind == U_PAUSE) { push_pause(&s, u[i].ms); i++; continue; }
        if (u[i].kind == U_NUMBER) { emit_number(&s, u[i].s); i++; continue; }

        // U_WORD: prova un match a FRASE greedy (parole consecutive unite da '_'), dal piu' lungo.
        int span = 0;
        while (span < MAX_PHRASE && i + span < nu && u[i + span].kind == U_WORD) span++;
        bool matched = false;
        for (int k = span; k >= 1; k--) {
            char slug[48]; int o = 0;
            bool toolong = false;
            for (int j = 0; j < k; j++) {
                const char *w = u[i + j].s;
                if (j > 0) { if (o < (int)sizeof(slug) - 1) slug[o++] = '_'; else { toolong = true; break; } }
                for (int c = 0; w[c]; c++) { if (o < (int)sizeof(slug) - 1) slug[o++] = w[c]; else { toolong = true; break; } }
                if (toolong) break;
            }
            slug[o] = 0;
            if (!toolong && o > 0 && has_clip && has_clip(slug, ud)) {
                push_clip(&s, slug, false);
                i += k;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // nessuna frase: parola singola coperta -> clip; altrimenti, se e' un ACRONIMO tutto-maiuscolo
        // (USB/GPS/MP3) non coperto, sillabalo con le clip lettera; poi prova a SCOMPORLA in sotto-parole
        // coperte (composti/derivati, zero file nuovi); se nemmeno cosi' -> UNKNOWN (-> "leggila").
        if (has_clip && has_clip(u[i].s, ud)) { push_clip(&s, u[i].s, false); i++; continue; }
        if (u[i].allcaps && spell_acronym(&s, u[i].s, has_clip, ud)) { i++; continue; }
        char parts[TTS_DECOMP_MAXPARTS + 1][48];
        // SOVRACCARICO: ogni sotto-parte e' >= MINSUB char, quindi una scomposizione (>=2 parti) richiede
        // >= 2*MINSUB char. Sotto quella soglia e' MATEMATICAMENTE impossibile: saltala senza sprecare
        // ricerche binarie su SD (bus SPI condiviso col display) nel caso comune della parola corta scoperta.
        int wl = (int)strlen(u[i].s);
        int np = (has_clip && wl >= 2 * TTS_DECOMP_MINSUB) ? tts_decompose(u[i].s, wl, has_clip, ud, parts, TTS_DECOMP_MAXPARTS) : -1;
        if (np >= 2) for (int j = 0; j < np && s.n < max; j++) push_clip(&s, parts[j], true);   // composizione
        else         push_unknown(&s, u[i].s);
        i++;
    }
    return s.n;
}
