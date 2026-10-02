---
name: casa
description: control the smart home: lights, switches, plugs, thermostat, covers, scenes, sensors (Home Assistant, Shelly, Tasmota, WLED)
triggers: accendi, spegni, luce, luci, lampada, presa, tapparella, tapparelle, tenda, termostato, riscaldamento, condizionatore, temperatura in casa, scena, casa, domotica, home assistant, shelly, tasmota, wled, sensore, turn on, turn off, lights, thermostat, blinds, smart home
offline: Per i comandi della casa serve la rete locale; Home Assistant si configura in Impostazioni > Casa.
---
Two ways to the home, cheapest first (one ACT per reply; read before acting):
1) Home Assistant (if configured in Settings > Casa):
   - ACT sh ha say <the user's request in their words>   -> Assist does it (it knows rooms and names).
     Reply "action_done"/"query_answer" = done: answer briefly with its text.
   - If Assist did not understand: ACT sh ha ls [room|name|domain] (one line per entity:
     id state level "Name" @Room), then act precisely:
     ha on|off|toggle ENTITY_OR_NAME ... | ha set light.x brightness_pct=40 color_name=red |
     ha set climate.x temperature=21 | ha set cover.x position=50 | ha call scene.turn_on scene.x
     Each action prints the entity's new state: check it before saying it is done.
   - Sensors / history questions: ha ls sensor, ha get ENTITY.
2) Devices without Home Assistant: ACT sh dev ls (dev scan finds Shelly/WLED once; it saves the list, so it asks), then
   dev on|off|toggle NAME, dev set NAME bri=0-100, dev get NAME.
Automations on the home ("ogni sera alle 23 spegni tutto"): an automazioni rule whose action is
{"type":"run_sh","input":{"command":"ha say spegni tutte le luci"}}.
Never guess entity ids: list first. Ask before locks, alarms, garage doors and anything that could
be unsafe if wrong.
