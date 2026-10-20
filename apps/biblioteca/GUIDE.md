# Biblioteca

## A cosa serve

La Biblioteca legge i file ZIM della libreria Kiwix: Wikipedia, Wikizionario, Wikivoyage, l'enciclopedia medica, le guide di riparazione di iFixit, la documentazione dei linguaggi di programmazione (DevDocs), i libri di Gutenberg e molto altro, senza Internet. Il catalogo ne conta migliaia, in tutte le lingue, aggiornati ogni mese.

Le voci si leggono in modalità lettura: testo pulito con titoli, paragrafi, elenchi, tabelle, codice, link, immagini e formule, in un carattere scelto per leggere a lungo. Menu di navigazione, note e link di modifica restano fuori.

## Come si usa

- **Libreria**: all'apertura vedi le raccolte copiate nella cartella `zim`, con copertina, lingua, data, numero di voci e dimensione. Tocca una raccolta per aprire la sua pagina principale, la **i** per le informazioni (autore, fonte, licenza, verifica dell'integrità del file).
- **Continua a leggere**: in cima alla libreria c'è l'ultima voce letta, alla posizione in cui l'avevi lasciata.
- **Cerca** (lente): i titoli compaiono mentre scrivi. Se nessun titolo inizia con quelle parole, **Cerca dentro i titoli** trova le parole in qualunque punto del titolo.
- **Leggere**: trascina il dito per scorrere, tocca un link per seguirlo, tieni premuto un link per vedere dove porta. La freccia indietro torna alla voce precedente, quella avanti rifà il percorso.
- **Immagini e formule**: compaiono un attimo dopo il testo. Tocca un'immagine per vederla a tutto schermo con la sua descrizione; tocca una formula per ingrandirla, nitida a qualunque dimensione. Le formule prendono il colore del testo, quindi si leggono anche con il tema scuro.
- **Sezioni** (elenco): l'indice della voce; tocca una sezione per andarci.
- **Segnalibri** (stella): aggiunge o toglie la voce. Segnalibri e cronologia si aprono dalla libreria o dal menu.
- **Menu** (tre puntini): voce a caso, tema chiaro o scuro, segnalibri, cronologia, informazioni sulla raccolta, dimensione del testo.
- **Scarica raccolte** (freccia in basso nella libreria): il catalogo di Kiwix nella lingua dell'interfaccia, con **Consigliate** (il meglio di Wikipedia, l'enciclopedia medica, il dizionario, Wikivoyage, Vikidia per i ragazzi...), **Tutte**, le categorie e la ricerca (lente). Il tasto **IT** cambia la lingua del catalogo.
- **Edizioni**: tocca una raccolta per vedere le sue edizioni, con dimensione e data: **con immagini**, **senza immagini**, **solo le introduzioni**. Sotto c'è lo spazio che resta sulla scheda per i download. **Scarica** avvia il download; se in libreria c'è un'edizione più vecchia della stessa raccolta, il tasto dice **Edizione più recente**.
- **Download** (freccia in basso, in alto a destra): avanzamento, velocità e tempo che manca, **pausa**, **riprendi** e **annulla** (due tocchi). I download continuano anche se chiudi l'app e dopo un riavvio riprendono da dove erano. Ogni pezzo da 4 MB viene verificato con l'impronta pubblicata da Kiwix, e se un server manda dati sbagliati si passa al successivo. Alla fine arriva una notifica e la raccolta compare in libreria.
- **Elimina dalla scheda**: nelle informazioni di una raccolta (la **i**), con due tocchi. Serve a togliere l'edizione vecchia dopo aver scaricato quella nuova.

## Tastiera e mouse

| Tasto | Cosa fa |
|---|---|
| Frecce su e giù | scorrono la voce (nelle liste: scelgono la riga) |
| Pag Giù, Pag Su, Spazio, Maiusc+Spazio | una pagina avanti o indietro |
| Inizio, Fine | inizio o fine della voce |
| Tab, Maiusc+Tab | passano da un pulsante o da un link all'altro |
| Invio | apre il link o il pulsante scelto, la ricerca scelta |
| Esc, Backspace, Alt+freccia sinistra | indietro |
| Alt+freccia destra | avanti |
| `/` o Ctrl+F | cerca |
| `t` | sezioni |
| `b` o Ctrl+D | segnalibro |
| `r` | voce a caso |
| `+` e `-` | dimensione del testo |
| `m` | menu |

Con una tastiera collegata la tastiera sullo schermo non compare. Il mouse funziona come il dito.

## Dove prendere i file

Il modo più semplice è **Scarica raccolte** nella libreria: serve la connessione a Internet solo per scaricare, poi si legge offline. Con un Wi-Fi debole servono tempo e pazienza: a 250 KB/s 1 GB richiede circa un'ora (il download va avanti da solo, anche ad app chiusa). Le raccolte oltre 2 GB vengono scritte in parti (`nome.zimaa`, `nome.zimab`…), che la Biblioteca legge come un file solo.

In alternativa, dal sito della libreria Kiwix (library.kiwix.org) si scaricano sul PC i file `.zim` e si copiano nella cartella `zim` della home della scheda SD (`/sdcard/home/zim`) con il lettore di schede: per le raccolte grandi è più veloce. I file oltre 2 GB vanno divisi in parti come sopra.

Per cominciare vanno bene le versioni `nopic` (senza foto) o `mini` (solo le introduzioni): Wikipedia in italiano `top_nopic` pesa circa 900 MB, il Wikizionario circa 360 MB, l'enciclopedia medica circa 90 MB.

Le stesse raccolte si leggono anche nel Terminale con il programma `zim`, e ANIMA le usa per rispondere.

## Limiti

Le immagini compaiono in WebP, PNG, JPEG, GIF (il primo fotogramma) e SVG; un PNG o un JPEG oltre un milione di pixel resta un riquadro grigio. Nelle raccolte `nopic` ci sono solo le formule. La ricerca trova i titoli, non le parole dentro le voci. Il catalogo lascia fuori le raccolte di video (TED). I link verso Internet non si aprono: la Biblioteca mostra l'indirizzo. Le raccolte fatte con zimit (copie di siti interi) danno risultati più grezzi.
