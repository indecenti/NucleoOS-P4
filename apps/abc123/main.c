// ABC 123 - NucleoOS ABI v2 educational game for kids (6-8 yrs).
//
// Seven touch mini-games (letters, numbers, counting, +/- math, spelling, odd-one-out, memory),
// localized to the system language (nv_lang: it/en/es/fr/de). Real emoji art via nv_gfx_image.
//
// PROGRESSION: each game is a session that starts at level 1 and gets harder every LEVEL_STEP
// correct answers (bigger counts, harder sums, longer sequences/words, more pairs). You have 3
// lives; a wrong answer costs one, 0 = game over. Your session score enters a per-game TOP-10.
// Stars are a persistent collectible (one per correct answer), NOT the difficulty driver. High
// scores + stars persist on SD via nv_save/nv_load.
//
// PERFORMANCE: immediate-mode but frame-skipping (g_redraw) — idle screens cost ~0 host calls.
#include "nucleo_sdk.h"

// ---- canvas + layout ----------------------------------------------------------------------------
static int W, H, MX, MY, HUD;   // HUD = reserved top-band height; game content starts below it
static int C_BG, C_BG2, C_INK, C_PANEL, C_SHADOW, C_GOOD, C_BAD, C_STAR, C_DIM;
static int C_ACC[8];
static int PASTEL[8];   // soft per-level backgrounds

static int g_redraw = 2;
static void mark_dirty(void) { g_redraw = 2; }
static int g_wrong_i = -1;                         // last wrong-tapped option (for red highlight)

// ---- tiny helpers -------------------------------------------------------------------------------
static int imin(int a, int b) { return a < b ? a : b; }
static int slen(const char *s) { return (int)strlen(s); }
static int rnd(int lo, int hi) { int r = nv_rand(); if (r < 0) r = -r; return lo + r % (hi - lo + 1); }
static void shuffle(int *a, int n) { for (int i = n - 1; i > 0; i--) { int j = rnd(0, i); int t = a[i]; a[i] = a[j]; a[j] = t; } }

static char g_numbuf[16];
static const char *istr(int v) {
    if (v == 0) { g_numbuf[0] = '0'; g_numbuf[1] = 0; return g_numbuf; }
    char tmp[16]; int i = 0, n = v < 0 ? -v : v;
    while (n > 0 && i < 15) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    int j = 0; if (v < 0) g_numbuf[j++] = '-'; while (i > 0) g_numbuf[j++] = tmp[--i]; g_numbuf[j] = 0; return g_numbuf;
}
static char g_chbuf[2];
static const char *chstr(char c) { g_chbuf[0] = c; g_chbuf[1] = 0; return g_chbuf; }
static int inrect(int x, int y, int w, int h, int px, int py) { return px >= x && px < x + w && py >= y && py < y + h; }

// ---- fixed-point trig (byte angle 0..255) + RGB565 blend: cheap smooth motion, zero float --------
static const short SINQ[65] = {
    0,25,50,75,100,125,150,175,200,224,249,273,297,
    321,345,369,392,415,438,460,483,505,526,548,569,590,
    610,630,650,669,688,706,724,742,759,775,792,807,822,
    837,851,865,878,891,903,915,926,936,946,955,964,972,
    980,987,993,999,1004,1009,1013,1016,1019,1021,1023,1024,1024,
};
static int isin(int a) { a &= 255;
    if (a < 64) return SINQ[a]; if (a < 128) return SINQ[128 - a];
    if (a < 192) return -SINQ[a - 128]; return -SINQ[256 - a]; }
static int lighten(int c, int amt) { int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r += (31 - r) * amt / 255; g += (63 - g) * amt / 255; b += (31 - b) * amt / 255; return (r << 11) | (g << 5) | b; }
// pop scale with a single overshoot: t,d ms -> 0..256 (256 = 100%, peak ~110%). For bouncy entrances.
static int pop_scale(int t, int d) { if (t < 0) t = 0; if (d < 1) d = 1; if (t >= d) return 256;
    int g = d * 60 / 100; if (t < g) return 282 * t / g; return 282 - 26 * (t - g) / (d - g); }

