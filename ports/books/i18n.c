// i18n.c — the interface in the five languages of the system (it, en, es, fr, de), chosen from
// nv_lang(); English when the system speaks another. Language names for the book's own language.
#include "bk.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "nucleo_sdk.h"

enum { L_IT, L_EN, L_ES, L_FR, L_DE, L_COUNT };
static int s_lang = L_EN;
static const char *const kCodes[L_COUNT] = {"it", "en", "es", "fr", "de"};

static const char *const kStr[S_COUNT][L_COUNT] = {
    [S_LIBRARY] = {"Libri", "Books", "Libros", "Livres", "B\xC3\xBC" "cher"},
    [S_EMPTY_TITLE] = {"La tua libreria \xC3\xA8 vuota", "Your library is empty", "Tu biblioteca est\xC3\xA1 vac\xC3\xAD" "a",
                       "Votre biblioth\xC3\xA8que est vide", "Deine Bibliothek ist leer"},
    [S_EMPTY_1] = {"Scarica libri gratuiti dal catalogo: classici, romanzi, manuali.",
                   "Download free books from the catalog: classics, novels, manuals.",
                   "Descarga libros gratuitos del cat\xC3\xA1logo: cl\xC3\xA1sicos, novelas, manuales.",
                   "T\xC3\xA9l\xC3\xA9" "chargez des livres gratuits du catalogue : classiques, romans, manuels.",
                   "Lade kostenlose B\xC3\xBC" "cher aus dem Katalog: Klassiker, Romane, Handb\xC3\xBC" "cher."},
    [S_EMPTY_2] = {"Oppure copia file EPUB, FB2, CBZ o TXT nella cartella books.",
                   "Or copy EPUB, FB2, CBZ or TXT files into the books folder.",
                   "O copia archivos EPUB, FB2, CBZ o TXT en la carpeta books.",
                   "Ou copiez des fichiers EPUB, FB2, CBZ ou TXT dans le dossier books.",
                   "Oder kopiere EPUB-, FB2-, CBZ- oder TXT-Dateien in den Ordner books."},
    [S_CONTINUE] = {"Continua a leggere", "Continue reading", "Seguir leyendo", "Reprendre la lecture", "Weiterlesen"},
    [S_OPEN_FAILED] = {"Impossibile aprire il libro", "The book could not be opened", "No se pudo abrir el libro",
                       "Impossible d'ouvrir le livre", "Das Buch konnte nicht ge\xC3\xB6" "ffnet werden"},
    [S_FORMAT_BAD] = {"File danneggiato o formato non supportato", "Damaged file or unsupported format",
                      "Archivo da\xC3\xB1" "ado o formato no compatible", "Fichier endommag\xC3\xA9 ou format non pris en charge",
                      "Besch\xC3\xA4" "digte Datei oder nicht unterst\xC3\xBCtztes Format"},
    [S_DRM] = {"Libro protetto da DRM: non pu\xC3\xB2 essere letto", "This book is DRM-protected and cannot be read",
               "Libro protegido con DRM: no se puede leer", "Livre prot\xC3\xA9g\xC3\xA9 par DRM : illisible",
               "DRM-gesch\xC3\xBCtztes Buch: nicht lesbar"},
    [S_LOADING] = {"Apertura\xE2\x80\xA6", "Opening\xE2\x80\xA6", "Abriendo\xE2\x80\xA6", "Ouverture\xE2\x80\xA6", "Wird ge\xC3\xB6" "ffnet\xE2\x80\xA6"},
    [S_SCANNING] = {"Libreria: %d di %d", "Library: %d of %d", "Biblioteca: %d de %d", "Biblioth\xC3\xA8que : %d sur %d",
                    "Bibliothek: %d von %d"},
    [S_CONTENTS] = {"Indice", "Contents", "\xC3\x8Dndice", "Sommaire", "Inhalt"},
    [S_BOOKMARKS] = {"Segnalibri", "Bookmarks", "Marcadores", "Signets", "Lesezeichen"},
    [S_NO_BOOKMARKS] = {"Nessun segnalibro. Tocca il nastro in alto per aggiungerne uno.",
                        "No bookmarks yet. Tap the ribbon at the top to add one.",
                        "Sin marcadores. Toca la cinta de arriba para a\xC3\xB1" "adir uno.",
                        "Aucun signet. Touchez le ruban en haut pour en ajouter un.",
                        "Keine Lesezeichen. Tippe oben auf das Band, um eines zu setzen."},
    [S_BOOKMARK_ADDED] = {"Segnalibro aggiunto", "Bookmark added", "Marcador a\xC3\xB1" "adido", "Signet ajout\xC3\xA9", "Lesezeichen gesetzt"},
    [S_BOOKMARK_REMOVED] = {"Segnalibro rimosso", "Bookmark removed", "Marcador eliminado", "Signet supprim\xC3\xA9", "Lesezeichen entfernt"},
    [S_SEARCH_IN] = {"Cerca nel libro", "Search in the book", "Buscar en el libro", "Rechercher dans le livre", "Im Buch suchen"},
    [S_SEARCHING] = {"Ricerca: capitolo %d di %d\xE2\x80\xA6", "Searching: chapter %d of %d\xE2\x80\xA6",
                     "Buscando: cap\xC3\xADtulo %d de %d\xE2\x80\xA6", "Recherche : chapitre %d sur %d\xE2\x80\xA6",
                     "Suche: Kapitel %d von %d\xE2\x80\xA6"},
    [S_NO_RESULTS] = {"Nessun risultato", "No results", "Sin resultados", "Aucun r\xC3\xA9sultat", "Keine Treffer"},
    [S_RESULTS] = {"%d risultati", "%d results", "%d resultados", "%d r\xC3\xA9sultats", "%d Treffer"},
    [S_APPEARANCE] = {"Aspetto", "Appearance", "Apariencia", "Apparence", "Darstellung"},
    [S_THEME] = {"Tema", "Theme", "Tema", "Th\xC3\xA8me", "Thema"},
    [S_TH_WHITE] = {"Bianco", "White", "Blanco", "Blanc", "Wei\xC3\x9F"},
    [S_TH_SEPIA] = {"Seppia", "Sepia", "Sepia", "S\xC3\xA9pia", "Sepia"},
    [S_TH_DARK] = {"Scuro", "Dark", "Oscuro", "Sombre", "Dunkel"},
    [S_TH_BLACK] = {"Nero", "Black", "Negro", "Noir", "Schwarz"},
    [S_TH_EINK] = {"E-ink", "E-ink", "E-ink", "E-ink", "E-Ink"},
    [S_TH_MONO] = {"1 bit", "1-bit", "1 bit", "1 bit", "1 Bit"},
    [S_FONT] = {"Carattere", "Font", "Fuente", "Police", "Schrift"},
    [S_SERIF] = {"Serif", "Serif", "Serif", "Serif", "Serif"},
    [S_SANS] = {"Sans", "Sans", "Sans", "Sans", "Sans"},
    [S_SIZE] = {"Dimensione", "Size", "Tama\xC3\xB1o", "Taille", "Gr\xC3\xB6\xC3\x9F" "e"},
    [S_SPACING] = {"Interlinea", "Line spacing", "Interlineado", "Interligne", "Zeilenabstand"},
    [S_MARGINS] = {"Margini", "Margins", "M\xC3\xA1rgenes", "Marges", "R\xC3\xA4nder"},
    [S_JUSTIFY] = {"Giustificato", "Justified", "Justificado", "Justifi\xC3\xA9", "Blocksatz"},
    [S_HYPHEN] = {"Sillabazione", "Hyphenation", "Divisi\xC3\xB3n sil\xC3\xA1" "bica", "C\xC3\xA9sure", "Silbentrennung"},
    [S_INDENT] = {"Rientro paragrafi", "Paragraph indent", "Sangr\xC3\xAD" "a", "Alin\xC3\xA9" "a", "Einzug"},
    [S_TURN] = {"Sfoglia", "Page turn", "Pasar p\xC3\xA1gina", "Tourner", "Bl\xC3\xA4ttern"},
    [S_TURN_SLIDE] = {"Scorri", "Slide", "Deslizar", "Glisser", "Schieben"},
    [S_TURN_COVER] = {"Copri", "Cover", "Cubrir", "Recouvrir", "Decken"},
    [S_TURN_NONE] = {"Nessuna", "None", "Ninguna", "Aucune", "Keine"},
    [S_EINK_REFRESH] = {"Refresh completo ogni %d pagine", "Full refresh every %d pages", "Refresco completo cada %d p\xC3\xA1ginas",
                        "Rafra\xC3\xAE" "chissement complet toutes les %d pages", "Vollst\xC3\xA4ndig auffrischen alle %d Seiten"},
    [S_TAP_ZONES] = {"Tocca i lati per sfogliare", "Tap the sides to turn pages", "Toca los lados para pasar p\xC3\xA1gina",
                     "Touchez les bords pour tourner", "Seitenr\xC3\xA4nder tippen zum Bl\xC3\xA4ttern"},
    [S_STATUS_LINE] = {"Riga di stato", "Status line", "L\xC3\xADnea de estado", "Ligne d'\xC3\xA9tat", "Statuszeile"},
    [S_ON] = {"S\xC3\xAC", "On", "S\xC3\xAD", "Oui", "An"},
    [S_OFF] = {"No", "Off", "No", "Non", "Aus"},
    [S_INFO] = {"Informazioni", "About this book", "Informaci\xC3\xB3n", "Informations", "Informationen"},
    [S_F_TITLE] = {"Titolo", "Title", "T\xC3\xADtulo", "Titre", "Titel"},
    [S_F_AUTHOR] = {"Autore", "Author", "Autor", "Auteur", "Autor"},
    [S_F_LANG] = {"Lingua", "Language", "Idioma", "Langue", "Sprache"},
    [S_F_PUBLISHER] = {"Editore", "Publisher", "Editorial", "\xC3\x89" "diteur", "Verlag"},
    [S_F_DATE] = {"Data", "Date", "Fecha", "Date", "Datum"},
    [S_F_RIGHTS] = {"Diritti", "Rights", "Derechos", "Droits", "Rechte"},
    [S_F_SOURCE] = {"Fonte", "Source", "Fuente", "Source", "Quelle"},
    [S_F_FILE] = {"File", "File", "Archivo", "Fichier", "Datei"},
    [S_F_SIZE] = {"Dimensione", "Size", "Tama\xC3\xB1o", "Taille", "Gr\xC3\xB6\xC3\x9F" "e"},
    [S_F_FORMAT] = {"Formato", "Format", "Formato", "Format", "Format"},
    [S_F_CHAPTERS] = {"Capitoli", "Chapters", "Cap\xC3\xADtulos", "Chapitres", "Kapitel"},
    [S_F_SUBJECT] = {"Argomento", "Subject", "Tema", "Sujet", "Thema"},
    [S_DELETE] = {"Elimina", "Delete", "Eliminar", "Supprimer", "L\xC3\xB6" "schen"},
    [S_DELETE_SURE] = {"Tocca ancora per eliminare", "Tap again to delete", "Toca de nuevo para eliminar",
                       "Touchez encore pour supprimer", "Zum L\xC3\xB6schen erneut tippen"},
    [S_DELETED] = {"Libro eliminato", "Book deleted", "Libro eliminado", "Livre supprim\xC3\xA9", "Buch gel\xC3\xB6scht"},
    [S_PAGE_OF] = {"%d di %d", "%d of %d", "%d de %d", "%d sur %d", "%d von %d"},
    [S_CHAPTER_N] = {"Capitolo %d", "Chapter %d", "Cap\xC3\xADtulo %d", "Chapitre %d", "Kapitel %d"},
    [S_PAGES_LEFT] = {"%d pagine alla fine del capitolo", "%d pages left in chapter", "%d p\xC3\xA1ginas para el final del cap\xC3\xADtulo",
                      "%d pages avant la fin du chapitre", "noch %d Seiten im Kapitel"},
    [S_PERCENT_READ] = {"%d%% letto", "%d%% read", "%d%% le\xC3\xAD" "do", "%d%% lu", "%d%% gelesen"},
    [S_GO_TO] = {"Vai", "Go", "Ir", "Aller", "Gehe zu"},
    [S_EXTERNAL_LINK] = {"Collegamento esterno: %s", "External link: %s", "Enlace externo: %s", "Lien externe : %s", "Externer Link: %s"},
    [S_NOT_FOUND] = {"Destinazione non trovata", "Link target not found", "Destino no encontrado", "Cible introuvable", "Ziel nicht gefunden"},
    [S_NOTE] = {"Nota", "Note", "Nota", "Note", "Anmerkung"},
    [S_SORT] = {"Ordina", "Sort", "Ordenar", "Trier", "Sortieren"},
    [S_SORT_RECENT] = {"Recenti", "Recent", "Recientes", "R\xC3\xA9" "cents", "Zuletzt"},
    [S_SORT_TITLE] = {"Titolo", "Title", "T\xC3\xADtulo", "Titre", "Titel"},
    [S_SORT_AUTHOR] = {"Autore", "Author", "Autor", "Auteur", "Autor"},
    [S_SORT_PROGRESS] = {"Avanzamento", "Progress", "Progreso", "Progression", "Fortschritt"},
    [S_NEW] = {"Nuovo", "New", "Nuevo", "Nouveau", "Neu"},
    [S_FINISHED] = {"Finito", "Finished", "Terminado", "Termin\xC3\xA9", "Gelesen"},
    [S_UNTITLED] = {"Senza titolo", "Untitled", "Sin t\xC3\xADtulo", "Sans titre", "Ohne Titel"},
    [S_UNKNOWN_AUTHOR] = {"Autore sconosciuto", "Unknown author", "Autor desconocido", "Auteur inconnu", "Unbekannter Autor"},
    [S_CATALOG] = {"Catalogo", "Catalog", "Cat\xC3\xA1logo", "Catalogue", "Katalog"},
    [S_CAT_LOADING] = {"Caricamento del catalogo\xE2\x80\xA6", "Loading the catalog\xE2\x80\xA6", "Cargando el cat\xC3\xA1logo\xE2\x80\xA6",
                       "Chargement du catalogue\xE2\x80\xA6", "Katalog wird geladen\xE2\x80\xA6"},
    [S_CAT_OFFLINE] = {"Catalogo non raggiungibile. Controlla il Wi-Fi.", "The catalog cannot be reached. Check the Wi-Fi.",
                       "No se puede acceder al cat\xC3\xA1logo. Comprueba el Wi-Fi.",
                       "Catalogue inaccessible. V\xC3\xA9rifiez le Wi-Fi.", "Katalog nicht erreichbar. Pr\xC3\xBC" "fe das WLAN."},
    [S_CAT_RETRY] = {"Riprova", "Retry", "Reintentar", "R\xC3\xA9" "essayer", "Erneut"},
    [S_CAT_ALL] = {"Tutti", "All", "Todos", "Tous", "Alle"},
    [S_CAT_LANGS] = {"Lingue", "Languages", "Idiomas", "Langues", "Sprachen"},
    [S_CAT_DOWNLOAD] = {"Scarica", "Download", "Descargar", "T\xC3\xA9l\xC3\xA9" "charger", "Laden"},
    [S_CAT_QUEUED] = {"In coda", "Queued", "En cola", "En attente", "Wartet"},
    [S_CAT_HAVE] = {"Nella libreria", "In your library", "En tu biblioteca", "Dans la biblioth\xC3\xA8que", "In der Bibliothek"},
    [S_CAT_READ] = {"Leggi", "Read", "Leer", "Lire", "Lesen"},
    [S_CAT_PROGRESS] = {"%d%%", "%d%%", "%d%%", "%d%%", "%d%%"},
    [S_CAT_FAILED] = {"Download non riuscito", "Download failed", "Descarga fallida", "\xC3\x89" "chec du t\xC3\xA9l\xC3\xA9" "chargement", "Download fehlgeschlagen"},
    [S_CAT_NO_ROOM] = {"Spazio insufficiente sulla scheda SD", "Not enough room on the SD card", "No hay espacio suficiente en la tarjeta SD",
                       "Espace insuffisant sur la carte SD", "Nicht genug Platz auf der SD-Karte"},
    [S_CAT_SOURCE] = {"Fonte", "Source", "Fuente", "Source", "Quelle"},
    [S_CAT_LICENSE] = {"Licenza", "License", "Licencia", "Licence", "Lizenz"},
    [S_CAT_SEARCH] = {"Cerca titolo o autore", "Search title or author", "Buscar t\xC3\xADtulo o autor", "Titre ou auteur", "Titel oder Autor suchen"},
    [S_CAT_DONE] = {"\xE2\x80\x9C%s\xE2\x80\x9D \xC3\xA8 nella libreria", "\xE2\x80\x9C%s\xE2\x80\x9D is in your library",
                    "\xE2\x80\x9C%s\xE2\x80\x9D est\xC3\xA1 en tu biblioteca", "\xC2\xAB %s \xC2\xBB est dans la biblioth\xC3\xA8que",
                    "\xE2\x80\x9E%s\xE2\x80\x9C ist in der Bibliothek"},
    [S_CAT_DOWNLOADS] = {"Download", "Downloads", "Descargas", "T\xC3\xA9l\xC3\xA9" "chargements", "Downloads"},
    [S_CAT_NONE] = {"Nessun libro in questa selezione", "No books in this selection", "Ning\xC3\xBAn libro en esta selecci\xC3\xB3n",
                    "Aucun livre dans cette s\xC3\xA9lection", "Keine B\xC3\xBC" "cher in dieser Auswahl"},
    [S_CAT_CANCEL] = {"Annulla", "Cancel", "Cancelar", "Annuler", "Abbrechen"},
    [S_CAT_BOOKS] = {"%d libri", "%d books", "%d libros", "%d livres", "%d B\xC3\xBC" "cher"},
    [S_CAT_POPULAR] = {"Popolari", "Popular", "Populares", "Populaires", "Beliebt"},
    [S_BACK_TO] = {"Torna a pagina %d", "Back to page %d", "Volver a la p\xC3\xA1gina %d", "Retour \xC3\xA0 la page %d", "Zur\xC3\xBC" "ck zu Seite %d"},
    [S_ZOOM_HINT] = {"Pizzica o tocca due volte per ingrandire", "Pinch or double-tap to zoom", "Pellizca o toca dos veces para ampliar",
                     "Pincez ou touchez deux fois pour zoomer", "Zum Zoomen ziehen oder doppelt tippen"},
    [S_BOOK_END] = {"Fine del libro", "The end of the book", "Fin del libro", "Fin du livre", "Ende des Buches"},
    [S_DOWNLOADING] = {"Download di %d libri in corso", "Downloading %d books", "Descargando %d libros", "T\xC3\xA9l\xC3\xA9" "chargement de %d livres",
                       "%d B\xC3\xBC" "cher werden geladen"},
    [S_COLUMNS] = {"Pagine affiancate", "Pages side by side", "P\xC3\xA1ginas lado a lado", "Pages c\xC3\xB4te \xC3\xA0 c\xC3\xB4te",
                   "Doppelseiten"},
    [S_AUTO] = {"Auto", "Auto", "Auto", "Auto", "Auto"},
    [S_CAT_CATEGORY] = {"Categoria", "Category", "Categor\xC3\xAD" "a", "Cat\xC3\xA9gorie", "Kategorie"},
    [S_ABOUT] = {"Informazioni su Libri", "About Books", "Acerca de Libros", "\xC3\x80 propos de Livres", "\xC3\x9C" "ber B\xC3\xBC" "cher"},
    [S_NOTICES] = {"Licenze di terze parti", "Third-party licenses", "Licencias de terceros", "Licences tierces", "Lizenzen Dritter"},
    [S_CAT_PD] = {"Pubblico dominio", "Public domain", "Dominio p\xC3\xBA" "blico", "Domaine public", "Gemeinfrei"},
};

