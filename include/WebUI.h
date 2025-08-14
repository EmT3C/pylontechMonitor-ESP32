#pragma once
#include <WebServer.h>
#include <cstddef>        // size_t
#include "batteryStack.h" // batteryStack, systemData

// Vorwärtsdeklarationen (reichen für Pointer-Parameter):
class BatteryLink;
template<unsigned int Size> class circular_log;

namespace WebUI {
  void init(WebServer* server,
            BatteryLink* link,
            batteryStack* stk,
            systemData* sys,
            char* rawBuf,
            size_t rawBufLen,
            circular_log<7000>* clog);
}
