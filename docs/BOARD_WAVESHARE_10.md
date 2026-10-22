# Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1 — porting board sheet

Candidate chosen for NucleoOS on a large screen (decision of 2026-10-16). Not bought yet.

- Shop: <https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7-8-10.1.htm?sku=33672> (SKU 33672, ~$90; 33150 = "-EN" variant)
- Documentation: <https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-X>
- Schematic: <https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-X/ESP32-P4-WIFI6-Touch-LCD-X-Schematic.pdf>
- Adapter schematic (USB, buttons, microphones): <https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-X/ESP32-P4-Connect-Adapter-Schematic.pdf>
- BSP and ESP-IDF examples: <https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-X>
  (BSP in `firmware/brookesia/components/esp32_p4_wifi6_touch_lcd_x/`, v2.0.3)

Sources: schematic read in full and the official BSP. Items marked **⚠ to verify** are not yet
confirmed on the real board.

## Summary

| | Guition JC1060P470C (current) | Waveshare 10.1" |
|---|---|---|
| SoC | ESP32-P4 | ESP32-P4NRW32 (ESP32-P4-Core module), 32 MB PSRAM in package |
| Flash | — | 32 MB NOR (GD25Q256) |
| Display | 7", 1024×600, JD9165 | 10.1", **800×1280 portrait**, **JD9365**, IPS, 400 cd/m² |
| MIPI-DSI | 2 lane, 750 Mbps, DPI 50 MHz | 2 lane, **1500 Mbps**, timing `JD9365_800_1280_PANEL_60HZ_DPI_CONFIG` |
| Touch | GT911 @ 0x5D | GT9271 (driver `esp_lcd_touch_gt911`), 10 points, 0x5D or 0x14 |
| Wi-Fi/BT | ESP32-C6 via SDIO | ESP32-C6-MINI-1U-H8 via SDIO (same pins) |
| Audio | ES8311 + ES7210 | ES8311 + ES7210 + NS4150B amplifier |
| External RTC | RX8130 (I2C 0x14) | **absent** |
| Ethernet | IP101 (RMII) | **absent** |
| Battery | — | 1.2 mm JST connector, ETA6003 charger, measurement on GPIO20 |
| Camera | — | MIPI-CSI 2 lane connector (OV5647 in the kit) |

## Pin map (from the schematic)

| Function | Waveshare GPIO | Guition GPIO | Notes |
|---|---|---|---|
| I2C SDA / SCL | 7 / 8 | 7 / 8 | same; bus shared by touch, ES8311, ES7210, camera |
| LCD reset | **27** | 5 | R76 10 kΩ to GND |
| PWM backlight | **26** | 23 | `LCD_BL_PWM`, 10-bit LEDC in the BSP; higher duty = brighter |
| Touch INT | **33** | 21 | connected via R32 0 Ω (the BSP does not use it) |
| Touch RST | **23** | 22 | connected via R37 0 Ω (the BSP does not use it) |
| I2S MCLK | 13 | 13 | same |
| I2S BCLK (SCLK) | 12 | 12 | same |
| I2S WS (LRCK) | 10 | 10 | same |
| I2S DOUT → ES8311 | 9 | 9 | same |
| I2S DIN ← ES7210 | **11** | 48 | analog microphones via ES7210 |
| Speaker amplifier | **53** | 11 | `PA_CTRL` to NS4150B, R106 10 kΩ to GND (off by default) |
| SD CLK/CMD/D0–D3 | 43/44/39/40/41/42 | 43/44/39/40/41/42 | same; pull-ups on the internal LDO VO4 |
| SD power | GPIO45 (R9 "NC/0R") | — | ⚠ to verify: with R9 not fitted the SD is always powered |
| C6 SDIO CLK/CMD | 18 / 19 | 18 / 19 | same as our `sdkconfig` |
| C6 SDIO D0–D3 | 14 / 15 / 16 / 17 | 14 / 15 / 16 / 17 | same |
| C6 reset (CHIP_PU) | 54 | — | R54 0 Ω |
| C6 IO2 | 6 | — | R52 0 Ω |
| Battery (ADC) | **20** | — | 200 k/100 k divider: V_bat = V_pin × 3 |
| BOOT | 35 | 35 | button on the adapter |
| UART0 TX / RX | 37 / 38 | 37 / 38 | CH343P bridge with DTR/RTS auto-reset |
| USB full-speed | 24 / 25 | — | only on the 40-pin connector |
| USB high-speed OTG | dedicated DP/DM | — | "USB-OTG" Type-C on the adapter |