void i18n_init(void) {
    char b[16] = "";
    nv_lang(b, sizeof b);
    s_lang = L_EN;
    for (int i = 0; i < L_COUNT; i++)
        if (!strncasecmp(b, kCodes[i], 2)) s_lang = i;
}

const char *tr(int id) {
    if (id < 0 || id >= S_COUNT) return "";
    const char *s = kStr[id][s_lang];
    return s ? s : kStr[id][L_EN] ? kStr[id][L_EN] : "";
}

const char *ui_lang(void) { return kCodes[s_lang]; }
int i18n_italian_keys(void) { return s_lang == L_IT; }

// Language names, by ISO 639-1 (and the 639-2 codes books use instead), in the interface language.
static const struct { const char *c1, *c2; const char *n[L_COUNT]; } kLangs[] = {
    {"it", "ita", {"Italiano", "Italian", "Italiano", "Italien", "Italienisch"}},
    {"en", "eng", {"Inglese", "English", "Ingl\xC3\xA9s", "Anglais", "Englisch"}},
    {"es", "spa", {"Spagnolo", "Spanish", "Espa\xC3\xB1ol", "Espagnol", "Spanisch"}},
    {"fr", "fra", {"Francese", "French", "Franc\xC3\xA9s", "Fran\xC3\xA7" "ais", "Franz\xC3\xB6sisch"}},
    {"de", "deu", {"Tedesco", "German", "Alem\xC3\xA1n", "Allemand", "Deutsch"}},
    {"pt", "por", {"Portoghese", "Portuguese", "Portugu\xC3\xA9s", "Portugais", "Portugiesisch"}},
    {"la", "lat", {"Latino", "Latin", "Lat\xC3\xADn", "Latin", "Latein"}},
    {"el", "ell", {"Greco", "Greek", "Griego", "Grec", "Griechisch"}},
    {"grc", "grc", {"Greco antico", "Ancient Greek", "Griego antiguo", "Grec ancien", "Altgriechisch"}},
    {"ru", "rus", {"Russo", "Russian", "Ruso", "Russe", "Russisch"}},
    {"nl", "nld", {"Olandese", "Dutch", "Neerland\xC3\xA9s", "N\xC3\xA9" "erlandais", "Niederl\xC3\xA4ndisch"}},
    {"pl", "pol", {"Polacco", "Polish", "Polaco", "Polonais", "Polnisch"}},
    {"sv", "swe", {"Svedese", "Swedish", "Sueco", "Su\xC3\xA9" "dois", "Schwedisch"}},
    {"da", "dan", {"Danese", "Danish", "Dan\xC3\xA9s", "Danois", "D\xC3\xA4nisch"}},
    {"fi", "fin", {"Finlandese", "Finnish", "Finland\xC3\xA9s", "Finnois", "Finnisch"}},
    {"no", "nor", {"Norvegese", "Norwegian", "Noruego", "Norv\xC3\xA9gien", "Norwegisch"}},
    {"hu", "hun", {"Ungherese", "Hungarian", "H\xC3\xBAngaro", "Hongrois", "Ungarisch"}},
    {"cs", "ces", {"Ceco", "Czech", "Checo", "Tch\xC3\xA8que", "Tschechisch"}},
    {"ca", "cat", {"Catalano", "Catalan", "Catal\xC3\xA1n", "Catalan", "Katalanisch"}},
    {"ro", "ron", {"Rumeno", "Romanian", "Rumano", "Roumain", "Rum\xC3\xA4nisch"}},
    {"uk", "ukr", {"Ucraino", "Ukrainian", "Ucraniano", "Ukrainien", "Ukrainisch"}},
    {"eo", "epo", {"Esperanto", "Esperanto", "Esperanto", "Esp\xC3\xA9ranto", "Esperanto"}},
    {"tr", "tur", {"Turco", "Turkish", "Turco", "Turc", "T\xC3\xBCrkisch"}},
};

