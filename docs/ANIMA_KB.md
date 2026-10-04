# ANIMA: pacchetti di conoscenza AKB6

Come ANIMA porta Wikipedia (e in seguito Wikidata) sulla SD in un formato pensato per il P4: una voce si trova
con una ventina di letture dalla SD e un solo blocco da decomprimere. Stato: convertitore e valutazione sul PC (`tools/kb/akb6.py`); **lettore sul dispositivo** in
`components/nv_anima/nucleo_anima_kb.c`, provato sul PC (`tests/host/unit/test_anima_kb.cpp`) e sul pacchetto reale.
Il tipo `data` dello store viene dopo.

## Da dove viene

Le fonti sono i file ZIM di Kiwix (openZIM), aggiornati ogni mese, in tutte e cinque le lingue. La variante `mini`
contiene introduzione e infobox di ogni voce. ZIM è la **materia prima**, convertita sul PC: sul dispositivo
l'HTML, i blocchi zstd da 1–2 MB e la ricerca per titolo esatto costerebbero tempo a ogni domanda. Il pacchetto
AKB6 contiene solo testo pulito, chiavi normalizzate come il resto di ANIMA e blocchi deflate decompressi dalla ROM
(`tinfl`).

| ZIM `top_mini` (luglio 2026) | it | en | es | fr | de |
|---|---|---|---|---|---|
| dimensione | 107 MB | 283 MB | 196 MB | 136 MB | 131 MB |

## Formato (versione 1)

Un file per pacchetto, little endian:

```
intestazione  "AKB6" u16 versione u16 flag char lingua[4] u32 entità u32 chiavi u32 blocchi u32 sezioni u32 0 char id[32]
sezioni       n x { char tag[4], u64 offset, u64 dimensione }
  META  JSON: fonte, data, licenza, attribuzione, conteggi
  KEYS  "chiave<TAB>id[,id...]\n" ordinate per byte; chiave = forma normale del tokenizzatore di ANIMA
        '~' = la chiave è ambigua per quell'id ("mercurio" -> pianeta, dio, elemento)
        '^' = il nome è una SEZIONE di quella voce ("Jimbo Kern" -> Personaggi di South Park)
  ENTS  per entità { u32 blocco, u32 offset, u32 lunghezza }
  BIDX  per blocco { u64 offset, u32 compresso, u32 decompresso }
  BLKS  blocchi raw-deflate da circa 32 KB di testo
  QIDS  "Q<n><TAB>id\n" ordinate: dall'ID Wikidata alla voce di questo pacchetto
        '@' in KEYS = il titolo della stessa entità in un'altra lingua ("napoleon" -> Napoleone Bonaparte)
record        campi separati da 0x1E: titolo, qid, pagina, coordinate, riassunto, passaggi (separati da 0x1F),
              fatti Wikidata ("born=1879-03-14|birthplace=Ulma|gender=m"; può mancare: i lettori lo trattano come vuoto)
```

Le chiavi vengono da titoli, redirect, titoli senza il loro "(qualificatore)", pagine-rinvio a una sezione e, tramite
Wikidata, dai titoli della stessa entità nelle altre quattro lingue. Riassunto = le prime due frasi (circa 350 caratteri,
quanto sta su uno schermo e in una risposta detta); il resto del paragrafo e i paragrafi seguenti (fino a 6) sono i
passaggi per "dimmi di più". Pronuncia, note, barre di navigazione e calendari delle pagine-anno sono tolti.
Un punto che manca già nella voce ("...naturalizzato statunitense Ha progettato...") viene aggiunto, ma solo nel testo semplice del paragrafo, mai dentro link, corsivi o virgolette, dove stanno titoli e nomi ("la Terza Era", "Kung Fu", "È nata una stella"). Serve inoltre una parola minuscola prima, una forma verbale o un pronome dopo ("Ha progettato", "It was", "Il est"), e la frase deve proseguire in minuscolo. Misura di ottobre 2026: 8 paragrafi corretti su circa 250.000 voci, nessun falso positivo nel campione.

## Misure: Wikipedia italiana, 50.000 voci migliori

