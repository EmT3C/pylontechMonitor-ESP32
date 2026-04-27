// PylontechMonitoring.ino — ESP32 + UART2 + Web + OTA + mDNS + MQTT
// inkl. Mesh-AP-Auswahl (BSSID) + optionalem Roaming

#include "Config.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <Preferences.h>
#include <circular_log.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <LittleFS.h>

#include "batteryStack.h"
#include "WebUI.h"
batteryStack g_stack{};
systemData   g_systemStack{};
dailyEnergyData g_dailyEnergy{};
statDebugData g_statDebug{};

#include "PylonLink.h"
#include "Parser.h"
#include "MQTTHandler.h"

// --- LED-Statushelfer ---
namespace Led {
  static bool s_ota = false;

  inline void writeRaw(bool on) {
    digitalWrite(LED_PIN,
      LED_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
  }

  void begin() {
    pinMode(LED_PIN, OUTPUT);
    writeRaw(false);
  }

  void setOTA(bool on) { s_ota = on; }

  void tick(bool wifiOK, bool mqttOK, bool alarm, bool charging, bool discharging, bool bootFault) {
    const unsigned long now = millis();

    if (s_ota) {
      writeRaw(((now / 80) % 2) != 0);
      return;
    }
    if (!wifiOK) {
      writeRaw(((now / 150) % 2) != 0);
      return;
    }
    if (wifiOK && !mqttOK) {
      writeRaw(((now / 500) % 2) != 0);
      return;
    }
    if (bootFault) {
      unsigned long ph = now % 2200;
      bool on = (ph < 120) ||
                (ph >= 240 && ph < 360) ||
                (ph >= 480 && ph < 600) ||
                (ph >= 720 && ph < 840);
      writeRaw(on);
      return;
    }
    if (alarm) {
      unsigned long ph = now % 1600;
      bool on = (ph < 100) || (ph >= 200 && ph < 300) || (ph >= 400 && ph < 500);
      writeRaw(on);
      return;
    }
    if (charging) {
      unsigned long ph = now % 1400;
      bool on = (ph < 180) || (ph >= 260 && ph < 440);
      writeRaw(on);
      return;
    }
    if (discharging) {
      unsigned long ph = now % 2200;
      bool on = (ph < 500);
      writeRaw(on);
      return;
    }

    writeRaw((now % 2000) < 40);
  }
}

// -----------------------------------------------------------------------------
// Globale Objekte

WebServer server(80);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, GMT_OFFSET_SEC);

circular_log<16384> g_log;

// Polling-Kommandos laufen nacheinander und koennen sich daher einen Buffer teilen.
char g_szRecvBuffPoll[16384];
char g_szRecvBuffCmd[16384];

// UART2
BatteryLink batt(Serial2, PIN_RX2, PIN_TX2);

#if ENABLE_MQTT
  WiFiClient   espClient;
  PubSubClient mqttClient(espClient);
#endif

RTC_DATA_ATTR uint32_t g_bootCount = 0;
RTC_DATA_ATTR uint32_t g_abnormalResetCount = 0;
RTC_DATA_ATTR uint8_t  g_lastPhaseRTC = 0;

static esp_reset_reason_t g_resetReason = ESP_RST_UNKNOWN;

enum class CrashPhase : uint8_t {
  Unknown = 0,
  Boot,
  Loop,
  PwrPoll,
  PwrsysPoll,
  StatPoll,
  MqttLoop,
  MqttPublish,
  Roam
};

static const char* resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:   return "Unknown";
    case ESP_RST_POWERON:   return "PowerOn";
    case ESP_RST_EXT:       return "External";
    case ESP_RST_SW:        return "Software";
    case ESP_RST_PANIC:     return "Panic";
    case ESP_RST_INT_WDT:   return "InterruptWDT";
    case ESP_RST_TASK_WDT:  return "TaskWDT";
    case ESP_RST_WDT:       return "OtherWDT";
    case ESP_RST_DEEPSLEEP: return "DeepSleep";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "Invalid";
  }
}

static const char* crashPhaseToString(CrashPhase phase) {
  switch (phase) {
    case CrashPhase::Boot:      return "Boot";
    case CrashPhase::Loop:      return "Loop";
    case CrashPhase::PwrPoll:   return "PWR";
    case CrashPhase::PwrsysPoll:return "PWRSYS";
    case CrashPhase::StatPoll:   return "STAT";
    case CrashPhase::MqttLoop:   return "MqttLoop";
    case CrashPhase::MqttPublish:return "MqttPublish";
    case CrashPhase::Roam:       return "Roam";
    default:                     return "Unknown";
  }
}

