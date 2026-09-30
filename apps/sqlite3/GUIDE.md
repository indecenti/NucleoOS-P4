# SQLite

## A cosa serve

La shell ufficiale di SQLite: crei e interroghi database SQL veri, salvati come file sulla scheda SD. Utile per note, inventari, registri, esperimenti di SQL. Ci sono le funzioni JSON e matematiche.

## Come si usa

```
sqlite3 note.db
```

Apre (o crea) `note.db` nella cartella home. Poi scrivi SQL, terminando ogni istruzione con `;`:

```sql
CREATE TABLE note(id INTEGER PRIMARY KEY, testo TEXT, quando TEXT DEFAULT CURRENT_TIMESTAMP);
INSERT INTO note(testo) VALUES ('comprare il latte');
SELECT * FROM note;
```

## Comandi utili

| Comando | Cosa fa |
|---|---|
| `.tables` | elenca le tabelle |
| `.schema` | mostra come sono fatte |
| `.mode box` | risultati in una tabella leggibile |
| `.headers on` | nomi delle colonne |
| `.import file.csv t` | importa un CSV (con `.mode csv`) |
| `.help` | tutti i comandi |
| `.quit` | esce (oppure **EOF**) |

## Limiti

Versione ridotta per la scheda: niente WAL, mmap, thread, estensioni caricabili e FTS5. Senza nome di file (`sqlite3` da solo) il database sta in memoria e si perde all'uscita.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
