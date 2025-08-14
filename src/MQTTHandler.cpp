// MQTTHandler.cpp
#include "MQTTHandler.h"
#include "Config.h"
#include <ArduinoJson.h>
#include <string.h>

// --- statische Member ---
PubSubClient* MQTTHandler::s_client        = nullptr;
batteryStack* MQTTHandler::s_stack         = nullptr;
systemData*   MQTTHandler::s_system        = nullptr;
unsigned long MQTTHandler::s_lastPublishMs = 0;
unsigned long MQTTHandler::s_lastAvailMs = 0;   // <— NEU

// -----------------------------------------------------------------------------

void MQTTHandler::init(PubSubClient* client, batteryStack* stack, systemData* system) {
  s_client = client;
  s_stack  = stack;
  s_system = system;

  if (s_client) {
    s_client->setKeepAlive(45);     // stabiler
    s_client->setSocketTimeout(10); // etwas großzügiger
    s_client->setBufferSize(1024);  // Discovery-Payloads
  }
}


void MQTTHandler::connectIfNeeded() {
  if (!s_client) return;
  if (s_client->connected()) return;

  // LWT: retained "offline"
  bool ok = false;
#if defined(MQTT_USER) && defined(MQTT_PASSWORD)
  ok = s_client->connect(
        WIFI_HOSTNAME,
        MQTT_USER, MQTT_PASSWORD,
        MQTT_TOPIC_ROOT "availability",   // willTopic
        1, true,                          // willQoS=1, willRetain=true
        "offline"                         // willMessage
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
    // sofort "online" retained
    s_client->publish(MQTT_TOPIC_ROOT "availability", "online", true);
    s_lastAvailMs = millis();

    // Discovery nur bei (Re-)Connect
    publishDiscovery();
  }
}

void MQTTHandler::heartbeatAvailability() {
  if (!s_client || !s_client->connected()) return;
  const unsigned long now = millis();
  if (now - s_lastAvailMs >= 60000UL) { // alle 60 s
    s_lastAvailMs = now;
    s_client->publish(MQTT_TOPIC_ROOT "availability", "online", true);
  }
}

void MQTTHandler::loop() {
  connectIfNeeded();
  if (s_client) {
    s_client->loop();
    heartbeatAvailability();   // <— NEU
  }
}

void MQTTHandler::publishIfConnected() {
  if (!s_client || !s_stack) return;
  s_client->loop();
  if (!s_client->connected()) return;

  const unsigned long now = millis();
  const unsigned long intervalMs = 3000;   // alle 3 s
  if (now - s_lastPublishMs < intervalMs) return;
  s_lastPublishMs = now;

  // Gesamt-Daten + base_state
  publishData();

  // pro-Batterie publish
  for (int i = 0; i < s_stack->batteryCount; ++i) {
    const pylonBattery& b = s_stack->batts[i];
    if (!b.isPresent) continue;

    char topic[96], payload[32];

    // SoC
    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/soc", i + 1);
    snprintf(payload, sizeof(payload), "%ld", b.soc);
    s_client->publish(topic, payload, true);

    // Spannung (V)
    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/voltage", i + 1);
    snprintf(payload, sizeof(payload), "%.3f", b.voltage / 1000.0);
    s_client->publish(topic, payload, true);

    // Strom (A)
    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/current", i + 1);
    snprintf(payload, sizeof(payload), "%.3f", b.current / 1000.0);
    s_client->publish(topic, payload, true);

    // State (String)
    const char* st =
        b.isCharging()    ? "Charge"  :
        b.isDischarging() ? "Dischg"  :
        b.isIdle()        ? "Idle"    :
        b.isBalancing()   ? "Balance" :
                            "Unknown";
    snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/state", i + 1);
    s_client->publish(topic, st, true);
    heartbeatAvailability();

    // Optional: binäre Flags
    // snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/charging", i + 1);
    // s_client->publish(topic, b.isCharging() ? "1" : "0", true);
    // snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/discharging", i + 1);
    // s_client->publish(topic, b.isDischarging() ? "1" : "0", true);
    // snprintf(topic, sizeof(topic), MQTT_TOPIC_ROOT "%d/idle", i + 1);
    // s_client->publish(topic, b.isIdle() ? "1" : "0", true);
  }
}

