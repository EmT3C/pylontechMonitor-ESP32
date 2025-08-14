// WebUI.cpp (oben bei den Includes)
#include "WebUI.h"
#include "PylonLink.h"        // <— jetzt hier einbinden
#include "circular_log.h"     // <— und hier
#include <ArduinoJson.h>
#include "Config.h"
#include <string.h>   // für memset

// ---------- Datei-lokale Zeiger (kein Namenskonflikt mit euren Globals) ----------
static WebServer*            s_server = nullptr;
static BatteryLink*          s_link   = nullptr;
static batteryStack*         s_stack  = nullptr;
static systemData*           s_system = nullptr;
static char*                 s_rawBuf = nullptr;
static size_t                s_rawLen = 0;       // <— NEU: Länge merken
static circular_log<7000>*   s_log    = nullptr;
// ---------- Helfer: /api/stack JSON ----------
static void sendJsonStack() {
  if (!s_server) return;

  StaticJsonDocument<4096> doc;

  if (s_stack) {
    // Neues, komfortables Schema (SI-Einheiten)
    doc["soc"]        = s_stack->soc;
    doc["state"]      = s_stack->baseState;
    doc["count"]      = s_stack->batteryCount;
    doc["voltage_V"]  = (float)s_stack->avgVoltage / 1000.0f;
    doc["current_A"]  = (float)s_stack->currentDC  / 1000.0f;
    doc["current_mA"] = s_stack->currentDC;
    doc["temp_c"]     = (float)s_stack->temp       / 1000.0f;
    doc["dc_W"]       = (long) s_stack->getPowerDC();
    doc["ac_W_est"]   = (long) s_stack->getEstPowerAc();

    // **Legacy-Keys für deine index.html**
    doc["batteryCount"] = s_stack->batteryCount;   // alt
    doc["baseState"]    = s_stack->baseState;      // alt
    doc["avgVoltage"]   = s_stack->avgVoltage;     // alt (mV)
    doc["currentDC"]    = s_stack->currentDC;      // alt (mA)

    JsonArray batts = doc.createNestedArray("batts");
    for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
      const pylonBattery& b = s_stack->batts[i];
      if (!b.isPresent) continue;

      JsonObject nb = batts.createNestedObject();
      nb["idx"]          = i + 1;
      nb["isPresent"]    = true;
      nb["soc"]          = b.soc;

      // Legacy (so erwartet es deine index.html)
      nb["voltage"]      = b.voltage;        // mV
      nb["current"]      = b.current;        // mA
      nb["tempr"]        = b.tempr;          // m°C
      nb["cellVoltLow"]  = b.cellVoltLow;    // mV
      nb["cellVoltHigh"] = b.cellVoltHigh;   // mV
      nb["cellTempLow"]  = b.cellTempLow;    // m°C
      nb["cellTempHigh"] = b.cellTempHigh;   // m°C
      nb["baseState"]    = (b.baseState[0] ? b.baseState : "Unknown");

      // Neues Schema parallel
      nb["voltage_V"]    = (float)b.voltage / 1000.0f;
      nb["current_A"]    = (float)b.current / 1000.0f;
      nb["temp_c"]       = (float)b.tempr   / 1000.0f;
    }
  }

  String out;
  serializeJson(doc, out);
  s_server->sendHeader("Cache-Control", "no-store");
  s_server->send(200, "application/json", out);
}

// ---------- Helfer: /api/system JSON ----------
static void sendJsonSystem() {
  if (!s_server) return;

  StaticJsonDocument<1024> doc;
  if (s_system) {
    doc["soc"]       = s_system->soc;
    doc["soh"]       = s_system->soh;
    doc["voltage"]   = s_system->voltage;     // mV
    doc["current"]   = s_system->current;     // mA
    doc["rc"]        = s_system->rc;          // mAh
    doc["fcc"]       = s_system->fcc;         // mAh
    doc["temp_avg"]  = s_system->temp_avg;    // m°C
    doc["temp_low"]  = s_system->temp_low;    // m°C
    doc["temp_high"] = s_system->temp_high;   // m°C
    doc["volt_avg"]  = s_system->volt_avg;    // mV
    doc["volt_low"]  = s_system->volt_low;    // mV
    doc["volt_high"] = s_system->volt_high;   // mV
  }
  String out; serializeJson(doc, out);
  s_server->sendHeader("Cache-Control", "no-store");
  s_server->send(200, "application/json", out);
}

// ---------- Init & Routen ----------
void WebUI::init(WebServer* server,
                 BatteryLink* link,
                 batteryStack* stk,
                 systemData* sys,
                 char* rawBuf,
                 size_t rawBufLen,
                 circular_log<7000>* clog)
{
  s_server = server;
  s_link   = link;
  s_stack  = stk;
  s_system = sys;
  s_rawBuf = rawBuf;
  s_rawLen = rawBufLen;
  s_log    = clog;

  // /log
  s_server->on("/log", HTTP_GET, [](){
    if (s_log) s_server->send(200, "text/html", s_log->c_str());
    else       s_server->send(200, "text/plain", "(no log)");
  });

  // /api/stack
  s_server->on("/api/stack", HTTP_GET, [](){
    sendJsonStack();
  });

  // /api/system
  s_server->on("/api/system", HTTP_GET, [](){
    sendJsonSystem();
  });

  // POST /api/cmd
  // Web-Konsole: POST /api/cmd (Body=Text)
  s_server->on("/api/cmd", HTTP_POST, [](){
  if (!s_link) { s_server->send(503, "text/plain", "no link"); return; }

  String code;
  if (s_server->hasArg("plain")) code = s_server->arg("plain");
  else if (s_server->hasArg("code")) code = s_server->arg("code");
  else if (s_server->args() >= 1) code = s_server->arg(0);
  code.trim();
  if (!code.length()) { s_server->send(400, "text/plain", "empty"); return; }

  if (s_log) { String s = "CMD: " + code; s_log->Log(s.c_str()); }

  memset(s_rawBuf, 0, s_rawLen);
  bool ok = false;

  // „help“ (und evtl. andere lange Befehle) -> bis "pylon>" warten, längeres Timeout
  if (code.equalsIgnoreCase("help")) {
    ok = s_link->sendAndReceivePrompt(code.c_str(), s_rawBuf, s_rawLen, 20000);
  } else {
    ok = s_link->sendAndReceive(code.c_str(), s_rawBuf, s_rawLen, 8000);
  }

  if (!ok) { s_server->send(504, "text/plain", "timeout"); return; }
  s_server->sendHeader("Cache-Control", "no-store");
  s_server->send(200, "text/plain", s_rawBuf);
});


  // Fallback: GET /cmd?code=...
  s_server->on("/cmd", HTTP_GET, [](){
    if (!s_link) { s_server->send(503, "text/plain", "no link"); return; }
    String code = s_server->arg("code");
    if (s_log) { String s = "CMD(GET): " + code; s_log->Log(s.c_str()); }
    if (!code.length()) { s_server->send(400, "text/plain", "missing code"); return; }

   memset(s_rawBuf, 0, s_rawLen);
   bool ok = s_link->sendAndReceivePrompt(code.c_str(), s_rawBuf, s_rawLen, 15000);
    if (!ok) { s_server->send(504, "text/plain", "timeout"); return; }

    char* p = strstr(s_rawBuf, "pylon>");
    if (p) *p = '\0';

    s_server->sendHeader("Cache-Control", "no-store");
    s_server->send(200, "text/plain", s_rawBuf);
  });

}
