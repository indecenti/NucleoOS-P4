# Scacchi

Una partita a scacchi contro il computer. Tu giochi con il **bianco** e muovi per primo, il computer
risponde con il nero.

## Come si muove

1. **Tocca un tuo pezzo.** La casella si illumina e compaiono le mosse possibili:
   - un **pallino** indica una casella libera dove puoi andare;
   - gli **angoli scuri** indicano un pezzo avversario che puoi catturare.
2. **Tocca la casella di arrivo.** Il pezzo si sposta e il computer inizia a pensare.

Puoi anche trascinare il pezzo con il dito e lasciarlo sulla casella di arrivo.
Per cambiare pezzo basta toccarne un altro; toccando di nuovo lo stesso pezzo lo deselezioni.

Le regole sono quelle ufficiali: arrocco (sposta il re di due caselle verso la torre), presa
*en passant* e promozione. Quando un pedone arriva in fondo diventa automaticamente una **donna**.

L'ultima mossa giocata resta evidenziata in giallo. Se il tuo re è sotto scacco, la sua casella si
colora di rosso.

## I pulsanti

- **Annulla** — torna indietro di una mossa tua (e della risposta del computer). Puoi usarlo più
  volte, anche a partita finita.
- **Nuova partita** — ricomincia da capo. Se una partita è in corso il pulsante diventa
  **Conferma?**: toccalo di nuovo entro tre secondi per ricominciare davvero.
- **Livello** — scegli quanto è forte il computer. Il cambio vale dalla sua mossa successiva, anche
  a partita in corso.

## I livelli

| Livello | Com'è il computer |
|---|---|
| **Facile** | Guarda poco avanti e ogni tanto gioca una mossa a caso. Adatto a chi impara. |
| **Medio** | Gioca in modo solido e punisce gli errori evidenti. |
| **Difficile** | Pensa più a lungo (circa due secondi) e calcola parecchie mosse avanti. |

Mentre il computer pensa, nel riquadro di stato compaiono tre puntini animati. Puoi comunque
toccare **Annulla** o **Nuova partita**: il computer si ferma subito.

## Come finisce una partita

- **Scacco matto** — il re sotto scacco non ha modo di salvarsi: vince chi ha dato il matto.
- **Patta per stallo** — chi deve muovere non ha mosse legali ma non è sotto scacco.
- **Patta: pezzi insufficienti** — sulla scacchiera non resta abbastanza materiale per dare matto
  (per esempio re contro re, o re e cavallo contro re).
- **Patta per ripetizione** — la stessa posizione si è ripetuta tre volte.
- **Patta: regola delle 50 mosse** — cinquanta mosse per parte senza catture né mosse di pedone.

## Il riquadro dei pezzi catturati

Sotto lo stato vedi i pezzi che tu (**Tu**) e il computer avete catturato. Il numero con il **+**
indica chi è in vantaggio di materiale (pedone 1, cavallo e alfiere 3, torre 5, donna 9).

## La partita resta salvata

Ogni mossa viene salvata: se chiudi l'app e la riapri, ritrovi la partita esattamente dove l'avevi
lasciata, con il livello scelto.

## Consigli

- Nelle prime mosse porta fuori pedoni centrali, cavalli e alfieri, poi arrocca per mettere il re al
  sicuro.
- Prima di muovere chiediti: *quale pezzo lascio indifeso?* Il computer se ne accorge subito.
- Sbagliato qualcosa? **Annulla** e riprova: è il modo più veloce per imparare.
- Se vinci facilmente, sali di livello.

---

Il motore di gioco è [mcu-max](https://github.com/gissio/mcu-max) di Gissio (licenza MIT), basato su
micro-Max di H.G. Muller.