namespace CrashTrace {
  static Preferences s_prefs;
  static bool s_prefsOpen = false;
  static unsigned long s_lastPersistMs = 0;
  static CrashPhase s_lastSavedPhase = CrashPhase::Unknown;
  static uint32_t s_lastSavedBootCount = 0;
  static constexpr unsigned long kPersistIntervalMs = 300000UL;

  static void begin() {
    s_prefsOpen = s_prefs.begin("crash-trace", false);
    if (!s_prefsOpen) return;
    s_lastSavedPhase = static_cast<CrashPhase>(s_prefs.getUChar("phase", (uint8_t)CrashPhase::Unknown));
    s_lastSavedBootCount = s_prefs.getULong("boot", 0);
  }

  static void persist(CrashPhase phase) {
    if (!s_prefsOpen) return;
    s_prefs.putUChar("phase", (uint8_t)phase);
    s_prefs.putULong("boot", g_bootCount);
    s_lastSavedPhase = phase;
    s_lastSavedBootCount = g_bootCount;
    s_lastPersistMs = millis();
  }

  static void mark(CrashPhase phase, bool forcePersist = false) {
    g_lastPhaseRTC = (uint8_t)phase;
    if (!s_prefsOpen) return;

    const bool changed = s_lastSavedPhase != phase;
    const bool bootChanged = s_lastSavedBootCount != g_bootCount;
    const bool due = (millis() - s_lastPersistMs) >= kPersistIntervalMs;

    // RTC-Daten reichen fuer die unmittelbare Crash-Phase; NVS nur sparsam
    // beschreiben, damit Flash-Schreiblast und Loop-Blockaden klein bleiben.
    if (bootChanged || (changed && (forcePersist || due))) {
      persist(phase);
    }
  }

  static const char* savedPhaseText() {
    return crashPhaseToString(s_lastSavedPhase);
  }

  static const char* rtcPhaseText() {
    return crashPhaseToString(static_cast<CrashPhase>(g_lastPhaseRTC));
  }
}

static void publishMqttDiagnosticSnapshot(bool force = false) {
#if ENABLE_MQTT
  static unsigned long s_lastDiagPublishMs = 0;
  const unsigned long now = millis();
  if (!force && (now - s_lastDiagPublishMs) < 10000UL) return;

  s_lastDiagPublishMs = now;
  MQTTHandler::publishDiagnostic(resetReasonToString(g_resetReason),
                                 CrashTrace::savedPhaseText(),
                                 CrashTrace::rtcPhaseText(),
                                 g_bootCount,
                                 g_abnormalResetCount,
                                 ESP.getFreeHeap(),
                                 ESP.getMinFreeHeap());
#else
  (void)force;
#endif
}

static void publishMqttDiagnosticEvent(const char* msg, bool forceSnapshot = false) {
  if (!msg || !*msg) return;
#if ENABLE_MQTT
  MQTTHandler::publishDiagnosticEvent(msg);
  publishMqttDiagnosticSnapshot(forceSnapshot);
#else
  (void)forceSnapshot;
#endif
}

static void makeRxExcerpt(const char* src, char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (!src) return;

  size_t j = 0;
  for (size_t i = 0; src[i] && j + 1 < outSize; ++i) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\r' || c == '\n' || c == '\t') {
      out[j++] = ' ';
    } else if (c >= 0x20 && c <= 0x7E) {
      out[j++] = (char)c;
    } else {
      out[j++] = '?';
    }
  }
  out[j] = '\0';
}

static bool rxLooksLikePromptOnly(const char* src) {
  if (!src) return false;

  while (*src && isspace((unsigned char)*src)) ++src;
  if (!*src) return true;

  return (strncmp(src, "pylon>", 6) == 0) ||
         (strncmp(src, "pylon_debug>", 12) == 0) ||
         (*src == '>' && src[1] == '\0');
}

