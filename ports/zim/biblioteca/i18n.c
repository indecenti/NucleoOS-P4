// i18n.c — the app's words in the five languages of the system (it, en, es, fr, de), dates, sizes
// and numbers written the local way, and the names of the languages of the collections.
#include "bib.h"

#include <stdio.h>
#include <string.h>

#include "nucleo_sdk.h"

enum { L_IT, L_EN, L_ES, L_FR, L_DE, L_N };
static int s_lang = L_EN;

static const char *const kStr[S_COUNT][L_N] = {
    [S_LIBRARY] = {"Biblioteca", "Library", "Biblioteca", "Biblioth\xC3\xA8que", "Bibliothek"},
    [S_EMPTY_TITLE] = {"Nessuna raccolta, per ora", "No collections yet", "Todav\xC3\xAD" "a no hay colecciones",
                       "Aucune collection pour l'instant", "Noch keine Sammlungen"},
    [S_EMPTY_1] = {"Copia i file .zim di Kiwix nella cartella zim della tua home (sulla scheda SD: home/zim).",
                   "Copy Kiwix .zim files to the zim folder of your home (on the SD card: home/zim).",
                   "Copia los archivos .zim de Kiwix en la carpeta zim de tu home (en la tarjeta SD: home/zim).",
                   "Copiez les fichiers .zim de Kiwix dans le dossier zim de votre home (sur la carte SD : home/zim).",
                   "Kopiere Kiwix-.zim-Dateien in den Ordner zim deines Home-Verzeichnisses (auf der SD-Karte: home/zim)."},
    [S_EMPTY_2] = {"Li trovi su library.kiwix.org: Wikipedia, Wikizionario, medicina, manuali e molto altro, in tante lingue.",
                   "Get them at library.kiwix.org: Wikipedia, Wiktionary, medicine, manuals and much more, in many languages.",
                   "Los encontrar\xC3\xA1s en library.kiwix.org: Wikipedia, Wikcionario, medicina, manuales y mucho m\xC3\xA1s, en muchos idiomas.",
                   "Vous les trouverez sur library.kiwix.org : Wikip\xC3\xA9" "dia, Wiktionnaire, m\xC3\xA9" "decine, manuels et bien plus, en de nombreuses langues.",
                   "Du findest sie auf library.kiwix.org: Wikipedia, Wiktionary, Medizin, Handb\xC3\xBC" "cher und vieles mehr, in vielen Sprachen."},
    [S_EMPTY_3] = {"I file oltre 2 GB vanno divisi in parti (.zimaa, .zimab\xE2\x80\xA6): vedi la guida dell'app.",
                   "Files over 2 GB must be split into parts (.zimaa, .zimab\xE2\x80\xA6): see the app's guide.",
                   "Los archivos de m\xC3\xA1s de 2 GB deben dividirse en partes (.zimaa, .zimab\xE2\x80\xA6): consulta la gu\xC3\xAD" "a de la app.",
                   "Les fichiers de plus de 2 Go doivent \xC3\xAAtre d\xC3\xA9" "coup\xC3\xA9s en parties (.zimaa, .zimab\xE2\x80\xA6) : voir le guide de l'app.",
                   "Dateien \xC3\xBC" "ber 2 GB m\xC3\xBCssen in Teile zerlegt werden (.zimaa, .zimab\xE2\x80\xA6): siehe die Anleitung der App."},
    [S_ARTICLES] = {"voci", "articles", "art\xC3\xAD" "culos", "articles", "Artikel"},
    [S_SEARCH_IN] = {"Cerca in %s", "Search %s", "Buscar en %s", "Rechercher dans %s", "In %s suchen"},
    [S_NO_RESULTS] = {"Nessun titolo inizia cos\xC3\xAC", "No title starts like this", "Ning\xC3\xBAn t\xC3\xAD" "tulo empieza as\xC3\xAD",
                      "Aucun titre ne commence ainsi", "Kein Titel beginnt so"},
    [S_SEARCH_TITLES] = {"Cerca \xC2\xAB%s\xC2\xBB dentro i titoli", "Find \xE2\x80\x9C%s\xE2\x80\x9D inside titles",
                         "Buscar \xC2\xAB%s\xC2\xBB dentro de los t\xC3\xAD" "tulos", "Chercher \xC2\xAB\xC2\xA0%s\xC2\xA0\xC2\xBB dans les titres",
                         "\xE2\x80\x9E%s\xE2\x80\x9C in Titeln suchen"},
    [S_SEARCHING] = {"Ricerca nei titoli\xE2\x80\xA6 %d%%", "Searching titles\xE2\x80\xA6 %d%%", "Buscando en los t\xC3\xAD" "tulos\xE2\x80\xA6 %d%%",
                     "Recherche dans les titres\xE2\x80\xA6 %d\xC2\xA0%%", "Titel werden durchsucht\xE2\x80\xA6 %d\xC2\xA0%%"},
    [S_SECTIONS] = {"Sezioni", "Sections", "Secciones", "Sections", "Abschnitte"},
    [S_BOOKMARKS] = {"Segnalibri", "Bookmarks", "Marcadores", "Signets", "Lesezeichen"},
    [S_HISTORY] = {"Cronologia", "History", "Historial", "Historique", "Verlauf"},
    [S_NO_BOOKMARKS] = {"Nessun segnalibro. Tocca la stella in una voce per aggiungerla.",
                        "No bookmarks. Tap the star on an article to add it.",
                        "Ning\xC3\xBAn marcador. Toca la estrella de un art\xC3\xAD" "culo para a\xC3\xB1" "adirlo.",
                        "Aucun signet. Touchez l'\xC3\xA9toile d'un article pour l'ajouter.",
                        "Keine Lesezeichen. Tippe in einem Artikel auf den Stern, um ihn hinzuzuf\xC3\xBCgen."},
    [S_NO_HISTORY] = {"Ancora nessuna voce letta.", "Nothing read yet.", "A\xC3\xBAn no has le\xC3\xAD" "do nada.",
                      "Rien lu pour l'instant.", "Noch nichts gelesen."},
    [S_RANDOM] = {"Voce a caso", "Random article", "Art\xC3\xAD" "culo al azar", "Article au hasard", "Zuf\xC3\xA4lliger Artikel"},
    [S_TEXT_SIZE] = {"Testo", "Text", "Texto", "Texte", "Text"},
    [S_THEME_DARK] = {"Tema scuro", "Dark theme", "Tema oscuro", "Th\xC3\xA8me sombre", "Dunkles Design"},
    [S_THEME_LIGHT] = {"Tema chiaro", "Light theme", "Tema claro", "Th\xC3\xA8me clair", "Helles Design"},
    [S_INFO] = {"Informazioni sulla raccolta", "About this collection", "Informaci\xC3\xB3n de la colecci\xC3\xB3n",
                "\xC3\x80 propos de la collection", "\xC3\x9C" "ber diese Sammlung"},
    [S_MAIN_PAGE] = {"Pagina principale", "Main page", "P\xC3\xA1gina principal", "Page d'accueil", "Hauptseite"},
    [S_VERIFY] = {"Verifica integrit\xC3\xA0", "Check integrity", "Verificar integridad", "V\xC3\xA9rifier l'int\xC3\xA9grit\xC3\xA9",
                  "Integrit\xC3\xA4t pr\xC3\xBC" "fen"},
    [S_VERIFYING] = {"Verifica in corso\xE2\x80\xA6 %d%%", "Checking\xE2\x80\xA6 %d%%", "Verificando\xE2\x80\xA6 %d%%",
                     "V\xC3\xA9rification\xE2\x80\xA6 %d\xC2\xA0%%", "Pr\xC3\xBC" "fung l\xC3\xA4uft\xE2\x80\xA6 %d\xC2\xA0%%"},
    [S_VERIFY_OK] = {"File integro: l'MD5 corrisponde", "The file is intact: MD5 matches", "Archivo \xC3\xADntegro: el MD5 coincide",
                     "Fichier intact : le MD5 correspond", "Datei intakt: MD5 stimmt"},
    [S_VERIFY_BAD] = {"File danneggiato: l'MD5 non corrisponde", "The file is damaged: MD5 mismatch",
                      "Archivo da\xC3\xB1" "ado: el MD5 no coincide", "Fichier endommag\xC3\xA9 : le MD5 ne correspond pas",
                      "Datei besch\xC3\xA4" "digt: MD5 stimmt nicht"},
    [S_VERIFY_NONE] = {"Questo file non ha un MD5 da verificare", "This file has no checksum", "Este archivo no tiene suma de control",
                       "Ce fichier n'a pas de somme de contr\xC3\xB4le", "Diese Datei hat keine Pr\xC3\xBC" "fsumme"},
    [S_BOOKMARK_ADDED] = {"Aggiunta ai segnalibri", "Bookmarked", "A\xC3\xB1" "adido a marcadores", "Ajout\xC3\xA9 aux signets",
                          "Lesezeichen gesetzt"},
    [S_BOOKMARK_REMOVED] = {"Tolta dai segnalibri", "Bookmark removed", "Quitado de marcadores", "Signet retir\xC3\xA9",
                            "Lesezeichen entfernt"},
    [S_EXTERNAL_LINK] = {"Link esterno, serve Internet: %s", "External link, needs the Internet: %s",
                         "Enlace externo, requiere Internet: %s", "Lien externe, Internet requis : %s",
                         "Externer Link, braucht Internet: %s"},
    [S_NOT_FOUND] = {"Questa pagina non \xC3\xA8 nella raccolta", "This page is not in the collection",
                     "Esta p\xC3\xA1gina no est\xC3\xA1 en la colecci\xC3\xB3n", "Cette page n'est pas dans la collection",
                     "Diese Seite ist nicht in der Sammlung"},
    [S_UNSUPPORTED] = {"Contenuto non visualizzabile (%s)", "This content cannot be shown (%s)",
                       "Este contenido no se puede mostrar (%s)", "Ce contenu ne peut pas \xC3\xAAtre affich\xC3\xA9 (%s)",
                       "Dieser Inhalt kann nicht angezeigt werden (%s)"},
    [S_OPEN_FAILED] = {"Impossibile aprire la raccolta: %s", "Cannot open the collection: %s",
                       "No se puede abrir la colecci\xC3\xB3n: %s", "Impossible d'ouvrir la collection : %s",
                       "Sammlung kann nicht ge\xC3\xB6" "ffnet werden: %s"},
    [S_DAMAGED] = {"File non leggibile", "Unreadable file", "Archivo ilegible", "Fichier illisible", "Datei nicht lesbar"},
    [S_CONTINUE] = {"Continua a leggere", "Continue reading", "Seguir leyendo", "Reprendre la lecture", "Weiterlesen"},
    [S_LOADING] = {"Apertura\xE2\x80\xA6", "Opening\xE2\x80\xA6", "Abriendo\xE2\x80\xA6", "Ouverture\xE2\x80\xA6", "Wird ge\xC3\xB6" "ffnet\xE2\x80\xA6"},
    [S_SCANNING] = {"Lettura delle raccolte\xE2\x80\xA6 %d/%d", "Reading the collections\xE2\x80\xA6 %d/%d",
                    "Leyendo las colecciones\xE2\x80\xA6 %d/%d", "Lecture des collections\xE2\x80\xA6 %d/%d",
                    "Sammlungen werden gelesen\xE2\x80\xA6 %d/%d"},
    [S_F_TITLE] = {"Titolo", "Title", "T\xC3\xADtulo", "Titre", "Titel"},
    [S_F_DESC] = {"Descrizione", "Description", "Descripci\xC3\xB3n", "Description", "Beschreibung"},
    [S_F_LANG] = {"Lingua", "Language", "Idioma", "Langue", "Sprache"},
    [S_F_DATE] = {"Data", "Date", "Fecha", "Date", "Datum"},
    [S_F_CREATOR] = {"Autore", "Creator", "Autor", "Auteur", "Urheber"},
    [S_F_PUBLISHER] = {"Editore", "Publisher", "Editor", "\xC3\x89" "diteur", "Herausgeber"},
    [S_F_SOURCE] = {"Fonte", "Source", "Fuente", "Source", "Quelle"},
    [S_F_LICENSE] = {"Licenza", "License", "Licencia", "Licence", "Lizenz"},
    [S_F_FILE] = {"File", "File", "Archivo", "Fichier", "Datei"},
    [S_F_SIZE] = {"Dimensione", "Size", "Tama\xC3\xB1o", "Taille", "Gr\xC3\xB6\xC3\x9F" "e"},
    [S_F_ENTRIES] = {"Voci", "Articles", "Art\xC3\xAD" "culos", "Articles", "Artikel"},
    [S_F_FORMAT] = {"Formato", "Format", "Formato", "Format", "Format"},
    [S_F_NAME] = {"Nome", "Name", "Nombre", "Nom", "Name"},
    [S_MONTHS] = {"gennaio|febbraio|marzo|aprile|maggio|giugno|luglio|agosto|settembre|ottobre|novembre|dicembre",
                  "January|February|March|April|May|June|July|August|September|October|November|December",
                  "enero|febrero|marzo|abril|mayo|junio|julio|agosto|septiembre|octubre|noviembre|diciembre",
                  "janvier|f\xC3\xA9vrier|mars|avril|mai|juin|juillet|ao\xC3\xBBt|septembre|octobre|novembre|d\xC3\xA9" "cembre",
                  "Januar|Februar|M\xC3\xA4rz|April|Mai|Juni|Juli|August|September|Oktober|November|Dezember"},
    [S_TOP] = {"Inizio della voce", "Top of the article", "Inicio del art\xC3\xAD" "culo", "D\xC3\xA9" "but de l'article",
               "Anfang des Artikels"},
    [S_COPY_HINT] = {"Cerca un titolo", "Search for a title", "Buscar un t\xC3\xAD" "tulo", "Rechercher un titre", "Titel suchen"},
    [S_MOVED_HINT] = {"Per averla sempre nella Biblioteca, sposta il file nella cartella zim della home",
                      "To keep it in the Library, move the file to the zim folder of your home",
                      "Para tenerla siempre en la Biblioteca, mueve el archivo a la carpeta zim de tu home",
                      "Pour la garder dans la Biblioth\xC3\xA8que, d\xC3\xA9placez le fichier dans le dossier zim de votre home",
                      "Um sie in der Bibliothek zu behalten, verschiebe die Datei in den Ordner zim deines Home-Verzeichnisses"},
};