`python tools/kb/akb6.py eval`: domande costruite da 1000 voci, 1000 redirect e 250 pagine-sezione scelte a caso,
in quattro forme ("X", "chi è X", "cos'è X", "x" in minuscolo).

| | giuste | sbagliate | ambigue | assenti | letture SD | KB letti |
|---|---|---|---|---|---|---|
| voci | 97,2% | 0,1% | 0% | 2,7% | 14 | 27 |
| redirect | 98,4% | 0,1% | 0,1% | 1,4% | 14 | 28 |
| sezioni | 96,4% | 0,4% | 0% | 3,2% | 14 | 27 |
| persone inesistenti | | | | 5/5 senza risposta | | |

Pacchetto: **19,8 MB contro 112 MB dello ZIM** (5,7 volte più piccolo), 49.970 voci, 179.771 chiavi, 1.129 blocchi.

Cosa queste misure **non** dicono: le domande nascono dai titoli, quindi misurano la ricerca per nome (chi è,
cos'è). Una domanda che non nomina la voce ("chi ha inventato la lampadina") ha bisogno del grafo dei fatti e della
ricerca semantica: sono i passi successivi, con un loro insieme di domande mai viste.

## Cinque lingue e Wikidata

`tools/kb/wikidata.py` chiede alle API di Wikimedia (in sequenza, 50 elementi per richiesta, con cache riprendibile):
1. `qids <zim>`: titolo -> ID Wikidata per ogni voce (API di Wikipedia, `pageprops`);
2. `sitelinks`: ID -> titolo in it/en/es/fr/de (API di Wikidata).

| `top_mini`, 50.000 voci | it | en | es | fr | de |
|---|---|---|---|---|---|
| ID Wikidata trovati | 49.995 | 49.998 | 49.994 | 49.992 | 49.998 |
| chiavi in altre lingue (`@`) | 96.627 | 92.448 | 104.696 | 99.320 | 103.062 |
| pacchetto | 22,7 MB | 48,5 MB | 32,6 MB | 27,5 MB | 24,5 MB |
| ZIM d'origine | 112 MB | 297 MB | 206 MB | 143 MB | 137 MB |

In tutto 117.162 entità distinte; i cinque pacchetti pesano circa 156 MB contro 895 MB di ZIM.

Sul dispositivo una domanda cerca prima nel pacchetto della lingua dell'utente, poi in inglese, poi negli altri; una voce
trovata altrove passa, con lo stesso ID, al pacchetto nella lingua dell'utente se c'è. Altrimenti la risposta dice da
quale Wikipedia viene: "(Wikipedia, it) …".

**Misure** (`akb6.py eval`, 1000 voci e 1000 redirect a caso; "titolo X -> Y" = il titolo della stessa entità nella
lingua X deve trovare la voce giusta nel pacchetto Y):

| pacchetto | voci giuste / sbagliate | redirect giuste / sbagliate | titoli da altre lingue: giuste | riassunti fuori soggetto | inventati |
|---|---|---|---|---|---|
| it | 97,1% / 0,2% | 98,3% / 0,1% | 95,5–97,9% | 0,9% | 0/5 |
| en | 98,7% / 0,3% | 96,0% / 0,3% | 96,6–98,1% | 0,6% | 0/5 |
| es | 98,3% / 0,5% | 97,8% / 0,5% | 96,5–97,9% | 2,2% | 0/5 |
| fr | 93,7% / 0,1% | 97,3% / 0,3% | 95,0–96,8% | 1,6% | 0/5 |
| de | 98,4% / 0,0% | 98,1% / 0,3% | 95,3–97,2% | 1,0% | 0/5 |

"Riassunti fuori soggetto" = il riassunto non nomina il titolo nelle prime righe: è la metrica che ha trovato le
tabelle informative e le didascalie finite nel testo (ora tolte); quelli rimasti sono quasi sempre corretti (titolo
scientifico, testo col nome comune: "Cygnus olor" -> "il cigno reale"). Gli "sbagliati" tra lingue sono quasi sempre un omonimo nativo che vince giustamente (in italiano "Roma" è la città,
non il popolo rom). Il francese ha più "assenti" perché le pagine-anno senza prosa vengono scartate.

## Comandi

```bash
python tools/kb/akb6.py build tools/kb/.cache/wikipedia_it_top_mini_2026-07.zim tools/kb/.cache/wikipedia_it_top.akb6
python tools/kb/akb6.py ask   tools/kb/.cache/wikipedia_it_top.akb6 "chi è Leonardo da Vinci"
python tools/kb/akb6.py eval  tools/kb/.cache/wikipedia_it_top.akb6 tools/kb/.cache/wikipedia_it_top_mini_2026-07.zim
python tools/kb/wikidata.py qids tools/kb/.cache/wikipedia_it_top_mini_2026-07.zim   # una volta per lingua, prima di build
python tools/kb/wikidata.py sitelinks                                                # dopo le cinque lingue
python tools/kb/facts.py fetch && python tools/kb/facts.py labels                    # fatti, poi rifare build
python tools/kb/facts.py fill                                                       # completa i nomi con "mul"
python tools/kb/facts.py show Q937                                                   # i fatti di un'entità, 5 lingue
```

Richiede `pip install libzim` (solo sul PC). Lo ZIM si scarica da download.kiwix.org con il suo `.sha256`;
`tools/kb/.cache/` è fuori da git.

## Licenza

Il testo è di Wikipedia, CC BY-SA 4.0: `META.attribution` va mostrata con la risposta (o nella traccia) e i
pacchetti si distribuiscono con la stessa licenza.

## Sul dispositivo

I pacchetti stanno in `/data/anima/kb/*.akb6` (trovati all'avvio). Lo strato risponde dopo le schede curate (L1) e il
grafo dei fatti, **prima** di web e modello: una risposta fondata e citata vale più di un giro in rete.

| Domanda | Risposta |
|---|---|
| "chi è Leonardo da Vinci", "cos'è un buco nero", "parlami della Divina Commedia", "Roma" | il riassunto della voce |
| "dimmi di più" | il passaggio successivo; alla fine "L'enciclopedia non dice altro su questo." |
| "cos'è il mercurio" | «mercurio» può essere: 1) … 2) … 3) …. Quale? — poi "il secondo" o "la divinità" |
| "chi è Jimbo Kern" | Ne parla la voce «Personaggi di South Park»: … |
| "who is Albert Einstein" (utente inglese, pacchetto italiano) | (Wikipedia, it) Albert Einstein … |
| un nome che non c'è | nessuna risposta inventata |