static void publishMqttDiagnosticFailure(const char* command,
                                         const char* errorText,
                                         const char* rxBuf = nullptr) {
#if ENABLE_MQTT
  char rxLen[24];
  char rxExcerpt[192];

  MQTTHandler::publishDiagnosticDetail("last_command", command ? command : "");
  MQTTHandler::publishDiagnosticDetail("last_error", errorText ? errorText : "");

  snprintf(rxLen, sizeof(rxLen), "%u", rxBuf ? (unsigned)strlen(rxBuf) : 0U);
  MQTTHandler::publishDiagnosticDetail("last_rx_len", rxLen);

  makeRxExcerpt(rxBuf, rxExcerpt, sizeof(rxExcerpt));
  MQTTHandler::publishDiagnosticDetail("last_rx_excerpt", rxExcerpt);

  publishMqttDiagnosticEvent(errorText ? errorText : "Unknown error", true);
#else
  (void)command;
  (void)errorText;
  (void)rxBuf;
#endif
}

static bool isAbnormalReset(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      return false;
  }
}

namespace StackGuard {
  static uint8_t s_missingBatteryCycles = 0;

  static bool shouldAcceptParsedStack(const batteryStack& current, const batteryStack& parsed) {
    if (!parsed.valid || parsed.batteryCount <= 0) return false;
    if (!current.valid || current.batteryCount <= 0) {
      s_missingBatteryCycles = 0;
      return true;
    }

    if (parsed.batteryCount >= current.batteryCount) {
      s_missingBatteryCycles = 0;
      return true;
    }

    if (s_missingBatteryCycles < 255) s_missingBatteryCycles++;
    return s_missingBatteryCycles >= 3;
  }

  static void markAccepted(const batteryStack& current, const batteryStack& parsed) {
    if (parsed.batteryCount >= current.batteryCount) {
      s_missingBatteryCycles = 0;
    } else if (s_missingBatteryCycles >= 3) {
      s_missingBatteryCycles = 0;
    }
  }

  static uint8_t missingCycles() {
    return s_missingBatteryCycles;
  }
}

// -----------------------------------------------------------------------------
// Mesh-AP-Auswahl + Roaming

static uint8_t g_targetBSSID[6] = {0};

namespace EnergyTracker {
  static Preferences s_prefs;
  static bool s_prefsOpen = false;
  static bool s_dirty = false;
  static unsigned long s_lastPersistMs = 0;

  static void begin(dailyEnergyData& energy) {
    s_prefsOpen = s_prefs.begin("daily-energy", false);
    if (!s_prefsOpen) return;

    energy.valid = true;
    energy.localDayNumber = s_prefs.getULong("day", 0);
    energy.chargeKWhToday = s_prefs.getFloat("chg", 0.0f);
    energy.dischargeKWhToday = s_prefs.getFloat("dsg", 0.0f);
  }

  static void persist(const dailyEnergyData& energy, bool force = false) {
    if (!s_prefsOpen) return;
    const unsigned long nowMs = millis();
    if (!force && (!s_dirty || (nowMs - s_lastPersistMs) < 300000UL)) return;

    s_prefs.putULong("day", energy.localDayNumber);
    s_prefs.putFloat("chg", energy.chargeKWhToday);
    s_prefs.putFloat("dsg", energy.dischargeKWhToday);
    s_lastPersistMs = nowMs;
    s_dirty = false;
  }

  static void update(dailyEnergyData& energy, const batteryStack& stack, NTPClient& clock) {
    static unsigned long lastIntegrateMs = 0;
    const unsigned long nowMs = millis();

    if (lastIntegrateMs == 0) {
      lastIntegrateMs = nowMs;
    }

    unsigned long dtMs = nowMs - lastIntegrateMs;
    lastIntegrateMs = nowMs;

    energy.valid = true;
    energy.lastUpdateMs = nowMs;

    const unsigned long epoch = clock.getEpochTime();
    const bool timeSynced = epoch > 1577836800UL;
    energy.timeSynced = timeSynced;
    energy.currentEpoch = epoch;

    if (timeSynced) {
      const unsigned long dayNumber = epoch / 86400UL;
      if (energy.localDayNumber == 0) {
        energy.localDayNumber = dayNumber;
        s_dirty = true;
      } else if (dayNumber != energy.localDayNumber) {
        energy.localDayNumber = dayNumber;
        energy.chargeKWhToday = 0.0f;
        energy.dischargeKWhToday = 0.0f;
        s_dirty = true;
      }
    }

    if (!stack.valid || dtMs == 0 || dtMs > 15000UL) {
      persist(energy);
      return;
    }

    const float powerW = (float)stack.getPowerDC();
    const float deltaHours = (float)dtMs / 3600000.0f;

    if (powerW > 0.0f) {
      energy.chargeKWhToday += (powerW * deltaHours) / 1000.0f;
      s_dirty = true;
    } else if (powerW < 0.0f) {
      energy.dischargeKWhToday += ((-powerW) * deltaHours) / 1000.0f;
      s_dirty = true;
    }

    persist(energy);
  }
}