void i18n_init(void) {
    char b[16] = {0};
    nv_lang(b, sizeof b);
    static const char *const kCodes[L_N] = {"it", "en", "es", "fr", "de"};
    s_lang = L_EN;
    for (int i = 0; i < L_N; i++)
        if (!strncmp(b, kCodes[i], 2)) s_lang = i;
}

const char *tr(int id) {
    if (id < 0 || id >= S_COUNT) return "";
    const char *s = kStr[id][s_lang];
    return s ? s : kStr[id][L_EN] ? kStr[id][L_EN] : "";
}

int i18n_italian_keys(void) { return s_lang == L_IT; }

const char *lang_name(const char *code) {   // ISO 639-3 (or -1) of a collection -> its own name
    static const struct { const char *a, *b, *name; } k[] = {
        {"ita", "it", "Italiano"}, {"eng", "en", "English"}, {"spa", "es", "Espa\xC3\xB1ol"}, {"fra", "fr", "Fran\xC3\xA7" "ais"},
        {"deu", "de", "Deutsch"}, {"por", "pt", "Portugu\xC3\xAAs"}, {"nld", "nl", "Nederlands"}, {"pol", "pl", "Polski"},
        {"ron", "ro", "Rom\xC3\xA2n\xC4\x83"}, {"cat", "ca", "Catal\xC3\xA0"}, {"swe", "sv", "Svenska"}, {"dan", "da", "Dansk"},
        {"nor", "no", "Norsk"}, {"fin", "fi", "Suomi"}, {"ces", "cs", "\xC4\x8C" "e\xC5\xA1tina"}, {"slk", "sk", "Sloven\xC4\x8D" "ina"},
        {"hun", "hu", "Magyar"}, {"tur", "tr", "T\xC3\xBCrk\xC3\xA7" "e"}, {"ell", "el", "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC"},
        {"rus", "ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"}, {"ukr", "uk", "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0"},
        {"srp", "sr", "\xD0\xA1\xD1\x80\xD0\xBF\xD1\x81\xD0\xBA\xD0\xB8"}, {"hrv", "hr", "Hrvatski"}, {"slv", "sl", "Sloven\xC5\xA1\xC4\x8D" "ina"},
        {"eus", "eu", "Euskara"}, {"glg", "gl", "Galego"}, {"lat", "la", "Latina"}, {"epo", "eo", "Esperanto"},
        {"vie", "vi", "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t"}, {"ind", "id", "Bahasa Indonesia"}, {"mul", "", "Multilingue"},
    };
    static char buf[24];
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strncmp(code, k[i].a, 3) || (k[i].b[0] && !strcmp(code, k[i].b))) return k[i].name;
    snprintf(buf, sizeof buf, "%s", code);   // a script the fonts lack, or an unknown code: as written
    for (char *p = buf; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    return buf;
}

