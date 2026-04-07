#include "MQTTHandler.h"
#include "Config.h"
#include <ArduinoJson.h>
#include <string.h>

PubSubClient* MQTTHandler::s_client        = nullptr;
batteryStack* MQTTHandler::s_stack         = nullptr;
systemData*   MQTTHandler::s_system        = nullptr;
dailyEnergyData* MQTTHandler::s_energy     = nullptr;
unsigned long MQTTHandler::s_lastPublishMs = 0;
unsigned long MQTTHandler::s_lastAvailMs   = 0;

void MQTTHandler::init(PubSubClient* client, batteryStack* stack, systemData* system, dailyEnergyData* energy) {
  s_client = client;
  s_stack  = stack;
  s_system = system;
  s_energy = energy;

  if (s_client) {
    s_client->setKeepAlive(45);
    s_client->setSocketTimeout(10);
    s_client->setBufferSize(1024);
  }
}

void MQTTHandler::connectIfNeeded() {
  if (!s_client) return;
  if (s_client->connected()) return;

  bool ok = false;
#if defined(MQTT_USER) && defined(MQTT_PASSWORD)
  ok = s_client->connect(
        WIFI_HOSTNAME,
        MQTT_USER, MQTT_PASSWORD,
        MQTT_TOPIC_ROOT "availability",
        1, true,
        "offline"
      );
#else
  ok = s_client->connect(
        WIFI_HOSTNAME,
        MQTT_TOPIC_ROOT "availability",
        1, true,
        "offline"
      );
#endif

  if (ok) {
    s_client->publish(MQTT_TOPIC_ROOT "availability", "online", true);
    s_lastAvailMs = millis();
    publishDiscovery();
  }
}

void MQTTHandler::heartbeatAvailability() {
  if (!s_client || !s_client->connected()) return;
  const unsigned long now = millis();
  if (now - s_lastAvailMs >= 60000UL) {
    s_lastAvailMs = now;
    s_client->publish(MQTT_TOPIC_ROOT "availability", "online", true);
  }
}

void MQTTHandler::loop() {
  connectIfNeeded();
  if (s_client) {
    s_client->loop();
    heartbeatAvailability();
  }
}

void MQTTHandler::publishIfConnected() {
  if (!s_client) return;
  s_client->loop();
  if (!s_client->connected()) return;

  const unsigned long now = millis();
  const unsigned long intervalMs = 2000;
  if (now - s_lastPublishMs < intervalMs) return;
  s_lastPublishMs = now;

  publishData();

  long stackCellDeltaMax = 0;

  for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
    const pylonBattery& b = s_stack->batts[i];
    if (!b.isPresent) continue;

    char topic[96], payload[32];

    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/soc", i + 1);
    snprintf(payload, sizeof(payload), "%ld", b.soc);
    s_client->publish(topic, payload, true);

    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/voltage", i + 1);
    snprintf(payload, sizeof(payload), "%.3f", b.voltage / 1000.0);
    s_client->publish(topic, payload, true);

    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/current", i + 1);
    snprintf(payload, sizeof(payload), "%.3f", b.current / 1000.0);
    s_client->publish(topic, payload, true);

    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/cycle_times", i + 1);
    snprintf(payload, sizeof(payload), "%ld", b.cycleTimes);
    s_client->publish(topic, payload, true);

    long delta = 0;
    if (b.cellVoltHigh > 0 && b.cellVoltLow > 0) {
      delta = b.cellVoltHigh - b.cellVoltLow;
    }

    if (delta > stackCellDeltaMax) {
      stackCellDeltaMax = delta;
    }

    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/cell_delta", i + 1);
    snprintf(payload, sizeof(payload), "%ld", delta);
    s_client->publish(topic, payload, true);

    {
      double w = (b.voltage / 1000.0) * (b.current / 1000.0);
      snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/power", i + 1);
      snprintf(payload, sizeof(payload), "%.0f", w);
      s_client->publish(topic, payload, true);
    }

    const char* st =
      b.isAlarm()       ? "Alarm"   :
      b.isProtect()     ? "Protect" :
      b.isCharging()    ? "Charge"  :
      b.isDischarging() ? "Dischg"  :
      b.isIdle()        ? "Idle"    :
      b.isBalancing()   ? "Balance" :
                          "Unknown";

    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/state", i + 1);
    s_client->publish(topic, st, true);

    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/alarm_text", i + 1);
    s_client->publish(topic, b.alarmText[0] ? b.alarmText : "Normal", true);
  }

  char maxBuf[32];
  snprintf(maxBuf, sizeof(maxBuf), "%ld", stackCellDeltaMax);
  s_client->publish(MQTT_TOPIC_ROOT "cell_delta_max", maxBuf, true);
}

