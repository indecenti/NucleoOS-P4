# Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1 — scheda per il porting

Candidata scelta per NucleoOS su schermo grande (decisione del 2026-10-16). Non ancora comprata.

- Negozio: <https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7-8-10.1.htm?sku=33672> (SKU 33672, ~90 $; 33150 = variante "-EN")
- Documentazione: <https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-X>
- Schema: <https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-X/ESP32-P4-WIFI6-Touch-LCD-X-Schematic.pdf>
- Schema adattatore (USB, tasti, microfoni): <https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-X/ESP32-P4-Connect-Adapter-Schematic.pdf>
- BSP ed esempi ESP-IDF: <https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-X>
  (BSP in `firmware/brookesia/components/esp32_p4_wifi6_touch_lcd_x/`, v2.0.3)

Fonti: schema letto per intero e BSP ufficiale. Le voci con **⚠ da verificare** non sono ancora
confermate sulla scheda vera.

## Riepilogo

| | Guition JC1060P470C (attuale) | Waveshare 10,1" |
|---|---|---|
| SoC | ESP32-P4 | ESP32-P4NRW32 (modulo ESP32-P4-Core), 32 MB PSRAM in package |
| Flash | — | 32 MB NOR (GD25Q256) |
| Schermo | 7", 1024×600, JD9165 | 10,1", **800×1280 verticale**, **JD9365**, IPS, 400 cd/m² |
| MIPI-DSI | 2 lane, 750 Mbps, DPI 50 MHz | 2 lane, **1500 Mbps**, timing `JD9365_800_1280_PANEL_60HZ_DPI_CONFIG` |
| Touch | GT911 @ 0x5D | GT9271 (driver `esp_lcd_touch_gt911`), 10 punti, 0x5D o 0x14 |
| Wi-Fi/BT | ESP32-C6 via SDIO | ESP32-C6-MINI-1U-H8 via SDIO (stessi pin) |
| Audio | ES8311 + ES7210 | ES8311 + ES7210 + ampli NS4150B |
| RTC esterno | RX8130 (I2C 0x14) | **assente** |
| Ethernet | IP101 (RMII) | **assente** |
| Batteria | — | connettore JST 1,2 mm, caricatore ETA6003, misura su GPIO20 |
| Fotocamera | — | connettore MIPI-CSI 2 lane (OV5647 nel kit) |

## Mappa dei pin (dallo schema)

| Funzione | GPIO Waveshare | GPIO Guition | Note |
|---|---|---|---|
| I2C SDA / SCL | 7 / 8 | 7 / 8 | uguale; bus condiviso da touch, ES8311, ES7210, fotocamera |
| LCD reset | **27** | 5 | R76 10 kΩ verso GND |
| Retroilluminazione PWM | **26** | 23 | `LCD_BL_PWM`, LEDC 10 bit nel BSP; duty più alto = più luminoso |
| Touch INT | **33** | 21 | collegato via R32 0 Ω (il BSP non lo usa) |
| Touch RST | **23** | 22 | collegato via R37 0 Ω (il BSP non lo usa) |
| I2S MCLK | 13 | 13 | uguale |
| I2S BCLK (SCLK) | 12 | 12 | uguale |
| I2S WS (LRCK) | 10 | 10 | uguale |
| I2S DOUT → ES8311 | 9 | 9 | uguale |
| I2S DIN ← ES7210 | **11** | 48 | microfoni analogici via ES7210 |
| Ampli altoparlante | **53** | 11 | `PA_CTRL` verso NS4150B, R106 10 kΩ verso GND (spento di default) |
| SD CLK/CMD/D0–D3 | 43/44/39/40/41/42 | 43/44/39/40/41/42 | uguale; pull-up sull'LDO interno VO4 |
| SD alimentazione | GPIO45 (R9 "NC/0R") | — | ⚠ da verificare: con R9 non montata la SD è sempre alimentata |
| C6 SDIO CLK/CMD | 18 / 19 | 18 / 19 | uguale al nostro `sdkconfig` |
| C6 SDIO D0–D3 | 14 / 15 / 16 / 17 | 14 / 15 / 16 / 17 | uguale |
| C6 reset (CHIP_PU) | 54 | — | R54 0 Ω |
| C6 IO2 | 6 | — | R52 0 Ω |
| Batteria (ADC) | **20** | — | partitore 200 k/100 k: V_bat = V_pin × 3 |
| BOOT | 35 | 35 | tasto sull'adattatore |
| UART0 TX / RX | 37 / 38 | 37 / 38 | bridge CH343P con auto-reset DTR/RTS |
| USB full-speed | 24 / 25 | — | solo sul connettore a 40 pin |
| USB high-speed OTG | DP/DM dedicati | — | Type-C "USB-OTG" sull'adattatore |

