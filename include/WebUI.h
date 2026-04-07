#pragma once
#include <WebServer.h>
#include <cstddef>
#include "batteryStack.h"

class BatteryLink;
template<unsigned int Size> class circular_log;

struct statDebugData {
  uint8_t currentIdx = 0;
  uint8_t maxBat = 0;
  uint8_t detected = 0;
  uint8_t highestPresentIdx = 0;
  bool initialRun = true;
  bool inProgress = false;
  bool lastSuccess = false;
  bool lastTimedOut = false;
  bool lastParseFailed = false;
  long lastCycleTimes = 0;
  unsigned long lastAttemptMs = 0;
  unsigned long lastSuccessMs = 0;
  char lastCommand[16] = {0};
  char lastMessage[64] = {0};
};

namespace WebUI {
  void init(WebServer* server,
            BatteryLink* link,
            batteryStack* stk,
            systemData* sys,
            dailyEnergyData* energy,
            statDebugData* statDbg,
            char* rawBuf,
            size_t rawBufLen,
            circular_log<7000>* clog);
}