La traccia porta sempre l'attribuzione del pacchetto (`META.attribution`). Lettura: bisezione su `KEYS` e un blocco
decompresso con `tinfl` (ROM dell'ESP32; miniz nei test sul PC), buffer sullo heap, file aperto e chiuso a ogni domanda.
Le domande in un'altra lingua usano gli ID Wikidata (sezione sopra). In spagnolo, francese e tedesco il motore legge la frase tradotta in
inglese, ma l'enciclopedia cerca il nome nella frase **originale** (`anima_lang_original`): il glossario trasformerebbe
"Sexe, Mensonges et Vidéo" in "… and Vidéo".

## Distribuzione: lo store, tipo `data`

I pacchetti si installano dallo store come le app, con un tocco, nella categoria **Conoscenza** ("Wikipedia in
italiano · 22 MB" ...). Non passano da git: i file sono asset di una release GitHub, lo store pubblica solo la loro
descrizione firmata.

```
tools/kb/publish.py            -> server/appstore/data_packs.json   (righe del catalogo, sha256, URL della release)
gh release create kb-AAAA.MM   -> i file .akb6 come asset            (a mano, quando si decide di pubblicare)
server/appstore/export_static.py --out <store> -> data/<id>/pack.sig firmati con la chiave dello store + catalogo
```

`pack.sig` (`nv_store_pkg::parse_data`, testato e fuzzato sul PC):

```
nucleoos-data-v1
<id>                       wiki-it-top
<versione>                 2026.7 (il mese del dump di Wikipedia)
<destinazione>             anima/kb   (sotto /sdcard/data; ammesse solo anima/kb e anima)
<sha256> <byte> <nome> <url>     un file per riga; righe consecutive con lo stesso nome = parti dello stesso file
sig <ECDSA P-256>
```

Sul dispositivo (`nv_appstore.cpp`, `install_data`):
- controlla la firma, che id e versione siano quelli del catalogo e che sulla SD ci sia spazio;
- scarica ogni file in `<nome>.part` con **ripresa** (HTTP `Range`): una connessione persa o un riavvio non fanno
  ricominciare da capo, e i byte già presenti vengono ricontrollati prima di continuare;
- calcola lo SHA-256 mentre scarica: un file che non corrisponde non viene mai messo al suo posto;
- solo alla fine scrive `/sdcard/data/packs/<id>.pack` (versione, destinazione, file): senza, il pacchetto non è
  installato. Da lì lo store sa se c'è un aggiornamento e cosa cancellare alla disinstallazione;
- avvisa ANIMA, che rilegge `/data/anima/kb` alla domanda successiva (`nucleo_anima_kb_invalidate`).

Un firmware più vecchio, che non conosce i pacchetti dati, li vede come "richiede un sistema più recente"
(`"abi": 99` nella riga) invece di provare a installarli come app.

## Fatti (Wikidata)

Le domande precise hanno una risposta esatta, non un riassunto: "quando è nato Einstein", "quanti abitanti ha
Lione", "who wrote Hamlet", "¿cuál es la capital de Perú?", "wann starb Goethe". `tools/kb/facts.py` interroga il
servizio SPARQL di Wikidata (a blocchi di 400 entità, riprende da dove si era fermato) per tutte le entità dei cinque
pacchetti; `akb6.py build` scrive i fatti nel campo 7 del record, con i nomi dei valori nella lingua del pacchetto.

| relazione | proprietà | esempio di risposta |
|---|---|---|
| born / died | P569 / P570 (+ P19 / P20) | "Marie Curie è nata il 7 novembre 1867 a Varsavia." |
| birthplace / deathplace | P19 / P20 | "Napoleone Bonaparte è morto a Longwood." |
| capital, currency, language, continent, country | P36, P38, P37, P30, P17 | "Giappone — moneta: yen." |
| population, area, elevation | P1082, P2046, P2044 | "Francia ha 68.373.433 abitanti." |
| author, director, composer | P50, P57, P86 | chi ha scritto / diretto / composto |
| founded, occupation | P571, P106 | anno di fondazione, professione |
| creator, inventor | P170, P61 | "Gioconda è opera di Leonardo da Vinci.", "Telefono — inventore o scopritore: …" |
| formula, symbol, atomic_number | P274, P246, P1086 | "Metano: formula chimica CH₄.", "Oro è un elemento chimico: simbolo Au, numero atomico 79." |

Le date come le scrivono gli storici: il servizio SPARQL consegna ogni data convertita nel calendario gregoriano prolettico e con gli anni astronomici, così il 15 marzo 44 a.C. di Cesare (giuliano) arrivava come -0043-03-13. `facts.py dates` scarica anche il calendario di ogni data e `history_date` riporta le date giuliane al giuliano e gli anni a.C. al conteggio storico (Cesare: 15 marzo 44 a.C.; Copernico: 24 maggio 1543). In italiano un luogo che è un edificio prende la sua preposizione ("nel Teatro di Pompeo", "nella Villa…").

Il genere (P21) serve solo alla grammatica (nato/nata, né/née). Date con la loro precisione (solo l'anno se Wikidata
sa solo l'anno; "a.C."/"BC"/"v. Chr."), numeri con il separatore delle migliaia della lingua, elenchi con "e/and/y/
et/und".

Sul dispositivo (`nucleo_anima_facts.c`) il livello sta prima dell'enciclopedia: riconosce la domanda (schemi nelle
cinque lingue, il più lungo vince), trova l'entità con le stesse chiavi multilingue e, tra omonimi, sceglie quella che
HA il fatto ("abitanti di Mercurio" non è mai il dio). Se nessuna lo ha, non risponde: passa la mano al riassunto o al
modello, niente fatti inventati. "e Napoleone?" subito dopo un fatto chiede la stessa cosa di un'altra entità. La
traccia dice "Wikidata (CC0)". Prove: `tests/host/unit/test_anima_kb.cpp` (sezione FACTS).

Dettagli che contano:
- I nomi dei valori. Wikidata tiene molti nomi di persona solo come etichetta "mul" (uguale in tutte le
  lingue, per esempio "Victor Hugo"), e "mul" riempie le lingue senza un'etichetta propria. Senza nome, il
  valore viene scartato.
- Gli articoli. In italiano e in francese non si scrive "di + Paese" senza l'articolo giusto: le frasi usano
  forme sempre corrette ("Australia ha come capitale Canberra.", "Japon — capitale : Tokyo.").
- I titoli. Si cerca prima il titolo come detto ("Les Misérables"), poi senza articolo ("la Francia" ->
  "francia").

Prova sui pacchetti veri con il motore del dispositivo, una domanda "lingua|domanda" per riga
("#fr|chiave" mostra la ricerca grezza della chiave e i fatti trovati):

```bash
cd ~/.cache/kbp && tests/host/build/x64/unit_anima_kb --probe tools/kb/.cache < domande.txt
```

Misura di ottobre 2026 su 26 domande nelle 5 lingue: 25 risposte giuste, 0 sbagliate.
- La 26ª, "Wer schrieb Faust?", passa la mano. Nel pacchetto tedesco "Faust" porta all'opera di Gounod, che ha
  un compositore e non un autore.
- Con i fatti, il 58–66% delle voci di ogni pacchetto.
- Limite noto: "e Newton?" dopo una data di nascita trova l'unità di misura "newton". La voce della persona è
  "Isaac Newton", quindi la risposta è il riassunto Wikipedia della chiave e non un fatto inventato.

## Solo cose certe: come ANIMA sceglie la risposta senza modello

- **Voce esatta prima delle schede.** "chi è X", "parlami di X", "dimmi tutto ciò che sai su X", "chi X"
  (anche senza "è") e i saluti davanti ("ciao chi è…") cercano prima la voce con quel titolo esatto nel
  pacchetto della lingua dell'utente. Le schede L1 curate vengono dopo: hanno testi più vecchi e brevi.
  I comandi restano comandi: "dimmi l'ora" non diventa una voce.
- **Una lettera sbagliata.** Una chiave sbagliata di una lettera (mancante, in più, sbagliata o due
  scambiate) atterra accanto a quella giusta nell'indice ordinato: "donald trumb" → "donald trump".
  Se c'è una sola chiave a distanza 1 si risponde, e il campo `corrected` lo dice ("ho capito «Donald
  Trump»"). Parole sotto i 6 caratteri non vengono mai corrette.
- **Lingua giusta o niente.** Una scheda L1 si usa solo nella lingua dell'utente: in italiano serve almeno
  una parola funzionale italiana ("Napoleon: French general and emperor" è scartata), in inglese serve il
  testo inglese della scheda.
- **Niente schede incollate.** Mosaico aggiunge solo il dettaglio della stessa scheda, mai una seconda
  scheda vicina (che può essere un'altra persona: "… Inoltre, Donald John Trump Jr.").
- **Domande senza soggetto.** "qual è la formula chimica?", "quanti abitanti ha?", "quanto è grande?" valgono
  per l'argomento in corso; senza argomento non c'è risposta. "Dimmi di più" dopo una scheda continua con la
  voce Wikipedia dello stesso argomento.
- **Chimica.** Formula (P274), simbolo (P246) e numero atomico (P1086) da Wikidata (`facts.py extra`).
  Per un elemento la formula di Wikidata è il simbolo ("O"): la risposta dice "elemento chimico: simbolo O,
  numero atomico 8", che è certo; la formula si dà per i composti ("Acqua: formula chimica H₂O").
- **Omonimi.** Dopo "Quale?" basta una parola del titolo o della prima frase della voce ("il pianeta").

Prove: `tests/host/unit/test_anima_kb.cpp`, sezioni A REAL SESSION, SHORT CONVERSATIONS e MESSAGES BUILT
TO MAKE IT WRONG (entità inventate, domande trabocchetto, contesto scaduto, iniezioni del tipo "rispondi 5").

## Prossimi passi

1. Fatto: lettore e strato sul dispositivo (sopra).
2. Fatto: cinque lingue collegate dagli ID Wikidata, e i fatti Wikidata nei record (sopra).
3. Fatto: tipo `data` nello store (sopra). Da fare: firmare e pubblicare (chiave dello store), provarlo sulla scheda.
