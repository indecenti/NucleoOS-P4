# ANIMA a mani libere (parola di attivazione)

Di' la parola di attivazione, poi la domanda: il dispositivo suona, apre ANIMA, registra finché
smetti di parlare, trascrive, risponde e (se è installata una voce) legge la risposta ad alta voce.

## Come funziona
- **Ascolto sempre attivo, offline**: ESP-SR WakeNet sul microfono di bordo (48 kHz → 16 kHz).
  Niente esce dal dispositivo finché non senti il suono di conferma.
- **Pausa automatica** mentre il dispositivo riproduce audio (musica, voce, video) o registra:
  così non si attiva da solo.
- **Fine della domanda**: si ferma dopo ~1 s di silenzio (massimo 12 s); se nessuno parla entro
  5 s annulla.
- **Trascrizione**: prima il server Whisper di casa (Impostazioni web ▸ IA ▸ Trascrizione voce in
  casa), poi la chiave cloud; in modalità *Locale* mai il cloud.

## Attivarlo nel firmware (una volta)
La funzione è opzionale nella build (`CONFIG_NV_WAKE_ESP_SR`, spenta di default):

    cd components/nv_wake && idf.py add-dependency "espressif/esp-sr^2.1" && cd ../..
    idf.py menuconfig    # NucleoOS wake word ▸ Hands-free ANIMA: ON
                         # ESP Speech Recognition ▸ Load Multiple Wake Words: scegli le parole
    idf.py build flash   # ESP-SR scrive i modelli nella partizione "model" (partitions.csv)

Senza l'opzione tutto compila come prima e le Impostazioni spiegano che la funzione non è nella build.

## Dove si configura
- **Dispositivo**: Impostazioni ▸ Anima ▸ Voce a mani libere (interruttore, parola, sensibilità,
  stato in tempo reale, dove va la voce). Nell'app ANIMA: `/wake`, `/wake on|off|low|normal|high`.
- **Web**: Impostazioni ▸ IA ▸ Voce a mani libere (stessa cosa, aggiornata ogni 3 s).
- **API**: `GET/POST /api/anima/wake` `{on, word, sens}`; lo stato è anche in `device_status` del
  server MCP (`tools/anima_mcp.py`).

## Da verificare sulla scheda
Il nucleo (decimatore, stati, fine domanda) è testato su PC (`tests/host`, unit `wake`); l'aggancio
a ESP-SR (`wake_esp_sr.c`) va provato sul dispositivo: nomi dei modelli, sensibilità reale e
consumo di CPU con WakeNet9 su P4.
