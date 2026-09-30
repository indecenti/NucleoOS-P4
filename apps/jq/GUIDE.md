# jq

## A cosa serve

jq è il coltellino svizzero del JSON: legge file JSON e ne estrae, filtra, trasforma e riformatta il contenuto con un piccolo linguaggio di filtri. Utile per capire al volo un file di configurazione, un export di dati o la risposta di un'API salvata con `curl`. Ci sono anche le espressioni regolari (`test`, `match`, `sub`, `gsub`, `capture`).

## Come si usa

`jq FILTRO file.json` applica il filtro al file (o all'input della pipe se non dai il file):

| Comando | Cosa fa |
|---|---|
| `jq . dati.json` | stampa il JSON indentato e leggibile |
| `jq .nome dati.json` | il campo `nome` |
| `jq -r '.app[].id' dati.json` | ogni `id` della lista `app`, come testo semplice |
| `jq '.app[] \| select(.peso > 400)' dati.json` | solo gli elementi che rispettano la condizione |
| `jq -c 'map(.peso) \| add' lista.json` | somma i pesi, risultato su una riga |
| `jq --arg n lua '.app[] \| select(.id == $n)' dati.json` | usa un valore passato da fuori |
| `jq -n '[range(5)] \| map(. * .)'` | senza input: calcola e basta |
| `cat dati.json \| jq keys` | con una pipe della shell |

Opzioni utili: `-r` testo senza virgolette, `-c` una riga per risultato, `-s` legge tutti i file come un'unica lista, `-S` ordina le chiavi, `-f filtro.jq` legge il filtro da un file.

## Limiti

La riga di comando del Terminale non passa bene le virgolette doppie dentro un argomento: se il filtro contiene stringhe tra `"..."`, scrivilo in un file e usa `jq -f filtro.jq dati.json`, oppure passa il valore con `--arg`. I colori ANSI si tolgono con `-M`. Memoria disponibile: fino a 8 MB (file JSON grandi, diciamo oltre 1-2 MB, possono non starci).

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