namespace StatRetry {
  static bool run(BatteryLink& link,
                  circular_log<16384>& log,
                  char* recvBuf,
                  size_t recvBufLen,
                  uint8_t statIdx,
                  batteryStack& stack,
                  statDebugData& dbg)
  {
    if (statIdx < 1 || statIdx > MAX_PYLON_BATTERIES) return false;

    char statCmd[16];
    snprintf(statCmd, sizeof(statCmd), "stat %u", statIdx);
    strncpy(dbg.lastCommand, statCmd, sizeof(dbg.lastCommand) - 1);
    dbg.lastCommand[sizeof(dbg.lastCommand) - 1] = 0;

    const int maxAttempts = 2;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
      memset(recvBuf, 0, recvBufLen);

      if (!link.sendAndReceivePrompt(statCmd, recvBuf, recvBufLen, 10000)) {
        dbg.lastTimedOut = true;
        strncpy(dbg.lastMessage,
                attempt < maxAttempts ? "timeout, retry" : "timeout",
                sizeof(dbg.lastMessage) - 1);
        dbg.lastMessage[sizeof(dbg.lastMessage) - 1] = 0;

        char msg[96];
        snprintf(msg, sizeof(msg),
                 attempt < maxAttempts
                   ? "STAT timeout idx=%u attempt=%d retrying"
                   : "STAT timeout idx=%u attempt=%d",
                 statIdx, attempt);
        log.Log(msg);
        publishMqttDiagnosticFailure(statCmd, msg, recvBuf);

        if (attempt < maxAttempts) {
          delay(50);
          continue;
        }
        return false;
      }

      char gotMsg[64];
      snprintf(gotMsg, sizeof(gotMsg),
               attempt < maxAttempts ? "got %s try %d" : "got %s",
               statCmd, attempt);
      log.Log(gotMsg);
      publishMqttDiagnosticEvent(gotMsg);

      pylonBattery newBatt = stack.batts[statIdx - 1];
      if (Parser::parseStat(recvBuf, &newBatt)) {
        stack.batts[statIdx - 1].cycleTimes = newBatt.cycleTimes;
        dbg.lastSuccess = true;
        dbg.lastTimedOut = false;
        dbg.lastParseFailed = false;
        dbg.lastCycleTimes = newBatt.cycleTimes;
        dbg.lastSuccessMs = millis();
        strncpy(dbg.lastMessage, attempt > 1 ? "ok after retry" : "ok", sizeof(dbg.lastMessage) - 1);
        dbg.lastMessage[sizeof(dbg.lastMessage) - 1] = 0;

        char okMsg[96];
        snprintf(okMsg, sizeof(okMsg),
                 attempt > 1
                   ? "STAT ok idx=%u cycleTimes=%ld after retry"
                   : "STAT ok idx=%u cycleTimes=%ld",
                 statIdx, newBatt.cycleTimes);
        log.Log(okMsg);
        publishMqttDiagnosticEvent(okMsg);
        return true;
      }

      dbg.lastParseFailed = true;
      strncpy(dbg.lastMessage,
              attempt < maxAttempts ? "parse failed, retry" : "parse failed",
              sizeof(dbg.lastMessage) - 1);
      dbg.lastMessage[sizeof(dbg.lastMessage) - 1] = 0;

      char failMsg[112];
      snprintf(failMsg, sizeof(failMsg),
               attempt < maxAttempts
                 ? "STAT parse failed idx=%u attempt=%d retrying"
                 : "STAT parse failed idx=%u attempt=%d - keeping previous cycleTimes",
               statIdx, attempt);
      log.Log(failMsg);
      publishMqttDiagnosticFailure(statCmd, failMsg, recvBuf);

      if (attempt < maxAttempts) {
        delay(50);
      }
    }

    return false;
  }
}

