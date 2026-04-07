#include "WebUI.h"
#include "PylonLink.h"
#include "circular_log.h"
#include <ArduinoJson.h>
#include "Config.h"
#include <string.h>
#include <Arduino.h>

static WebServer*          s_server = nullptr;
static BatteryLink*        s_link   = nullptr;
static batteryStack*       s_stack  = nullptr;
static systemData*         s_system = nullptr;
static dailyEnergyData*    s_energy = nullptr;
static statDebugData*      s_statDbg = nullptr;
static char*               s_rawBuf = nullptr;
static size_t              s_rawLen = 0;
static circular_log<7000>* s_log    = nullptr;

static bool startsWithIgnoreCase(const char* text, const char* prefix) {
  if (!text || !prefix) return false;
  while (*prefix) {
    if (!*text) return false;
    if (tolower((unsigned char)*text) != tolower((unsigned char)*prefix)) return false;
    ++text;
    ++prefix;
  }
  return true;
}

static bool equalsIgnoreCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static bool commandNeedsPrompt(const char* cmd) {
  if (!cmd) return false;

  return equalsIgnoreCase(cmd, "help") ||
         startsWithIgnoreCase(cmd, "stat") ||
         startsWithIgnoreCase(cmd, "log") ||
         startsWithIgnoreCase(cmd, "bat") ||
         startsWithIgnoreCase(cmd, "data") ||
         startsWithIgnoreCase(cmd, "datalist");
}

static void sendJsonDocument(JsonDocument& doc) {
  if (!s_server) return;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  s_server->sendHeader("Cache-Control", "no-store");
  s_server->send(200, "application/json", out);
}

static void sendJsonStack() {
  if (!s_server) return;

  StaticJsonDocument<4096> doc;

  if (s_stack) {
    doc["valid"]         = s_stack->valid;
    doc["lastUpdateMs"]  = s_stack->lastUpdateMs;
    doc["soc"]           = s_stack->soc;
    doc["state"]         = s_stack->baseState;
    doc["count"]         = s_stack->batteryCount;
    doc["voltage_V"]     = (float)s_stack->avgVoltage / 1000.0f;
    doc["current_A"]     = (float)s_stack->currentDC / 1000.0f;
    doc["current_mA"]    = s_stack->currentDC;
    doc["temp_c"]        = (float)s_stack->temp / 1000.0f;
    doc["dc_W"]          = (long)s_stack->getPowerDC();
    doc["ac_W_est"]      = (long)s_stack->getEstPowerAc();
    doc["isNormal"]      = s_stack->isNormal();
    if (s_energy) {
      doc["chargeKWhToday"]    = s_energy->chargeKWhToday;
      doc["dischargeKWhToday"] = s_energy->dischargeKWhToday;
      doc["energyTimeSynced"]  = s_energy->timeSynced;
      doc["currentEpoch"]      = s_energy->currentEpoch;
    }

    // Legacy-/Kompatibilitätsfelder
    doc["batteryCount"]  = s_stack->batteryCount;
    doc["baseState"]     = s_stack->baseState;
    doc["avgVoltage"]    = s_stack->avgVoltage;
    doc["currentDC"]     = s_stack->currentDC;

    JsonArray batts = doc.createNestedArray("batts");
    for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
      const pylonBattery& b = s_stack->batts[i];
      if (!b.isPresent) continue;

      JsonObject nb = batts.createNestedObject();
      nb["idx"]           = i + 1;
      nb["isPresent"]     = true;
      nb["soc"]           = b.soc;
      nb["voltage"]       = b.voltage;
      nb["current"]       = b.current;
      nb["tempr"]         = b.tempr;
      nb["cellVoltLow"]   = b.cellVoltLow;
      nb["cellVoltHigh"]  = b.cellVoltHigh;
      nb["cellTempLow"]   = b.cellTempLow;
      nb["cellTempHigh"]  = b.cellTempHigh;
      nb["baseState"]     = (b.baseState[0] ? b.baseState : "Unknown");
      nb["voltage_V"]     = (float)b.voltage / 1000.0f;
      nb["current_A"]     = (float)b.current / 1000.0f;
      nb["temp_c"]        = (float)b.tempr / 1000.0f;
      nb["isNormal"]      = b.isNormal();
      nb["cycleTimes"]    = b.cycleTimes;
      nb["alarmText"]     = (b.alarmText[0] ? b.alarmText : "Normal");
    }
  }

  sendJsonDocument(doc);
}

