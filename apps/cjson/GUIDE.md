# JSON

## A cosa serve

Controlla se un file JSON è valido, lo formatta in modo leggibile, lo comprime su una riga o ne estrae un singolo valore. Comodo per ispezionare configurazioni, risposte di API e log.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `cjson file.json` | valida e stampa indentato |
| `cjson -c file.json` | stampa compatto, su una riga |
| `cjson -q .utente.nome file.json` | estrae il valore a quel percorso |
| `cjson -q .elenco.0 file.json` | primo elemento di un array |
| `cjson -l file.jsonl` | un valore JSON per riga (JSON Lines) |
| `cjson` | legge dall'input: incolla il JSON, poi **EOF** |

Se il file non è valido, `cjson` lo segnala con un errore.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
