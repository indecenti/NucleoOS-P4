# Home

## What it is

Your home's panel with Home Assistant: lights, plugs, fans, covers, thermostats, locks, media players, robot vacuums, scenes, scripts and sensors (temperature, humidity, power, light level, doors and windows, motion, water leaks, smoke), by room.

## How to use it

- Pick a room at the top, or **All**. With many rooms, the arrow at the end of the row shows the others.
- **Tap a tile** to switch it: lights, plugs, fans, switches.
- **Covers**: one tap opens or closes; while one moves, a tap stops it.
- On a **light that is on**, tap the bar at the bottom: left is dimmer, right is brighter.
- On a **thermostat**, tap **−** or **+** for half a degree less or more.
- **Media players**: a tap pauses or resumes. **Vacuums**: a tap starts one, another sends it back to its base.
- **Scenes** and **scripts** run with a tap.
- More than 12 devices: use the **‹ ›** arrows at the top right, or swipe sideways.

Tiles take a colour when the device is active: yellow for lights and plugs, blue for open covers, orange or blue for a thermostat that heats or cools, red for an unlocked lock or an alarm (water, smoke, gas).

The top right corner says how updates arrive: **Live** when Home Assistant tells the app as soon as something changes, **Every 5 s** when the app has to ask.

## Safety

- **Unlocking a lock** and **opening a garage, gate or door** always ask first. Locking takes one tap.
- The app never sees the Home Assistant token: the system keeps it and makes the requests.
- The app only sends the commands meant for each kind of device, from a fixed list.

## Keyboard

- **Arrows** and **Tab** move the selection, **Enter** or **Space** uses the selected tile or button.
- **+** and **−** change the selected light's brightness (10 % a step) or the thermostat's temperature (half a degree).
- **PgUp** and **PgDn** turn the page, **Esc** closes the confirmation or the app.

## Before you start

In **Settings > Home** enter your Home Assistant address and an access token (in Home Assistant: profile > Security > Long-lived access tokens). If the token is refused, the app says so: create a new one.

The app shows up to 72 devices. Names that start with their device's name lose it, as in Home Assistant's own cards.