static void sendJsonSystem() {
  if (!s_server) return;

  StaticJsonDocument<2048> doc;

  if (s_system) {
    doc["valid"]        = s_system->valid;
    doc["lastUpdateMs"] = s_system->lastUpdateMs;
    doc["soc"]          = s_system->soc;
    doc["soh"]          = s_system->soh;
    doc["voltage"]      = s_system->voltage;
    doc["current"]      = s_system->current;
    doc["rc"]           = s_system->rc;
    doc["fcc"]          = s_system->fcc;
    doc["temp_avg"]     = s_system->temp_avg;
    doc["temp_low"]     = s_system->temp_low;
    doc["temp_high"]    = s_system->temp_high;
    doc["volt_avg"]     = s_system->volt_avg;
    doc["volt_low"]     = s_system->volt_low;
    doc["volt_high"]    = s_system->volt_high;
    doc["state"]        = s_system->state;
    doc["alarmState"]   = s_system->alarmState;

    doc["voltage_V"]    = (float)s_system->voltage / 1000.0f;
    doc["current_A"]    = (float)s_system->current / 1000.0f;
    doc["temp_avg_c"]   = (float)s_system->temp_avg / 1000.0f;
    doc["temp_low_c"]   = (float)s_system->temp_low / 1000.0f;
    doc["temp_high_c"]  = (float)s_system->temp_high / 1000.0f;
    doc["volt_avg_V"]   = (float)s_system->volt_avg / 1000.0f;
    doc["volt_low_V"]   = (float)s_system->volt_low / 1000.0f;
    doc["volt_high_V"]  = (float)s_system->volt_high / 1000.0f;

    doc["rec_chg_voltage"]      = s_system->rec_chg_voltage;
    doc["rec_dsg_voltage"]      = s_system->rec_dsg_voltage;
    doc["rec_chg_current"]      = s_system->rec_chg_current;
    doc["rec_dsg_current"]      = s_system->rec_dsg_current;

    doc["sys_rec_chg_voltage"]  = s_system->sys_rec_chg_voltage;
    doc["sys_rec_dsg_voltage"]  = s_system->sys_rec_dsg_voltage;
    doc["sys_rec_chg_current"]  = s_system->sys_rec_chg_current;
    doc["sys_rec_dsg_current"]  = s_system->sys_rec_dsg_current;

    doc["rec_chg_voltage_V"]    = (float)s_system->rec_chg_voltage / 1000.0f;
    doc["rec_dsg_voltage_V"]    = (float)s_system->rec_dsg_voltage / 1000.0f;
    doc["rec_chg_current_A"]    = (float)s_system->rec_chg_current / 1000.0f;
    doc["rec_dsg_current_A"]    = (float)s_system->rec_dsg_current / 1000.0f;

    doc["sys_rec_chg_voltage_V"] = (float)s_system->sys_rec_chg_voltage / 1000.0f;
    doc["sys_rec_dsg_voltage_V"] = (float)s_system->sys_rec_dsg_voltage / 1000.0f;
    doc["sys_rec_chg_current_A"] = (float)s_system->sys_rec_chg_current / 1000.0f;
    doc["sys_rec_dsg_current_A"] = (float)s_system->sys_rec_dsg_current / 1000.0f;
  }

  sendJsonDocument(doc);
}

