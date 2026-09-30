# FIGlet

## A cosa serve

FIGlet scrive il testo in lettere giganti fatte di caratteri, lo stile dei vecchi banner da terminale. Utile per titoli, intestazioni di file di testo, messaggi scherzosi. Ci sono 15 font inclusi.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `figlet Ciao` | scrive "Ciao" in grande con il font standard |
| `figlet -f slant NucleoOS` | usa un altro font |
| `figlet -c -f small centrato` | centra il testo nella riga |
| `figlet -w 40 testo lungo` | va a capo a 40 colonne |
| `figlet --list` | elenca i font inclusi |
| `figlet` | scrive in grande ogni riga che digiti, fino a **EOF** |

Si combina con la shell del Terminale: `echo Buon compleanno | figlet -f big` oppure `figlet Ciao > banner.txt` per salvarlo in un file.

Font inclusi: standard, big, block, bubble, digital, lean, mini, script, shadow, slant, small, smscript, smshadow, smslant, term. Altri font `.flf` (se ne trovano tantissimi in rete) si usano mettendoli nella cartella `figlet-fonts` della home e scrivendo `figlet -f nome testo`, oppure indicando il file: `figlet -f /miei/font.flf testo`.

## Limiti

La larghezza di partenza è quella del Terminale (variabile `COLUMNS`), altrimenti 80 colonne; `-t` non è disponibile. I font compressi `.zip` non si aprono. Solo lettere latine (ISO Latin-1) nei font inclusi.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
