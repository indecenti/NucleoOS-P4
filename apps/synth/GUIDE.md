# Synth

Un sintetizzatore polifonico da suonare con le dita. Dentro c'è il motore **AMY**, che riproduce due
strumenti storici: il **Roland Juno-106** (analogico, suoni caldi e pieni) e lo **Yamaha DX7**
(sintesi FM, suoni brillanti e "di vetro"), più un pianoforte.

## La tastiera

In basso ci sono **due ottave** di tastiera, da Do a Do. Il Do di ogni ottava porta scritto il suo
nome (per esempio **C3**, **C4**).

- **Suona più tasti insieme**: la tastiera riconosce fino a cinque dita, quindi puoi fare accordi.
- **Scivola con il dito** da un tasto all'altro per un glissato: ogni tasto che attraversi suona.
- **Più in basso tocchi il tasto, più forte suona**, come se lo colpissi con più decisione. In alto
  il suono è più morbido.

Il tasto premuto si colora di rosa finché tieni giù il dito; quando lo alzi la nota si spegne con la
coda del suono scelto.

## I suoni

### I dieci preset

I dieci pulsanti grandi sotto la barra in alto sono una scelta di suoni già pronti:

| Pulsante | Suono | Numero |
|---|---|---|
| **Juno Pad** | tappeto morbido e lento del Juno | 47 |
| **Archi** | sezione d'archi del Juno | 64 |
| **Ottoni** | ottoni sintetici del Juno | 0 |
| **Basso Juno** | basso sintetico del Juno | 36 |
| **Lead** | suono solista del Juno | 32 |
| **Piano el.** | piano elettrico del DX7 | 138 |
| **Campane** | campane tubolari del DX7 | 153 |
| **Basso FM** | basso del DX7 | 142 |
| **Organo** | organo elettrico del DX7 | 144 |
| **Pianoforte** | pianoforte di AMY | 256 |

Il preset attivo è evidenziato in magenta. Sotto il nome di ogni pulsante c'è lo strumento di origine
e il numero del suono.

### Tutti i 257 suoni

Nella barra in alto vedi il **numero** e il **nome** del suono in uso. Con le frecce **<** e **>**
passi al suono precedente o successivo:

- **0-127**: i 128 suoni originali del Juno-106 (gruppi A e B);
- **128-255**: i 128 suoni di fabbrica del DX7;
- **256**: il pianoforte.

Dopo il 256 si ricomincia da 0, e da 0 andando indietro si arriva al 256.

## I controlli

- **Volume** — trascina il cursore. A metà (50) il volume è quello normale; più a destra è più
  forte.
- **Timbro** — trascina il cursore a destra per un suono più brillante, a sinistra per uno più
  scuro. A 50 il suono è come l'ha pensato chi lo ha creato.
- **Riverbero** — tocca il pulsante per accenderlo (**ON**) o spegnerlo (**OFF**). Acceso, il suono
  ha l'eco di una stanza grande.
- **Ottava** — con **-** e **+** sposti la tastiera di un'ottava più in basso o più in alto. In mezzo
  vedi il Do da cui parte la tastiera (da **C1** a **C6**).

In alto a destra la scritta **DSP** indica quanto lavora il processore per generare il suono. Se
compare **Audio occupato**, l'altoparlante è già usato da un'altra app (per esempio la Musica che
sta suonando): fermala e il Synth riprende da solo entro un paio di secondi.

## Le impostazioni restano salvate

Suono scelto, ottava, volume, timbro e riverbero vengono ricordati: alla prossima apertura ritrovi
tutto come l'avevi lasciato.

## Consigli

- I suoni lenti come **Juno Pad** e **Archi** rendono al meglio con accordi tenuti a lungo e il
  riverbero acceso.
- Per i bassi abbassa di un'ottava con **-**.
- Le **Campane** e il **Piano el.** suonano bene nelle ottave alte: prova **C4** o **C5**.
- Si suonano fino a sei note insieme.

---

Il motore di sintesi è [AMY](https://github.com/shorepine/amy) di Brian Whitman e Dan Ellis
(licenza MIT). I suoni Juno-106 e DX7 sono quelli inclusi in AMY.