static void sendJsonStatus() {
  if (!s_server) return;

  StaticJsonDocument<6144> doc;

  JsonObject meta = doc.createNestedObject("meta");
  meta["uptimeMs"] = millis();

  JsonObject stack = doc.createNestedObject("stack");
  if (s_stack) {
    stack["valid"]         = s_stack->valid;
    stack["lastUpdateMs"]  = s_stack->lastUpdateMs;
    stack["soc"]           = s_stack->soc;
    stack["batteryCount"]  = s_stack->batteryCount;
    stack["baseState"]     = s_stack->baseState;
    stack["avgVoltage"]    = s_stack->avgVoltage;
    stack["currentDC"]     = s_stack->currentDC;
    stack["temp"]          = s_stack->temp;
    stack["avgVoltage_V"]  = (float)s_stack->avgVoltage / 1000.0f;
    stack["currentDC_A"]   = (float)s_stack->currentDC / 1000.0f;
    stack["temp_c"]        = (float)s_stack->temp / 1000.0f;
    stack["isNormal"]      = s_stack->isNormal();

    JsonArray batts = stack.createNestedArray("batts");
    for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
      const pylonBattery& b = s_stack->batts[i];
      if (!b.isPresent) continue;

      JsonObject nb = batts.createNestedObject();
      nb["idx"]          = i + 1;
      nb["soc"]          = b.soc;
      nb["voltage"]      = b.voltage;
      nb["current"]      = b.current;
      nb["tempr"]        = b.tempr;
      nb["cellVoltLow"]  = b.cellVoltLow;
      nb["cellVoltHigh"] = b.cellVoltHigh;
      nb["cellTempLow"]  = b.cellTempLow;
      nb["cellTempHigh"] = b.cellTempHigh;
      nb["baseState"]    = (b.baseState[0] ? b.baseState : "Unknown");
      nb["cycleTimes"]   = b.cycleTimes;
      nb["alarmText"]    = (b.alarmText[0] ? b.alarmText : "Normal");
    }
  }

  JsonObject system = doc.createNestedObject("system");
  if (s_system) {
    system["valid"]         = s_system->valid;
    system["lastUpdateMs"]  = s_system->lastUpdateMs;
    system["soc"]           = s_system->soc;
    system["soh"]           = s_system->soh;
    system["voltage"]       = s_system->voltage;
    system["current"]       = s_system->current;
    system["rc"]            = s_system->rc;
    system["fcc"]           = s_system->fcc;
    system["temp_avg"]      = s_system->temp_avg;
    system["temp_low"]      = s_system->temp_low;
    system["temp_high"]     = s_system->temp_high;
    system["volt_avg"]      = s_system->volt_avg;
    system["volt_low"]      = s_system->volt_low;
    system["volt_high"]     = s_system->volt_high;
    system["state"]         = s_system->state;
    system["alarmState"]    = s_system->alarmState;
    system["voltage_V"]     = (float)s_system->voltage / 1000.0f;
    system["current_A"]     = (float)s_system->current / 1000.0f;

    system["rec_chg_voltage"]      = s_system->rec_chg_voltage;
    system["rec_dsg_voltage"]      = s_system->rec_dsg_voltage;
    system["rec_chg_current"]      = s_system->rec_chg_current;
    system["rec_dsg_current"]      = s_system->rec_dsg_current;

    system["sys_rec_chg_voltage"]  = s_system->sys_rec_chg_voltage;
    system["sys_rec_dsg_voltage"]  = s_system->sys_rec_dsg_voltage;
    system["sys_rec_chg_current"]  = s_system->sys_rec_chg_current;
    system["sys_rec_dsg_current"]  = s_system->sys_rec_dsg_current;

    system["rec_chg_voltage_V"]    = (float)s_system->rec_chg_voltage / 1000.0f;
    system["rec_dsg_voltage_V"]    = (float)s_system->rec_dsg_voltage / 1000.0f;
    system["rec_chg_current_A"]    = (float)s_system->rec_chg_current / 1000.0f;
    system["rec_dsg_current_A"]    = (float)s_system->rec_dsg_current / 1000.0f;

    system["sys_rec_chg_voltage_V"] = (float)s_system->sys_rec_chg_voltage / 1000.0f;
    system["sys_rec_dsg_voltage_V"] = (float)s_system->sys_rec_dsg_voltage / 1000.0f;
    system["sys_rec_chg_current_A"] = (float)s_system->sys_rec_chg_current / 1000.0f;
    system["sys_rec_dsg_current_A"] = (float)s_system->sys_rec_dsg_current / 1000.0f;
  }

  JsonObject derived = doc.createNestedObject("derived");
  if (s_stack && s_stack->valid) {
    derived["dc_W"]     = s_stack->getPowerDC();
    derived["ac_W_est"] = s_stack->getEstPowerAc();
  }

  JsonObject energy = doc.createNestedObject("energy");
  if (s_energy) {
    energy["valid"] = s_energy->valid;
    energy["timeSynced"] = s_energy->timeSynced;
    energy["lastUpdateMs"] = s_energy->lastUpdateMs;
    energy["currentEpoch"] = s_energy->currentEpoch;
    energy["localDayNumber"] = s_energy->localDayNumber;
    energy["chargeKWhToday"] = s_energy->chargeKWhToday;
    energy["dischargeKWhToday"] = s_energy->dischargeKWhToday;
  }

  sendJsonDocument(doc);
}