const char *lang_name(const char *code) {
    if (!code || !*code) return "";
    char c[8];
    size_t k = 0;
    for (; code[k] && code[k] != '-' && code[k] != '_' && k + 1 < sizeof c; k++) c[k] = (char)(code[k] | 0x20);
    c[k] = 0;
    for (size_t i = 0; i < sizeof kLangs / sizeof kLangs[0]; i++)
        if (!strcmp(c, kLangs[i].c1) || !strcmp(c, kLangs[i].c2)) return kLangs[i].n[s_lang];
    return code;
}

void fmt_size(uint64_t b, char *out, size_t cap) {
    const char *sep = s_lang == L_EN ? "." : ",";
    if (b < 1024) snprintf(out, cap, "%u B", (unsigned)b);
    else if (b < 1024 * 1024) snprintf(out, cap, "%u KB", (unsigned)((b + 512) / 1024));
    else if (b < 10ull * 1024 * 1024) snprintf(out, cap, "%u%s%u MB", (unsigned)(b >> 20), sep, (unsigned)((b % (1 << 20)) * 10 >> 20));
    else snprintf(out, cap, "%u MB", (unsigned)((b + (1 << 19)) >> 20));
}

void fmt_date(int64_t t, char *out, size_t cap) {
    if (t <= 0) { snprintf(out, cap, "-"); return; }
    const time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    if (s_lang == L_EN) snprintf(out, cap, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    else snprintf(out, cap, "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
}
