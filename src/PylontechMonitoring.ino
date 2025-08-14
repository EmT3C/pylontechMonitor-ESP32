// PylontechMonitoring.ino — ESP32 + UART2 + Web + OTA + mDNS + MQTT
// inkl. Mesh-AP-Auswahl (BSSID) + optionalem Roaming

#include "Config.h"              // MUSS vor Timerlibs/Makros kommen
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <circular_log.h>
#include <esp_wifi.h>            // für esp_wifi_set_ps()
#include <LittleFS.h>

#include "batteryStack.h"
batteryStack g_stack{};
systemData   g_systemStack{};

#include "PylonLink.h"           // BatteryLink (UART2)
#include "Parser.h"              // Parser::parsePwr / parsePwrsys
#include "WebUI.h"               // WebUI::init(server, link, stack, system, rawBuf, log)
#include "MQTTHandler.h"         // Klasse MQTTHandler (mit publishIfConnected etc.)

// -----------------------------------------------------------------------------
// Globale Objekte

WebServer server(80);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, GMT_OFFSET_SEC);

circular_log<7000> g_log;
char g_szRecvBuff[16384];

// UART2: Pins aus Config.h
BatteryLink batt(Serial2, PIN_RX2, PIN_TX2);

#ifdef ENABLE_MQTT
  WiFiClient   espClient;
  PubSubClient mqttClient(espClient);
#endif

// -----------------------------------------------------------------------------
// Mesh-AP-Auswahl + Roaming

static uint8_t g_targetBSSID[6] = {0};

bool connectToBestAP(const char* ssid, const char* pass) {
  Serial.println(F("Scanning for best AP..."));
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
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

  // WICHTIG: begin mit Kanal + BSSID => bindet an *diesen* AP
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
  if (millis() - lastCheck < 7000) return;  // alle 7 s prüfen
  lastCheck = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  int curRSSI = WiFi.RSSI();
  if (curRSSI > -67) return; // gut genug, nicht roamen

  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) return;

  int bestRSSI = curRSSI;
  int bestChan = WiFi.channel();
  uint8_t bestBSSID[6]; memcpy(bestBSSID, g_targetBSSID, 6);
  bool foundBetter = false;

  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != ssid) continue;
    int r = WiFi.RSSI(i);
    const uint8_t* b = WiFi.BSSID(i);

    // Hysterese: nur wechseln, wenn mind. 8 dB besser und anderes BSSID
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
  // WiFi-Grundkonfig
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.setSleep(false);               // geringere Latenz
  esp_wifi_set_ps(WIFI_PS_NONE);      // doppelt hält besser
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // Statische IP (falls in Config.h definiert)
  #ifdef USE_STATIC_IP
    #ifndef IP_ADDRESS
      #error "USE_STATIC_IP ist gesetzt, aber IP_ADDRESS fehlt in Config.h"
    #endif
    WiFi.config(IP_ADDRESS, IP_GATEWAY, IP_SUBNET, IP_DNS1, IP_DNS2);
  #endif

  // Mit bestem AP verbinden (BSSID-basiert)
  connectToBestAP(WIFI_SSID, WIFI_PASS);

  // OTA
  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA
    .onStart([](){ Serial.println("OTA start"); })
    .onEnd([](){ Serial.println("\nOTA end"); })
    .onProgress([](unsigned int p, unsigned int t){
      Serial.printf("OTA %u%%\r", (p*100)/t);
    })
    .onError([](ota_error_t e){ Serial.printf("OTA Error[%u]\n", e); });
  ArduinoOTA.begin();
  Serial.printf("OTA ready at %s (%s)\n",
                WIFI_HOSTNAME, WiFi.localIP().toString().c_str());

  // mDNS
  if (MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("arduino", "tcp", 3232);
  }

  // NTP
  timeClient.begin();

  // UART2 zur Batterie
  batt.begin(DEFAULT_BAUD);

  // WebUI-Routen
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
  server.on("/health", []() { server.send(200, "text/plain", "OK"); });
  server.on("/diag", [](){
    String s = "RSSI=" + String(WiFi.RSSI()) +
               " dBm\nSleep=" + String(WiFi.getSleep() ? "on" : "off") +
               "\nHeap=" + String(ESP.getFreeHeap());
    server.send(200, "text/plain", s);
  });
  server.on("/log", [](){ server.send(200, "text/html", g_log.c_str()); });

  WebUI::init(&server, &batt, &g_stack, &g_systemStack,
            g_szRecvBuff,
            sizeof(g_szRecvBuff),
            &g_log);
  server.begin();
  Serial.println("HTTP server started");

#ifdef ENABLE_MQTT
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(1024);              // Discovery-JSON etwas größer
  MQTTHandler::init(&mqttClient, &g_stack, &g_systemStack);
#endif
}

// -----------------------------------------------------------------------------
// Loop

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  timeClient.update();

#ifdef ENABLE_MQTT
  // kümmert sich um Reconnect + Discovery bei (Re-)Connect
  MQTTHandler::loop();
#endif

  // Batterie pollen (leichtes Rate-Limit via millis)
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll >= 3000) {
    lastPoll = millis();

    if (batt.sendAndReceive("pwr", g_szRecvBuff, sizeof(g_szRecvBuff), 4000)) {
      if (&g_log) g_log.Log("got PWR");
      Parser::parsePwr(g_szRecvBuff, &g_stack);

      // optional: nur die L1-Zeile in Log mitschreiben
      const char* p1 = strstr(g_szRecvBuff, "\r\r\n1     ");
      if (p1) {
        char lbuf[160];
        strncpy(lbuf, p1+3, sizeof(lbuf)-1); lbuf[sizeof(lbuf)-1]=0;
        char* e = strchr(lbuf, '\n'); if (e) *e = 0;
        g_log.Log("PWR L1:");
        g_log.Log(lbuf);
      }
    }

    static uint8_t n = 0;
    if ((++n % 5) == 0) {
      if (batt.sendAndReceive("pwrsys", g_szRecvBuff, sizeof(g_szRecvBuff), 6000)) {
        Parser::parsePwrsys(g_szRecvBuff, &g_systemStack);
      }
    }
  }

#ifdef ENABLE_MQTT
  // Daten publizieren (intern ~alle 3 s)
  MQTTHandler::publishIfConnected();
#endif

  // Optionales Roaming, falls aktueller AP schwach ist
  roamIfNeeded(WIFI_SSID, WIFI_PASS);
}

