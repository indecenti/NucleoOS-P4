// ANIMA in Spanish, French and German — see anima_lang.h. libc only (runs in the host harness unchanged).
#include "anima_lang.h"
#include "anima_phrases.h"
#include "nucleo_anima_lex.h"     // anima_dict_tokenize: the one normalizer every table uses
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

anima_xlang_t anima_xlang(const char *lang)
{
    if (!lang || !lang[0] || !lang[1]) return ANIMA_XL_NONE;
    const char a = (char)tolower((unsigned char)lang[0]), b = (char)tolower((unsigned char)lang[1]);
    if (lang[2] && isalpha((unsigned char)lang[2])) return ANIMA_XL_NONE;     // "eng", "deu": not ours
    if (a == 'e' && b == 's') return ANIMA_XL_ES;
    if (a == 'f' && b == 'r') return ANIMA_XL_FR;
    if (a == 'd' && b == 'e') return ANIMA_XL_DE;
    return ANIMA_XL_NONE;
}

static anima_xlang_t s_current;
static char s_original[256];
void anima_lang_set_current(anima_xlang_t xl) { s_current = xl; if (xl == ANIMA_XL_NONE) s_original[0] = 0; }
anima_xlang_t anima_lang_current(void) { return s_current; }
void anima_lang_set_original(const char *text) { snprintf(s_original, sizeof s_original, "%s", text ? text : ""); }
const char *anima_lang_original(void) { return s_original; }

const char *anima_xlang_code(anima_xlang_t xl)
{
    return xl == ANIMA_XL_ES ? "es" : xl == ANIMA_XL_FR ? "fr" : xl == ANIMA_XL_DE ? "de" : "";
}

// ---- folding --------------------------------------------------------------------------------------

// The byte after 0xC3 (U+00C0..U+00FF) -> ASCII, or NULL when it is no letter we fold.
static const char *fold_c3(unsigned char d)
{
    static const char *const T[64] = {
        /* C0 */ "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        /* D0 */ NULL,"n","o","o","o","o","o",NULL,"o","u","u","u","u","y",NULL,"ss",
        /* E0 */ "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        /* F0 */ NULL,"n","o","o","o","o","o",NULL,"o","u","u","u","u","y",NULL,"y" };
    return (d >= 0x80 && d <= 0xBF) ? T[d - 0x80] : NULL;
}

void anima_lang_fold(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!cap) return;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 3 < cap; p++) {
        const unsigned char c = *p;
        const char *rep = NULL; char one[2] = { 0, 0 };
        if (c == 0xC3 && p[1]) { rep = fold_c3(p[1]); p++; if (!rep) rep = " "; }
        else if (c == 0xC5 && (p[1] == 0x92 || p[1] == 0x93)) { rep = "oe"; p++; }          // Œ œ
        else if (c == 0xC2 && p[1]) { rep = " "; p++; }                                        // ¿ ¡ « » nbsp
        else if (c == 0xE2 && p[1] == 0x80 && p[2]) { rep = " "; p += 2; }                     // ’ “ ” – …
        else if (c >= 0x80) rep = " ";                                                         // other scripts
        else if (c == '\'') rep = " ";
        else if (c == '-' && o > 0 && isalpha((unsigned char)out[o - 1]) && isalpha(p[1])) rep = " ";   // rappelle-moi                                                         // l'heure -> l heure
        else { one[0] = (char)tolower(c); rep = one; }
        for (const char *q = rep; *q && o + 1 < cap; q++) out[o++] = *q;
    }
    out[o] = 0;
}

// ---- understanding: whole phrase, then glossary ---------------------------------------------------

static bool phrase_to_en(const char *folded, anima_xlang_t xl, char *out, size_t cap)
{
    char tok[ANIMA_DICT_TOKENS][ANIMA_DICT_TOKLEN];
    const int n = anima_dict_tokenize(folded, tok);
    if (n == 0 || n >= ANIMA_DICT_TOKENS) return false;
    const char *code = anima_xlang_code(xl);
    char key[200]; int o = snprintf(key, sizeof key, "%s:", code);
    for (int i = 0; i < n && o < (int)sizeof key; i++)
        if (!anima_phrase_is_filler_lang(code, tok[i])) o += snprintf(key + o, sizeof key - o, "%s%s", key[o - 1] == ':' ? "" : " ", tok[i]);
    if (o >= (int)sizeof key || key[o - 1] == ':') return false;
    const char *c = anima_phrase_lookup(key);
    if (!c) return false;
    snprintf(out, cap, "%s", c);
    return true;
}

typedef struct { const char *from, *to; } gl_t;      // `from`: folded words; `to`: English ("" = drop)

