# MQTT Explorer

## What it is

See what flows on your MQTT broker: topics as they arrive, the payload of the last message (JSON is pretty-printed), a short history and a message count per topic.

## How to use it

The app uses the broker set in **Settings > Home**: the address and password stay with the system. Tap a topic to see its payload. **Filters** chooses what to listen to (`+` and `#` wildcards, e.g. `zigbee2mqtt/#`; the system does not allow `#` alone). **Publish** sends a test message to the topic you type. **Pause** freezes the list so you can read it.

## License

NucleoOS, MIT license.