void MQTTHandler::publishDiscovery() {
  if (!s_client) return;

  const String node = WIFI_HOSTNAME;

  auto pub_cfg = [&](const String& object_id,
                     const String& name,
                     const String& state_topic,
                     const char* unit = nullptr,
                     const char* device_class = nullptr,
                     bool with_state_class = true,
                     const char* entity_category = nullptr,
                     const char* icon = nullptr)
  {
    StaticJsonDocument<512> doc;
    doc["name"]                  = name;
    doc["state_topic"]           = state_topic;
    doc["unique_id"]             = node + "_" + object_id;
    doc["availability_topic"]    = String(MQTT_TOPIC_ROOT) + "availability";
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";

    if (with_state_class && (unit || device_class)) {
      doc["state_class"] = "measurement";
    }
    if (unit)         doc["unit_of_measurement"] = unit;
    if (device_class) doc["device_class"]        = device_class;
    if (entity_category) doc["entity_category"]  = entity_category;
    if (icon)         doc["icon"]                = icon;

    JsonObject dev = doc.createNestedObject("device");
    JsonArray ids  = dev.createNestedArray("identifiers");
    ids.add(node);
    dev["manufacturer"] = "Pylontech";
    dev["model"]        = "Battery Monitor";
    dev["name"]         = node;

    char payload[512];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    String topic = String(HA_DISCOVERY_SENSOR_PREFIX) + node + "/" + object_id + "/config";
    s_client->publish(topic.c_str(), (uint8_t*)payload, len, true);
  };

  pub_cfg("soc",
          "Battery SoC",
          String(MQTT_TOPIC_ROOT) + "soc",
          "%",
          "battery",
          true,
          nullptr,
          "mdi:battery-medium");
  pub_cfg("temp",         "Battery Temp",       String(MQTT_TOPIC_ROOT) + "temp",         "°C", "temperature");
  pub_cfg("currentDC",    "Battery Current",    String(MQTT_TOPIC_ROOT) + "currentDC",    "mA", "current");
  pub_cfg("avgVoltage",   "Battery Voltage",    String(MQTT_TOPIC_ROOT) + "avgVoltage",   "V",  "voltage");
  pub_cfg("dc_power",
          "Battery DC Power",
          String(MQTT_TOPIC_ROOT) + "dc_power",
          "W",
          "power",
          true,
          nullptr,
          "mdi:gauge");
  pub_cfg("ac_power_est", "Battery AC Power",   String(MQTT_TOPIC_ROOT) + "ac_power_est", "W",  "power");
  pub_cfg("charge_kwh_today",
          "Laden heute",
          String(MQTT_TOPIC_ROOT) + "charge_kwh_today",
          "kWh",
          "energy",
          false,
          nullptr,
          "mdi:battery-arrow-up");
  pub_cfg("discharge_kwh_today",
          "Entladen heute",
          String(MQTT_TOPIC_ROOT) + "discharge_kwh_today",
          "kWh",
          "energy",
          false,
          nullptr,
          "mdi:battery-arrow-down");

  pub_cfg("cell_delta_max",
          "Battery Cell Delta Max",
          String(MQTT_TOPIC_ROOT) + "cell_delta_max",
          "mV",
          nullptr,
          false);

  {
    StaticJsonDocument<384> doc;
    doc["name"]                  = "Battery State";
    doc["state_topic"]           = String(MQTT_TOPIC_ROOT) + "base_state";
    doc["unique_id"]             = node + "_base_state";
    doc["availability_topic"]    = String(MQTT_TOPIC_ROOT) + "availability";
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";
    doc["icon"]                  = "mdi:battery-heart-variant";

    JsonObject dev = doc.createNestedObject("device");
    JsonArray ids  = dev.createNestedArray("identifiers");
    ids.add(node);
    dev["manufacturer"] = "Pylontech";
    dev["model"]        = "Battery Monitor";
    dev["name"]         = node;

    char payload[384];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    String topic = String(HA_DISCOVERY_SENSOR_PREFIX) + node + "/base_state/config";
    s_client->publish(topic.c_str(), (uint8_t*)payload, len, true);
  }

  pub_cfg("system_soc",       "System SOC",        String(MQTT_TOPIC_ROOT) + "system_soc",       "%",  "battery");
  pub_cfg("system_soh",       "System SOH",        String(MQTT_TOPIC_ROOT) + "system_soh",       "%",  nullptr);
  pub_cfg("system_voltage",   "System Voltage",    String(MQTT_TOPIC_ROOT) + "system_voltage",   "V",  "voltage");
  pub_cfg("system_current",   "System Current",    String(MQTT_TOPIC_ROOT) + "system_current",   "A",  "current");
  pub_cfg("system_rc",        "System RC",         String(MQTT_TOPIC_ROOT) + "system_rc",        "mAh", nullptr);
  pub_cfg("system_fcc",       "System FCC",        String(MQTT_TOPIC_ROOT) + "system_fcc",       "mAh", nullptr);
  pub_cfg("system_temp_avg",  "System Temp Avg",   String(MQTT_TOPIC_ROOT) + "system_temp_avg",  "°C", "temperature");
  pub_cfg("system_temp_low",  "System Temp Low",   String(MQTT_TOPIC_ROOT) + "system_temp_low",  "°C", "temperature");
  pub_cfg("system_temp_high", "System Temp High",  String(MQTT_TOPIC_ROOT) + "system_temp_high", "°C", "temperature");
  pub_cfg("system_volt_avg",  "System Volt Avg",   String(MQTT_TOPIC_ROOT) + "system_volt_avg",  "V",  "voltage");
  pub_cfg("system_volt_low",  "System Volt Low",   String(MQTT_TOPIC_ROOT) + "system_volt_low",  "V",  "voltage");
  pub_cfg("system_volt_high", "System Volt High",  String(MQTT_TOPIC_ROOT) + "system_volt_high", "V",  "voltage");
  pub_cfg("rec_chg_voltage",     "Recommend Charge Voltage",        String(MQTT_TOPIC_ROOT) + "rec_chg_voltage",     "V",  "voltage");
  pub_cfg("rec_dsg_voltage",     "Recommend Discharge Voltage",     String(MQTT_TOPIC_ROOT) + "rec_dsg_voltage",     "V",  "voltage");
  pub_cfg("rec_chg_current",     "Recommend Charge Current",        String(MQTT_TOPIC_ROOT) + "rec_chg_current",     "A",  "current");
  pub_cfg("rec_dsg_current",     "Recommend Discharge Current",     String(MQTT_TOPIC_ROOT) + "rec_dsg_current",     "A",  "current");

  pub_cfg("sys_rec_chg_voltage", "System Recommend Charge Voltage", String(MQTT_TOPIC_ROOT) + "sys_rec_chg_voltage", "V",  "voltage");
  pub_cfg("sys_rec_dsg_voltage", "System Recommend Discharge Voltage", String(MQTT_TOPIC_ROOT) + "sys_rec_dsg_voltage", "V", "voltage");
  pub_cfg("sys_rec_chg_current", "System Recommend Charge Current", String(MQTT_TOPIC_ROOT) + "sys_rec_chg_current", "A",  "current");
  pub_cfg("sys_rec_dsg_current", "System Recommend Discharge Current", String(MQTT_TOPIC_ROOT) + "sys_rec_dsg_current", "A", "current");
  {
    StaticJsonDocument<384> doc;
    doc["name"]            = "Pylontech Online";
    doc["state_topic"]     = String(MQTT_TOPIC_ROOT) + "availability";
    doc["unique_id"]       = node + "_online";
    doc["device_class"]    = "connectivity";
    doc["payload_on"]      = "online";
    doc["payload_off"]     = "offline";
    doc["entity_category"] = "diagnostic";

    JsonObject dev = doc.createNestedObject("device");
    JsonArray ids  = dev.createNestedArray("identifiers");
    ids.add(node);
    dev["manufacturer"] = "Pylontech";
    dev["model"]        = "Battery Monitor";
    dev["name"]         = node;

    char payload[384];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    String topic = String(HA_DISCOVERY_BINARY_PREFIX) + node + "/online/config";
    s_client->publish(topic.c_str(), (uint8_t*)payload, len, true);
  }

  for (int i = 1; i <= MAX_PYLON_BATTERIES; ++i) {
    const String idp = "b" + String(i);

    pub_cfg(idp + "_soc",      String("Battery ") + i + " SoC",      String(MQTT_TOPIC_ROOT) + i + "/soc",      "%", "battery");
    pub_cfg(idp + "_voltage",  String("Battery ") + i + " Voltage",  String(MQTT_TOPIC_ROOT) + i + "/voltage",  "V", "voltage");
    pub_cfg(idp + "_current",  String("Battery ") + i + " Current",  String(MQTT_TOPIC_ROOT) + i + "/current",  "A", "current");
    pub_cfg(idp + "_power",
            String("Battery ") + i + " Power",
            String(MQTT_TOPIC_ROOT) + i + "/power",
            "W",
            "power",
            true,
            nullptr,
            "mdi:gauge");
    pub_cfg(idp + "_cell_delta", String("Battery ") + i + " Cell Delta", String(MQTT_TOPIC_ROOT) + i + "/cell_delta", "mV", nullptr, false);
    pub_cfg(idp + "_cycle_times", String("Battery ") + i + " Cycle Times", String(MQTT_TOPIC_ROOT) + i + "/cycle_times", nullptr, nullptr, false);
    pub_cfg(idp + "_alarm_text",
            String("Battery ") + i + " Alarm",
            String(MQTT_TOPIC_ROOT) + i + "/alarm_text",
            nullptr,
            nullptr,
            false,
            nullptr,
            "mdi:alert-circle-outline");
    {
      StaticJsonDocument<384> doc;
      doc["name"]                  = String("Battery ") + i + " State";
      doc["state_topic"]           = String(MQTT_TOPIC_ROOT) + i + "/state";
      doc["unique_id"]             = node + "_" + idp + "_state";
      doc["availability_topic"]    = String(MQTT_TOPIC_ROOT) + "availability";
      doc["payload_available"]     = "online";
      doc["payload_not_available"] = "offline";
      doc["icon"]                  = "mdi:battery";

      JsonObject dev = doc.createNestedObject("device");
      JsonArray ids  = dev.createNestedArray("identifiers");
      ids.add(node);
      dev["manufacturer"] = "Pylontech";
      dev["model"]        = "Battery Monitor";
      dev["name"]         = node;

      char payload[384];
      size_t len = serializeJson(doc, payload, sizeof(payload));
      String topic = String(HA_DISCOVERY_SENSOR_PREFIX) + node + "/" + idp + "_state/config";
      s_client->publish(topic.c_str(), (uint8_t*)payload, len, true);
    }
  }
}