// The command domain: verbs, apps, settings, time words, maths, the dictionary. Longest match wins, so
// "pon el volumen al" beats "pon". Grammar words map to their English twins so the engine's own rules
// (direction, numbers, "to <n>") keep working.
static const gl_t GL_ES[] = {
    {"nueva york","new york"},{"londres","london"},{"pekin","beijing"},{"moscu","moscow"},{"atenas","athens"},{"lisboa","lisbon"},{"roma","rome"},{"sidney","sydney"},{"tokio","tokyo"},{"seul","seoul"},{"bombay","mumbai"},{"ciudad de mexico","mexico city"},
    {"abre","open"},{"abrir","open"},{"abreme","open"},{"lanza","open"},{"inicia","open"},{"arranca","open"},
    {"ejecuta","open"},{"muestra","show"},{"muestrame","show"},{"ensename","show"},{"ver","show"},
    {"pon musica","play music"},{"pon la musica","play music"},{"pon algo de musica","play music"},
    {"reproduce","play"},{"cierra","close"},{"cerrar","close"},{"cierrame","close"},{"sal de","close"},
    {"salir de","close"},{"musica","music"},{"canciones","songs"},{"notas","notes"},{"nota","note"},
    {"calculadora","calculator"},{"ajustes","settings"},{"configuracion","settings"},{"opciones","settings"},
    {"galeria","gallery"},{"fotos","photos"},{"imagenes","pictures"},{"archivos","files"},{"carpetas","folders"},
    {"documentos","documents"},{"camara","camera"},{"videos","videos"},{"video","video"},{"peliculas","movies"},
    {"tareas","tasks"},{"grabadora","recorder"},{"juegos","games"},{"tienda","store"},{"asistente","assistant"},
    {"pantalla","screen"},{"teclado","keyboard"},
    {"pon el volumen al","set the volume to"},{"pon el volumen a","set the volume to"},
    {"volumen al","volume to"},{"volumen a","volume to"},{"volumen","volume"},
    {"pon el brillo al","set brightness to"},{"pon el brillo a","set brightness to"},{"brillo al","brightness to"},
    {"brillo","brightness"},{"por ciento","percent"},{"porciento","percent"},
    {"pon un temporizador de","set a timer for"},{"pon un temporizador para","set a timer for"},
    {"temporizador de","timer for"},{"temporizador","timer"},{"cuenta atras","timer"},{"alarma","alarm"},
    {"minutos","minutes"},{"minuto","minute"},{"segundos","seconds"},{"horas","hours"},{"hora","hour"},
    {"recuerdame que","remind me to"},{"recuerdame","remind me to"},{"manana","tomorrow"},{"hoy","today"},
    {"pasado manana","the day after tomorrow"},{"a las","at"},{"a la una","at 1"},{"esta noche","tonight"},
    {"cuanto es","what is"},{"cuanto son","what is"},{"cuanto hace","what is"},{"calcula","calculate"},
    {"mas","plus"},{"menos","minus"},{"multiplicado por","times"},{"por","times"},{"dividido por","divided by"},
    {"dividido entre","divided by"},{"raiz cuadrada de","square root of"},{"raiz de","square root of"},
    {"al cuadrado","squared"},{"al cubo","cubed"},{"elevado a","to the power of"},{"por ciento de","percent of"},
    {"convierte","convert"},{"kilometros","km"},{"metros","meters"},{"millas","miles"},{"grados","degrees"},
    {"que tiempo hace en","what's the weather in"},{"que tiempo hace","what's the weather"},
    {"el tiempo en","the weather in"},{"va a llover","will it rain"},{"llueve","is it raining"},
    {"que hora es en","what time is it in"},{"que hora es","what time is it"},
    {"que significa","what does mean"},{"que quiere decir","what does mean"},{"significado de","meaning of"},
    {"definicion de","definition of"},{"define","define"},{"sinonimos de","synonyms of"},{"sinonimo de","synonym of"},
    {"lo contrario de","opposite of"},{"el contrario de","opposite of"},{"contrario de","opposite of"},
    {"antonimo de","antonym of"},{"traduce","translate"},{"traducir","translate"},{"como se dice","how do you say"},
    {"al ingles","to english"},{"en ingles","in english"},{"al italiano","to italian"},{"en italiano","in italian"},
    {"al espanol","to spanish"},{"en espanol","to spanish"},{"al frances","to french"},{"en frances","to french"},
    {"al aleman","to german"},{"en aleman","to german"},
    {"crea una nota","create a note"},{"nueva nota","new note"},{"llamada","called"},
    {"no abras","don't open"},{"no cierres","don't close"},{"no","no"},{"si","yes"},
    {"y","and"},{"en","in"},{"otra vez","again"},{"de nuevo","again"},{"repite","repeat"},
};
static const gl_t GL_FR[] = {
    {"londres","london"},{"pekin","beijing"},{"moscou","moscow"},{"athenes","athens"},{"lisbonne","lisbon"},{"mexico","mexico city"},
    {"ouvre","open"},{"ouvrir","open"},{"ouvre moi","open"},{"lance","open"},{"lancer","open"},{"demarre","open"},
    {"affiche","show"},{"montre","show"},{"montre moi","show"},{"mets de la musique","play music"},
    {"mets la musique","play music"},{"joue","play"},{"ferme","close"},{"fermer","close"},{"quitte","close"},
    {"musique","music"},{"chansons","songs"},{"notes","notes"},{"note","note"},{"calculatrice","calculator"},
    {"parametres","settings"},{"reglages","settings"},{"options","settings"},{"galerie","gallery"},
    {"photos","photos"},{"images","pictures"},{"fichiers","files"},{"dossiers","folders"},{"documents","documents"},
    {"appareil photo","camera"},{"camera","camera"},{"videos","videos"},{"video","video"},{"films","movies"},
    {"taches","tasks"},{"enregistreur","recorder"},{"dictaphone","recorder"},{"jeux","games"},{"boutique","store"},
    {"assistant","assistant"},{"ecran","screen"},{"clavier","keyboard"},
    {"mets le volume a","set the volume to"},{"regle le volume a","set the volume to"},{"volume a","volume to"},
    {"volume","volume"},{"mets la luminosite a","set brightness to"},{"regle la luminosite a","set brightness to"},
    {"luminosite a","brightness to"},{"luminosite","brightness"},{"pour cent","percent"},
    {"mets un minuteur de","set a timer for"},{"lance un minuteur de","set a timer for"},{"minuteur de","timer for"},
    {"minuteur","timer"},{"minuterie","timer"},{"compte a rebours","timer"},{"alarme","alarm"},
    {"minutes","minutes"},{"minute","minute"},{"secondes","seconds"},{"heures","hours"},
    {"rappelle moi de","remind me to"},{"rappelle moi d","remind me to"},{"rappelle moi","remind me to"},
    {"demain","tomorrow"},{"aujourd hui","today"},{"apres demain","the day after tomorrow"},{"ce soir","tonight"},
    {"combien font","what is"},{"combien fait","what is"},{"combien ca fait","what is"},{"calcule","calculate"},
    {"plus","plus"},{"moins","minus"},{"fois","times"},{"multiplie par","times"},{"divise par","divided by"},
    {"racine carree de","square root of"},{"racine de","square root of"},{"au carre","squared"},{"au cube","cubed"},
    {"puissance","to the power of"},{"pour cent de","percent of"},{"convertis","convert"},{"kilometres","km"},
    {"metres","meters"},{"miles","miles"},{"degres","degrees"},
    {"quel temps fait il a","what's the weather in"},{"quel temps fait il","what's the weather"},
    {"la meteo a","the weather in"},{"meteo a","weather in"},{"meteo","weather"},{"va t il pleuvoir","will it rain"},
    {"quelle heure est il a","what time is it in"},{"quelle heure est il","what time is it"},
    {"que veut dire","what does mean"},{"qu est ce que veut dire","what does mean"},{"que signifie","what does mean"},
    {"qu est ce que signifie","what does mean"},{"signification de","meaning of"},{"definition de","definition of"},
    {"definis","define"},{"synonymes de","synonyms of"},{"synonyme de","synonym of"},{"le contraire de","opposite of"},
    {"contraire de","opposite of"},{"antonyme de","antonym of"},{"traduis","translate"},{"traduire","translate"},
    {"comment dit on","how do you say"},{"comment on dit","how do you say"},{"en anglais","to english"},
    {"en italien","to italian"},{"en espagnol","to spanish"},{"en francais","to french"},
    {"en allemand","to german"},{"cree une note","create a note"},{"nouvelle note","new note"},{"appelee","called"},
    {"n ouvre pas","don't open"},{"ne ferme pas","don't close"},{"non","no"},{"oui","yes"},
    {"et","and"},{"en","in"},{"encore","again"},{"de nouveau","again"},{"repete","repeat"},
};
static const gl_t GL_DE[] = {
    {"peking","beijing"},{"moskau","moscow"},{"rom","rome"},{"athen","athens"},{"lissabon","lisbon"},{"tokio","tokyo"},{"mexiko stadt","mexico city"},
    {"offne","open"},{"offnen","open"},{"oeffne","open"},{"starte","open"},{"starten","open"},{"zeig","show"},
    {"zeige","show"},{"zeig mir","show"},{"spiel musik","play music"},{"spiele musik","play music"},
    {"spiel","play"},{"schliesse","close"},{"schliess","close"},{"schliessen","close"},{"beende","close"},
    {"musik","music"},{"lieder","songs"},{"notizen","notes"},{"notiz","note"},{"rechner","calculator"},
    {"taschenrechner","calculator"},{"einstellungen","settings"},{"optionen","settings"},{"galerie","gallery"},
    {"fotos","photos"},{"bilder","pictures"},{"dateien","files"},{"ordner","folders"},{"dokumente","documents"},
    {"kamera","camera"},{"videos","videos"},{"video","video"},{"filme","movies"},{"aufgaben","tasks"},
    {"rekorder","recorder"},{"diktiergerat","recorder"},{"spiele","games"},{"store","store"},
    {"assistent","assistant"},{"bildschirm","screen"},{"tastatur","keyboard"},
    {"stell die lautstarke auf","set the volume to"},{"lautstarke auf","volume to"},{"lautstarke","volume"},
    {"stell die helligkeit auf","set brightness to"},{"helligkeit auf","brightness to"},{"helligkeit","brightness"},
    {"prozent","percent"},{"stell einen timer auf","set a timer for"},{"stell einen timer fur","set a timer for"},
    {"stelle einen timer fur","set a timer for"},{"timer fur","timer for"},{"timer auf","timer for"},
    {"timer","timer"},{"wecker","alarm"},{"minuten","minutes"},{"minute","minute"},{"sekunden","seconds"},
    {"stunden","hours"},{"stunde","hour"},{"erinnere mich daran","remind me to"},{"erinnere mich an","remind me to"},
    {"erinnere mich","remind me to"},{"morgen","tomorrow"},{"heute","today"},{"ubermorgen","the day after tomorrow"},
    {"heute abend","tonight"},{"um","at"},
    {"wie viel ist","what is"},{"wieviel ist","what is"},{"was ist","what is"},{"rechne","calculate"},
    {"plus","plus"},{"minus","minus"},{"mal","times"},{"multipliziert mit","times"},{"geteilt durch","divided by"},
    {"durch","divided by"},{"quadratwurzel aus","square root of"},{"quadratwurzel von","square root of"},
    {"wurzel aus","square root of"},{"wurzel von","square root of"},{"zum quadrat","squared"},{"hoch","to the power of"},
    {"prozent von","percent of"},{"rechne um","convert"},{"kilometer","km"},{"meter","meters"},{"meilen","miles"},
    {"grad","degrees"},{"wie ist das wetter in","what's the weather in"},{"wie ist das wetter","what's the weather"},
    {"wetter in","weather in"},{"wetter","weather"},{"regnet es","is it raining"},{"wird es regnen","will it rain"},
    {"wie spat ist es in","what time is it in"},{"wie viel uhr ist es in","what time is it in"},
    {"was bedeutet","what does mean"},{"was heisst","what does mean"},{"bedeutung von","meaning of"},
    {"definition von","definition of"},{"definiere","define"},{"synonyme fur","synonyms of"},{"synonyme von","synonyms of"},
    {"synonym fur","synonym of"},{"das gegenteil von","opposite of"},{"gegenteil von","opposite of"},
    {"antonym von","antonym of"},{"ubersetze","translate"},{"ubersetzen","translate"},{"wie sagt man","how do you say"},
    {"auf englisch","to english"},{"ins englische","to english"},{"auf italienisch","to italian"},
    {"ins italienische","to italian"},{"auf spanisch","to spanish"},{"ins spanische","to spanish"},
    {"auf franzosisch","to french"},{"ins franzosische","to french"},{"auf deutsch","to german"},{"ins deutsche","to german"},{"erstelle eine notiz","create a note"},{"neue notiz","new note"},
    {"namens","called"},{"offne nicht","don't open"},{"nein","no"},{"ja","yes"},
    {"und","and"},{"nochmal","again"},{"noch einmal","again"},{"wiederhole","repeat"},
};