static void sendJsonStatDebug() {
  if (!s_server) return;

  StaticJsonDocument<1024> doc;
  if (s_statDbg) {
    doc["currentIdx"] = s_statDbg->currentIdx;
    doc["maxBat"] = s_statDbg->maxBat;
    doc["detected"] = s_statDbg->detected;
    doc["highestPresentIdx"] = s_statDbg->highestPresentIdx;
    doc["initialRun"] = s_statDbg->initialRun;
    doc["inProgress"] = s_statDbg->inProgress;
    doc["lastSuccess"] = s_statDbg->lastSuccess;
    doc["lastTimedOut"] = s_statDbg->lastTimedOut;
    doc["lastParseFailed"] = s_statDbg->lastParseFailed;
    doc["lastCycleTimes"] = s_statDbg->lastCycleTimes;
    doc["lastAttemptMs"] = s_statDbg->lastAttemptMs;
    doc["lastSuccessMs"] = s_statDbg->lastSuccessMs;
    doc["lastCommand"] = s_statDbg->lastCommand;
    doc["lastMessage"] = s_statDbg->lastMessage;
  }

  sendJsonDocument(doc);
}

void WebUI::init(WebServer* server,
                 BatteryLink* link,
                 batteryStack* stk,
                 systemData* sys,
                 dailyEnergyData* energy,
                 statDebugData* statDbg,
                 char* rawBuf,
                 size_t rawBufLen,
                 circular_log<7000>* clog)
{
  s_server = server;
  s_link   = link;
  s_stack  = stk;
  s_system = sys;
  s_energy = energy;
  s_statDbg = statDbg;
  s_rawBuf = rawBuf;
  s_rawLen = rawBufLen;
  s_log    = clog;

  s_server->on("/log", HTTP_GET, []() {
    if (s_log) s_server->send(200, "text/html", s_log->c_str());
    else       s_server->send(200, "text/plain", "(no log)");
  });

  s_server->on("/api/stack", HTTP_GET, []() {
    sendJsonStack();
  });

  s_server->on("/api/system", HTTP_GET, []() {
    sendJsonSystem();
  });

  s_server->on("/api/status", HTTP_GET, []() {
    sendJsonStatus();
  });

  s_server->on("/api/stat_debug", HTTP_GET, []() {
    sendJsonStatDebug();
  });

  s_server->on("/api/restart", HTTP_POST, []() {
    if (s_log) s_log->Log("HTTP: restart requested");
    s_server->send(200, "text/plain", "restarting");
    s_server->client().stop();
    delay(300);
    ESP.restart();
  });

  s_server->on("/api/cmd", HTTP_POST, []() {
    if (!s_link) {
      s_server->send(503, "text/plain", "no link");
      return;
    }
    if (!s_rawBuf || s_rawLen == 0) {
      s_server->send(500, "text/plain", "raw buffer not configured");
      return;
    }

    String code;
    if (s_server->hasArg("plain")) code = s_server->arg("plain");
    else if (s_server->hasArg("code")) code = s_server->arg("code");
    else if (s_server->args() >= 1) code = s_server->arg(0);

    code.trim();
    if (!code.length()) {
      s_server->send(400, "text/plain", "empty");
      return;
    }

    if (s_log) {
      String s = "CMD: " + code;
      s_log->Log(s.c_str());
    }

    memset(s_rawBuf, 0, s_rawLen);

    bool ok = false;
    if (commandNeedsPrompt(code.c_str())) {
      ok = s_link->sendAndReceivePrompt(code.c_str(), s_rawBuf, s_rawLen, 20000);
    } else {
      ok = s_link->sendAndReceive(code.c_str(), s_rawBuf, s_rawLen, 8000);
    }

    if (!ok) {
      s_server->send(504, "text/plain", "timeout");
      return;
    }

    s_server->sendHeader("Cache-Control", "no-store");
    s_server->send(200, "text/plain", s_rawBuf);
  });

  s_server->on("/cmd", HTTP_GET, []() {
    if (!s_link) {
      s_server->send(503, "text/plain", "no link");
      return;
    }
    if (!s_rawBuf || s_rawLen == 0) {
      s_server->send(500, "text/plain", "raw buffer not configured");
      return;
    }

    String code = s_server->arg("code");
    if (!code.length()) {
      s_server->send(400, "text/plain", "missing code");
      return;
    }

    if (s_log) {
      String s = "CMD(GET): " + code;
      s_log->Log(s.c_str());
    }

    memset(s_rawBuf, 0, s_rawLen);

    bool ok = s_link->sendAndReceivePrompt(code.c_str(), s_rawBuf, s_rawLen, 15000);
    if (!ok) {
      s_server->send(504, "text/plain", "timeout");
      return;
    }

    char* p = strstr(s_rawBuf, "pylon_debug>");
    if (!p) p = strstr(s_rawBuf, "pylon>");
    if (p) *p = '\0';

    s_server->sendHeader("Cache-Control", "no-store");
    s_server->send(200, "text/plain", s_rawBuf);
  });
}
