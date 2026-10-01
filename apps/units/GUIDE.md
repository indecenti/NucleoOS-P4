# GNU units

## A cosa serve

Converte tra oltre 3000 unità di misura e ci fa i calcoli: lunghezze, pesi, velocità, energia, pressione, misure di cucina, temperature, valute, elementi chimici, costanti fisiche. Capisce espressioni intere, per esempio `230 V * 10 A` in kW.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `units '5 mph' 'km/hr'` | converte (mostra il fattore e quello inverso) |
| `units -t '3 cups' ml` | solo il risultato (`-t`, comodo negli script) |
| `units -t 'tempF(100)' tempC` | temperature: `tempC`, `tempF`, `tempK` con le parentesi |
| `units -t '230 V * 10 A' kW` | calcola con le unità |
| `units -t '2 hours + 30 min' s` | somma grandezze dello stesso tipo |
| `units -t '100 euro' 'US$'` | valute |
| `units -t oxygen 'g/mol'` | massa degli elementi |
| `units` | prompt interattivo: scrivi l'unità di partenza, Invio, poi quella di arrivo |

## Da sapere

- `h` è la costante di Planck: per le ore scrivi `hr` o `hour` (`km/hr`, non `km/h`).
- `degF` e `degC` sono differenze di temperatura; per i valori usa `tempF(...)` e `tempC(...)`.
- Le tue unità vanno nel file `.units` della home, una per riga (`pizza 32 cm`).

## Limiti

I tassi di cambio sono quelli inclusi nella versione 2.24 (novembre 2024) e non si aggiornano da soli: per tassi recenti metti un tuo `currency.units` nella cartella `units` della home. Il comando `help` interno non è disponibile (richiede un visualizzatore esterno).

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