static const gl_t *gl_table(anima_xlang_t xl, size_t *n)
{
    switch (xl) {
        case ANIMA_XL_ES: *n = sizeof GL_ES / sizeof GL_ES[0]; return GL_ES;
        case ANIMA_XL_FR: *n = sizeof GL_FR / sizeof GL_FR[0]; return GL_FR;
        case ANIMA_XL_DE: *n = sizeof GL_DE / sizeof GL_DE[0]; return GL_DE;
        default: *n = 0; return NULL;
    }
}

#define GL_WORDS 48
#define GL_MAXN  5

// "what does mean" + "efimero" -> "what does efimero mean": the English dictionary frame wraps its word.
static void gl_fix_mean(char *s, size_t cap)
{
    char *p = strstr(s, "what does mean ");
    if (!p) return;
    const char *w = p + strlen("what does mean ");
    const size_t n = strcspn(w, " ?!.");
    char word[96], rest[256];
    if (!n || n >= sizeof word) return;
    memcpy(word, w, n); word[n] = 0;
    snprintf(rest, sizeof rest, "%s", w + n);
    snprintf(p, cap - (size_t)(p - s), "what does %s mean%s", word, rest);
}

// Split the user's ORIGINAL text into words on the same separators the fold uses (¿ ¡ « » quotes,
// apostrophes, a hyphen between letters), so each word can be matched folded but emitted as written.
static int gl_words(const char *in, char *buf, size_t cap, char *w[GL_WORDS])
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; p++) {
        const unsigned char c = *p;
        const bool letter_before = o > 0 && (isalpha((unsigned char)buf[o - 1]) || (unsigned char)buf[o - 1] >= 0x80);
        const bool letter_after = isalpha(p[1]) || p[1] >= 0x80;
        if (c == 0xC2 && p[1]) { buf[o++] = ' '; p++; }                                  // ¿ ¡ « » nbsp
        else if (c == 0xE2 && p[1] == 0x80 && p[2]) { buf[o++] = ' '; p += 2; }          // ’ “ ” – …
        else if (c == '\'' || (c == '-' && letter_before && letter_after)) buf[o++] = ' ';
        else buf[o++] = (char)c;
    }
    buf[o] = 0;
    int n = 0;
    for (char *t = strtok(buf, " \t"); t && n < GL_WORDS; t = strtok(NULL, " \t")) w[n++] = t;
    return n;
}

