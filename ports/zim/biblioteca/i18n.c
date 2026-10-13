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
    [S_EMPTY_1] = {"Scaricale dal catalogo di Kiwix con il tasto qui sotto: Wikipedia, Wikizionario, medicina, manuali e molto altro, in tante lingue.",
                   "Download them from the Kiwix catalog with the button below: Wikipedia, Wiktionary, medicine, manuals and much more, in many languages.",
                   "Desc\xC3\xA1rgalas del cat\xC3\xA1logo de Kiwix con el bot\xC3\xB3n de abajo: Wikipedia, Wikcionario, medicina, manuales y mucho m\xC3\xA1s, en muchos idiomas.",
                   "T\xC3\xA9l\xC3\xA9" "chargez-les depuis le catalogue Kiwix avec le bouton ci-dessous : Wikip\xC3\xA9" "dia, Wiktionnaire, m\xC3\xA9" "decine, manuels et bien plus, en de nombreuses langues.",
                   "Lade sie mit der Taste unten aus dem Kiwix-Katalog: Wikipedia, Wiktionary, Medizin, Handb\xC3\xBC" "cher und vieles mehr, in vielen Sprachen."},
    [S_EMPTY_2] = {"Oppure copia i file .zim nella cartella zim della tua home (sulla scheda SD: home/zim).",
                   "Or copy .zim files to the zim folder of your home (on the SD card: home/zim).",
                   "O copia los archivos .zim en la carpeta zim de tu home (en la tarjeta SD: home/zim).",
                   "Ou copiez les fichiers .zim dans le dossier zim de votre home (sur la carte SD : home/zim).",
                   "Oder kopiere .zim-Dateien in den Ordner zim deines Home-Verzeichnisses (auf der SD-Karte: home/zim)."},
    [S_EMPTY_3] = {"I file copiati oltre 2 GB vanno divisi in parti (.zimaa, .zimab\xE2\x80\xA6): vedi la guida dell'app.",
                   "Copied files over 2 GB must be split into parts (.zimaa, .zimab\xE2\x80\xA6): see the app's guide.",
                   "Los archivos copiados de m\xC3\xA1s de 2 GB deben dividirse en partes (.zimaa, .zimab\xE2\x80\xA6): consulta la gu\xC3\xAD" "a de la app.",
                   "Les fichiers copi\xC3\xA9s de plus de 2 Go doivent \xC3\xAAtre d\xC3\xA9" "coup\xC3\xA9s en parties (.zimaa, .zimab\xE2\x80\xA6) : voir le guide de l'app.",
                   "Kopierte Dateien \xC3\xBC" "ber 2 GB m\xC3\xBCssen in Teile zerlegt werden (.zimaa, .zimab\xE2\x80\xA6): siehe die Anleitung der App."},
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
    [S_GET_TITLE] = {"Scarica raccolte", "Download collections", "Descargar colecciones", "T\xC3\xA9l\xC3\xA9" "charger des collections", "Sammlungen herunterladen"},
    [S_GET_RECOMMENDED] = {"Consigliate", "Recommended", "Recomendadas", "Recommand\xC3\xA9" "es", "Empfohlen"},
    [S_GET_ALL] = {"Tutte", "All", "Todas", "Toutes", "Alle"},
    [S_GET_DOWNLOADS] = {"Download", "Downloads", "Descargas", "T\xC3\xA9l\xC3\xA9" "chargements", "Downloads"},
    [S_GET_OTHER] = {"Altro", "Other", "Otras", "Autres", "Andere"},
    [S_GET_LOADING] = {"Carico il catalogo di Kiwix\xE2\x80\xA6", "Loading the Kiwix catalog\xE2\x80\xA6", "Cargando el cat\xC3\xA1logo de Kiwix\xE2\x80\xA6", "Chargement du catalogue Kiwix\xE2\x80\xA6", "Kiwix-Katalog wird geladen\xE2\x80\xA6"},
    [S_GET_OFFLINE] = {"Il catalogo non si raggiunge: serve una connessione a Internet.", "The catalog cannot be reached: an Internet connection is needed.", "No se puede acceder al cat\xC3\xA1logo: hace falta una conexi\xC3\xB3n a Internet.", "Le catalogue est inaccessible : une connexion Internet est n\xC3\xA9" "cessaire.", "Der Katalog ist nicht erreichbar: Eine Internetverbindung wird ben\xC3\xB6tigt."},
    [S_GET_RETRY] = {"Riprova", "Try again", "Reintentar", "R\xC3\xA9" "essayer", "Erneut versuchen"},
    [S_GET_MAXI] = {"Con immagini", "With pictures", "Con im\xC3\xA1genes", "Avec images", "Mit Bildern"},
    [S_GET_NOPIC] = {"Senza immagini", "Without pictures", "Sin im\xC3\xA1genes", "Sans images", "Ohne Bilder"},
    [S_GET_MINI] = {"Solo le introduzioni", "Introductions only", "Solo las introducciones", "Introductions seulement", "Nur die Einleitungen"},
    [S_GET_DOWNLOAD] = {"Scarica", "Download", "Descargar", "T\xC3\xA9l\xC3\xA9" "charger", "Laden"},
    [S_GET_QUEUED] = {"In coda", "Waiting", "En cola", "En attente", "Wartet"},
    [S_GET_HAVE] = {"Gi\xC3\xA0 nella Biblioteca", "Already in the library", "Ya est\xC3\xA1 en la biblioteca", "D\xC3\xA9j\xC3\xA0 dans la biblioth\xC3\xA8que", "Schon in der Bibliothek"},
    [S_GET_UPDATE] = {"Edizione pi\xC3\xB9 recente", "Newer edition", "Edici\xC3\xB3n m\xC3\xA1s reciente", "\xC3\x89" "dition plus r\xC3\xA9" "cente", "Neuere Ausgabe"},
    [S_GET_ROOM] = {"Spazio per i download: %s", "Room for downloads: %s", "Espacio para descargas: %s", "Place pour les t\xC3\xA9l\xC3\xA9" "chargements : %s", "Platz f\xC3\xBCr Downloads: %s"},
    [S_GET_NO_ROOM] = {"Non c'\xC3\xA8 abbastanza spazio sulla scheda", "Not enough room on the card", "No hay espacio suficiente en la tarjeta", "Pas assez de place sur la carte", "Nicht genug Platz auf der Karte"},
    [S_GET_PAUSE] = {"Pausa", "Pause", "Pausa", "Pause", "Pause"},
    [S_GET_RESUME] = {"Riprendi", "Resume", "Reanudar", "Reprendre", "Fortsetzen"},
    [S_GET_RETRY_DL] = {"Riprova", "Retry", "Reintentar", "R\xC3\xA9" "essayer", "Wiederholen"},
    [S_GET_CANCEL_SURE] = {"Tocca ancora per annullare", "Tap again to cancel", "Toca otra vez para cancelar", "Touchez encore pour annuler", "Zum Abbrechen noch einmal tippen"},
    [S_GET_DONE] = {"Scaricata e verificata", "Downloaded and checked", "Descargada y verificada", "T\xC3\xA9l\xC3\xA9" "charg\xC3\xA9" "e et v\xC3\xA9rifi\xC3\xA9" "e", "Geladen und gepr\xC3\xBC" "ft"},
    [S_GET_FAILED] = {"Non riuscito", "Failed", "Fall\xC3\xB3", "\xC3\x89" "chec", "Fehlgeschlagen"},
    [S_GET_PAUSED] = {"In pausa", "Paused", "En pausa", "En pause", "Pausiert"},
    [S_GET_CHECKING] = {"Verifica\xE2\x80\xA6", "Checking\xE2\x80\xA6", "Verificando\xE2\x80\xA6", "V\xC3\xA9rification\xE2\x80\xA6", "Pr\xC3\xBC" "fung\xE2\x80\xA6"},
    [S_GET_PREPARING] = {"Preparazione\xE2\x80\xA6", "Getting ready\xE2\x80\xA6", "Preparando\xE2\x80\xA6", "Pr\xC3\xA9paration\xE2\x80\xA6", "Vorbereitung\xE2\x80\xA6"},
    [S_GET_PROGRESS] = {"%s di %s", "%s of %s", "%s de %s", "%s sur %s", "%s von %s"},
    [S_GET_SPEED] = {"%s/s, mancano %s", "%s/s, %s left", "%s/s, faltan %s", "%s/s, encore %s", "%s/s, noch %s"},
    [S_GET_NONE] = {"Nessun download", "No downloads", "Ninguna descarga", "Aucun t\xC3\xA9l\xC3\xA9" "chargement", "Keine Downloads"},
    [S_GET_NOTE] = {"I download continuano anche ad app chiusa e dopo un riavvio riprendono da dove erano.", "Downloads go on with the app closed, and after a restart they resume where they were.", "Las descargas siguen con la app cerrada y tras un reinicio contin\xC3\xBA" "an donde estaban.", "Les t\xC3\xA9l\xC3\xA9" "chargements continuent app ferm\xC3\xA9" "e et reprennent o\xC3\xB9 ils en \xC3\xA9taient apr\xC3\xA8s un red\xC3\xA9marrage.", "Downloads laufen bei geschlossener App weiter und machen nach einem Neustart dort weiter, wo sie waren."},
    [S_GET_STARTED] = {"Download avviato: %s", "Download started: %s", "Descarga iniciada: %s", "T\xC3\xA9l\xC3\xA9" "chargement lanc\xC3\xA9 : %s", "Download gestartet: %s"},
    [S_GET_FINISHED] = {"%s: pronta da leggere", "%s: ready to read", "%s: lista para leer", "%s : pr\xC3\xAAte \xC3\xA0 \xC3\xAAtre lue", "%s: bereit zum Lesen"},
    [S_GET_NOT_STARTED] = {"Download non avviato: %s", "The download did not start: %s", "La descarga no empez\xC3\xB3: %s", "Le t\xC3\xA9l\xC3\xA9" "chargement n'a pas d\xC3\xA9marr\xC3\xA9 : %s", "Der Download hat nicht begonnen: %s"},
    [S_GET_EXISTS] = {"il file c'\xC3\xA8 gi\xC3\xA0 o si sta scaricando", "the file is there already or being downloaded", "el archivo ya existe o se est\xC3\xA1 descargando", "le fichier existe d\xC3\xA9j\xC3\xA0 ou est en cours de t\xC3\xA9l\xC3\xA9" "chargement", "die Datei ist schon da oder wird geladen"},
    [S_GET_EDITIONS] = {"Edizioni", "Editions", "Ediciones", "\xC3\x89" "ditions", "Ausgaben"},
    [S_GET_WAIT_NET] = {"In attesa della rete", "Waiting for the network", "Esperando la red", "En attente du r\xC3\xA9seau", "Wartet auf das Netz"},
    [S_DELETE] = {"Elimina dalla scheda", "Delete from the card", "Borrar de la tarjeta", "Supprimer de la carte", "Von der Karte l\xC3\xB6schen"},
    [S_DELETE_SURE] = {"Tocca ancora per eliminarla", "Tap again to delete it", "Toca otra vez para borrarla", "Touchez encore pour la supprimer", "Zum L\xC3\xB6schen noch einmal tippen"},
    [S_DELETED] = {"Eliminata: %s", "Deleted: %s", "Borrada: %s", "Supprim\xC3\xA9" "e : %s", "Gel\xC3\xB6scht: %s"},
    [S_GET_NO_MATCH] = {"Nessuna raccolta corrisponde", "No collection matches", "Ninguna colecci\xC3\xB3n coincide",
                        "Aucune collection ne correspond", "Keine Sammlung passt"},
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
const char *i18n_lang3(void) { static const char *const k[L_N] = {"ita", "eng", "spa", "fra", "deu"}; return k[s_lang]; }

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

// A download speed: "180 KB/s" under a megabyte a second, "1,4 MB/s" above (decimal units, like fmt_size).
void fmt_speed(uint64_t bps, char *out, size_t cap) {
    if (bps < 1000000) snprintf(out, cap, "%d %s/s", (int)((bps + 500) / 1000), s_lang == L_FR ? "ko" : "KB");
    else snprintf(out, cap, "%d%c%d %s/s", (int)(bps / 1000000), dec_sep(), (int)(bps / 100000) % 10, s_lang == L_FR ? "Mo" : "MB");
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