static int fit_scale(const char *s, int maxw, int maxh, int cap) {
    int len = slen(s); if (len < 1) len = 1;
    int sw = maxw / (6 * len), sh = maxh / 7, sc = sw < sh ? sw : sh;
    if (sc > cap) sc = cap; if (sc < 1) sc = 1; return sc;
}
static void text_cs(int cx, int cy, const char *s, int col, int sc) {
    nv_gfx_text(cx - (6 * sc * slen(s)) / 2, cy - (7 * sc) / 2, s, col, sc);
}
static void text_fit(int cx, int cy, const char *s, int col, int maxw, int maxh, int cap) {
    text_cs(cx, cy, s, col, fit_scale(s, maxw, maxh, cap));
}
static void fill_round(int x, int y, int w, int h, int r, int col) {
    if (2 * r > w) r = w / 2; if (2 * r > h) r = h / 2;
    if (r < 1) { nv_gfx_rect(x, y, w, h, col); return; }
    nv_gfx_rect(x + r, y, w - 2 * r, h, col);
    nv_gfx_rect(x, y + r, w, h - 2 * r, col);
    nv_gfx_circle(x + r, y + r, r, col);
    nv_gfx_circle(x + w - 1 - r, y + r, r, col);
    nv_gfx_circle(x + r, y + h - 1 - r, r, col);
    nv_gfx_circle(x + w - 1 - r, y + h - 1 - r, r, col);
}
static int darken(int c, int amt) { int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r -= r * amt / 255; g -= g * amt / 255; b -= b * amt / 255; return (r << 11) | (g << 5) | b; }
static void card(int x, int y, int w, int h, int fill, int border, const char *label, int tcol) {
    int r = h / 6; if (r > 24) r = 24; if (r < 6) r = 6;
    fill_round(x, y + 5, w, h, r, darken(fill, 42));               // colored 3D lip under the card — no gray poke
    fill_round(x, y, w, h, r, border);
    fill_round(x + 6, y + 6, w - 12, h - 12, r - 2, fill);
    int gh = (h - 12) * 34 / 100, gr = r - 4; if (gr < 3) gr = 3;   // rounded gloss -> never covers the corners
    if (gh > 10) fill_round(x + 10, y + 9, w - 20, gh, gr, lighten(fill, 55));
    if (label && label[0]) text_fit(x + w / 2, y + h / 2, label, tcol, w - 24, h - 24, 16);
}
// Star = the real star.565 asset (1 blit) instead of 10 triangles. Prettier and far lighter.
// `col` is ignored (the art is pre-coloured); kept so every call site stays unchanged.
static void draw_star(int cx, int cy, int r, int col) { (void)col; if (r < 2) r = 2; nv_gfx_image("star", cx - r, cy - r, 2 * r, 2 * r); }
static void draw_heart(int cx, int cy, int r, int col) {
    nv_gfx_circle(cx - r / 2, cy - r / 4, r / 2 + 1, col);
    nv_gfx_circle(cx + r / 2, cy - r / 4, r / 2 + 1, col);
    nv_gfx_tri(cx - r, cy - r / 6, cx + r, cy - r / 6, cx, cy + r, col);
}
// Settings gear: 8 teeth around a ring with a hole (bg color for the hole).
static const int GEAR_X[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
static const int GEAR_Y[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
static void draw_gear(int cx, int cy, int r, int col, int bg) {
    for (int i = 0; i < 8; i++)
        nv_gfx_rect(cx + GEAR_X[i] * r / 10 - r / 5, cy + GEAR_Y[i] * r / 10 - r / 5, 2 * r / 5, 2 * r / 5, col);
    nv_gfx_circle(cx, cy, r, col);
    nv_gfx_circle(cx, cy, r * 45 / 100, bg);
}
// Back button: a colored rounded button with a white left-pointing arrow (clearer than a "<" glyph).
static void draw_back_btn(int x, int y, int w, int h) {
    int r = h / 4; if (r > 16) r = 16; if (r < 4) r = 4;
    fill_round(x, y, w, h, r, C_INK);                       // border (no shadow -> never pokes the HUD)
    fill_round(x + 4, y + 4, w - 8, h - 8, r - 2, C_ACC[3]);
    int cx = x + w / 2, cy = y + h / 2, s = imin(w, h) / 4;
    nv_gfx_tri(cx + s * 2 / 3, cy - s, cx + s * 2 / 3, cy + s, cx - s * 2 / 3, cy, C_PANEL);
}
// Big X (two thick diagonal bars) — the static wrong-answer mark.
static void draw_x(int cx, int cy, int s, int t, int col) {
    nv_gfx_tri(cx - s - t, cy - s + t, cx - s + t, cy - s - t, cx + s + t, cy + s - t, col);
    nv_gfx_tri(cx - s - t, cy - s + t, cx + s + t, cy + s - t, cx + s - t, cy + s + t, col);
    nv_gfx_tri(cx + s - t, cy - s - t, cx + s + t, cy - s + t, cx - s + t, cy + s + t, col);
    nv_gfx_tri(cx + s - t, cy - s - t, cx - s + t, cy + s + t, cx - s - t, cy + s - t, col);
}
// Exit button: red rounded button with a white X — closes the app back to the launcher.
static void draw_exit_btn(int x, int y, int w, int h) {
    int r = h / 4; if (r > 16) r = 16; if (r < 4) r = 4;
    fill_round(x, y, w, h, r, C_INK);
    fill_round(x + 4, y + 4, w - 8, h - 8, r - 2, C_BAD);
    draw_x(x + w / 2, y + h / 2, imin(w, h) / 5, 3, C_PANEL);
}

// ---- localization -------------------------------------------------------------------------------
enum { L_IT, L_EN, L_ES, L_FR, L_DE, L_COUNT };
static int g_lang = L_IT;
static const char *T_MODE_L[L_COUNT] = { "LETTERE", "LETTERS", "LETRAS", "LETTRES", "BUCHSTABEN" };
static const char *T_MODE_N[L_COUNT] = { "NUMERI", "NUMBERS", "NUMEROS", "NOMBRES", "ZAHLEN" };
static const char *T_MODE_M[L_COUNT] = { "MISTO", "MIXED", "MIXTO", "MIXTE", "GEMISCHT" };
static const char *T_GOOD[L_COUNT]   = { "BRAVO!", "GREAT!", "GENIAL!", "BRAVO!", "SUPER!" };
static const char *T_BAD[L_COUNT]    = { "RIPROVA", "TRY AGAIN", "OTRA VEZ", "ENCORE", "NOCHMAL" };
static const char *T_LVUP[L_COUNT]   = { "LIVELLO SU!", "LEVEL UP!", "NIVEL MAS!", "NIVEAU SUP!", "LEVEL UP!" };
static const char *T_OVER[L_COUNT]   = { "FINE", "GAME OVER", "FIN", "TERMINE", "ENDE" };
static const char *T_SCOREW[L_COUNT] = { "PUNTI", "SCORE", "PUNTOS", "POINTS", "PUNKTE" };
static const char *T_REC[L_COUNT]    = { "NUOVO RECORD!", "NEW RECORD!", "NUEVO RECORD!", "NOUVEAU RECORD!", "NEUER REKORD!" };
static const char *T_REPLAY[L_COUNT] = { "RIGIOCA", "AGAIN", "OTRA VEZ", "REJOUER", "NOCHMAL" };
static const char *T_BEST[L_COUNT]   = { "RECORD", "SCORES", "RECORDS", "SCORES", "REKORDE" };
static const char *T_SUB[L_COUNT]    = { "IMPARA GIOCANDO", "LEARN AND PLAY", "APRENDE JUGANDO", "APPRENDS EN JOUANT", "SPIELEND LERNEN" };
static const char *T_SET[L_COUNT]    = { "IMPOSTAZIONI", "SETTINGS", "AJUSTES", "REGLAGES", "EINSTELLUNGEN" };
static const char *T_LANGL[L_COUNT]  = { "LINGUA", "LANGUAGE", "IDIOMA", "LANGUE", "SPRACHE" };
static const char *LANG_CODE[L_COUNT] = { "IT", "EN", "ES", "FR", "DE" };
static const char *LANG_LC[L_COUNT]   = { "it", "en", "es", "fr", "de" };   // for nv_speak / voice packs
static const char *T_NAME[L_COUNT]   = { "NOME", "NAME", "NOMBRE", "NOM", "NAME" };
static const char *T_RESET[L_COUNT]  = { "AZZERA RECORD", "RESET SCORES", "BORRAR RECORDS", "EFFACER SCORES", "REKORDE LOESCHEN" };
static const char *T_READY[L_COUNT]  = { "PRONTI", "READY", "LISTOS", "PRETS", "BEREIT" };
static const char *T_VOICE[L_COUNT]  = { "VOCE", "VOICE", "VOZ", "VOIX", "STIMME" };
static const char *T_VON[L_COUNT]    = { "ACCESA", "ON", "ACTIVA", "ACTIVE", "AN" };
static const char *T_VOFF[L_COUNT]   = { "SPENTA", "OFF", "APAGADA", "COUPEE", "AUS" };
// Spoken number words 0..20 (only it/en are in the voice pack) and math operators, for the countdown
// and the CONTI game. nv_tts wants the WORD ("tre"), not the digit; ASCII is fine (it folds accents).
static const char *NUMW_IT[21] = { "ZERO","UNO","DUE","TRE","QUATTRO","CINQUE","SEI","SETTE","OTTO","NOVE",
    "DIECI","UNDICI","DODICI","TREDICI","QUATTORDICI","QUINDICI","SEDICI","DICIASSETTE","DICIOTTO","DICIANNOVE","VENTI" };
static const char *NUMW_EN[21] = { "ZERO","ONE","TWO","THREE","FOUR","FIVE","SIX","SEVEN","EIGHT","NINE",
    "TEN","ELEVEN","TWELVE","THIRTEEN","FOURTEEN","FIFTEEN","SIXTEEN","SEVENTEEN","EIGHTEEN","NINETEEN","TWENTY" };
static const char *num_word(int n) {   // NULL = not voiceable in this language (skip)
    if (n < 0 || n > 20) return 0;
    if (g_lang == L_IT) return NUMW_IT[n];
    if (g_lang == L_EN) return NUMW_EN[n];
    return 0;
}
static const char *op_word(int minus) {
    if (g_lang == L_IT) return minus ? "MENO" : "PIU";
    if (g_lang == L_EN) return minus ? "MINUS" : "PLUS";
    return 0;
}
static const char *T_Q1[L_COUNT] = { "CHE LETTERA", "WHICH LETTER", "QUE LETRA", "QUELLE LETTRE", "WELCHER BUCHSTABE" };
static const char *T_Q2[L_COUNT] = { "QUANTI SONO", "HOW MANY", "CUANTOS SON", "COMBIEN", "WIE VIELE" };
static const char *T_Q3[L_COUNT] = { "TOCCA IN ORDINE", "TAP IN ORDER", "TOCA EN ORDEN", "TOUCHE EN ORDRE", "DER REIHE NACH" };
static const char *T_Q4[L_COUNT] = { "TROVA LE COPPIE", "FIND THE PAIRS", "BUSCA PAREJAS", "TROUVE LES PAIRES", "FINDE DIE PAARE" };
static const char *T_Q5[L_COUNT] = { "QUANTO FA", "HOW MUCH", "CUANTO ES", "COMBIEN FONT", "WIE VIEL" };
static const char *T_Q6[L_COUNT] = { "TOCCA LA FIGURA", "TAP THE PICTURE", "TOCA LA IMAGEN", "TOUCHE L IMAGE", "TIPPE DAS BILD" };
static const char *T_Q7[L_COUNT] = { "COMPONI LA PAROLA", "SPELL THE WORD", "ESCRIBE LA PALABRA", "EPELLE LE MOT", "BUCHSTABIERE" };
static const char *T_G1[L_COUNT] = { "INIZIALE", "INITIAL", "INICIAL", "INITIALE", "ANFANG" };
static const char *T_G2[L_COUNT] = { "CONTA", "COUNT", "CONTAR", "COMPTER", "ZAEHLEN" };
static const char *T_G3[L_COUNT] = { "IN ORDINE", "IN ORDER", "EN ORDEN", "EN ORDRE", "REIHE" };
static const char *T_G4[L_COUNT] = { "ABBINA", "MATCH", "PAREJAS", "PAIRES", "PAARE" };
static const char *T_G5[L_COUNT] = { "CONTI", "MATH", "CUENTAS", "CALCUL", "RECHNEN" };
static const char *T_G6[L_COUNT] = { "TROVA", "FIND", "ENCUENTRA", "TROUVE", "FINDE" };
static const char *T_G7[L_COUNT] = { "COMPONI", "SPELL", "DELETREA", "EPELER", "WORT" };

static const char *T_ALPHA[L_COUNT] = {
    "ABCDEFGHILMNOPQRSTUVZ", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
};
static const char *g_letters; static int g_nlet;
static void detect_lang(void) {
    char buf[8] = { 0 };
    nv_lang(buf, sizeof buf);
    if      (buf[0] == 'i' && buf[1] == 't') g_lang = L_IT;
    else if (buf[0] == 'e' && buf[1] == 's') g_lang = L_ES;
    else if (buf[0] == 'f' && buf[1] == 'r') g_lang = L_FR;
    else if (buf[0] == 'd' && buf[1] == 'e') g_lang = L_DE;
    else                                     g_lang = L_EN;
    g_letters = T_ALPHA[g_lang]; g_nlet = slen(g_letters);
}

// ---- objects (real emoji assets on SD) ----------------------------------------------------------
#define O_COUNT 101
static const char *IMG_NAME[O_COUNT] = {
    "sun","moon","star","cloud","tree","flower","leaf","apple","banana","grapes","strawberry",
    "carrot","cake","icecream","pizza","egg","house","ball","car","boat","train","book","key",
    "cup","hat","shoe","clock","balloon","gift","bell","umbrella","guitar","cat","dog","fish",
    "bird","bee","frog","lion","bear","rabbit","butterfly","heart","dice","duck",
    "cow","horse","pig","monkey","elephant","tiger","penguin","owl","turtle","panda",
    "cherry","watermelon","orange","lemon","corn","bread","cookie","hamburger",
    "pencil","bicycle","plane","rocket","crown","rainbow",
    "dolphin","snail","ladybug","snake","spider","mouse","fox","wolf","koala","chicken","goat","deer",
    "peach","pear","pineapple","coconut","tomato","mushroom","cheese","candy",
    "drum","robot","phone","bus",
    "shark","dragon","castle","cactus","unicorn","whale","mountain","kite",
};
static const char *OBJ_W[O_COUNT][L_COUNT] = {
    {"SOLE","SUN","SOL","SOLEIL","SONNE"},{"LUNA","MOON","LUNA","LUNE","MOND"},
    {"STELLA","STAR","ESTRELLA","ETOILE","STERN"},{"NUVOLA","CLOUD","NUBE","NUAGE","WOLKE"},
    {"ALBERO","TREE","ARBOL","ARBRE","BAUM"},{"FIORE","FLOWER","FLOR","FLEUR","BLUME"},
    {"FOGLIA","LEAF","HOJA","FEUILLE","BLATT"},{"MELA","APPLE","MANZANA","POMME","APFEL"},
    {"BANANA","BANANA","PLATANO","BANANE","BANANE"},{"UVA","GRAPES","UVA","RAISIN","TRAUBE"},
    {"FRAGOLA","STRAWBERRY","FRESA","FRAISE","ERDBEERE"},{"CAROTA","CARROT","ZANAHORIA","CAROTTE","KAROTTE"},
    {"TORTA","CAKE","PASTEL","GATEAU","KUCHEN"},{"GELATO","ICE CREAM","HELADO","GLACE","EIS"},
    {"PIZZA","PIZZA","PIZZA","PIZZA","PIZZA"},{"UOVO","EGG","HUEVO","OEUF","EI"},
    {"CASA","HOUSE","CASA","MAISON","HAUS"},{"PALLA","BALL","PELOTA","BALLON","BALL"},
    {"AUTO","CAR","COCHE","VOITURE","AUTO"},{"BARCA","BOAT","BARCO","BATEAU","BOOT"},
    {"TRENO","TRAIN","TREN","TRAIN","ZUG"},{"LIBRO","BOOK","LIBRO","LIVRE","BUCH"},
    {"CHIAVE","KEY","LLAVE","CLE","SCHLUESSEL"},{"TAZZA","CUP","TAZA","TASSE","TASSE"},
    {"CAPPELLO","HAT","SOMBRERO","CHAPEAU","HUT"},{"SCARPA","SHOE","ZAPATO","CHAUSSURE","SCHUH"},
    {"OROLOGIO","CLOCK","RELOJ","HORLOGE","UHR"},{"PALLONCINO","BALLOON","GLOBO","BALLON","LUFTBALLON"},
    {"REGALO","GIFT","REGALO","CADEAU","GESCHENK"},{"CAMPANA","BELL","CAMPANA","CLOCHE","GLOCKE"},
    {"OMBRELLO","UMBRELLA","PARAGUAS","PARAPLUIE","SCHIRM"},{"CHITARRA","GUITAR","GUITARRA","GUITARE","GITARRE"},
    {"GATTO","CAT","GATO","CHAT","KATZE"},{"CANE","DOG","PERRO","CHIEN","HUND"},
    {"PESCE","FISH","PEZ","POISSON","FISCH"},{"UCCELLO","BIRD","PAJARO","OISEAU","VOGEL"},
    {"APE","BEE","ABEJA","ABEILLE","BIENE"},{"RANA","FROG","RANA","GRENOUILLE","FROSCH"},
    {"LEONE","LION","LEON","LION","LOEWE"},{"ORSO","BEAR","OSO","OURS","BAER"},
    {"CONIGLIO","RABBIT","CONEJO","LAPIN","HASE"},{"FARFALLA","BUTTERFLY","MARIPOSA","PAPILLON","FALTER"},
    {"CUORE","HEART","CORAZON","COEUR","HERZ"},{"DADO","DICE","DADO","DE","WUERFEL"},
    {"ANATRA","DUCK","PATO","CANARD","ENTE"},
    {"MUCCA","COW","VACA","VACHE","KUH"},{"CAVALLO","HORSE","CABALLO","CHEVAL","PFERD"},
    {"MAIALE","PIG","CERDO","COCHON","SCHWEIN"},{"SCIMMIA","MONKEY","MONO","SINGE","AFFE"},
    {"ELEFANTE","ELEPHANT","ELEFANTE","ELEPHANT","ELEFANT"},{"TIGRE","TIGER","TIGRE","TIGRE","TIGER"},
    {"PINGUINO","PENGUIN","PINGUINO","PINGOUIN","PINGUIN"},{"GUFO","OWL","BUHO","HIBOU","EULE"},
    {"TARTARUGA","TURTLE","TORTUGA","TORTUE","SCHILDKROETE"},{"PANDA","PANDA","PANDA","PANDA","PANDA"},
    {"CILIEGIA","CHERRY","CEREZA","CERISE","KIRSCHE"},{"ANGURIA","WATERMELON","SANDIA","PASTEQUE","MELONE"},
    {"ARANCIA","ORANGE","NARANJA","ORANGE","ORANGE"},{"LIMONE","LEMON","LIMON","CITRON","ZITRONE"},
    {"MAIS","CORN","MAIZ","MAIS","MAIS"},{"PANE","BREAD","PAN","PAIN","BROT"},
    {"BISCOTTO","COOKIE","GALLETA","BISCUIT","KEKS"},{"PANINO","BURGER","HAMBURGUESA","BURGER","BURGER"},
    {"MATITA","PENCIL","LAPIZ","CRAYON","STIFT"},{"BICICLETTA","BICYCLE","BICICLETA","VELO","FAHRRAD"},
    {"AEREO","PLANE","AVION","AVION","FLUGZEUG"},{"RAZZO","ROCKET","COHETE","FUSEE","RAKETE"},
    {"CORONA","CROWN","CORONA","COURONNE","KRONE"},{"ARCOBALENO","RAINBOW","ARCOIRIS","ARCENCIEL","REGENBOGEN"},
    {"DELFINO","DOLPHIN","DELFIN","DAUPHIN","DELFIN"},{"LUMACA","SNAIL","CARACOL","ESCARGOT","SCHNECKE"},
    {"COCCINELLA","LADYBUG","MARIQUITA","COCCINELLE","MARIENKAEFER"},{"SERPENTE","SNAKE","SERPIENTE","SERPENT","SCHLANGE"},
    {"RAGNO","SPIDER","ARANA","ARAIGNEE","SPINNE"},{"TOPO","MOUSE","RATON","SOURIS","MAUS"},
    {"VOLPE","FOX","ZORRO","RENARD","FUCHS"},{"LUPO","WOLF","LOBO","LOUP","WOLF"},
    {"KOALA","KOALA","KOALA","KOALA","KOALA"},{"GALLINA","CHICKEN","GALLINA","POULE","HUHN"},
    {"CAPRA","GOAT","CABRA","CHEVRE","ZIEGE"},{"CERVO","DEER","CIERVO","CERF","HIRSCH"},
    {"PESCA","PEACH","MELOCOTON","PECHE","PFIRSICH"},{"PERA","PEAR","PERA","POIRE","BIRNE"},
    {"ANANAS","PINEAPPLE","PINA","ANANAS","ANANAS"},{"COCCO","COCONUT","COCO","NOIXCOCO","KOKOSNUSS"},
    {"POMODORO","TOMATO","TOMATE","TOMATE","TOMATE"},{"FUNGO","MUSHROOM","SETA","CHAMPIGNON","PILZ"},
    {"FORMAGGIO","CHEESE","QUESO","FROMAGE","KAESE"},{"CARAMELLA","CANDY","CARAMELO","BONBON","BONBON"},
    {"TAMBURO","DRUM","TAMBOR","TAMBOUR","TROMMEL"},{"ROBOT","ROBOT","ROBOT","ROBOT","ROBOTER"},
    {"TELEFONO","PHONE","TELEFONO","TELEPHONE","TELEFON"},{"AUTOBUS","BUS","AUTOBUS","BUS","BUS"},
    {"SQUALO","SHARK","TIBURON","REQUIN","HAI"},{"DRAGO","DRAGON","DRAGON","DRAGON","DRACHE"},
    {"CASTELLO","CASTLE","CASTILLO","CHATEAU","SCHLOSS"},{"CACTUS","CACTUS","CACTUS","CACTUS","KAKTUS"},
    {"UNICORNO","UNICORN","UNICORNIO","LICORNE","EINHORN"},{"BALENA","WHALE","BALLENA","BALEINE","WAL"},
    {"MONTAGNA","MOUNTAIN","MONTANA","MONTAGNE","BERG"},{"AQUILONE","KITE","COMETA","CERFVOLANT","DRACHEN"},
};
static char obj_initial(int kind) { return OBJ_W[kind][g_lang][0]; }
static void draw_icon(int kind, int cx, int cy, int s) { nv_gfx_image(IMG_NAME[kind], cx - s, cy - s, 2 * s, 2 * s); }
// Icons are drawn at a fixed size — no scaling animation (objects must never move/resize).
static void draw_icon_pop(int kind, int cx, int cy, int base) { draw_icon(kind, cx, cy, base); }

// ---- screens + session --------------------------------------------------------------------------
enum { SC_MENU, SC_INITIAL, SC_COUNT, SC_ALPHA, SC_MATCH, SC_SUM, SC_ODD, SC_SPELL, SC_OVER, SC_SCORES, SC_SETTINGS, SC_NAME };
enum { MODE_LETTERS, MODE_NUMBERS, MODE_MIXED };
#define N_GAMES 7
#define LIVES 3
// Correct-rounds needed to level up, per game (index = the g_game passed to start_game: INIZIALE,
// CONTA, ALPHA, MATCH, SUM, ODD, SPELL). on_correct() fires once per COMPLETED round — for the
// single-tap games that's one tap, but ALPHA/MATCH/SPELL build a whole multi-tap sequence/board/word
// before it fires once, so they'd need far more real play to level up at the same threshold. Smaller
// steps here keep the pace roughly comparable across all 7 games.
static const int LEVEL_STEP_G[N_GAMES] = { 5, 5, 4, 3, 5, 5, 4 };

static void start_game(int game);   // fwd (used by the game-over replay button)
static int g_screen = SC_MENU, g_mode = MODE_MIXED;
static int g_stars;                 // persistent collectible
static int g_hi[N_GAMES][10];       // persistent top-10 per game (desc)
static int g_game, g_lvl, g_score, g_lives, g_prog, g_over_rank, g_streak;
static int fb_good_until, fb_bad_until, fb_good_start, g_advance, g_pending_over, g_lvup_until;
static int g_bad_start;

static int g_voice = 1;   // TTS on/off (toggled in Settings; packed into the save blob's lang slot)
static int g_quit = 0;    // set by the menu Exit button -> main loop returns to the launcher
// ---- speech clock: every utterance advances g_speak_until to its estimated end, so the next
// spoken moment (and the round advance) can WAIT for it — nothing gets cut. Duration ~ text length.
static int g_speak_until = 0;
static int est_ms(const char *s) {
    int letters = 0, spaces = 0;
    for (const char *p = s; *p; p++) { if (*p == ' ') spaces++; else letters++; }
    int ms = 150 + letters * 72 + spaces * 150;
    return ms < 320 ? 320 : ms > 2000 ? 2000 : ms;
}
static void say(const char *w) { if (!g_voice || !w || !w[0]) return; nv_speak(w, LANG_LC[g_lang]); g_speak_until = nv_millis() + est_ms(w); }
static void say_obj(int kind) { say(OBJ_W[kind][g_lang]); }
static void say_num(int n) { say(num_word(n)); }
// Speak a single letter: the pack aliases every lett_<x> corpus clip under the bare 1-char slug "x"
// (tools/merge_local.py), so passing the char works for all 26 letters, both it/en.
static void say_letter(char c) {
    if (!g_voice || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return;
    char b[2] = { c, 0 }; nv_speak(b, LANG_LC[g_lang]); g_speak_until = nv_millis() + 480;
}
static void say_tile(char c) { if (c >= '0' && c <= '9') say_num(c - '0'); else say_letter(c); }
static int g_cd_n = 0, g_cd_next = 0, g_cd_step = 0;   // start-of-game 3-2-1 countdown (n>0 = counting)
// Gratifying / consolation phrases (only slugs present in the it/en voice pack).
// Gender-neutral only — no "bravissimo/a", no address terms (we never ask/know the player's gender).
static const char *PRAISE_IT[8] = { "BRAVO","PERFETTO","SUPER","EVVIVA","FANTASTICO","OTTIMO","MAGNIFICO","GRANDE" };
static const char *PRAISE_EN[6] = { "SUPER","WOW","GREAT","PERFECT","BRAVO","AWESOME" };
static const char *NEG_IT[3]    = { "RIPROVA","QUASI","CORAGGIO" };
static const char *NEG_EN[3]    = { "ALMOST","AGAIN","OOPS" };
static int g_vfx_at = 0, g_vfx_kind = 0;   // scheduled reward(0)/consolation(1) voice; 0 = none
static int g_last_pr = -1, g_last_ng = -1; // last phrase index -> never repeat back-to-back (variety)
static void say_praise(void) {
    int n = g_lang == L_EN ? 6 : g_lang == L_IT ? 8 : 0; if (!n) return;
    int k; do { k = rnd(0, n - 1); } while (n > 1 && k == g_last_pr); g_last_pr = k;
    say(g_lang == L_EN ? PRAISE_EN[k] : PRAISE_IT[k]); }
static void say_neg(void) {
    if (g_lang != L_IT && g_lang != L_EN) return;
    int k; do { k = rnd(0, 2); } while (k == g_last_ng); g_last_ng = k;
    say(g_lang == L_EN ? NEG_EN[k] : NEG_IT[k]); }
// Schedule the reward/consolation voice to fire when the CURRENT utterance ends (the tapped number,
// or immediately if nothing is speaking) — so the number is heard in full first, then the phrase.
static void schedule_vfx(int kind) { if (!g_voice) return; int now = nv_millis();
    g_vfx_at = g_speak_until > now ? g_speak_until : now; g_vfx_kind = kind; }
// Beautiful polyphonic SFX (tools/gen_abc_sfx.py). Durations must match the generated WAVs so the
// SFX is fitted onto the same audio timeline as the voice — they NEVER play at once.
#define WIN_MS 430
#define LVL_MS 1119
#define LOSE_MS 540
static int g_sfx_at = 0; static const char *g_sfx_name = 0;   // one pending SFX (fires from the loop)
static void schedule_sfx(const char *name, int dur) {         // plays when the current audio ends
    int now = nv_millis();
    g_sfx_at = g_speak_until > now ? g_speak_until : now;
    g_sfx_name = name; g_speak_until = g_sfx_at + dur;         // audio busy until the SFX ends
}
// Which objects have a spoken clip (generated from the packs). IT is missing 27 nouns (no audio in
// the corpus); EN has all. So spoken games pick only voiceable objects -> the word ALWAYS plays.
static const char OBJ_VOICE_IT[O_COUNT + 1] = "11101111110011111111111111101101111100111110011110100010001110111101110011111011010011010011101100010";
// EN covers every object except 3 of the newest wave (cactus/unicorn/kite have no corpus clip at all).
static const char OBJ_VOICE_EN[O_COUNT + 1] = "11111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111100110";
static int obj_voiceable(int kind) {
    if (kind < 0 || kind >= O_COUNT) return 1;
    if (g_lang == L_IT) return OBJ_VOICE_IT[kind] == '1';
    if (g_lang == L_EN) return OBJ_VOICE_EN[kind] == '1';
    return 1;   // es/fr/de have no pack at all (silent regardless — don't restrict)
}
static int rnd_obj(void) {   // random object that can be SPOKEN (when voice on), so reveals aren't silent
    if (!g_voice) return rnd(0, O_COUNT - 1);
    for (int t = 0; t < 40; t++) { int k = rnd(0, O_COUNT - 1); if (obj_voiceable(k)) return k; }
    return rnd(0, O_COUNT - 1);
}

// ---- no-repeat memory: keep the last N shown targets per session so a round never re-poses the
// same letter/number/word/object as a recent one. It's a sliding ring: push each new target, and
// generators retry (bounded) until their candidate isn't in it. Cap is set per game in start_game
// and kept below the pool size so there's always a fresh choice; reset when a new session starts.
#define RECENT_MAX 24
static int g_recent[RECENT_MAX], g_recent_n, g_recent_cap;
static void recent_reset(int cap) { g_recent_n = 0; g_recent_cap = cap < 0 ? 0 : (cap > RECENT_MAX ? RECENT_MAX : cap); }
static int recent_has(int key) { for (int i = 0; i < g_recent_n; i++) if (g_recent[i] == key) return 1; return 0; }
static void recent_push(int key) {
    if (g_recent_cap < 1) return;
    if (g_recent_n < g_recent_cap) { g_recent[g_recent_n++] = key; return; }
    for (int i = 1; i < g_recent_cap; i++) g_recent[i - 1] = g_recent[i];
    g_recent[g_recent_cap - 1] = key;
}
// Per-game ring depth (INIZIALE, CONTA, ALPHA, MATCH, SUM, ODD, SPELL). Small for the number games
// (tiny pools at low levels), large for the object/word games.
static const int RECENT_CAP_G[N_GAMES] = { 12, 4, 5, 3, 6, 12, 10 };

// persistence blob: [magic, stars, hi[7][10]]
#define SAVE_MAGIC 0x41424334
static char g_name[16] = "";
static int g_save_buf[3 + 4 + N_GAMES * 10];   // [magic, lang, stars, name(16B=4 int), hi[7][10]]
static void save_state(void) {
    // buf[1] low byte = language; high byte = voice sentinel (1=on, 2=off; 0 = legacy save -> default on)
    g_save_buf[0] = SAVE_MAGIC; g_save_buf[1] = (g_lang & 0xFF) | ((g_voice ? 1 : 2) << 8); g_save_buf[2] = g_stars;
    memcpy(&g_save_buf[3], g_name, 16);
    int p = 7; for (int g = 0; g < N_GAMES; g++) for (int k = 0; k < 10; k++) g_save_buf[p++] = g_hi[g][k];
    nv_save("save.dat", g_save_buf, (int)sizeof g_save_buf);
}
static void load_state(void) {
    int n = nv_load("save.dat", g_save_buf, (int)sizeof g_save_buf);
    if (n == (int)sizeof g_save_buf && g_save_buf[0] == SAVE_MAGIC) {
        int l = g_save_buf[1] & 0xFF; if (l >= 0 && l < L_COUNT) g_lang = l;   // saved language overrides system
        int vf = (g_save_buf[1] >> 8) & 0xFF; g_voice = (vf == 2) ? 0 : 1;     // legacy (0) -> voice on
        g_stars = g_save_buf[2];
        memcpy(g_name, &g_save_buf[3], 16); g_name[15] = 0;
        int p = 7; for (int g = 0; g < N_GAMES; g++) for (int k = 0; k < 10; k++) g_hi[g][k] = g_save_buf[p++];
    } else {
        g_stars = 0; g_name[0] = 0; g_voice = 1; for (int g = 0; g < N_GAMES; g++) for (int k = 0; k < 10; k++) g_hi[g][k] = 0;
    }
}
static void reset_records(void) { for (int g = 0; g < N_GAMES; g++) for (int k = 0; k < 10; k++) g_hi[g][k] = 0; save_state(); }
static int record_score(int game, int score) {
    for (int i = 0; i < 10; i++) if (score > g_hi[game][i]) {
        for (int k = 9; k > i; k--) g_hi[game][k] = g_hi[game][k - 1];
        g_hi[game][i] = score; return i;
    }
    return -1;
}

static void tone_tap(void)  { nv_gfx_tone(760, 12); }
static void tone_step(void) { nv_gfx_tone(988, 60); }

// ---- confetti RAIN: fall straight down at constant speed (no gravity/no explosion), fluttering ----
// via a sine of the fall distance (classic 16-bit sway trick — zero per-particle state). Each drop is
// one nv_gfx_rect (cheapest primitive). Culled off the bottom edge; bounded -> zero idle cost.
#define MAXP 20
static struct { int x, y, vy, col; short life; } g_p[MAXP];
static int g_pn;
static void emit_rain(int n) {
    int pal[6]; pal[0] = C_ACC[0]; pal[1] = C_ACC[1]; pal[2] = C_ACC[2]; pal[3] = C_ACC[3]; pal[4] = C_STAR; pal[5] = C_ACC[5];
    for (int k = 0; k < n && g_pn < MAXP; k++) {
        g_p[g_pn].x = rnd(0, W) << 8;
        g_p[g_pn].y = -(rnd(0, H / 3)) << 8;          // stagger the start just above the top edge
        g_p[g_pn].vy = rnd(11, 16) << 8;              // fast, steady fall (px/frame, 8.8)
        g_p[g_pn].life = 160; g_p[g_pn].col = pal[rnd(0, 5)];
        g_pn++;
    }
    mark_dirty();
}
static void particles_frame(void) {                 // move + cull, then draw — once per rendered frame
    int w = 0;
    for (int i = 0; i < g_pn; i++) {
        g_p[i].y += g_p[i].vy; g_p[i].life--;          // constant velocity = rain (no acceleration)
        if (g_p[i].life > 0 && (g_p[i].y >> 8) < H + 8) { if (w != i) g_p[w] = g_p[i]; w++; }
    }
    g_pn = w;
    for (int i = 0; i < g_pn; i++) {
        int py = g_p[i].y >> 8;
        int px = (g_p[i].x >> 8) + isin(py * 3 + i * 40) * 4 / 1024;   // gentle side-to-side flutter
        nv_gfx_rect(px - 3, py - 3, 6, 6, g_p[i].col);
    }
}

// Rich sound effects via nv_sound(): polyphonic WAVs from apps/abc123/snd/ (win / levelup / lose),
// built by tools/build_abc_sounds.py. Taps and progress steps keep the cheap built-in tone.
static int animating(void) { int now = nv_millis();
    return g_pn > 0 || now < fb_good_until || now < fb_bad_until || now < g_lvup_until; }
static int good_lock(void) { return nv_millis() < fb_good_until; }

static void feedback_good(void) { int now = nv_millis(); fb_good_start = now; fb_good_until = now + 700;
    if (g_speak_until + 200 > fb_good_until) fb_good_until = g_speak_until + 200;   // cover the pending SFX/voice timeline
    g_advance = 1; }
static void feedback_bad(void)  { g_bad_start = nv_millis(); fb_bad_until = g_bad_start + 470; mark_dirty(); }

// correct answer: score + collectible + progress; level up every LEVEL_STEP.
static void on_correct(void) {
    g_streak++;
    g_score += 10 * g_lvl + 2 * (g_streak - 1);   // streak bonus rewards a run of correct answers
    g_stars++; g_prog++;
    int now = nv_millis();
    int lvup = 0;
    if (g_prog >= LEVEL_STEP_G[g_game]) { g_prog = 0; g_lvl++; g_lvup_until = now + 900; lvup = 1; }
    save_state();
    // Reward audio: level-up keeps the fanfare; otherwise a spoken praise (scheduled a beat later so
    // the number the child just tapped is heard first). Voice off -> the win jingle.
    if (lvup) schedule_sfx("levelup", LVL_MS);                            // triumphant fanfare (no voice)
    else { schedule_sfx("win", WIN_MS); if (g_voice) schedule_vfx(0); }   // bright chime, THEN praise — sequenced
    emit_rain(lvup ? 16 : 10);   // celebratory confetti raining down from the top; bigger on a level-up
    feedback_good();
}
static void on_wrong(void) {
    g_streak = 0;
    if (g_lives > 0) g_lives--;
    schedule_sfx("lose", LOSE_MS);              // soft lose chime, always — mirrors on_correct's chime+voice
    if (g_voice) schedule_vfx(1);               // then the spoken "riprova/quasi", once the chime ends
    feedback_bad();
    if (g_lives <= 0) g_pending_over = 1;
}

// ---- layout helpers -----------------------------------------------------------------------------
static void home_rect(int *x, int *y, int *w, int *h) { *x = 16; *y = 10; *w = 112; *h = H / 9 - 8; }   // bigger touch target for little fingers
static void exit_rect(int *x, int *y, int *w, int *h)     { *x = 16;  *y = 12; *w = 76; *h = H / 9 - 8; }   // in the top bar, like the game back button
static void settings_rect(int *x, int *y, int *w, int *h) { *x = 100; *y = 12; *w = 76; *h = H / 9 - 8; }   // gear, right of Exit
static void answer3_rect(int i, int *x, int *y, int *w, int *h) {
    int gap = W / 26, bw = (W - 2 * MX - 2 * gap) / 3, bh = H / 4;
    *x = MX + i * (bw + gap); *y = H - MY - bh; *w = bw; *h = bh;
}
// N answer cards in one bottom row (INIZIALE: count grows with level). Cards shrink as N rises.
static void answer_n_rect(int i, int n, int *x, int *y, int *w, int *h) {
    if (n < 1) n = 1;
    int gap = W / 40, bw = (W - 2 * MX - (n - 1) * gap) / n, bh = H / 4;
    *x = MX + i * (bw + gap); *y = H - MY - bh; *w = bw; *h = bh;
}
static void menu_mode_rect(int i, int *x, int *y, int *w, int *h) {
    int gap = W / 40, bw = (W - 2 * MX - 2 * gap) / 3, bh = H / 11;
    *x = MX + i * (bw + gap); *y = MY + H / 8; *w = bw; *h = bh;
}
static void menu_game_rect(int i, int *x, int *y, int *w, int *h) {
    int cols = 4, gap = W / 60;
    int gy0 = MY + H / 8 + H / 11 + H / 26;
    int bw = (W - 2 * MX - (cols - 1) * gap) / cols;
    int bh = (H - MY - gy0 - gap) / 2;
    int r = i / cols, c = i % cols;
    *x = MX + c * (bw + gap); *y = gy0 + r * (bh + gap); *w = bw; *h = bh;
}
#define MAX_SEQ 7
static int al_n;
static void alpha_tile_rect(int cell, int *x, int *y, int *w, int *h) {
    int cols = 5, gap = W / 60, cw = (W - 2 * MX) / cols, top = HUD + H / 6, ch = (H - MY - top) / 2;
    int r = cell / cols, c = cell % cols;
    *x = MX + c * cw + gap; *y = top + r * ch + gap; *w = cw - 2 * gap; *h = ch - 2 * gap;
}
#define MAX_TILES 20
static int mt_tiles;
static int mt_cols(void) { return mt_tiles <= 6 ? 3 : (mt_tiles <= 12 ? 4 : 5); }
static void match_tile_rect(int i, int *x, int *y, int *w, int *h) {
    int cols = mt_cols(), gap = W / 80, rows = (mt_tiles + cols - 1) / cols;
    int cw = (W - 2 * MX) / cols, top = HUD + H / 16, ch = (H - MY - top) / (rows < 2 ? 2 : rows);
    int r = i / cols, c = i % cols;
    *x = MX + c * cw + gap; *y = top + r * ch + gap; *w = cw - 2 * gap; *h = ch - 2 * gap;
}
static void odd_rect(int i, int *x, int *y, int *w, int *h) {
    int gap = W / 40, cw = (W - 2 * MX - gap) / 2, top = HUD + H / 4, ch = (H - MY - top - gap) / 2;
    int r = i / 2, c = i % 2;
    *x = MX + c * (cw + gap); *y = top + r * (ch + gap); *w = cw; *h = ch;
}
#define MAXW 10
static void spell_slot_rect(int i, int n, int *x, int *y, int *w, int *h) {
    int gap = W / 90, tw = (W - 2 * MX) / (n > 6 ? n : 6), sx = W / 2 - (n * tw) / 2;
    *x = sx + i * tw + gap; *y = H * 52 / 100; *w = tw - 2 * gap; *h = H / 8;
}
static void spell_tile_rect(int i, int n, int *x, int *y, int *w, int *h) {
    int gap = W / 90, tw = (W - 2 * MX) / (n > 6 ? n : 6), sx = W / 2 - (n * tw) / 2;
    *x = sx + i * tw + gap; *y = H - MY - H / 7; *w = tw - 2 * gap; *h = H / 7;
}

// ---- background + HUD + feedback ----------------------------------------------------------------
static int cur_bg(void) { return (g_screen >= SC_INITIAL && g_screen <= SC_SPELL) ? PASTEL[g_lvl % 8] : C_BG; }
static void draw_bg(void) {
    nv_gfx_clear(cur_bg());                                              // flat fill — cheapest bg (1 host call)
    if (g_screen == SC_MENU) nv_gfx_rect(0, 0, W, MY + H / 12, C_BG2);   // menu header band
}
static void draw_stars_badge(int cx, int cy) {
    draw_star(cx, cy, 22, C_STAR);
    nv_gfx_text(cx + 28, cy - 16, istr(g_stars), C_INK, 4);
}
static char g_lvbuf[10];
static const char *lv_str(void) {
    g_lvbuf[0] = 'L'; g_lvbuf[1] = 'V'; g_lvbuf[2] = ' ';
    const char *n = istr(g_lvl); int p = 3; while (*n) g_lvbuf[p++] = *n++; g_lvbuf[p] = 0; return g_lvbuf;
}
static void draw_hud(void) {
    int bh = H / 9, bary = 6, cy = bary + bh / 2;
    fill_round(6, bary, W - 12, bh, 16, C_SHADOW);          // bar shadow/border
    fill_round(10, bary, W - 20, bh - 4, 14, C_PANEL);      // bar face
    int x, y, w, h; home_rect(&x, &y, &w, &h);
    draw_back_btn(x, y, w, h);
    for (int i = 0; i < LIVES; i++) draw_heart(x + w + 28 + i * 40, cy, 16, i < g_lives ? C_BAD : C_SHADOW);
    text_cs(W * 40 / 100, cy, lv_str(), C_INK, 4);
    for (int i = 0; i < LEVEL_STEP_G[g_game]; i++) nv_gfx_circle(W * 47 / 100 + i * (W / 46), cy, 7, i < g_prog ? C_GOOD : C_DIM);
    text_cs(W * 72 / 100, cy, istr(g_score), C_INK, 5);
    draw_stars_badge(W - MX - 66, cy);
}
// Centered rounded panel behind the feedback — the overlay covers the play area so the scene under
// it is skipped entirely (see render). Flat shadow + rounded face: cheap and clean.
static void draw_panel(int col) {
    int px = W * 14 / 100, py = H * 18 / 100, pw = W * 72 / 100, ph = H * 60 / 100;
    nv_gfx_rect(px + 6, py + 8, pw, ph, C_SHADOW);
    fill_round(px, py, pw, ph, 30, C_INK);
    fill_round(px + 8, py + 8, pw - 16, ph - 16, 24, col);
}
static void draw_feedback(void) {
    int now = nv_millis();
    if (now < fb_bad_until) {                              // error overlay — a quick friendly shake
        int el = now - g_bad_start, sh = (el < 300) ? isin(now * 4) * 11 / 1024 : 0;
        draw_panel(C_BAD);
        int cx = W / 2 + sh, cy = H * 38 / 100;
        nv_gfx_circle(cx, cy, H / 9, C_PANEL);
        draw_x(cx, cy, H / 16, 11, C_BAD);
        text_cs(W / 2 + sh, H * 66 / 100, T_BAD[g_lang], C_PANEL, 6);
    }
    if (now < fb_good_until) {                             // win overlay — the reward star pops in
        draw_panel(C_GOOD);
        int rr = (H / 8) * pop_scale(now - fb_good_start, 300) / 256;
        draw_star(W / 2, H * 38 / 100, rr, C_STAR);        // real star art, bounces up to full size
        const char *gt;
        if (now < g_lvup_until) gt = T_LVUP[g_lang];
        else if (g_streak >= 3) { static char cb[16]; int p = 0; const char *c = "COMBO x"; while (*c) cb[p++] = *c++; const char *n = istr(g_streak); while (*n) cb[p++] = *n++; cb[p] = 0; gt = cb; }
        else gt = T_GOOD[g_lang];
        text_cs(W / 2, H * 66 / 100, gt, C_PANEL, 6);
    }
}

// ================================================================================================
//  GAME 1 — INIZIALE
// ================================================================================================
#define IN_MAX_OPT 6
static int in_obj, in_n; static char in_opt[IN_MAX_OPT];
static void new_initial(void) {
    in_n = 2 + g_lvl; if (in_n > IN_MAX_OPT) in_n = IN_MAX_OPT; if (in_n > g_nlet) in_n = g_nlet;   // +1 choice each level
    in_obj = rnd_obj(); for (int t = 0; t < 40 && recent_has(in_obj); t++) in_obj = rnd_obj();
    recent_push(in_obj);
    char correct = obj_initial(in_obj);   // voiceable object -> the word always plays
    // Difficulty ramp: at level 1 distractors come from far away in the alphabet (easy to tell apart
    // on sight); the minimum distance shrinks each level, so by ~level 7 any letter (incl. neighbours
    // of `correct`) is fair game. A bounded retry count guarantees termination for edge-of-alphabet
    // letters where few far-away candidates exist.
    int ci = 0; for (int k = 0; k < g_nlet; k++) if (g_letters[k] == correct) { ci = k; break; }
    int mindist = 8 - g_lvl; if (mindist > g_nlet / 3) mindist = g_nlet / 3; if (mindist < 1) mindist = 1;
    int slot = rnd(0, in_n - 1); in_opt[slot] = correct;
    for (int i = 0; i < in_n; i++) { if (i == slot) continue; char c; int ok; int tries = 0;
        do { int k = rnd(0, g_nlet - 1); c = g_letters[k]; int d = k - ci; if (d < 0) d = -d;
             ok = (c != correct) && (d >= mindist || tries > 30);
             for (int j = 0; j < in_n; j++) if (j != i && in_opt[j] == c) ok = 0;
             tries++; } while (!ok);
        in_opt[i] = c; }
    say_obj(in_obj);   // hear the object
    mark_dirty();
}
static void hit_initial(int tx, int ty) {
    if (good_lock()) return;
    { int s = imin(W, H) / 5; if (inrect(W / 2 - s, H * 42 / 100 - s, 2 * s, 2 * s, tx, ty)) {   // tap the picture -> hear it again
        say_obj(in_obj); return; } }
    for (int i = 0; i < in_n; i++) { int x, y, w, h; answer_n_rect(i, in_n, &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) { say_letter(in_opt[i]);   // hear the letter you pressed
            if (in_opt[i] == obj_initial(in_obj)) on_correct(); else { g_wrong_i = i; on_wrong(); } return; } }
}
static void draw_initial(void) {
    draw_bg();
    if (good_lock()) text_fit(W / 2, HUD + H / 18, OBJ_W[in_obj][g_lang], C_GOOD, W - 2 * MX, H / 10, 9);
    else text_fit(W / 2, HUD + H / 30, T_Q1[g_lang], C_DIM, W - 2 * MX, H / 14, 5);
    draw_icon_pop(in_obj, W / 2, H * 42 / 100, imin(W, H) / 5);
    int bad = nv_millis() < fb_bad_until;
    for (int i = 0; i < in_n; i++) { int x, y, w, h; answer_n_rect(i, in_n, &x, &y, &w, &h);
        int ai = i % 8;
        int ok = good_lock() && in_opt[i] == obj_initial(in_obj), wr = bad && i == g_wrong_i;
        card(x, y, w, h, ok ? C_GOOD : (wr ? C_BAD : C_PANEL), C_ACC[ai], chstr(in_opt[i]), (ok || wr) ? C_PANEL : C_ACC[ai]); }
    draw_hud();
}

// ================================================================================================
//  GAME 2 — CONTA
// ================================================================================================
static int cn_n, cn_kind, cn_opt[3];
static void new_count(void) {
    int maxn = 3 + g_lvl * 2; if (maxn > 20) maxn = 20;
    cn_n = rnd(1, maxn); for (int t = 0; t < 40 && recent_has(cn_n); t++) cn_n = rnd(1, maxn);
    recent_push(cn_n);
    cn_kind = rnd(0, O_COUNT - 1);
    int slot = rnd(0, 2); cn_opt[slot] = cn_n;
    for (int i = 0; i < 3; i++) { if (i == slot) continue; int v, ok;
        do { int off = rnd(1, 3) * (rnd(0, 1) ? 1 : -1); v = cn_n + off; if (v < 1) v = cn_n + rnd(1, 3);
             ok = (v != cn_n); for (int j = 0; j < 3; j++) if (j != i && cn_opt[j] == v) ok = 0; } while (!ok);
        cn_opt[i] = v; }
    mark_dirty();
}
static void hit_count(int tx, int ty) {
    if (good_lock()) return;
    for (int i = 0; i < 3; i++) { int x, y, w, h; answer3_rect(i, &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) { say_num(cn_opt[i]);   // hear the number you pressed
            if (cn_opt[i] == cn_n) on_correct(); else { g_wrong_i = i; on_wrong(); }
            return; } }
}
// Draw n copies of an icon auto-gridded to FILL the box (ax,ay,aw,ah): pick the column count that
// maximizes cell size, size icons to the cell, center a partial last row.
static void draw_icons_fit(int kind, int n, int ax, int ay, int aw, int ah) {
    if (n < 1 || aw < 1 || ah < 1) return;
    int cols = 1, best = 0;
    for (int c = 1; c <= n; c++) { int rows = (n + c - 1) / c; int cell = imin(aw / c, ah / rows); if (cell > best) { best = cell; cols = c; } }
    int rows = (n + cols - 1) / cols, cw = aw / cols, ch = ah / rows, isz = imin(cw, ch) * 42 / 100;
    for (int i = 0; i < n; i++) {
        int r = i / cols, c = i % cols;
        int rowitems = (r == rows - 1) ? (n - r * cols) : cols;
        int rgx = ax + (aw - rowitems * cw) / 2;
        draw_icon(kind, rgx + c * cw + cw / 2, ay + r * ch + ch / 2, isz);
    }
}
static void draw_count(void) {
    draw_bg();
    text_fit(W / 2, HUD + H / 30, T_Q2[g_lang], C_DIM, W - 2 * MX, H / 14, 5);
    int top = HUD + H / 12, bot = H - MY - H / 4;
    draw_icons_fit(cn_kind, cn_n, MX, top, W - 2 * MX, bot - top);
    int bad = nv_millis() < fb_bad_until;
    for (int i = 0; i < 3; i++) { int x, y, w, h; answer3_rect(i, &x, &y, &w, &h);
        int ok = good_lock() && cn_opt[i] == cn_n, wr = bad && i == g_wrong_i;
        card(x, y, w, h, ok ? C_GOOD : (wr ? C_BAD : C_PANEL), C_ACC[i + 1], istr(cn_opt[i]), (ok || wr) ? C_PANEL : C_ACC[i + 1]); }
    draw_hud();
}

// ================================================================================================
//  GAME 3 — IN ORDINE
// ================================================================================================
static char al_seq[MAX_SEQ], al_ch[MAX_SEQ]; static int al_cell[MAX_SEQ], al_done[MAX_SEQ], al_prog;
static void new_alpha(void) {
    al_n = 3 + g_lvl / 2; if (al_n > MAX_SEQ) al_n = MAX_SEQ;
    int usenum = (g_mode == MODE_NUMBERS) || (g_mode == MODE_MIXED && rnd(0, 1));
    if (usenum && al_n > 8) al_n = 8;
    int hi = usenum ? (9 - al_n + 1) : (g_nlet - al_n), lo = usenum ? 1 : 0;
    int start = rnd(lo, hi); for (int t = 0; t < 40 && recent_has(usenum * 1000 + start); t++) start = rnd(lo, hi);
    recent_push(usenum * 1000 + start);
    if (usenum) for (int i = 0; i < al_n; i++) al_seq[i] = (char)('0' + start + i);
    else        for (int i = 0; i < al_n; i++) al_seq[i] = g_letters[start + i];
    int cells[10]; for (int i = 0; i < 10; i++) cells[i] = i; shuffle(cells, 10);
    int order[MAX_SEQ]; for (int i = 0; i < al_n; i++) order[i] = i; shuffle(order, al_n);
    for (int i = 0; i < al_n; i++) { al_ch[i] = al_seq[order[i]]; al_cell[i] = cells[i]; al_done[i] = 0; }
    al_prog = 0; mark_dirty();
}
static void hit_alpha(int tx, int ty) {
    if (good_lock()) return;
    for (int i = 0; i < al_n; i++) { if (al_done[i]) continue; int x, y, w, h; alpha_tile_rect(al_cell[i], &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) {
            if (al_ch[i] == al_seq[al_prog]) { al_done[i] = 1; al_prog++;
                say_tile(al_ch[i]);   // voice the tile (letter name or number)
                if (al_prog >= al_n) on_correct(); else { tone_tap(); mark_dirty(); } }
            else on_wrong(); return; } }
}
static void draw_alpha(void) {
    draw_bg();
    text_fit(W / 2, HUD + H / 34, T_Q3[g_lang], C_DIM, W - 2 * MX, H / 16, 4);
    int step = W / 16, sx = W / 2 - (al_n * step) / 2;
    for (int i = 0; i < al_n; i++) { int col = (i < al_prog) ? C_GOOD : (i == al_prog ? C_ACC[3] : C_DIM);
        text_cs(sx + i * step + step / 2, HUD + H / 10, chstr(al_seq[i]), col, 5); }
    for (int i = 0; i < al_n; i++) { if (al_done[i]) continue; int x, y, w, h; alpha_tile_rect(al_cell[i], &x, &y, &w, &h);
        card(x, y, w, h, C_PANEL, C_ACC[2], chstr(al_ch[i]), C_INK); }
    draw_hud();
}

// ================================================================================================
//  GAME 4 — ABBINA (memory) — mismatch is neutral (no life); exit via home
// ================================================================================================
// mt_val holds either a letter's ASCII code or a plain number value (mt_isnum tells which — the two
// domains never overlap, so a plain equality check on mt_val is enough to test a match).
static int mt_val[MAX_TILES], mt_isnum[MAX_TILES]; static int mt_state[MAX_TILES], mt_first, mt_flip_until;
static void new_match(void) {
    int pairs = 2 + g_lvl, cap = (g_mode == MODE_NUMBERS) ? 9 : 10;
    if (pairs > cap) pairs = cap; if (pairs < 3) pairs = 3;
    mt_tiles = pairs * 2;
    int numhi = g_lvl >= 4 ? 20 : 9;   // level 4+: numbers can go above ten (harder to tell tiles apart)
    int pool[10], pool_num[10], sig = 0;
    // Rebuild the whole board until its value-set differs from recent boards (bounded), so two
    // sessions-in-a-row never open with the same tiles. sig is order-independent.
    for (int attempt = 0; attempt < 8; attempt++) {
        sig = 0;
        for (int i = 0; i < pairs; i++) { int v, isnum; int ok;
            do { isnum = (g_mode == MODE_NUMBERS) || (g_mode == MODE_MIXED && rnd(0, 1));
                 v = isnum ? rnd(1, numhi) : g_letters[rnd(0, g_nlet - 1)];
                 ok = 1; for (int j = 0; j < i; j++) if (pool[j] == v && pool_num[j] == isnum) ok = 0; } while (!ok);
            pool[i] = v; pool_num[i] = isnum; sig += ((v << 1) | isnum); }
        if (!recent_has(sig)) break;
    }
    recent_push(sig);
    int idx[MAX_TILES]; for (int i = 0; i < mt_tiles; i++) idx[i] = i; shuffle(idx, mt_tiles);
    for (int i = 0; i < mt_tiles; i++) { mt_val[idx[i]] = pool[i / 2]; mt_isnum[idx[i]] = pool_num[i / 2]; mt_state[idx[i]] = 0; }
    mt_first = -1; mt_flip_until = 0; mark_dirty();
}
static void tick_match(void) {
    int now = nv_millis();
    if (mt_flip_until && now >= mt_flip_until) { for (int i = 0; i < mt_tiles; i++) if (mt_state[i] == 1) mt_state[i] = 0; mt_first = -1; mt_flip_until = 0; mark_dirty(); }
}
static void hit_match(int tx, int ty) {
    int now = nv_millis();
    if (good_lock() || mt_flip_until) return;
    for (int i = 0; i < mt_tiles; i++) { if (mt_state[i] != 0) continue; int x, y, w, h; match_tile_rect(i, &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) {
            mt_state[i] = 1; tone_tap(); mark_dirty();
            if (mt_isnum[i]) say_num(mt_val[i]); else say_letter((char)mt_val[i]);   // voice the tile on flip
            if (mt_first < 0) mt_first = i;
            else { if (mt_val[mt_first] == mt_val[i]) { mt_state[mt_first] = 2; mt_state[i] = 2; mt_first = -1;
                       int all = 1; for (int k = 0; k < mt_tiles; k++) if (mt_state[k] != 2) all = 0; if (all) on_correct(); else tone_step(); }
                   else { mt_flip_until = now + 900; nv_gfx_tone(300, 90); mark_dirty(); } }   // show BOTH flipped cards, then flip back (no error overlay hiding them)
            return; } }
}
static void draw_match(void) {
    draw_bg();
    text_fit(W / 2, HUD + H / 34, T_Q4[g_lang], C_DIM, W - 2 * MX, H / 16, 4);
    for (int i = 0; i < mt_tiles; i++) { int x, y, w, h; match_tile_rect(i, &x, &y, &w, &h);
        if (mt_state[i] == 0) { card(x, y, w, h, C_ACC[0], C_INK, "", 0); nv_gfx_circle(x + w / 2, y + h / 2, imin(w, h) / 5, C_PANEL); }
        else { const char *lbl = mt_isnum[i] ? istr(mt_val[i]) : chstr((char)mt_val[i]);
            card(x, y, w, h, (mt_state[i] == 2) ? C_GOOD : C_PANEL, C_ACC[3], lbl, C_INK); } }
    draw_hud();
}

// ================================================================================================
//  GAME 5 — CONTI (+/-)
// ================================================================================================
static int sm_a, sm_b, sm_op, sm_ans, sm_kind, sm_opt[3];
static const char *eq_str(void) {
    static char s[24]; int p = 0;
    const char *a = istr(sm_a); while (*a) s[p++] = *a++;
    s[p++] = ' '; s[p++] = sm_op ? '-' : '+'; s[p++] = ' ';
    const char *b = istr(sm_b); while (*b) s[p++] = *b++; s[p] = 0; return s;
}
static void say_sum(void) {   // pronounce the equation as words, e.g. "TRE PIU TRE" (it/en pack only)
    if (!g_voice) return;
    const char *wa = num_word(sm_a), *wo = op_word(sm_op), *wb = num_word(sm_b);
    if (!wa || !wo || !wb) return;
    char buf[48]; int p = 0; const char *s;
    for (s = wa; *s; ) buf[p++] = *s++; buf[p++] = ' ';
    for (s = wo; *s; ) buf[p++] = *s++; buf[p++] = ' ';
    for (s = wb; *s; ) buf[p++] = *s++; buf[p] = 0;
    say(buf);
}
static void new_sum(void) {
    sm_op = (g_lvl >= 3 && rnd(0, 1)) ? 1 : 0;
    int hi = 2 + g_lvl * 2; if (hi > 15) hi = 15;
    for (int t = 0; t < 40; t++) {
        if (!sm_op) { sm_a = rnd(1, hi); sm_b = rnd(1, hi); sm_ans = sm_a + sm_b; }
        else        { sm_a = rnd(2, hi + 3); sm_b = rnd(1, sm_a); sm_ans = sm_a - sm_b; }
        if (!recent_has(sm_op * 10000 + sm_a * 100 + sm_b)) break;
    }
    recent_push(sm_op * 10000 + sm_a * 100 + sm_b);
    sm_kind = rnd(0, O_COUNT - 1);
    int slot = rnd(0, 2); sm_opt[slot] = sm_ans;
    for (int i = 0; i < 3; i++) { if (i == slot) continue; int v, ok;
        do { int off = rnd(1, 3) * (rnd(0, 1) ? 1 : -1); v = sm_ans + off; if (v < 0) v = sm_ans + rnd(1, 3);
             ok = (v != sm_ans); for (int j = 0; j < 3; j++) if (j != i && sm_opt[j] == v) ok = 0; } while (!ok);
        sm_opt[i] = v; }
    say_sum();
    mark_dirty();
}
static void hit_sum(int tx, int ty) {
    if (good_lock()) return;
    if (ty < H * 44 / 100) { say_sum(); return; }   // tap the equation -> hear it again
    for (int i = 0; i < 3; i++) { int x, y, w, h; answer3_rect(i, &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) { say_num(sm_opt[i]);   // hear the number you pressed
            if (sm_opt[i] == sm_ans) on_correct(); else { g_wrong_i = i; on_wrong(); }
            return; } }
}
static void draw_sum(void) {
    draw_bg();
    text_fit(W / 2, HUD + H / 30, T_Q5[g_lang], C_DIM, W - 2 * MX, H / 14, 5);
    text_fit(W / 2, H * 32 / 100, eq_str(), C_INK, W - 2 * MX, H / 5, 16);
    // object aid at the first couple of levels (numbers-only from level 3), auto-sized to fill space.
    if (g_lvl <= 2 && !sm_op) {
        int ay = H * 44 / 100, ah = (H - MY - H / 4) - ay - 6, boxw = (W - 2 * MX) / 2 - W / 18;
        draw_icons_fit(sm_kind, sm_a, MX, ay, boxw, ah);
        text_cs(W / 2, ay + ah / 2, "+", C_DIM, 8);
        draw_icons_fit(sm_kind, sm_b, W - MX - boxw, ay, boxw, ah);
    }
    int bad = nv_millis() < fb_bad_until;
    for (int i = 0; i < 3; i++) { int x, y, w, h; answer3_rect(i, &x, &y, &w, &h);
        int ok = good_lock() && sm_opt[i] == sm_ans, wr = bad && i == g_wrong_i;
        card(x, y, w, h, ok ? C_GOOD : (wr ? C_BAD : C_PANEL), C_ACC[i + 2], istr(sm_opt[i]), (ok || wr) ? C_PANEL : C_ACC[i + 2]); }
    draw_hud();
}

// ================================================================================================
//  GAME 6 — DIVERSO (odd one out)
// ================================================================================================
// Read the shown WORD, tap the matching picture among 4 (word -> meaning).
static int tr_kind[4], tr_ans, tr_target;
static void new_odd(void) {
    int ks[4]; ks[0] = rnd_obj();   // the spoken target must be voiceable
    for (int t = 0; t < 40 && recent_has(ks[0]); t++) ks[0] = rnd_obj();
    recent_push(ks[0]);
    for (int i = 1; i < 4; i++) { int k, ok; do { k = rnd(0, O_COUNT - 1); ok = 1; for (int j = 0; j < i; j++) if (ks[j] == k) ok = 0; } while (!ok); ks[i] = k; }
    tr_ans = rnd(0, 3);
    int tmp = ks[0]; ks[0] = ks[tr_ans]; ks[tr_ans] = tmp;   // move the target into the answer slot
    for (int i = 0; i < 4; i++) tr_kind[i] = ks[i];
    tr_target = tr_kind[tr_ans];
    if (g_lvl >= 4) {   // one trickier distractor sharing the target's initial letter
        char ti = OBJ_W[tr_target][g_lang][0];
        for (int s = 0; s < 4; s++) { if (s == tr_ans) continue;
            int found = -1;
            for (int t = 0; t < 30; t++) { int k = rnd(0, O_COUNT - 1);
                if (OBJ_W[k][g_lang][0] == ti && k != tr_target) {
                    int dup = 0; for (int j = 0; j < 4; j++) if (j != s && tr_kind[j] == k) dup = 1;
                    if (!dup) { found = k; break; } } }
            if (found >= 0) { tr_kind[s] = found; break; }
        }
    }
    say_obj(tr_target);   // hear the word to find
    mark_dirty();
}
static void hit_odd(int tx, int ty) {
    if (good_lock()) return;
    if (ty > HUD - H / 20 && ty < HUD + H / 4) {   // tap the word banner -> hear it again
        say_obj(tr_target); return; }
    for (int i = 0; i < 4; i++) { int x, y, w, h; odd_rect(i, &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) { if (i == tr_ans) on_correct(); else { g_wrong_i = i; on_wrong(); } return; } }
}
static void draw_odd(void) {
    draw_bg();
    text_cs(W / 2, HUD + H / 40, T_Q6[g_lang], C_DIM, 3);
    text_fit(W / 2, HUD + H / 8, OBJ_W[tr_target][g_lang], C_ACC[3], W - 2 * MX, H / 10, 12);
    int bad = nv_millis() < fb_bad_until;
    for (int i = 0; i < 4; i++) { int x, y, w, h; odd_rect(i, &x, &y, &w, &h);
        int ok = good_lock() && i == tr_ans, wr = bad && i == g_wrong_i;
        card(x, y, w, h, ok ? C_GOOD : (wr ? C_BAD : C_PANEL), C_ACC[i], "", 0);
        draw_icon_pop(tr_kind[i], x + w / 2, y + h / 2, imin(w, h) * 38 / 100); }
    draw_hud();
}

// ================================================================================================
//  GAME 7 — COMPONI (spelling)
// ================================================================================================
static int sp_kind, sp_len, sp_prog;
static char sp_word[MAXW + 1], sp_tile[MAXW]; static int sp_used[MAXW];
static void new_spell(void) {
    int maxlen = 4 + g_lvl / 2; if (maxlen > 8) maxlen = 8;
    int tries = 0;
    do { sp_kind = rnd(0, O_COUNT - 1); sp_len = slen(OBJ_W[sp_kind][g_lang]); tries++; }
    while ((sp_len < 3 || sp_len > maxlen || (g_voice && !obj_voiceable(sp_kind)) || recent_has(sp_kind)) && tries < 60);
    recent_push(sp_kind);
    if (sp_len > MAXW) sp_len = MAXW;
    const char *w = OBJ_W[sp_kind][g_lang];
    for (int i = 0; i < sp_len; i++) sp_word[i] = w[i]; sp_word[sp_len] = 0;
    int order[MAXW]; for (int i = 0; i < sp_len; i++) order[i] = i; shuffle(order, sp_len);
    for (int i = 0; i < sp_len; i++) { sp_tile[i] = sp_word[order[i]]; sp_used[i] = 0; }
    sp_prog = 0;
    say_obj(sp_kind);   // hear the word to spell
    mark_dirty();
}
static void hit_spell(int tx, int ty) {
    if (good_lock()) return;
    { int s = imin(W, H) / 7; if (inrect(W / 2 - s, HUD + H / 5 - s, 2 * s, 2 * s, tx, ty)) {   // tap the picture -> hear it again
        say_obj(sp_kind); return; } }
    for (int i = 0; i < sp_len; i++) { if (sp_used[i]) continue; int x, y, w, h; spell_tile_rect(i, sp_len, &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) {
            if (sp_tile[i] == sp_word[sp_prog]) { sp_used[i] = 1; sp_prog++; say_letter(sp_tile[i]);   // spell it aloud
                if (sp_prog >= sp_len) on_correct(); else { tone_step(); mark_dirty(); } }
            else on_wrong(); return; } }
}
static void draw_spell(void) {
    draw_bg();
    text_fit(W / 2, HUD + H / 30, T_Q7[g_lang], C_DIM, W - 2 * MX, H / 16, 4);
    draw_icon_pop(sp_kind, W / 2, HUD + H / 5, imin(W, H) / 7);
    for (int i = 0; i < sp_len; i++) { int x, y, w, h; spell_slot_rect(i, sp_len, &x, &y, &w, &h);
        if (i < sp_prog) card(x, y, w, h, C_GOOD, C_INK, chstr(sp_word[i]), C_PANEL);
        else nv_gfx_rect(x, y + h - 8, w, 8, C_DIM); }
    for (int i = 0; i < sp_len; i++) { int x, y, w, h; spell_tile_rect(i, sp_len, &x, &y, &w, &h);
        if (sp_used[i]) card(x, y, w, h, C_BG2, C_DIM, "", 0);
        else card(x, y, w, h, C_PANEL, C_ACC[5], chstr(sp_tile[i]), C_INK); }
    draw_hud();
}

// ---- round dispatch -----------------------------------------------------------------------------
static void new_round(void) {
    g_wrong_i = -1;
    switch (g_screen) {
    case SC_INITIAL: new_initial(); break; case SC_COUNT: new_count(); break;
    case SC_ALPHA: new_alpha(); break; case SC_MATCH: new_match(); break;
    case SC_SUM: new_sum(); break; case SC_ODD: new_odd(); break; case SC_SPELL: new_spell(); break;
    }
}
static void dispatch_hit(int tx, int ty) {
    switch (g_screen) {
    case SC_INITIAL: hit_initial(tx, ty); break; case SC_COUNT: hit_count(tx, ty); break;
    case SC_ALPHA: hit_alpha(tx, ty); break; case SC_MATCH: hit_match(tx, ty); break;
    case SC_SUM: hit_sum(tx, ty); break; case SC_ODD: hit_odd(tx, ty); break;
    case SC_SPELL: hit_spell(tx, ty); break;
    }
}

// ---- game over / scores -------------------------------------------------------------------------
static void end_session(void) {
    g_cd_n = 0; g_vfx_at = 0; g_sfx_at = 0;   // cancel any running countdown / pending voice / SFX
    g_pending_over = 0;   // consume any pending game-over so we never record the score twice
    if (g_score <= 0) { g_screen = SC_MENU; mark_dirty(); return; }   // nothing scored -> straight back to menu
    g_over_rank = record_score(g_game, g_score); save_state(); g_screen = SC_OVER; mark_dirty();
}
static const char *GAME_LBL[N_GAMES];   // filled at start (lang-dependent)
static void apply_lang(int l) {
    if (l < 0 || l >= L_COUNT) l = L_EN;
    g_lang = l; g_letters = T_ALPHA[l]; g_nlet = slen(T_ALPHA[l]);
    GAME_LBL[0] = T_G1[l]; GAME_LBL[1] = T_G2[l]; GAME_LBL[2] = T_G3[l];
    GAME_LBL[3] = T_G4[l]; GAME_LBL[4] = T_G5[l]; GAME_LBL[5] = T_G6[l]; GAME_LBL[6] = T_G7[l];
}
static void draw_over(void) {
    draw_bg();
    int hx, hy, hw, hh; home_rect(&hx, &hy, &hw, &hh);
    draw_back_btn(hx, hy, hw, hh);
    text_fit(W / 2, H * 11 / 100, T_OVER[g_lang], C_ACC[0], W * 6 / 10, H / 11, 12);   // FINE title
    draw_icon(67, W / 2, H * 26 / 100, imin(W, H) / 9);                                // crown
    // score panel — rounded white card, gold rim; PUNTI label sits well above the big number (breathing room)
    int pw = W * 46 / 100, ph = H * 24 / 100, px = W / 2 - pw / 2, py = H * 41 / 100;
    card(px, py, pw, ph, C_PANEL, C_STAR, "", 0);
    text_cs(W / 2, py + ph * 26 / 100, T_SCOREW[g_lang], C_DIM, 4);
    text_cs(W / 2, py + ph * 66 / 100, istr(g_score), C_INK, 9);
    // record / rank as a pill just under the card
    int ry = py + ph + H / 40, rh = H / 12;
    if (g_over_rank == 0) { int rw = W * 36 / 100; card(W / 2 - rw / 2, ry, rw, rh, C_STAR, C_INK, T_REC[g_lang], C_INK); }
    else if (g_over_rank > 0) { char b[24]; int p = 0; const char *t = "TOP "; while (*t) b[p++] = *t++; const char *n = istr(g_over_rank + 1); while (*n) b[p++] = *n++; b[p] = 0;
        int rw = W * 26 / 100; card(W / 2 - rw / 2, ry, rw, rh, C_ACC[3], C_INK, b, C_PANEL); }
    int bw = (W - 2 * MX - W / 20) / 2, bh = H / 7, by = H - MY - bh;
    card(MX, by, bw, bh, C_ACC[2], C_INK, T_REPLAY[g_lang], C_PANEL);
    card(MX + bw + W / 20, by, bw, bh, C_PANEL, C_INK, T_BEST[g_lang], C_INK);
}
static void hit_over(int tx, int ty) {
    int hx, hy, hw, hh; home_rect(&hx, &hy, &hw, &hh);
    if (inrect(hx, hy, hw, hh, tx, ty)) { tone_tap(); g_screen = SC_MENU; mark_dirty(); return; }
    int bw = (W - 2 * MX - W / 20) / 2, bh = H / 7, by = H - MY - bh;
    if (inrect(MX, by, bw, bh, tx, ty)) { tone_tap(); start_game(g_game); }
    else if (inrect(MX + bw + W / 20, by, bw, bh, tx, ty)) { tone_tap(); g_screen = SC_SCORES; mark_dirty(); }
}
static void draw_scores(void) {
    draw_bg();
    text_fit(W / 2, HUD + H / 30, T_BEST[g_lang], C_ACC[3], W / 2, H / 11, 9);
    if (g_name[0]) text_cs(W - MX - 90, HUD + H / 26, g_name, C_ACC[2], 4);
    int top = HUD + H / 9, rowh = (H - MY - top) / N_GAMES;
    for (int g = 0; g < N_GAMES; g++) {
        int y = top + g * rowh;
        card(MX, y + 4, W - 2 * MX, rowh - 8, C_PANEL, C_ACC[g % 8], "", 0);
        text_cs(W * 30 / 100, y + rowh / 2, GAME_LBL[g], C_INK, 4);
        text_cs(W * 78 / 100, y + rowh / 2, istr(g_hi[g][0]), C_ACC[g % 8], 5);
    }
    int x, y, w, h; home_rect(&x, &y, &w, &h);
    draw_back_btn(x, y, w, h);
}

// ================================================================================================
//  MENU
// ================================================================================================
static const int G_ICON[N_GAMES] = { 7, 43, 2, 42, 12, 41, 21 };  // apple,dice,star,heart,cake,butterfly,book
static void start_game(int game) {
    g_game = game; g_screen = SC_INITIAL + game;
    recent_reset(RECENT_CAP_G[game]);   // fresh no-repeat window for this session
    g_lvl = 1; g_score = 0; g_lives = LIVES; g_prog = 0; g_streak = 0; g_pending_over = 0; g_advance = 0;
    fb_good_until = 0; fb_bad_until = 0; g_lvup_until = 0; g_pn = 0; g_vfx_at = 0; g_sfx_at = 0; g_speak_until = 0;
    g_cd_n = 3; g_cd_step = nv_millis(); g_cd_next = g_cd_step + 650; say_num(3);   // 3-2-1 before the first round
}
static void hit_menu(int tx, int ty) {
    { int ex, ey, ew, eh; exit_rect(&ex, &ey, &ew, &eh);
      if (inrect(ex, ey, ew, eh, tx, ty)) { tone_tap(); g_quit = 1; return; } }        // Exit -> back to launcher
    { int gx, gy, gw, gh; settings_rect(&gx, &gy, &gw, &gh);
      if (inrect(gx, gy, gw, gh, tx, ty)) { tone_tap(); g_screen = SC_SETTINGS; mark_dirty(); return; } }
    for (int i = 0; i < 3; i++) { int x, y, w, h; menu_mode_rect(i, &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) { g_mode = i; tone_tap(); mark_dirty(); return; } }
    for (int i = 0; i < N_GAMES; i++) { int x, y, w, h; menu_game_rect(i, &x, &y, &w, &h);
        if (inrect(x, y, w, h, tx, ty)) { tone_tap(); start_game(i); return; } }
    // 8th cell = scores
    int x, y, w, h; menu_game_rect(7, &x, &y, &w, &h);
    if (inrect(x, y, w, h, tx, ty)) { tone_tap(); g_screen = SC_SCORES; mark_dirty(); }
}
static void draw_menu(void) {
    draw_bg();
    // top bar — the SAME white rounded panel as the in-game HUD, so every page looks consistent
    int bh = H / 9, bary = 6, cy = bary + bh / 2;
    fill_round(6, bary, W - 12, bh, 16, C_SHADOW);
    fill_round(10, bary, W - 20, bh - 4, 14, C_PANEL);
    { int ex, ey, ew, eh; exit_rect(&ex, &ey, &ew, &eh); draw_exit_btn(ex, ey, ew, eh); }
    { int gx, gy, gw, gh; settings_rect(&gx, &gy, &gw, &gh); draw_gear(gx + gw / 2, gy + gh / 2, 16, C_ACC[3], C_PANEL); }
    text_cs(W / 2, cy, "ABC 123", C_INK, 5);
    draw_stars_badge(W - MX - 66, cy);
    text_cs(W / 2, bary + bh + H / 26, T_SUB[g_lang], C_DIM, 2);   // subtitle just below the bar
    const char *ml[3] = { T_MODE_L[g_lang], T_MODE_N[g_lang], T_MODE_M[g_lang] };
    for (int i = 0; i < 3; i++) { int x, y, w, h; menu_mode_rect(i, &x, &y, &w, &h); int on = (g_mode == i);
        card(x, y, w, h, on ? C_ACC[3] : C_PANEL, C_INK, ml[i], on ? C_PANEL : C_INK); }
    for (int i = 0; i < N_GAMES; i++) { int x, y, w, h; menu_game_rect(i, &x, &y, &w, &h);
        card(x, y, w, h, C_ACC[i % 8], C_INK, "", 0);
        int ic = imin(w, h);
        nv_gfx_circle(x + w / 2, y + h * 36 / 100, ic / 4 + 6, C_PANEL);   // white disc behind emoji
        draw_icon(G_ICON[i], x + w / 2, y + h * 36 / 100, ic / 5);
        text_fit(x + w / 2, y + h * 82 / 100, GAME_LBL[i], C_PANEL, w - 16, h / 4, 4); }
    // scores card (8th)
    int x, y, w, h; menu_game_rect(7, &x, &y, &w, &h);
    card(x, y, w, h, C_STAR, C_INK, "", 0);
    nv_gfx_circle(x + w / 2, y + h * 36 / 100, imin(w, h) / 4 + 6, C_PANEL);
    draw_star(x + w / 2, y + h * 36 / 100, imin(w, h) / 5, C_STAR);
    text_fit(x + w / 2, y + h * 82 / 100, T_BEST[g_lang], C_INK, w - 16, h / 4, 4);
}

// ================================================================================================
//  SETTINGS — change language (one button per supported language)
// ================================================================================================
static void lang_btn_rect(int i, int *x, int *y, int *w, int *h) {
    int gap = W / 60, bw = (W - 2 * MX - 4 * gap) / 5;
    *x = MX + i * (bw + gap); *y = HUD + H / 6; *w = bw; *h = H / 9;
}
static void voice_btn_rect(int i, int *x, int *y, int *w, int *h) {   // 0 = ACCESA, 1 = SPENTA
    int gap = W / 40, bw = (W - 2 * MX - gap) / 2;
    *x = MX + i * (bw + gap); *y = H * 48 / 100; *w = bw; *h = H / 10;
}
static void name_btn_rect(int *x, int *y, int *w, int *h) { *x = MX; *y = H * 64 / 100; *w = W - 2 * MX; *h = H / 10; }
static void reset_btn_rect(int *x, int *y, int *w, int *h) { *x = MX; *y = H * 82 / 100; *w = W - 2 * MX; *h = H / 10; }
static void draw_settings(void) {
    draw_bg();
    int x, y, w, h; home_rect(&x, &y, &w, &h); draw_back_btn(x, y, w, h);
    text_fit(W / 2, HUD + H / 28, T_SET[g_lang], C_ACC[3], W * 7 / 10, H / 12, 9);
    text_cs(W / 2, HUD + H / 11, T_LANGL[g_lang], C_DIM, 3);
    for (int i = 0; i < L_COUNT; i++) { int lx, ly, lw, lh; lang_btn_rect(i, &lx, &ly, &lw, &lh); int on = (g_lang == i);
        card(lx, ly, lw, lh, on ? C_ACC[2] : C_PANEL, C_INK, LANG_CODE[i], on ? C_PANEL : C_INK); }
    text_cs(W / 2, H * 45 / 100, T_VOICE[g_lang], C_DIM, 3);
    for (int i = 0; i < 2; i++) { int vx, vy, vw, vh; voice_btn_rect(i, &vx, &vy, &vw, &vh); int on = (i == 0) ? g_voice : !g_voice;
        card(vx, vy, vw, vh, on ? C_ACC[2] : C_PANEL, C_INK, i == 0 ? T_VON[g_lang] : T_VOFF[g_lang], on ? C_PANEL : C_INK); }
    text_cs(W / 2, H * 61 / 100, T_NAME[g_lang], C_DIM, 3);
    { int nx, ny, nw, nh; name_btn_rect(&nx, &ny, &nw, &nh); card(nx, ny, nw, nh, C_PANEL, C_ACC[3], g_name[0] ? g_name : "- - -", C_INK); }
    { int rx, ry, rw, rh; reset_btn_rect(&rx, &ry, &rw, &rh); card(rx, ry, rw, rh, C_BAD, C_INK, T_RESET[g_lang], C_PANEL); }
}
static void hit_settings(int tx, int ty) {
    for (int i = 0; i < L_COUNT; i++) { int lx, ly, lw, lh; lang_btn_rect(i, &lx, &ly, &lw, &lh);
        if (inrect(lx, ly, lw, lh, tx, ty)) { tone_tap(); apply_lang(i); save_state(); mark_dirty(); return; } }
    for (int i = 0; i < 2; i++) { int vx, vy, vw, vh; voice_btn_rect(i, &vx, &vy, &vw, &vh);
        if (inrect(vx, vy, vw, vh, tx, ty)) { tone_tap(); g_voice = (i == 0) ? 1 : 0; save_state(); mark_dirty(); return; } }
    int nx, ny, nw, nh; name_btn_rect(&nx, &ny, &nw, &nh);
    if (inrect(nx, ny, nw, nh, tx, ty)) { tone_tap(); g_screen = SC_NAME; mark_dirty(); return; }
    int rx, ry, rw, rh; reset_btn_rect(&rx, &ry, &rw, &rh);
    if (inrect(rx, ry, rw, rh, tx, ty)) { tone_tap(); reset_records(); nv_sound("lose"); mark_dirty(); return; }
}
// ---- name entry (on-screen A-Z keyboard) --------------------------------------------------------
static void name_key_rect(int i, int *x, int *y, int *w, int *h) {
    int cols = 7, gap = W / 90, top = HUD + H / 4;
    int cw = (W - 2 * MX) / cols, ch = (H - MY - top) / 4;
    int r = i / cols, c = i % cols;
    *x = MX + c * cw + gap; *y = top + r * ch + gap; *w = cw - 2 * gap; *h = ch - 2 * gap;
}
static void draw_name(void) {
    draw_bg();
    int x, y, w, h; home_rect(&x, &y, &w, &h); draw_back_btn(x, y, w, h);
    text_fit(W / 2, HUD + H / 28, T_NAME[g_lang], C_ACC[3], W * 7 / 10, H / 12, 8);
    card(W / 4, HUD + H / 9, W / 2, H / 9, C_PANEL, C_ACC[3], g_name[0] ? g_name : "", C_INK);
    for (int i = 0; i < 28; i++) { int kx, ky, kw, kh; name_key_rect(i, &kx, &ky, &kw, &kh);
        const char *lbl; int fill = C_PANEL, tc = C_INK;
        if (i < 26) { lbl = chstr((char)('A' + i)); }
        else if (i == 26) { lbl = "DEL"; fill = C_ACC[1]; tc = C_PANEL; }
        else { lbl = "OK"; fill = C_ACC[2]; tc = C_PANEL; }
        card(kx, ky, kw, kh, fill, C_INK, lbl, tc); }
}
static void hit_name(int tx, int ty) {
    for (int i = 0; i < 28; i++) { int kx, ky, kw, kh; name_key_rect(i, &kx, &ky, &kw, &kh);
        if (inrect(kx, ky, kw, kh, tx, ty)) { tone_tap();
            int len = slen(g_name);
            if (i < 26) { if (len < 12) { g_name[len] = (char)('A' + i); g_name[len + 1] = 0; } }
            else if (i == 26) { if (len > 0) g_name[len - 1] = 0; }
            else { save_state(); g_screen = SC_SETTINGS; }
            mark_dirty(); return; } }
}

// ---- render -------------------------------------------------------------------------------------
static void draw_game(void) {
    switch (g_screen) {
    case SC_INITIAL: draw_initial(); break; case SC_COUNT: draw_count(); break;
    case SC_ALPHA: draw_alpha(); break; case SC_MATCH: draw_match(); break;
    case SC_SUM: draw_sum(); break; case SC_ODD: draw_odd(); break; case SC_SPELL: draw_spell(); break;
    }
}
static void draw_countdown(void) {   // big bouncy 3-2-1 over bg + HUD (round content not built yet)
    text_fit(W / 2, HUD + H / 7, T_READY[g_lang], C_DIM, W - 2 * MX, H / 9, 6);
    int el = nv_millis() - g_cd_step, sc = 20 * pop_scale(el, 300) / 256; if (sc < 2) sc = 2;
    text_cs(W / 2, H * 55 / 100, istr(g_cd_n), C_ACC[3], sc);
}
static void render(void) {
    if (g_screen >= SC_INITIAL && g_screen <= SC_SPELL) {
        // Any feedback panel covers the whole play area — draw just the bg under it and skip the
        // costly scene AND HUD (all hidden by the panel). The heaviest frames become the cheapest.
        int now = nv_millis();
        if (g_cd_n > 0) { draw_bg(); draw_hud(); draw_countdown(); }
        else if (good_lock() || now < fb_bad_until) draw_bg();
        else draw_game();
        draw_feedback();
    } else switch (g_screen) {
        case SC_MENU: draw_menu(); break; case SC_OVER: draw_over(); break; case SC_SCORES: draw_scores(); break;
        case SC_SETTINGS: draw_settings(); break; case SC_NAME: draw_name(); break;
    }
    particles_frame();   // confetti overlay + one physics step (once per rendered frame)
}

// ---- main loop ----------------------------------------------------------------------------------
NV_EXPORT("run")
void run(void) {
    W = nv_gfx_width(); H = nv_gfx_height(); MX = W / 22; MY = H / 16; HUD = H / 9 + 18;
    C_BG = NV_RGB(250, 246, 236); C_BG2 = NV_RGB(255, 236, 205);
    C_INK = NV_RGB(52, 54, 74); C_PANEL = NV_RGB(255, 255, 255);
    C_SHADOW = NV_RGB(225, 218, 205); C_DIM = NV_RGB(150, 150, 165);
    C_GOOD = NV_RGB(64, 196, 96); C_BAD = NV_RGB(235, 84, 84); C_STAR = NV_RGB(255, 200, 48);
    C_ACC[0] = NV_RGB(235, 90, 100); C_ACC[1] = NV_RGB(245, 150, 40); C_ACC[2] = NV_RGB(70, 175, 110);
    C_ACC[3] = NV_RGB(70, 130, 230); C_ACC[4] = NV_RGB(170, 100, 220); C_ACC[5] = NV_RGB(240, 110, 170);
    C_ACC[6] = NV_RGB(40, 180, 200); C_ACC[7] = NV_RGB(120, 190, 60);
    PASTEL[0] = NV_RGB(250, 246, 236); PASTEL[1] = NV_RGB(219, 238, 251); PASTEL[2] = NV_RGB(252, 227, 238);
    PASTEL[3] = NV_RGB(226, 246, 227); PASTEL[4] = NV_RGB(252, 244, 213); PASTEL[5] = NV_RGB(237, 228, 251);
    PASTEL[6] = NV_RGB(252, 233, 219); PASTEL[7] = NV_RGB(216, 246, 244);

    detect_lang();
    load_state();            // may override g_lang with the saved language
    apply_lang(g_lang);
    g_screen = SC_MENU; mark_dirty();
    int prev_down = 0, lastx = 0, lasty = 0, was_anim = 0;

    while (nv_gfx_present()) {
        if (g_quit) break;                         // menu Exit button -> close app
        int ix, iy, down = nv_touch(&ix, &iy);
        if (down) { lastx = ix; lasty = iy; }
        int tap = (!down && prev_down); prev_down = down;
        if (tap) tone_tap();                       // a click on every touch

        if (nv_gfx_back()) {                       // OS back gesture: navigate the in-app stack
            if (g_screen == SC_MENU) break;        // at root -> exit to launcher
            else if (g_screen >= SC_INITIAL && g_screen <= SC_SPELL) end_session();
            else if (g_screen == SC_NAME) { save_state(); g_screen = SC_SETTINGS; mark_dirty(); }
            else { g_screen = SC_MENU; mark_dirty(); }   // over / scores / settings -> menu
        }

        if (g_cd_n > 0) {                          // start-of-game 3-2-1, voiced with number words
            int now = nv_millis();
            if (now >= g_cd_next) {
                g_cd_n--; g_cd_step = now;
                if (g_cd_n > 0) { g_cd_next = now + 650; say_num(g_cd_n); }
                else new_round();                  // GO -> first round (speaks the word / equation)
            }
            mark_dirty();
        }
        if (g_sfx_at && nv_millis() >= g_sfx_at) { g_sfx_at = 0; nv_sound(g_sfx_name); }   // SFX fires first
        if (g_vfx_at && nv_millis() >= g_vfx_at) {   // then the scheduled reward/consolation voice
            g_vfx_at = 0;
            if (g_vfx_kind) say_neg();
            else { say_praise(); if (g_speak_until + 450 > fb_good_until) fb_good_until = g_speak_until + 450; }   // hold overlay past the praise + a small breath before the next question
        }
        if (g_screen == SC_MATCH) tick_match();
        if (g_pending_over && nv_millis() >= fb_bad_until) { g_pending_over = 0; end_session(); }
        // advance only once the SFX + praise have fired AND finished (fb_good_until pushed out) — no cut, no overlap
        else if (g_advance && !g_vfx_at && !g_sfx_at && nv_millis() >= fb_good_until) { g_advance = 0; new_round(); }

        if (tap) {
            int in_game = (g_screen >= SC_INITIAL && g_screen <= SC_SPELL);
            if (in_game) {
                int hx, hy, hw, hh; home_rect(&hx, &hy, &hw, &hh);
                if (inrect(hx, hy, hw, hh, lastx, lasty)) { tone_tap(); end_session(); }
                else if (g_cd_n == 0) dispatch_hit(lastx, lasty);   // ignore game taps during countdown
            } else if (g_screen == SC_MENU) hit_menu(lastx, lasty);
            else if (g_screen == SC_OVER) hit_over(lastx, lasty);
            else if (g_screen == SC_SCORES) {
                int hx, hy, hw, hh; home_rect(&hx, &hy, &hw, &hh);
                if (inrect(hx, hy, hw, hh, lastx, lasty)) { tone_tap(); g_screen = SC_MENU; mark_dirty(); }
            }
            else if (g_screen == SC_SETTINGS) {
                int hx, hy, hw, hh; home_rect(&hx, &hy, &hw, &hh);
                if (inrect(hx, hy, hw, hh, lastx, lasty)) { tone_tap(); g_screen = SC_MENU; mark_dirty(); }
                else hit_settings(lastx, lasty);
            }
            else if (g_screen == SC_NAME) {
                int hx, hy, hw, hh; home_rect(&hx, &hy, &hw, &hh);
                if (inrect(hx, hy, hw, hh, lastx, lasty)) { tone_tap(); save_state(); g_screen = SC_SETTINGS; mark_dirty(); }
                else hit_name(lastx, lasty);
            }
        }

        // Redraw when dirty or while an overlay is up; force two clean frames as it ends so BOTH
        // double buffers are cleared (else a stale overlay frame would flicker back).
        int anim = animating();
        if (anim || was_anim) g_redraw = 2;
        was_anim = anim;
        if (g_redraw > 0) { render(); g_redraw--; }
    }
}