static void glossary_to_en(const char *orig, anima_xlang_t xl, char *out, size_t cap)
{
    size_t ng; const gl_t *g = gl_table(xl, &ng);
    char buf[512]; char *ow[GL_WORDS];
    const int nw = gl_words(orig, buf, sizeof buf, ow);
    static char fw[GL_WORDS][64];                                  // the words folded (engine task only)
    for (int i = 0; i < nw; i++) anima_lang_fold(ow[i], fw[i], sizeof fw[i]);
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < nw; ) {
        int took = 0; const char *to = NULL; char tail[8] = "";
        for (int n = GL_MAXN; n >= 1 && !took; n--) {
            if (i + n > nw) continue;
            char key[160]; size_t k = 0; bool ok = true;
            for (int j = 0; j < n && ok; j++) {                    // the words without edge punctuation
                const char *s = fw[i + j]; size_t len = strlen(s);
                while (len && !isalnum((unsigned char)*s)) { s++; len--; }
                while (len && !isalnum((unsigned char)s[len - 1])) len--;
                if (!len || k + len + 2 >= sizeof key) { ok = false; break; }
                if (j) key[k++] = ' ';
                memcpy(key + k, s, len); k += len;
            }
            if (!ok) continue;
            key[k] = 0;
            for (size_t e = 0; e < ng; e++)
                if (!strcmp(g[e].from, key)) {
                    to = g[e].to; took = n;
                    const char *last = fw[i + n - 1]; size_t t2 = strlen(last);
                    while (t2 && !isalnum((unsigned char)last[t2 - 1])) t2--;
                    snprintf(tail, sizeof tail, "%.6s", last + t2);   // keep "?" / "!" / ","
                    break;
                }
        }
        // "à 9", "a 9", "um 9": a bare preposition before a number is a time ("remind me ... at 9")
        if (!took && i + 1 < nw && isdigit((unsigned char)fw[i + 1][0]) &&
            (!strcmp(fw[i], "a") || !strcmp(fw[i], "um"))) { to = "at"; took = 1; }
        const char *emit = took ? to : ow[i];                     // untranslated words stay as written
        if (emit[0] || tail[0]) {
            const int r = snprintf(out + o, cap - o, "%s%s%s", (o && emit[0]) ? " " : "", emit, tail);
            if (r < 0 || (size_t)r >= cap - o) break;
            o += (size_t)r;
        }
        i += took ? took : 1;
    }
    gl_fix_mean(out, cap);
}

// A bare "yes" — only the WHOLE answer counts ("si llueve..." is an "if", not a yes): it confirms a
// pending question ("Intendi «…»? (sì/no)", a model's action waiting for approval).
static bool is_yes(const char *folded, anima_xlang_t xl)
{
    static const char *const ES[] = { "si", "vale", "claro", "de acuerdo", "adelante", "hazlo", "por supuesto", "ok", NULL };
    static const char *const FR[] = { "oui", "d accord", "vas y", "bien sur", "carrement", "fais le", "ok", "ouais", NULL };
    static const char *const DE[] = { "ja", "klar", "gerne", "mach das", "in ordnung", "einverstanden", "okay", "ok",
                                      "jawohl", "genau", NULL };
    const char *const *l = xl == ANIMA_XL_ES ? ES : xl == ANIMA_XL_FR ? FR : DE;
    char tok[ANIMA_DICT_TOKENS][ANIMA_DICT_TOKLEN], j[64];
    const int n = anima_dict_tokenize(folded, tok);
    if (n < 1 || n > 3) return false;
    int o = 0;
    for (int i = 0; i < n; i++) o += snprintf(j + o, sizeof j - o, "%s%s", i ? " " : "", tok[i]);
    for (int i = 0; l[i]; i++) if (!strcmp(j, l[i])) return true;
    return false;
}