**40-pin connector (J8)**, free pins: 2, 3, 4, 5, 21, 22, 28, 29, 30, 31, 32, 34, 36, 46, 47,
48, 49, 50, 51, 52 (plus 7/8 I2C, 37/38 UART, 24/25 USB-FS, 35 BOOT). TP6 = GPIO36, TP7 = GPIO45.

**I2C addresses:** ES8311 0x18 (CE to ground), ES7210 0x40, touch 0x5D or 0x14 (the BSP tries
both). Without the RX8130, 0x14 no longer conflicts.

## Power

- **MIPI D-PHY** on the internal LDO **VO3 at 2500 mV**: same as `NV_MIPI_LDO_CHAN` / `NV_MIPI_LDO_MV`.
- **Flash** on LDO VO1 (R43 0 Ω). **SD** on LDO VO4, as in the BSP (`ldo_chan_id = 4`).
- **3V3** from the TPS63020 buck-boost; core **1.2 V** from the MP1605.
- **Panel IOVCC** at 1.8 V from an always-on RT9193-18 LDO.
- **Backlight**: AP3032 boost, 3 strings of 7 LEDs, about 153 mA on the 10.1". `BL_EN` has a
  fixed pull-up, so brightness is adjusted only with the PWM on GPIO26.
- **SYS_EN switch** on the adapter. With a battery the board stays on without USB.

## Chip revision

Waveshare builds differently for **P4 v1.x** and **v3.x**:
- `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` or `CONFIG_ESP32P4_REV_MIN_300`;
- DSI PHY clock: `MIPI_DSI_PHY_CLK_SRC_DEFAULT` (< v3) or `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT` (v3).

When the board arrives, the revision must be read (`esptool chip_id`) before the first flash.

## Porting work

1. **Board selection at compile time.** A `nv_pins_waveshare10.h` chosen via Kconfig is needed, with
   the GPIOs from the table above.
2. **Panel.** Add the dependency `espressif/esp_lcd_jd9365` (^2.0.2) and copy from the BSP
   (`esp32_p4_wifi6_touch_lcd_x.c`, `#else` branch of the 10.1", lines ~238–461) the initial
   command table. 1500 Mbps per lane, 60 Hz timing from the driver.
3. **Orientation.** The panel is portrait (800×1280); NucleoOS is designed for landscape.
   `nv_disp` can already rotate with the PPA into the back buffer (`nv_hal.cpp`, near
   `nv_disp_create`), but here the rotation would be **always** active, not only on request.
   ⚠ To measure: PPA cost and PSRAM bandwidth at 1280×800, and the direct video blit
   (`nv_hal_video_blit`) which today writes without rotation.
4. **Touch.** Same GT911 driver. With INT/RST connected (33/23) the current logic can be kept.
   The coordinates must be rotated like the screen.
5. **Audio.** `nv_audio` changes DIN (11) and PA (53). The other I2S pins stay the same.
6. **Services without hardware.** `nv_rtc` (no RX8130) and `nv_eth` (no IP101) must stay
   off. `nv_eth` already handles the absence; for `nv_rtc` it needs to be checked.
7. **New features to exploit.** Battery level (GPIO20, ×3), CSI camera, USB-OTG on a dedicated Type-C.
8. **Resolution in apps.** 38 places use `NV_LCD_H_RES`/`NV_LCD_V_RES` and adapt by themselves.
   However, there are about 130 files with 1024/600 hand-written (raw count: many are buffer
   sizes). They need to be reviewed.
9. **Memory.** An RGB565 framebuffer goes from 1.2 MB to 2 MB, so 4 MB with double buffering plus
   the rotation buffer. The PSRAM budget must be rechecked.

## To verify with the board in hand

- P4 chip revision.
- That the SD is always powered (R9 / GPIO45).
- Actual touch address (0x5D or 0x14) and whether INT works on GPIO33.
- That the backlight PWM polarity matches the BSP (100% = maximum).
- **esp_hosted firmware on the C6: this is the biggest risk.** We use `esp_hosted` **2.12.9** on the
  P4 side. Waveshare declares compatibility with the C6 factory firmware using `esp_hosted` 1.4.*
  on IDF 5.5 (2.12+ only on IDF 6.0). If the factory firmware does not work with 2.12.9, the C6 must be
  reflashed. This can be done from the 4-pin (2.54 mm) pads for C6 programming, or via OTA
  through esp_hosted.
- PPA rotation performance at 60 Hz.
