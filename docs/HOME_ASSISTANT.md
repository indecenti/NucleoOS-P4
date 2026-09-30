# NucleoOS in Home Assistant

The panel joins Home Assistant through MQTT discovery: no YAML, no custom integration.

## Setup

1. In Home Assistant install a broker (Settings > Add-ons > **Mosquitto broker**) and the
   **MQTT** integration (Settings > Devices & services). Create a Home Assistant user for the
   panel (or use a Mosquitto login).
2. On the panel: **Settings > Home**
   - Broker: `homeassistant.local` or the IP of the machine running Mosquitto
   - Port: `1883`
   - Username / password: the MQTT login
   - **Save and connect**. Status turns to *Connected · <ip>:1883*.
3. In Home Assistant the device **NucleoOS xxxxxx** appears under the MQTT integration.

Switching *Connect to Home Assistant* off removes the device from Home Assistant.
The password is kept only on the panel (not in the SD backup, never shown again).

## Entities

| Entity | Type | Notes |
|---|---|---|
| Screen | light | off = screen sleep (lock screen if enabled), brightness 1-100 % |
| Volume | number | 0-100 |
| Mute, Do not disturb | switch | |
| Notification | notify | text, or JSON `{"title": "...", "message": "..."}` |
| Speak | notify | offline voice (needs a voice pack on the SD) |
| App | select | open an app by id, `home` = launcher |
| Go home, Lock screen, Restart | button | unlocking stays on the panel |
| Locked | binary_sensor | |
| Touch activity | binary_sensor | on while the panel was touched in the last 60 s (presence) |
| Chip temperature, Wi-Fi signal, Uptime, free RAM/PSRAM, IP | sensor | diagnostic, every 60 s |

Notify and Speak are limited to 1 per second (burst 5).

## Automation examples

```yaml
# Doorbell -> notification + voice on the panel
- action: notify.send_message
  target: { entity_id: notify.nucleoos_a1b2c3_notification }
  data: { title: "Door", message: "Someone is at the door" }
- action: notify.send_message
  target: { entity_id: notify.nucleoos_a1b2c3_speak }
  data: { message: "Someone is at the door" }

# Screen off at night
- action: light.turn_off
  target: { entity_id: light.nucleoos_a1b2c3_screen }
```

## Topics (for other MQTT tools)

- `nucleo/<node>/status` — `online` / `offline` (retained, LWT)
- `nucleo/<node>/<entity>/state`, commands on `nucleo/<node>/<entity>/set`
- `nucleo/<node>/diag` — JSON with temp, rssi, uptime, sram, psram, ip
- discovery: `homeassistant/<component>/<node>/<entity>/config` (retained)

`<node>` is shown in Settings > Home (*Device id*).

Limits of this version: plain MQTT only (no TLS), one broker.