void anima_lang_to_en(const char *in, anima_xlang_t xl, char *out, size_t cap)
{
    if (!cap) return;
    char folded[512];
    anima_lang_fold(in ? in : "", folded, sizeof folded);
    if (xl == ANIMA_XL_NONE) { snprintf(out, cap, "%s", in ? in : ""); return; }
    if (is_yes(folded, xl)) { snprintf(out, cap, "yes"); return; }
    if (phrase_to_en(folded, xl, out, cap)) return;
    glossary_to_en(in ? in : "", xl, out, cap);
}

// ---- replies -------------------------------------------------------------------------------------

// `en` is matched against the whole reply; "%s" captures any text (possibly empty). Targets put the
// captures back with {1} {2} {3}, so a language may reorder them. intent NULL = any intent.
typedef struct { const char *intent, *en, *es, *fr, *de; } rp_t;

static const rp_t RP[] = {
    { NULL, "Raising the volume.", "Subo el volumen.", "J'augmente le volume.", "Ich mache lauter." },
    { NULL, "Lowering the volume.", "Bajo el volumen.", "Je baisse le volume.", "Ich mache leiser." },
    { NULL, "Turning the volume up.", "Subo el volumen.", "J'augmente le volume.", "Ich mache lauter." },
    { NULL, "Turning the volume down.", "Bajo el volumen.", "Je baisse le volume.", "Ich mache leiser." },
    { NULL, "Setting the volume to %s.", "Pongo el volumen al {1}.", "Je règle le volume à {1}.", "Ich stelle die Lautstärke auf {1}." },
    { NULL, "Raising the brightness.", "Subo el brillo.", "J'augmente la luminosité.", "Ich mache den Bildschirm heller." },
    { NULL, "Lowering the brightness.", "Bajo el brillo.", "Je baisse la luminosité.", "Ich mache den Bildschirm dunkler." },
    { NULL, "Brightening the screen.", "Subo el brillo.", "J'augmente la luminosité.", "Ich mache den Bildschirm heller." },
    { NULL, "Dimming the screen.", "Bajo el brillo.", "Je baisse la luminosité.", "Ich mache den Bildschirm dunkler." },
    { NULL, "Setting the brightness to %s.", "Pongo el brillo al {1}.", "Je règle la luminosité à {1}.", "Ich stelle die Helligkeit auf {1}." },
    { NULL, "Opening %s.", "Abro {1}.", "J'ouvre {1}.", "Ich öffne {1}." },
    { NULL, "Closing %s.", "Cierro {1}.", "Je ferme {1}.", "Ich schließe {1}." },
    { NULL, "Going back to the Home screen.", "Vuelvo a la pantalla de inicio.", "Je reviens à l'écran d'accueil.", "Zurück zum Startbildschirm." },
    { NULL, "Pausing.", "Pongo en pausa.", "Je mets en pause.", "Ich pausiere." },
    { NULL, "Resuming.", "Reanudo.", "Je reprends.", "Ich spiele weiter." },
    { "greeting", "Hi! %s", "¡Hola! Pídeme abrir una app, la hora, el espacio libre... o lo que quieras.",
      "Salut ! Demande-moi d'ouvrir une app, l'heure, l'espace libre... ou autre chose.",
      "Hallo! Frag mich nach einer App, der Uhrzeit, dem freien Speicher... oder etwas anderem." },
    { NULL, "You're welcome!", "¡De nada!", "De rien !", "Gern geschehen!" },
    { "whoami", "I'm ANIMA%s", "Soy ANIMA, el asistente sin conexión de NucleoOS. Funciono sin internet, en el dispositivo.",
      "Je suis ANIMA, l'assistant hors ligne de NucleoOS. Je fonctionne sans internet, sur l'appareil.",
      "Ich bin ANIMA, der Offline-Assistent von NucleoOS. Ich arbeite ohne Internet, direkt auf dem Gerät." },
    { NULL, "Sorry — try rephrasing and I'll have another go.", "Perdona: dilo de otra manera y lo intento otra vez.",
      "Désolé : reformule et je réessaie.", "Entschuldige – sag es anders, dann versuche ich es nochmal." },
    { NULL, "OK, I won't do anything.", "Vale, no hago nada.", "D'accord, je ne fais rien.", "Okay, ich mache nichts." },
    { NULL, "OK. Say it in other words and I'll learn it.", "Vale. Dímelo con otras palabras y lo aprendo.",
      "D'accord. Dis-le autrement et je l'apprendrai.", "Okay. Sag es mit anderen Worten, dann lerne ich es." },
    { NULL, "I don't know that yet. Without a model I can open and close apps, tell the time and date, do maths, set volume and brightness, timers and reminders, and answer from what is on the SD.%s",
      "Todavía no lo sé. Sin modelo puedo abrir y cerrar apps, decir la hora y la fecha, hacer cálculos, ajustar volumen y brillo, temporizadores y recordatorios, y responder con lo que hay en la SD.{1}",
      "Je ne le sais pas encore. Sans modèle, je peux ouvrir et fermer des apps, donner l'heure et la date, calculer, régler le volume et la luminosité, des minuteurs et des rappels, et répondre avec ce qu'il y a sur la carte SD.{1}",
      "Das weiß ich noch nicht. Ohne Modell kann ich Apps öffnen und schließen, Uhrzeit und Datum sagen, rechnen, Lautstärke und Helligkeit einstellen, Timer und Erinnerungen setzen und mit dem antworten, was auf der SD-Karte ist.{1}" },
    { NULL, "I don't know.%s", "No lo sé.{1}", "Je ne sais pas.{1}", "Das weiß ich nicht.{1}" },
    { NULL, "Timer %s started: it rings at %s.", "Temporizador de {1} en marcha: sonará a las {2}.",
      "Minuteur de {1} lancé : il sonnera à {2}.", "Timer über {1} gestartet: er klingelt um {2}." },
    { NULL, "No timers or alarms running.", "No hay temporizadores ni alarmas activos.", "Aucun minuteur ni alarme en cours.",
      "Keine Timer oder Wecker aktiv." },
    { NULL, "Alarm set for %s tomorrow: %s.", "Alarma para mañana a las {1}: {2}.", "Alarme réglée pour demain à {1} : {2}.",
      "Wecker für morgen um {1}: {2}." },
    { NULL, "Alarm set for %s: %s.", "Alarma a las {1}: {2}.", "Alarme réglée à {1} : {2}.", "Wecker um {1}: {2}." },
    { NULL, "Alarm set for %s.", "Alarma a las {1}.", "Alarme réglée à {1}.", "Wecker um {1}." },
    { "calc", "It's %s.", "Da {1}.", "Ça fait {1}.", "Das ergibt {1}." },
    { "year", "It's %s.", "Estamos en {1}.", "Nous sommes en {1}.", "Wir haben {1}." },
    { "season", "It's %s.", "Estamos en {1}.", "Nous sommes en {1}.", "Wir haben {1}." },
    { NULL, "I can't divide by zero.", "No puedo dividir por cero.", "Je ne peux pas diviser par zéro.", "Durch null kann ich nicht teilen." },
    { NULL, "Result: %s.", "Resultado: {1}.", "Résultat : {1}.", "Ergebnis: {1}." },
    { NULL, "I need internet to check the weather.", "Necesito internet para consultar el tiempo.",
      "J'ai besoin d'internet pour consulter la météo.", "Für das Wetter brauche ich Internet." },
    { NULL, "I need internet for the news.", "Necesito internet para las noticias.", "J'ai besoin d'internet pour les actualités.",
      "Für Nachrichten brauche ich Internet." },
    { NULL, "Running %s.", "Versión {1}.", "Version {1}.", "Version {1}." },
    { NULL, "Battery: %s.", "Batería: {1}.", "Batterie : {1}.", "Akku: {1}." },
    { NULL, "SD space: %s.", "Espacio en la SD: {1}.", "Espace sur la carte SD : {1}.", "Speicher auf der SD: {1}." },
    { NULL, "In %s it's %s.", "En {1} son las {2}.", "À {1}, il est {2}.", "In {1} ist es {2}." },
    { NULL, "Today is %s.", "Hoy es {1}.", "Aujourd'hui, nous sommes le {1}.", "Heute ist {1}." },
    { NULL, "Tomorrow is %s.", "Mañana es {1}.", "Demain, nous serons le {1}.", "Morgen ist {1}." },
    { NULL, "Yesterday was %s.", "Ayer fue {1}.", "Hier, c'était le {1}.", "Gestern war {1}." },
    { NULL, "Creating %s in %s.", "Creo {1} en {2}.", "Je crée {1} dans {2}.", "Ich erstelle {1} in {2}." },
    { NULL, "I can't schedule an action for later yet, so I haven't done it.%s",
      "Todavía no puedo programar una acción para más tarde, así que no la he hecho. Puedo poner un recordatorio: «recuérdame en 10 minutos que ...».",
      "Je ne peux pas encore programmer une action pour plus tard, donc je ne l'ai pas faite. Je peux créer un rappel : « rappelle-moi dans 10 minutes de ... ».",
      "Ich kann noch keine Aktion für später planen, deshalb habe ich nichts getan. Ich kann eine Erinnerung setzen: „erinnere mich in 10 Minuten an ...“." },
    { NULL, "I can't repeat an action over time yet, so I haven't set anything. I can set a single reminder.",
      "Todavía no puedo repetir una acción en el tiempo, así que no he programado nada. Puedo poner un único recordatorio.",
      "Je ne peux pas encore répéter une action dans le temps, donc je n'ai rien programmé. Je peux créer un seul rappel.",
      "Ich kann noch keine Aktion wiederholen lassen, deshalb habe ich nichts eingestellt. Ich kann eine einzelne Erinnerung setzen." },
    { NULL, "I'm not sure I understood: do you mean \"%s\"? (yes/no)", "No estoy seguro de haberte entendido: ¿quieres decir «{1}»? (sí/no)",
      "Je ne suis pas sûr d'avoir compris : tu veux dire « {1} » ? (oui/non)", "Ich bin nicht sicher, ob ich dich verstanden habe: meinst du „{1}“? (ja/nein)" },
    { NULL, "More about what? Tell me the topic.", "¿Más sobre qué? Dime el tema.", "Plus sur quoi ? Dis-moi le sujet.",
      "Mehr worüber? Nenne mir das Thema." },
    { NULL, "\"%s\" in Italian: %s.", "«{1}» en italiano: {2}.", "« {1} » en italien : {2}.", "„{1}“ auf Italienisch: {2}." },
    { NULL, "\"%s\" in English: %s.", "«{1}» en inglés: {2}.", "« {1} » en anglais : {2}.", "„{1}“ auf Englisch: {2}." },
    { NULL, "\"%s\" (a form of \"%s\") in Italian: %s.", "«{1}» (forma de «{2}») en italiano: {3}.",
      "« {1} » (forme de « {2} ») en italien : {3}.", "„{1}“ (Form von „{2}“) auf Italienisch: {3}." },
    { NULL, "\"%s\" (a form of \"%s\") in English: %s.", "«{1}» (forma de «{2}») en inglés: {3}.",
      "« {1} » (forme de « {2} ») en anglais : {3}.", "„{1}“ (Form von „{2}“) auf Englisch: {3}." },
    { NULL, "\"%s\" in Spanish: %s.", "«{1}» en español: {2}.", "« {1} » en espagnol : {2}.", "„{1}“ auf Spanisch: {2}." },
    { NULL, "\"%s\" in French: %s.", "«{1}» en francés: {2}.", "« {1} » en français : {2}.", "„{1}“ auf Französisch: {2}." },
    { NULL, "\"%s\" in German: %s.", "«{1}» en alemán: {2}.", "« {1} » en allemand : {2}.", "„{1}“ auf Deutsch: {2}." },
    { NULL, "\"%s\" (a form of \"%s\") in Spanish: %s.", "«{1}» (forma de «{2}») en español: {3}.",
      "« {1} » (forme de « {2} ») en espagnol : {3}.", "„{1}“ (Form von „{2}“) auf Spanisch: {3}." },
    { NULL, "\"%s\" (a form of \"%s\") in French: %s.", "«{1}» (forma de «{2}») en francés: {3}.",
      "« {1} » (forme de « {2} ») en français : {3}.", "„{1}“ (Form von „{2}“) auf Französisch: {3}." },
    { NULL, "\"%s\" (a form of \"%s\") in German: %s.", "«{1}» (forma de «{2}») en alemán: {3}.",
      "« {1} » (forme de « {2} ») en allemand : {3}.", "„{1}“ (Form von „{2}“) auf Deutsch: {3}." },
    { NULL, "\"%s\": I have no offline definition; in English: %s.", "«{1}»: no tengo la definición sin conexión; en inglés: {2}.",
      "« {1} » : je n'ai pas la définition hors ligne ; en anglais : {2}.", "„{1}“: keine Offline-Definition; auf Englisch: {2}." },
    { NULL, "I don't have \"%s\" in the offline IT<->EN dictionary.", "No tengo «{1}» en el diccionario sin conexión IT<->EN.",
      "Je n'ai pas « {1} » dans le dictionnaire hors ligne IT<->EN.", "„{1}“ steht nicht im Offline-Wörterbuch IT<->EN." },
    { NULL, "\"%s\" is not in the offline dictionary.", "«{1}» no está en el diccionario sin conexión.",
      "« {1} » n'est pas dans le dictionnaire hors ligne.", "„{1}“ steht nicht im Offline-Wörterbuch." },
};

