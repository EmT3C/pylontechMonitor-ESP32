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

  void tick(bool wifiOK, bool mqttOK, bool alarm) {
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
    if (alarm) {
      unsigned long ph = now % 1600;
      bool on = (ph < 100) || (ph >= 200 && ph < 300) || (ph >= 400 && ph < 500);
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

circular_log<7000> g_log;

// Polling-Kommandos laufen nacheinander und koennen sich daher einen Buffer teilen.
char g_szRecvBuffPoll[16384];
char g_szRecvBuffCmd[16384];

// UART2
BatteryLink batt(Serial2, PIN_RX2, PIN_TX2);

#if ENABLE_MQTT
  WiFiClient   espClient;
  PubSubClient mqttClient(espClient);
#endif

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
                  circular_log<7000>& log,
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

        if (attempt < maxAttempts) {
          delay(2000);
          continue;
        }
        return false;
      }

      char gotMsg[64];
      snprintf(gotMsg, sizeof(gotMsg),
               attempt < maxAttempts ? "got %s try %d" : "got %s",
               statCmd, attempt);
      log.Log(gotMsg);

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

      if (attempt < maxAttempts) {
        delay(2000);
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
               "\nHeap=" + String(ESP.getFreeHeap());
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
  ArduinoOTA.handle();
  server.handleClient();
  timeClient.update();
  EnergyTracker::update(g_dailyEnergy, g_stack, timeClient);

#if ENABLE_MQTT
  MQTTHandler::loop();
#endif

  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  bool mqttOK = false;
#if ENABLE_MQTT
  mqttOK = mqttClient.connected();
#endif

  bool alarm = (strcmp(g_stack.baseState, "Alarm!") == 0) ||
               (strcmp(g_systemStack.alarmState, "Alarm") == 0);

  Led::tick(wifiOK, mqttOK, alarm);

  // ---------------------------
  // Hauptpolling
  // ---------------------------
  static uint32_t lastPollPwr = 0;
  if (millis() - lastPollPwr >= 2000) {
    lastPollPwr = millis();

    memset(g_szRecvBuffPoll, 0, sizeof(g_szRecvBuffPoll));
    if (batt.sendAndReceive("pwr", g_szRecvBuffPoll, sizeof(g_szRecvBuffPoll), 4000)) {
      g_log.Log("got PWR");
      Parser::parsePwr(g_szRecvBuffPoll, &g_stack);

      const char* p1 = strstr(g_szRecvBuffPoll, "\r\r\n1     ");
      if (p1) {
        char lbuf[160];
        strncpy(lbuf, p1 + 3, sizeof(lbuf) - 1);
        lbuf[sizeof(lbuf) - 1] = 0;
        char* e = strchr(lbuf, '\n');
        if (e) *e = 0;
        g_log.Log("PWR L1:");
        g_log.Log(lbuf);
      }
    }
  }

  // ---------------------------
  // pwrsys langsamer pollen
  // ---------------------------
  static uint32_t lastPollPwrsys = 0;
  if (millis() - lastPollPwrsys >= 15000) {
    lastPollPwrsys = millis();

    memset(g_szRecvBuffPoll, 0, sizeof(g_szRecvBuffPoll));
    if (batt.sendAndReceive("pwrsys", g_szRecvBuffPoll, sizeof(g_szRecvBuffPoll), 6000)) {
      g_log.Log("got PWRSYS");
      Parser::parsePwrsys(g_szRecvBuffPoll, &g_systemStack);
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
      }
    }

    g_statDebug.initialRun = statInitialRun;
    g_statDebug.inProgress = false;
  }
}

#if ENABLE_MQTT
  MQTTHandler::publishIfConnected();
#endif

  roamIfNeeded(WIFI_SSID, WIFI_PASS);
}
