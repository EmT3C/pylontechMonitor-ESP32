#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <PubSubClient.h>
#include "batteryStack.h"

class MQTTHandler {
public:
  static void init(PubSubClient* client, batteryStack* stack, systemData* system = nullptr, dailyEnergyData* energy = nullptr);
  static void loop();
  static void publishIfConnected();
  static void publishDiscovery();
  static void publishData();
  static void publishDiagnostic(const char* resetReason,
                                const char* savedPhase,
                                const char* rtcPhase,
                                uint32_t bootCount,
                                uint32_t abnormalResetCount,
                                uint32_t freeHeap,
                                uint32_t minFreeHeap);
  static void publishDiagnosticEvent(const char* eventText);
  static void publishDiagnosticDetail(const char* key, const char* value);

private:
  static void connectIfNeeded();
  static void heartbeatAvailability();

  static PubSubClient* s_client;
  static batteryStack* s_stack;
  static systemData*   s_system;
  static dailyEnergyData* s_energy;
  static unsigned long s_lastPublishMs;
  static unsigned long s_lastAvailMs;
};

#endif // MQTT_HANDLER_H