// -----------------------------------------------------------------------------

void MQTTHandler::publishDiscovery() {
  if (!s_client) return;

  const String node = WIFI_HOSTNAME;

  auto pub_cfg = [&](const String& object_id,
                     const String& name,
                     const String& state_topic,
                     const char* unit = nullptr,
                     const char* device_class = nullptr)
  {
    StaticJsonDocument<512> doc;
    doc["name"]                  = name;
    doc["state_topic"]           = state_topic;
    doc["unique_id"]             = node + "_" + object_id;
    doc["availability_topic"]    = String(MQTT_TOPIC_ROOT) + "availability";
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";
    // Für Numerik schöner in HA:
    if (unit || device_class) doc["state_class"] = "measurement";
    if (unit)         doc["unit_of_measurement"] = unit;
    if (device_class) doc["device_class"]        = device_class;

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

  // 1) Gesamt-Sensoren (SoC, Temp, Current, Voltage, DC/AC Power)
  struct Sensor { const char* id; const char* name; const char* unit; const char* devClass; };
  const Sensor sensors[] = {
    {"soc",         "Battery SoC",        "%",  "battery"},
    {"temp",        "Battery Temp",       "°C", "temperature"},
    {"currentDC",   "Battery Current",    "mA", "current"},
    {"avgVoltage",  "Battery Voltage",    "V",  "voltage"},
    {"dc_power",    "Battery DC Power",   "W",  "power"},
    {"ac_power_est","Battery AC Power",   "W",  "power"},
  };
  for (auto &s : sensors) {
    pub_cfg(s.id, s.name, String(MQTT_TOPIC_ROOT) + s.id, s.unit, s.devClass);
  }

  // 2) Gesamtzustand als Text (base_state)
  {
    StaticJsonDocument<512> doc;
    doc["name"]                  = "Battery State";
    doc["state_topic"]           = String(MQTT_TOPIC_ROOT) + "base_state";
    doc["unique_id"]             = node + "_base_state";
    doc["availability_topic"]    = String(MQTT_TOPIC_ROOT) + "availability";
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";

    JsonObject dev = doc.createNestedObject("device");
    JsonArray ids  = dev.createNestedArray("identifiers");
    ids.add(node);
    dev["manufacturer"] = "Pylontech";
    dev["model"]        = "Battery Monitor";
    dev["name"]         = node;

    char payload[512];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    String topic = String(HA_DISCOVERY_SENSOR_PREFIX) + node + "/base_state/config";
    s_client->publish(topic.c_str(), (uint8_t*)payload, len, true);
  }

  // 2b) System RC/FCC (mAh)
  pub_cfg("system_rc",  "System RC",  String(MQTT_TOPIC_ROOT) + "system_rc",  "mAh", nullptr);
  pub_cfg("system_fcc", "System FCC", String(MQTT_TOPIC_ROOT) + "system_fcc", "mAh", nullptr);
  pub_cfg("system_soh", "System SOH", String(MQTT_TOPIC_ROOT) + "system_soh", "%", nullptr);

  // 3) Pro-Batterie: SoC, Voltage, Current + Text-State
  for (int i = 1; i <= MAX_PYLON_BATTERIES; ++i) {
    const String idp = "b" + String(i);

    // Mess-Sensoren
    pub_cfg(idp + "_soc",
            String("Battery ") + i + " SoC",
            String(MQTT_TOPIC_ROOT) + i + "/soc",
            "%", "battery");

    pub_cfg(idp + "_voltage",
            String("Battery ") + i + " Voltage",
            String(MQTT_TOPIC_ROOT) + i + "/voltage",
            "V", "voltage");

    pub_cfg(idp + "_current",
            String("Battery ") + i + " Current",
            String(MQTT_TOPIC_ROOT) + i + "/current",
            "A", "current");

    // Text-State je Batterie
    {
      StaticJsonDocument<384> doc;
      doc["name"]                  = String("Battery ") + i + " State";
      doc["state_topic"]           = String(MQTT_TOPIC_ROOT) + i + "/state";
      doc["unique_id"]             = node + "_" + idp + "_state";
      doc["availability_topic"]    = String(MQTT_TOPIC_ROOT) + "availability";
      doc["payload_available"]     = "online";
      doc["payload_not_available"] = "offline";

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
    {
      StaticJsonDocument<384> doc;
      doc["name"] = "Pylontech Online";
      doc["state_topic"] = String(MQTT_TOPIC_ROOT) + "availability";  // z.B. "pylontech-esp32/availability"
      doc["unique_id"] = String(WIFI_HOSTNAME) + "_online";
      doc["device_class"] = "connectivity";
      doc["payload_on"]  = "online";
      doc["payload_off"] = "offline";
      doc["entity_category"] = "diagnostic"; // optional

      JsonObject dev = doc.createNestedObject("device");
      JsonArray ids = dev.createNestedArray("identifiers");
      ids.add(WIFI_HOSTNAME);
      dev["manufacturer"] = "Pylontech";
      dev["model"]        = "Battery Monitor";
      dev["name"]         = WIFI_HOSTNAME;

      char payload[384];
      size_t len = serializeJson(doc, payload, sizeof(payload));
      String topic = String(HA_DISCOVERY_BINARY_PREFIX) + WIFI_HOSTNAME + "/online/config";
      s_client->publish(topic.c_str(), (uint8_t*)payload, len, true); // retained
    }
  }
}



// -----------------------------------------------------------------------------

void MQTTHandler::publishData() {
  if (!s_client || !s_stack) return;

  char buf[32];

  // SoC
  snprintf(buf, sizeof(buf), "%d", s_stack->soc);
  s_client->publish(MQTT_TOPIC_ROOT "soc", buf, true);

  // Temp (°C aus m°C)
  snprintf(buf, sizeof(buf), "%.1f", s_stack->temp / 1000.0);
  s_client->publish(MQTT_TOPIC_ROOT "temp", buf, true);

  // CurrentDC (mA)
  snprintf(buf, sizeof(buf), "%ld", s_stack->currentDC);
  s_client->publish(MQTT_TOPIC_ROOT "currentDC", buf, true);

  // Voltage (V aus mV)
  snprintf(buf, sizeof(buf), "%.3f", s_stack->avgVoltage / 1000.0);
  s_client->publish(MQTT_TOPIC_ROOT "avgVoltage", buf, true);

  // base_state (Text)
  s_client->publish(MQTT_TOPIC_ROOT "base_state", s_stack->baseState, true);

  // Optional: Systemdaten
  if (s_system) {
    snprintf(buf, sizeof(buf), "%d", s_system->soc);
    s_client->publish(MQTT_TOPIC_ROOT "system_soc", buf, true);

    snprintf(buf, sizeof(buf), "%d", s_system->soh);
    s_client->publish(MQTT_TOPIC_ROOT "system_soh", buf, true);

    snprintf(buf, sizeof(buf), "%.3f", s_system->voltage / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_voltage", buf, true);

    // DC Power (W) aus mV/mA
    double pdc = (s_stack->avgVoltage / 1000.0) * (s_stack->currentDC / 1000.0);
    snprintf(buf, sizeof(buf), "%.0f", pdc);
    s_client->publish(MQTT_TOPIC_ROOT "dc_power", buf, true);

    // AC Power est. (W) – nutzt deine Schätzung aus batteryStack
    long pac = s_stack->getEstPowerAc();
    snprintf(buf, sizeof(buf), "%ld", pac);
    s_client->publish(MQTT_TOPIC_ROOT "ac_power_est", buf, true);


    // current
    snprintf(buf, sizeof(buf), "%.3f", s_system->current / 1000.0);
    s_client->publish(MQTT_TOPIC_ROOT "system_current", buf, true);

    // NEU: RC und FCC (mAh)
    snprintf(buf, sizeof(buf), "%ld", s_system->rc);
    s_client->publish(MQTT_TOPIC_ROOT "system_rc", buf, true);

    snprintf(buf, sizeof(buf), "%ld", s_system->fcc);
    s_client->publish(MQTT_TOPIC_ROOT "system_fcc", buf, true);


    // weitere Felder bei Bedarf: temp_avg/high/low, volt_avg/high/low, rc, fcc...
  }
}