bool connectToBestAP(const char* ssid, const char* pass) {
  Serial.println(F("Scanning for best AP..."));
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    Serial.println(F("No networks visible."));
    return false;
  }

  int bestIdx  = -1;
  int bestRSSI = -999;
  int bestChan = 0;
  uint8_t bestBSSID[6] = {0};

  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) {
      int r = WiFi.RSSI(i);
      if (r > bestRSSI) {
        bestRSSI = r;
        bestIdx  = i;
        bestChan = WiFi.channel(i);
        const uint8_t* b = WiFi.BSSID(i);
        memcpy(bestBSSID, b, 6);
      }
    }
  }

  if (bestIdx < 0) {
    Serial.println(F("SSID not found in scan."));
    return false;
  }

  memcpy(g_targetBSSID, bestBSSID, 6);
  Serial.printf("Connecting to %s  BSSID %02X:%02X:%02X:%02X:%02X:%02X  ch=%d  RSSI=%d dBm\n",
                ssid,
                g_targetBSSID[0], g_targetBSSID[1], g_targetBSSID[2],
                g_targetBSSID[3], g_targetBSSID[4], g_targetBSSID[5],
                bestChan, bestRSSI);

  WiFi.begin(ssid, pass, bestChan, g_targetBSSID, true);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
    Serial.print('.');
  }
  Serial.println();

  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("WiFi %s, IP=%s, RSSI=%d dBm\n",
                ok ? "connected" : "FAILED",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return ok;
}

void roamIfNeeded(const char* ssid, const char* pass) {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 7000) return;
  lastCheck = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  int curRSSI = WiFi.RSSI();
  if (curRSSI > -67) return;

  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) return;

  int bestRSSI = curRSSI;
  int bestChan = WiFi.channel();
  uint8_t bestBSSID[6];
  memcpy(bestBSSID, g_targetBSSID, 6);

  bool foundBetter = false;

  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != ssid) continue;
    int r = WiFi.RSSI(i);
    const uint8_t* b = WiFi.BSSID(i);

    if (r >= (curRSSI + 8) && memcmp(b, g_targetBSSID, 6) != 0) {
      bestRSSI = r;
      bestChan = WiFi.channel(i);
      memcpy(bestBSSID, b, 6);
      foundBetter = true;
    }
  }

  if (foundBetter) {
    Serial.printf("Roaming to stronger AP: %02X:%02X:%02X:%02X:%02X:%02X (ch %d, %d dBm)\n",
                  bestBSSID[0], bestBSSID[1], bestBSSID[2],
                  bestBSSID[3], bestBSSID[4], bestBSSID[5],
                  bestChan, bestRSSI);
    memcpy(g_targetBSSID, bestBSSID, 6);
    WiFi.begin(ssid, pass, bestChan, g_targetBSSID, true);
  }
}

// -----------------------------------------------------------------------------
// Setup