**Connettore a 40 pin (J8)**, pin liberi: 2, 3, 4, 5, 21, 22, 28, 29, 30, 31, 32, 34, 36, 46, 47,
48, 49, 50, 51, 52 (più 7/8 I2C, 37/38 UART, 24/25 USB-FS, 35 BOOT). TP6 = GPIO36, TP7 = GPIO45.

**Indirizzi I2C:** ES8311 0x18 (CE a massa), ES7210 0x40, touch 0x5D o 0x14 (il BSP prova
entrambi). Senza RX8130 lo 0x14 non è più in conflitto.

## Alimentazione

- **MIPI D-PHY** sull'LDO interno **VO3 a 2500 mV**: uguale a `NV_MIPI_LDO_CHAN` / `NV_MIPI_LDO_MV`.
- **Flash** su LDO VO1 (R43 0 Ω). **SD** su LDO VO4, come nel BSP (`ldo_chan_id = 4`).
- **3V3** da buck-boost TPS63020; **1,2 V** del core da MP1605.
- **IOVCC del pannello** a 1,8 V da un LDO RT9193-18 sempre acceso.
- **Retroilluminazione**: AP3032 boost, 3 serie da 7 LED, circa 153 mA sul 10,1". `BL_EN` ha un
  pull-up fisso, quindi la luminosità si regola solo col PWM su GPIO26.
- **Interruttore SYS_EN** sull'adattatore. Con batteria la scheda resta accesa senza USB.

## Revisione del chip

Waveshare compila diversamente per **P4 v1.x** e **v3.x**:
- `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` oppure `CONFIG_ESP32P4_REV_MIN_300`;
- clock PHY DSI: `MIPI_DSI_PHY_CLK_SRC_DEFAULT` (< v3) oppure `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT` (v3).

All'arrivo bisogna leggere la revisione (`esptool chip_id`) prima del primo flash.

## Lavoro di porting

1. **Selezione della scheda in fase di compilazione.** Serve un `nv_pins_waveshare10.h` scelto da
   Kconfig, con i GPIO della tabella sopra.
2. **Pannello.** Aggiungere la dipendenza `espressif/esp_lcd_jd9365` (^2.0.2) e copiare dal BSP
   (`esp32_p4_wifi6_touch_lcd_x.c`, ramo `#else` del 10,1", righe ~238–461) la tabella di comandi
   iniziali. 1500 Mbps per lane, timing 60 Hz del driver.
3. **Orientamento.** Il pannello è verticale (800×1280); NucleoOS è pensato in orizzontale.
   `nv_disp` sa già ruotare con il PPA nel buffer posteriore (`nv_hal.cpp`, vicino a
   `nv_disp_create`), ma qui la rotazione sarebbe **sempre** attiva, non solo su richiesta.
   ⚠ Da misurare: costo di PPA e banda PSRAM a 1280×800, e il blit video diretto
   (`nv_hal_video_blit`) che oggi scrive senza rotazione.
4. **Touch.** Stesso driver GT911. Con INT/RST collegati (33/23) si può tenere la logica attuale.
   Le coordinate vanno ruotate come lo schermo.
5. **Audio.** `nv_audio` cambia DIN (11) e PA (53). Gli altri pin I2S restano uguali.
6. **Servizi senza hardware.** `nv_rtc` (niente RX8130) e `nv_eth` (niente IP101) devono restare
   spenti. `nv_eth` gestisce già l'assenza; per `nv_rtc` va controllato.
7. **Novità da sfruttare.** Livello batteria (GPIO20, ×3), fotocamera CSI, USB-OTG su Type-C dedicato.
8. **Risoluzione nelle app.** 38 punti usano `NV_LCD_H_RES`/`NV_LCD_V_RES` e si adattano da soli.
   Ci sono però circa 130 file con 1024/600 scritti a mano (conteggio grezzo: molti sono dimensioni
   di buffer). Vanno passati in rassegna.
9. **Memoria.** Un framebuffer RGB565 passa da 1,2 MB a 2 MB, quindi 4 MB con doppio buffer più
   il buffer di rotazione. Bisogna ricontrollare il budget PSRAM.

## Da verificare a scheda in mano

- Revisione del chip P4.
- Che la SD sia sempre alimentata (R9 / GPIO45).
- Indirizzo reale del touch (0x5D o 0x14) e se l'INT funziona su GPIO33.
- Che la polarità del PWM della retroilluminazione corrisponda al BSP (100% = massimo).
- **Firmware esp_hosted sul C6: è il rischio maggiore.** Noi usiamo `esp_hosted` **2.12.9** lato
  P4. Waveshare dichiara compatibilità con il firmware di fabbrica del C6 usando `esp_hosted` 1.4.*
  su IDF 5.5 (2.12+ solo su IDF 6.0). Se il firmware di fabbrica non regge la 2.12.9, il C6 va
  riflashato. Si può fare dai pad a 4 pin (2,54 mm) per la programmazione del C6, oppure via OTA
  tramite esp_hosted.
- Prestazioni della rotazione PPA a 60 Hz.