// Match `pat` ("%s" = any text) against all of `s`; captures go to cap[k] (max 3). Backtracking, short.
static bool rp_match(const char *pat, const char *s, char cap[3][512], int k)
{
    const char *ph = strstr(pat, "%s");
    if (!ph) return !strcmp(pat, s);
    const size_t lit = (size_t)(ph - pat);
    if (strncmp(pat, s, lit)) return false;
    if (k >= 3) return false;
    const char *rest = ph + 2, *from = s + lit;
    for (const char *e = from + strlen(from); e >= from; e--) {             // longest capture first
        if ((size_t)(e - from) >= 512) continue;
        if (!*rest || !strncmp(rest, e, strcspn(rest, "%"))) {
            memcpy(cap[k], from, (size_t)(e - from)); cap[k][e - from] = 0;
            if (rp_match(rest, e, cap, k + 1)) return true;
        }
    }
    return false;
}

static void rp_fill(const char *tmpl, char cap[3][512], char *out, size_t outcap)
{
    size_t o = 0;
    for (const char *p = tmpl; *p && o + 1 < outcap; ) {
        if (p[0] == '{' && p[1] >= '1' && p[1] <= '3' && p[2] == '}') {
            for (const char *q = cap[p[1] - '1']; *q && o + 1 < outcap; q++) out[o++] = *q;
            p += 3;
        } else out[o++] = *p++;
    }
    out[o] = 0;
}

