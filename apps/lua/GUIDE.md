# Lua

## A cosa serve

Lua 5.4 è un linguaggio di scripting piccolo e veloce, molto usato nei giochi e nell'automazione. Qui hai sia il prompt interattivo, per provare al volo, sia l'esecuzione di file `.lua`.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `lua` | apre il prompt interattivo (`>`) |
| `lua script.lua` | esegue un file dalla cartella home |
| `lua script.lua a b` | passa argomenti, letti in `arg[1]`, `arg[2]` |

Nel prompt scrivi un'espressione e premi Invio:

```lua
> 2^10
1024.0
> for i = 1, 3 do print(i) end
```

Per uscire: **EOF**, oppure `os.exit()`.

## Limiti

Non ci sono processi esterni: `os.execute`, `io.popen`, `os.tmpname` e `io.tmpfile` rispondono "not supported". Memoria disponibile: fino a 8 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
