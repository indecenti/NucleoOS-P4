# openHASP

## A cosa serve

openHASP trasforma il pannello in un'interfaccia touch per **Home Assistant**. È il progetto della community HASwitchPlate: le pagine (pulsanti, interruttori, slider, termometri, orologi, icone) si descrivono in un file JSONL e si comandano via MQTT. Chi usa già openHASP con Home Assistant ritrova le stesse pagine e la stessa integrazione.

## Cosa serve prima

1. Un broker MQTT, di solito il componente aggiuntivo **Mosquitto broker** di Home Assistant.
2. Sul pannello: **Impostazioni > Casa**, inserisci broker, utente e password MQTT e attiva la connessione.
3. In Home Assistant installa l'integrazione **openHASP** da HACS (`HASwitchPlate/openHASP-custom-component`).

## Come si usa

- Apri **openHASP**: il pannello si presenta su MQTT con il nome **plate** (topic `hasp/plate/...`).
- In Home Assistant l'integrazione lo trova da sola: aggiungilo e indica il tuo file di pagine (`pages_jsonl`). L'integrazione carica le pagine e collega gli oggetti alle entità.
- In alternativa, senza Home Assistant, copia un file **pages.jsonl** nella cartella `/sdcard/apps/openhasp/data/` (app **File** o companion web) e riapri l'app.
- Il gesto **indietro** torna alla pagina precedente; dalla prima pagina chiude l'app.

### Configurazione facoltativa

Un file `/sdcard/apps/openhasp/data/config.json` cambia nome del nodo, gruppo, pagina iniziale e fuso orario:

```json
{"mqtt": {"name": "cucina", "group": "casa"}, "time": {"zone": "CET-1CEST,M3.5.0,M10.5.0/3"}}
```

### Prova rapida da un PC

```
mosquitto_pub -t hasp/plate/command/p1b2.text -m "Ciao"
mosquitto_pub -t hasp/plate/command/page -m 2
```

## Limiti

- Immagini da URL (`http://...`) e il servizio `push_image` non sono supportati: le immagini PNG vanno copiate nella cartella dati e indicate come `L:/nome.png`.
- Niente GPIO, relè, web UI, OTA e Wi-Fi propri di openHASP: rete, aggiornamenti e MQTT sono quelli del sistema.
- `reboot` e `restart` non fanno nulla.

## Crediti

openHASP (MIT) © openHASP contributors, <https://github.com/HASwitchPlate/openHASP>. LVGL 7 (MIT), ArduinoJson (MIT), FreeType (FTL: "Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved."). I testi completi delle licenze sono nel file `LICENSE.txt` dell'app.
