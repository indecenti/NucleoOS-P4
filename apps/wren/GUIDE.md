# Wren

## A cosa serve

Wren è un linguaggio di scripting piccolo, veloce e ordinato, a classi (un po' Smalltalk, un po' Lua, con una sintassi alla C). Ha fiber, closure, liste, mappe e range. Qui hai il prompt interattivo e l'esecuzione di file `.wren`.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `wren` | apre il prompt interattivo (`>`) |
| `wren script.wren` | esegue un file dalla cartella home |
| `wren script.wren a b` | passa argomenti, letti con `Process.arguments` |
| `wren -e 'System.print(6 * 7)'` | esegue una riga al volo |

Nel prompt un'espressione mostra il suo valore; se lasci aperta una parentesi `(`, `[` o `{` la riga continua con `...`:

```wren
> 1 + 2 * 3
7
> var lista = [1, 2, 3, 4]
> lista.map {|n| n * n }.toList
[1, 4, 9, 16]
> System.print("due più due fa %(2 + 2)")
due più due fa 4
> class Cane {
...   construct new(nome) { _nome = nome }
...   abbaia() { "%(_nome): bau!" }
... }
> Cane.new("Fido").abbaia()
Fido: bau!
```

Il modulo `io` legge e scrive file e legge l'input:

```wren
import "io" for File, Stdin
File.write("/nota.txt", "ciao da Wren")
System.print(File.read("/nota.txt"))
var riga = Stdin.readLine()
```

Negli script `import "nome"` carica `nome.wren` dalla stessa cartella. Ci sono anche i moduli `random` e `meta`. Per uscire: **EOF**.

## Limiti

Questa non è la `wren_cli` ufficiale (che richiede libuv): mancano timer, rete, processi e il modulo `os`. Del modulo `io` ci sono solo `File.read`, `File.write`, `File.exists`, `Stdin.readLine` e `Process.arguments`. Memoria disponibile: fino a 8 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