// Day and month names, and "Saturday, October 3, 2026" -> "sábado, 3 de octubre de 2026".
static const char *const WD_EN[7] = { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
static const char *const MO_EN[12] = { "January","February","March","April","May","June","July","August","September",
                                       "October","November","December" };
static const char *const WD_X[3][7] = {
    { "domingo","lunes","martes","miércoles","jueves","viernes","sábado" },
    { "dimanche","lundi","mardi","mercredi","jeudi","vendredi","samedi" },
    { "Sonntag","Montag","Dienstag","Mittwoch","Donnerstag","Freitag","Samstag" } };
static const char *const MO_X[3][12] = {
    { "enero","febrero","marzo","abril","mayo","junio","julio","agosto","septiembre","octubre","noviembre","diciembre" },
    { "janvier","février","mars","avril","mai","juin","juillet","août","septembre","octobre","novembre","décembre" },
    { "Januar","Februar","März","April","Mai","Juni","Juli","August","September","Oktober","November","Dezember" } };

static int name_at(const char *s, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) {
        const size_t l = strlen(names[i]);
        if (!strncmp(s, names[i], l) && !isalpha((unsigned char)s[l])) return i;
    }
    return -1;
}

static void localize_dates(char *s, size_t cap, anima_xlang_t xl)
{
    const int L = (int)xl - 1;
    char *out = (char *)malloc(cap);
    if (!out) return;
    size_t o = 0;
    for (const char *p = s; *p && o + 1 < cap; ) {
        const bool boundary = p == s || !isalpha((unsigned char)p[-1]);
        int wd = boundary ? name_at(p, WD_EN, 7) : -1, mo = -1, d = 0, y = 0, used = 0;
        const char *q = p;
        if (wd >= 0 && !strncmp(p + strlen(WD_EN[wd]), ", ", 2)) q = p + strlen(WD_EN[wd]) + 2;
        if (boundary || q != p) mo = name_at(q, MO_EN, 12);
        if (mo >= 0 && sscanf(q + strlen(MO_EN[mo]), " %d, %d%n", &d, &y, &used) == 2 && d >= 1 && d <= 31) {
            const char *wn = (wd >= 0 && q != p) ? WD_X[L][wd] : NULL;
            char date[96];
            if (xl == ANIMA_XL_ES)      snprintf(date, sizeof date, "%s%s%d de %s de %d", wn ? wn : "", wn ? ", " : "", d, MO_X[L][mo], y);
            else if (xl == ANIMA_XL_FR) snprintf(date, sizeof date, "%s%s%d %s %d", wn ? wn : "", wn ? " " : "", d, MO_X[L][mo], y);
            else                        snprintf(date, sizeof date, "%s%s%d. %s %d", wn ? wn : "", wn ? ", " : "", d, MO_X[L][mo], y);
            for (const char *c = date; *c && o + 1 < cap; c++) out[o++] = *c;
            p = q + strlen(MO_EN[mo]) + used;
            continue;
        }
        if (wd >= 0) {                                                        // a day name alone
            for (const char *c = WD_X[L][wd]; *c && o + 1 < cap; c++) out[o++] = *c;
            p += strlen(WD_EN[wd]);
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    snprintf(s, cap, "%s", out);
    free(out);
}

// Fixed pieces of the dictionary replies (nucleo_anima_lex.c), swapped inside the text.
typedef struct { const char *en, *es, *fr, *de; } sub_t;
static const sub_t SUBS[] = {
    { " Synonyms: ", " Sinónimos: ", " Synonymes : ", " Synonyme: " },
    { " — synonyms: ", " — sinónimos: ", " — synonymes : ", " — Synonyme: " },
    { " — opposites: ", " — contrarios: ", " — contraires : ", " — Gegenteile: " },
    { "\" is a form of \"", "» es una forma de «", " » est une forme de « ", "“ ist eine Form von „" },
    { " (Italian), in English: ", " (italiano), en inglés: ", " (italien), en anglais : ", " (Italienisch), auf Englisch: " },
    { " (English), in Italian: ", " (inglés), en italiano: ", " (anglais), en italien : ", " (Englisch), auf Italienisch: " },
    { " (Italian)", " (italiano)", " (italien)", " (Italienisch)" },
    { " (English)", " (inglés)", " (anglais)", " (Englisch)" },
    { ": I have no offline definition; in English: ", ": no tengo la definición sin conexión; en inglés: ",
      " : je n'ai pas la définition hors ligne ; en anglais : ", ": keine Offline-Definition; auf Englisch: " },
    { ": I have no offline definition; in Italian: ", ": no tengo la definición sin conexión; en italiano: ",
      " : je n'ai pas la définition hors ligne ; en italien : ", ": keine Offline-Definition; auf Italienisch: " },
    { "Starting from ", "Partiendo de ", "En partant de ", "Ausgehend von " },
};

static void swap_all(char *s, size_t cap, const char *from, const char *to)
{
    const size_t fl = strlen(from), tl = strlen(to);
    for (char *p = strstr(s, from); p; p = strstr(p + tl, from)) {
        const size_t len = strlen(s);
        if (len - fl + tl + 1 > cap) return;
        memmove(p + tl, p + fl, len - (size_t)(p - s) - fl + 1);
        memcpy(p, to, tl);
    }
}

// "\"ephemeral\" — ..." opens with straight quotes: give it the language's quotes.
static void lex_quotes(char *s, size_t cap, anima_xlang_t xl)
{
    if (s[0] != '"') return;
    char *end = strchr(s + 1, '"');
    if (!end) return;
    const char *open = xl == ANIMA_XL_DE ? "„" : xl == ANIMA_XL_FR ? "« " : "«";
    const char *close = xl == ANIMA_XL_DE ? "“" : xl == ANIMA_XL_FR ? " »" : "»";
    char *buf = (char *)malloc(cap);
    if (!buf) return;
    snprintf(buf, cap, "%s%.*s%s%s", open, (int)(end - s - 1), s + 1, close, end + 1);
    snprintf(s, cap, "%s", buf);
    free(buf);
}

bool anima_lang_localize(anima_result_t *r, anima_xlang_t xl)
{
    if (!r || xl == ANIMA_XL_NONE || !r->reply[0]) return false;
    typedef struct { char cap[3][512]; char inner[1024]; char out[1024]; } scratch_t;
    scratch_t *k = (scratch_t *)calloc(1, sizeof *k);
    if (!k) return false;
    bool done = false;
    // "Starting from 36: it's 41." (a maths follow-up) wraps another reply: translate the inner one too.
    if (!strncmp(r->reply, "Starting from ", 14)) {
        const char *colon = strstr(r->reply, ": ");
        if (colon) {
            anima_result_t *in = (anima_result_t *)calloc(1, sizeof *in);
            if (in) {
                snprintf(in->intent, sizeof in->intent, "%s", r->intent);
                snprintf(in->reply, sizeof in->reply, "%s", colon + 2);
                if (in->reply[0] >= 'a' && in->reply[0] <= 'z') in->reply[0] = (char)(in->reply[0] - 'a' + 'A');
                anima_lang_localize(in, xl);
                snprintf(k->out, sizeof k->out, "%.*s: %s", (int)(colon - r->reply), r->reply, in->reply);
                snprintf(r->reply, sizeof r->reply, "%s", k->out);
                free(in);
            }
        }
    }
    for (size_t i = 0; i < sizeof RP / sizeof RP[0] && !done; i++) {
        if (RP[i].intent && strcmp(RP[i].intent, r->intent)) continue;
        memset(k->cap, 0, sizeof k->cap);
        if (!rp_match(RP[i].en, r->reply, k->cap, 0)) continue;
        const char *t = xl == ANIMA_XL_ES ? RP[i].es : xl == ANIMA_XL_FR ? RP[i].fr : RP[i].de;
        rp_fill(t, k->cap, k->out, sizeof k->out);
        snprintf(r->reply, sizeof r->reply, "%s", k->out);
        done = true;
    }
    const int L = (int)xl - 1;
    for (size_t i = 0; i < sizeof SUBS / sizeof SUBS[0]; i++) {
        const char *to = L == 0 ? SUBS[i].es : L == 1 ? SUBS[i].fr : SUBS[i].de;
        if (strstr(r->reply, SUBS[i].en)) { swap_all(r->reply, sizeof r->reply, SUBS[i].en, to); done = true; }
    }
    if (!strcmp(r->intent, "define") || !strcmp(r->intent, "synonyms") || !strcmp(r->intent, "antonyms"))
        lex_quotes(r->reply, sizeof r->reply, xl);
    char before[64]; snprintf(before, sizeof before, "%s", r->reply);
    localize_dates(r->reply, sizeof r->reply, xl);
    if (strncmp(before, r->reply, sizeof before - 1)) done = true;
    free(k);
    return done;
}