void fmt_date(const char *ymd, char *out, size_t cap) {   // "2026-07-01" -> "luglio 2026"
    int y = 0, m = 0;
    if (sscanf(ymd, "%d-%d", &y, &m) != 2 || m < 1 || m > 12) { snprintf(out, cap, "%s", ymd); return; }
    const char *p = tr(S_MONTHS);
    for (int i = 1; i < m && p; i++) {
        p = strchr(p, '|');
        if (p) p++;
    }
    if (!p) { snprintf(out, cap, "%s", ymd); return; }
    const char *e = strchr(p, '|');
    snprintf(out, cap, "%.*s %d", (int)(e ? e - p : (long)strlen(p)), p, y);
}

static char dec_sep(void) { return s_lang == L_EN ? '.' : ','; }

void fmt_size(uint64_t b, char *out, size_t cap) {
    const char *unit = s_lang == L_FR ? "Mo" : "MB", *gunit = s_lang == L_FR ? "Go" : "GB";
    const double mb = (double)b / 1e6;
    if (mb < 10) snprintf(out, cap, "%d%c%d %s", (int)mb, dec_sep(), (int)(mb * 10) % 10, unit);
    else if (mb < 1000) snprintf(out, cap, "%d %s", (int)(mb + 0.5), unit);
    else snprintf(out, cap, "%d%c%d %s", (int)(mb / 1000), dec_sep(), (int)(mb / 100) % 10, gunit);
}

void fmt_count(long n, char *out, size_t cap) {
    const char *sep = s_lang == L_EN ? "," : s_lang == L_FR ? "\xE2\x80\xAF" : ".";
    char digits[24];
    snprintf(digits, sizeof digits, "%ld", n < 0 ? 0 : n);
    const int len = (int)strlen(digits);
    size_t k = 0;
    for (int i = 0; i < len && k + 4 < cap; i++) {
        if (i && (len - i) % 3 == 0) {
            for (const char *s = sep; *s && k + 1 < cap; s++) out[k++] = *s;
        }
        out[k++] = digits[i];
    }
    out[k] = 0;
}