void setup() {
  Serial.begin(115200);
  delay(200);

  g_resetReason = esp_reset_reason();
  g_bootCount++;
  if (isAbnormalReset(g_resetReason)) {
    g_abnormalResetCount++;
  }

  CrashTrace::begin();
  CrashTrace::mark(CrashPhase::Boot, true);

  {
    char bootMsg[224];
    snprintf(bootMsg, sizeof(bootMsg),
             "BOOT reset=%s bootCount=%lu abnormalResets=%lu savedPhase=%s rtcPhase=%s",
             resetReasonToString(g_resetReason),
             (unsigned long)g_bootCount,
             (unsigned long)g_abnormalResetCount,
             CrashTrace::savedPhaseText(),
             CrashTrace::rtcPhaseText());
    Serial.println(bootMsg);
    g_log.Log(bootMsg);
    publishMqttDiagnosticEvent(bootMsg, true);
  }

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  #ifdef USE_STATIC_IP
    #ifndef IP_ADDRESS
      #error "USE_STATIC_IP ist gesetzt, aber IP_ADDRESS fehlt in Config.h"
    #endif
    WiFi.config(IP_ADDRESS, IP_GATEWAY, IP_SUBNET, IP_DNS1, IP_DNS2);
  #endif

  connectToBestAP(WIFI_SSID, WIFI_PASS);

  Led::begin();

  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA
    .onStart([]() {
      Led::setOTA(true);
      Serial.println("OTA start");
    })
    .onEnd([]() {
      Led::setOTA(false);
      Serial.println("\nOTA end");
    })
    .onProgress([](unsigned int p, unsigned int t) {
      Serial.printf("OTA %u%%\r", (p * 100) / t);
    })
    .onError([](ota_error_t e) {
      Led::setOTA(false);
      Serial.printf("OTA Error[%u]\n", e);
    });

  ArduinoOTA.begin();
  Serial.printf("OTA ready at %s (%s)\n",
                WIFI_HOSTNAME, WiFi.localIP().toString().c_str());

  if (MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("arduino", "tcp", 3232);
  }

  timeClient.begin();
  EnergyTracker::begin(g_dailyEnergy);
  batt.begin(DEFAULT_BAUD);
  Parser::init(&g_log);

  server.on("/", []() {
    if (LittleFS.exists("/index.html")) {
      File f = LittleFS.open("/index.html", "r");
      server.streamFile(f, "text/html");
      f.close();
    } else {
      server.send(404, "text/plain", "index.html not found");
    }
  });

  server.onNotFound([]() {
    String path = server.uri();
    if (path == "/") path = "/index.html";

    if (LittleFS.exists(path)) {
      String ct = "text/plain";
      if (path.endsWith(".html")) ct = "text/html";
      else if (path.endsWith(".css")) ct = "text/css";
      else if (path.endsWith(".js")) ct = "application/javascript";
      else if (path.endsWith(".json")) ct = "application/json";
      else if (path.endsWith(".svg")) ct = "image/svg+xml";
      else if (path.endsWith(".png")) ct = "image/png";

      File f = LittleFS.open(path, "r");
      server.streamFile(f, ct);
      f.close();
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });

  server.on("/health", []() {
    server.send(200, "text/plain", "OK");
  });

  server.on("/diag", []() {
    String s = "RSSI=" + String((WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0) +
               " dBm\nSleep=" + String(WiFi.getSleep() ? "on" : "off") +
               "\nHeap=" + String(ESP.getFreeHeap()) +
               "\nMinFreeHeap=" + String(ESP.getMinFreeHeap()) +
               "\nReset=" + String(resetReasonToString(g_resetReason)) +
               "\nBootCount=" + String(g_bootCount) +
               "\nAbnormalResets=" + String(g_abnormalResetCount) +
               "\nLastPhaseSaved=" + String(CrashTrace::savedPhaseText()) +
               "\nLastPhaseRTC=" + String(CrashTrace::rtcPhaseText());
    server.send(200, "text/plain", s);
  });

  server.on("/log", []() {
    server.send(200, "text/html", g_log.c_str());
  });

  WebUI::init(&server, &batt, &g_stack, &g_systemStack, &g_dailyEnergy,
              &g_statDebug,
              g_szRecvBuffCmd,
              sizeof(g_szRecvBuffCmd),
              &g_log);

  server.begin();
  Serial.println("HTTP server started");

#if ENABLE_MQTT
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(1024);
  MQTTHandler::init(&mqttClient, &g_stack, &g_systemStack, &g_dailyEnergy);
#endif
}

// -----------------------------------------------------------------------------
// Loop

void loop() {
  CrashTrace::mark(CrashPhase::Loop);
  ArduinoOTA.handle();
  server.handleClient();
  timeClient.update();
  EnergyTracker::update(g_dailyEnergy, g_stack, timeClient);

#if ENABLE_MQTT
  CrashTrace::mark(CrashPhase::MqttLoop);
  MQTTHandler::loop();
  publishMqttDiagnosticSnapshot();
#endif

  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  bool mqttOK = false;
#if ENABLE_MQTT
  mqttOK = mqttClient.connected();
#endif

  {
    static bool s_lastWifi = false;
    static bool s_lastMqtt = false;
    if (wifiOK != s_lastWifi) {
      s_lastWifi = wifiOK;
      if (wifiOK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "WiFi: connected RSSI=%d", WiFi.RSSI());
        g_log.Log(msg);
        publishMqttDiagnosticEvent(msg);
      } else {
        g_log.Log("WiFi: disconnected");
        publishMqttDiagnosticEvent("WiFi: disconnected", true);
      }
    }
#if ENABLE_MQTT
    if (mqttOK != s_lastMqtt) {
      s_lastMqtt = mqttOK;
      g_log.Log(mqttOK ? "MQTT: connected" : "MQTT: disconnected");
      publishMqttDiagnosticEvent(mqttOK ? "MQTT: connected" : "MQTT: disconnected", true);
    }
#endif
  }

  bool alarm = (strcmp(g_stack.baseState, "Alarm!") == 0) ||
               (strcmp(g_systemStack.alarmState, "Alarm") == 0);
  bool charging = strcmp(g_stack.baseState, "Charge") == 0;
  bool discharging = strcmp(g_stack.baseState, "Dischg") == 0;
  bool bootFault = isAbnormalReset(g_resetReason) && millis() < 30000UL;

  Led::tick(wifiOK, mqttOK, alarm, charging, discharging, bootFault);

  // ---------------------------
  // Hauptpolling
  // ---------------------------
  static uint32_t lastPollPwr = 0;
  if (millis() - lastPollPwr >= 2000UL) {
    lastPollPwr = millis();
    CrashTrace::mark(CrashPhase::PwrPoll);

    memset(g_szRecvBuffPoll, 0, sizeof(g_szRecvBuffPoll));
    const unsigned long pwrT0 = millis();
    if (batt.sendAndReceive("pwr", g_szRecvBuffPoll, sizeof(g_szRecvBuffPoll), 4000)) {
      const unsigned long pwrMs = millis() - pwrT0;
      batteryStack parsedStack = g_stack;
      if (Parser::parsePwr(g_szRecvBuffPoll, &parsedStack)) {
        if (StackGuard::shouldAcceptParsedStack(g_stack, parsedStack)) {
          const bool stateChanged = strcmp(g_stack.baseState, parsedStack.baseState) != 0
                                 || g_stack.batteryCount != parsedStack.batteryCount;
          batteryStack previousStack = g_stack;
          g_stack = parsedStack;
          StackGuard::markAccepted(previousStack, parsedStack);
          if (stateChanged || pwrMs > 2000) {
            char msg[80];
            snprintf(msg, sizeof(msg), "PWR %dbats state=%s SoC=%d%% %lums",
                     g_stack.batteryCount, g_stack.baseState, g_stack.soc, pwrMs);
            g_log.Log(msg);
            publishMqttDiagnosticEvent(msg);
          }
        } else {
          char msg[96];
          snprintf(msg, sizeof(msg),
                   "PWR transient drop %d->%d held (%u)",
                   g_stack.batteryCount,
                   parsedStack.batteryCount,
                   StackGuard::missingCycles());
          g_log.Log(msg);
          publishMqttDiagnosticEvent(msg, true);
        }
      } else {
        g_log.Log("PWR parse failed - keeping previous values");
        publishMqttDiagnosticFailure("pwr", "PWR parse failed - keeping previous values", g_szRecvBuffPoll);
      }
    } else {
      char msg[48];
      snprintf(msg, sizeof(msg), "PWR timeout after %lums", millis() - pwrT0);
      g_log.Log(msg);
      publishMqttDiagnosticFailure("pwr", msg, g_szRecvBuffPoll);
    }
  }

  // ---------------------------
  // pwrsys langsamer pollen
  // ---------------------------
  static uint32_t lastPollPwrsys = 0;
  const bool chargeSuppressed =
    g_systemStack.valid &&
    g_systemStack.soc >= 99 &&
    g_systemStack.rec_chg_current == 0 &&
    g_systemStack.sys_rec_chg_current == 0;
  const unsigned long pwrsysPollInterval = chargeSuppressed ? 300000UL : 15000UL;
  const unsigned long pwrsysTimeoutMs = chargeSuppressed ? 5000UL : 6000UL;

  if (millis() - lastPollPwrsys >= pwrsysPollInterval) {
    lastPollPwrsys = millis();
    CrashTrace::mark(CrashPhase::PwrsysPoll);

    memset(g_szRecvBuffPoll, 0, sizeof(g_szRecvBuffPoll));
    const unsigned long pwrsysT0 = millis();
    if (batt.sendAndReceive("pwrsys", g_szRecvBuffPoll, sizeof(g_szRecvBuffPoll), pwrsysTimeoutMs)) {
      const unsigned long pwrsysMs = millis() - pwrsysT0;
      systemData parsedSystem = g_systemStack;
      if (Parser::parsePwrsys(g_szRecvBuffPoll, &parsedSystem)) {
        g_systemStack = parsedSystem;
        if (pwrsysMs > 3000) {
          char msg[48];
          snprintf(msg, sizeof(msg), "PWRSYS slow: %lums", pwrsysMs);
          g_log.Log(msg);
          publishMqttDiagnosticEvent(msg);
        }
      } else {
        if (chargeSuppressed && rxLooksLikePromptOnly(g_szRecvBuffPoll)) {
          g_log.Log("PWRSYS prompt-only in idle/full - keeping previous values");
          publishMqttDiagnosticEvent("PWRSYS prompt-only in idle/full - keeping previous values");
        } else {
          g_log.Log("PWRSYS parse failed - keeping previous values");
          publishMqttDiagnosticFailure("pwrsys", "PWRSYS parse failed - keeping previous values", g_szRecvBuffPoll);
        }
      }
    } else {
      if (chargeSuppressed && rxLooksLikePromptOnly(g_szRecvBuffPoll)) {
        g_log.Log("PWRSYS prompt-only timeout in idle/full - keeping previous values");
        publishMqttDiagnosticEvent("PWRSYS prompt-only timeout in idle/full - keeping previous values");
      } else {
        char msg[48];
        snprintf(msg, sizeof(msg), "PWRSYS timeout after %lums", millis() - pwrsysT0);
        g_log.Log(msg);
        publishMqttDiagnosticFailure("pwrsys", msg, g_szRecvBuffPoll);
      }
    }
  }

// ---------------------------
// stat pro Batterie:
// - erste Runde nach dem Start, aber mit 60 s Wartezeit
// - erste Runde immer fuer alle moeglichen Batterie-Slots
// - danach nur noch alle 4 h pro erkannter Batterie
// - alte cycleTimes bleiben bei Fehlern erhalten
// ---------------------------
static uint32_t statBootDelayStart = millis();
static uint32_t lastPollStat = 0;
static uint8_t  statIdx = 1;
static bool     statInitialRun = true;

const unsigned long statBootDelayMs       = 60000UL;       // 60 s nach Boot warten
const unsigned long statInitialIntervalMs = 30000UL;       // 30 s zwischen Batterien beim Initiallauf
const unsigned long statRegularIntervalMs = 14400000UL;    // 4 h zwischen Batterien danach

if (millis() - statBootDelayStart >= statBootDelayMs) {
  unsigned long statInterval = statInitialRun ? statInitialIntervalMs : statRegularIntervalMs;

  if (lastPollStat == 0 || (millis() - lastPollStat >= statInterval)) {
    lastPollStat = millis();
    CrashTrace::mark(CrashPhase::StatPoll);

    int highestPresentIdx = 0;
    for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
      if (g_stack.batts[i].isPresent) {
        highestPresentIdx = i + 1;
      }
    }

    int maxBat = statInitialRun
      ? MAX_PYLON_BATTERIES
      : (highestPresentIdx > 0 ? highestPresentIdx : g_stack.batteryCount);

    if (maxBat < 1) maxBat = 1;
    if (maxBat > MAX_PYLON_BATTERIES) maxBat = MAX_PYLON_BATTERIES;

    if (statIdx > maxBat) statIdx = 1;

    g_statDebug.currentIdx = statIdx;
    g_statDebug.maxBat = maxBat;
    g_statDebug.detected = g_stack.batteryCount;
    g_statDebug.highestPresentIdx = highestPresentIdx;
    g_statDebug.initialRun = statInitialRun;
    g_statDebug.inProgress = true;
    g_statDebug.lastSuccess = false;
    g_statDebug.lastTimedOut = false;
    g_statDebug.lastParseFailed = false;
    g_statDebug.lastAttemptMs = millis();

    {
      char dbg[96];
      snprintf(dbg, sizeof(dbg),
               "STAT sched idx=%u max=%d initial=%s detected=%d highest=%d",
               statIdx, maxBat, statInitialRun ? "yes" : "no",
               g_stack.batteryCount, highestPresentIdx);
      g_log.Log(dbg);
      publishMqttDiagnosticEvent(dbg);
    }

    StatRetry::run(batt,
                   g_log,
                   g_szRecvBuffPoll,
                   sizeof(g_szRecvBuffPoll),
                   statIdx,
                   g_stack,
                   g_statDebug);

    statIdx++;

    if (statIdx > maxBat) {
      statIdx = 1;

      if (statInitialRun) {
        statInitialRun = false;
        g_log.Log("initial stat round completed");
        publishMqttDiagnosticEvent("initial stat round completed");
      }
    }

    g_statDebug.initialRun = statInitialRun;
    g_statDebug.inProgress = false;
  }
}

#if ENABLE_MQTT
  CrashTrace::mark(CrashPhase::MqttPublish);
  MQTTHandler::publishIfConnected();
#endif

  CrashTrace::mark(CrashPhase::Roam);
  roamIfNeeded(WIFI_SSID, WIFI_PASS);
}
