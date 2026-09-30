# Zip

## A cosa serve

Crea, elenca ed estrae archivi `.zip`, direttamente sulla scheda SD. Utile per impacchettare file da portare altrove o aprire archivi scaricati.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `zip archivio.zip file1 file2` | crea l'archivio con quei file |
| `zip -l archivio.zip` | elenca il contenuto |
| `zip -x archivio.zip` | estrae nella cartella corrente |
| `zip -x archivio.zip cartella` | estrae in `cartella` |

I percorsi sono relativi alla cartella home: `zip -x giochi.zip giochi` estrae in `/sdcard/home/giochi`.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
