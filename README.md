# Pylontech ESP32 Monitor

Deutschsprachiger ESP32-Monitor fuer Pylontech-Batterien mit:
- Web-UI
- OTA-Updates
- MQTT-Anbindung
- Frontend aus LittleFS

Kurzbeschreibung fuer GitHub:
Deutschsprachiger ESP32-Monitor fuer Pylontech-Batterien mit Web-UI, OTA, MQTT und LittleFS-Frontend.

Empfohlene GitHub-Topics:
`esp32`, `pylontech`, `battery-monitor`, `mqtt`, `arduino`, `platformio`, `littlefs`, `ota`, `webui`, `home-assistant`

## Hinweis zur Sprache

Dieses Release ist bewusst auf Deutsch gehalten.
Auf dem eingesetzten System ist kein Sprachpaket installiert, daher sind Weboberflaeche, Kommentare und Projektdokumentation in deutscher Sprache gepflegt.

## Lokale Konfiguration

Dieses Repository enthaelt keine echten Zugangsdaten.

1. `include/Config.local.example.h` nach `include/Config.local.h` kopieren
2. WLAN-, MQTT- und Netzwerkeinstellungen lokal eintragen

`include/Config.local.h` ist per Git ausgeschlossen und bleibt nur auf dem Geraet bzw. der lokalen Entwicklungsumgebung.

## Build

```bash
PLATFORMIO_CORE_DIR=.platformio-core ~/.local/bin/platformio run
```

## LittleFS hochladen

Wenn Dateien in `data/` geaendert wurden, sollte anschliessend auch das LittleFS-Dateisystem hochgeladen werden:

```bash
PLATFORMIO_CORE_DIR=.platformio-core ~/.local/bin/platformio run -t uploadfs
```