void MQTTHandler::publishData() {
  if (!s_client) return;

  char buf[32];

  if (s_energy && s_energy->valid) {
    snprintf(buf, sizeof(buf), "%.3f", s_energy->chargeKWhToday);
    s_client->publish(MQTT_TOPIC_ROOT "charge_kwh_today", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_energy->dischargeKWhToday);
    s_client->publish(MQTT_TOPIC_ROOT "discharge_kwh_today", buf, true);
  }

  if (!s_stack || !s_stack->valid) return;

  snprintf(buf, sizeof(buf), "%d", s_stack->soc);
  s_client->publish(MQTT_TOPIC_ROOT "soc", buf, true);

  snprintf(buf, sizeof(buf), "%.1f", s_stack->temp / 1000.0);
  s_client->publish(MQTT_TOPIC_ROOT "temp", buf, true);

  snprintf(buf, sizeof(buf), "%ld", s_stack->currentDC);
  s_client->publish(MQTT_TOPIC_ROOT "currentDC", buf, true);

  snprintf(buf, sizeof(buf), "%.3f", s_stack->avgVoltage / 1000.0);
  s_client->publish(MQTT_TOPIC_ROOT "avgVoltage", buf, true);

  s_client->publish(MQTT_TOPIC_ROOT "base_state", s_stack->baseState, true);

  double pdc = (s_stack->avgVoltage / 1000.0) * (s_stack->currentDC / 1000.0);
  snprintf(buf, sizeof(buf), "%.0f", pdc);
  s_client->publish(MQTT_TOPIC_ROOT "dc_power", buf, true);

  long pac = s_stack->getEstPowerAc();
  snprintf(buf, sizeof(buf), "%ld", pac);
  s_client->publish(MQTT_TOPIC_ROOT "ac_power_est", buf, true);

  if (s_system && s_system->valid) {
    snprintf(buf, sizeof(buf), "%d", s_system->soc);
    s_client->publish(MQTT_TOPIC_ROOT "system_soc", buf, true);

    snprintf(buf, sizeof(buf), "%d", s_system->soh);
    s_client->publish(MQTT_TOPIC_ROOT "system_soh", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->voltage / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_voltage", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->current / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_current", buf, true);

    snprintf(buf, sizeof(buf), "%ld", s_system->rc);
    s_client->publish(MQTT_TOPIC_ROOT "system_rc", buf, true);

    snprintf(buf, sizeof(buf), "%ld", s_system->fcc);
    s_client->publish(MQTT_TOPIC_ROOT "system_fcc", buf, true);

    snprintf(buf, sizeof(buf), "%.1f", s_system->temp_avg / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_temp_avg", buf, true);

    snprintf(buf, sizeof(buf), "%.1f", s_system->temp_low / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_temp_low", buf, true);

    snprintf(buf, sizeof(buf), "%.1f", s_system->temp_high / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_temp_high", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->volt_avg / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_volt_avg", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->volt_low / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_volt_low", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->volt_high / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_volt_high", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->rec_chg_voltage / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "rec_chg_voltage", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->rec_dsg_voltage / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "rec_dsg_voltage", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->rec_chg_current / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "rec_chg_current", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->rec_dsg_current / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "rec_dsg_current", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->sys_rec_chg_voltage / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "sys_rec_chg_voltage", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->sys_rec_dsg_voltage / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "sys_rec_dsg_voltage", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->sys_rec_chg_current / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "sys_rec_chg_current", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->sys_rec_dsg_current / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "sys_rec_dsg_current", buf, true);
  }
}
