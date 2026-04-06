# Pylontech ESP32 Monitor

ESP32-based Pylontech battery monitor with:
- Web UI
- OTA updates
- MQTT publishing
- LittleFS-hosted frontend

## Local configuration

This repository does not store live credentials.

1. Copy `include/Config.local.example.h` to `include/Config.local.h`
2. Fill in your Wi-Fi, MQTT, and network settings

`include/Config.local.h` is ignored by Git and stays local on your machine.

## Build

```bash
PLATFORMIO_CORE_DIR=.platformio-core ~/.local/bin/platformio run
```

## Upload filesystem

If you changed the web UI in `data/`, upload LittleFS too:

```bash
PLATFORMIO_CORE_DIR=.platformio-core ~/.local/bin/platformio run -t uploadfs
```
