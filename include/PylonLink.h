#pragma once
#include <HardwareSerial.h>
#include "circular_log.h"

class BatteryLink {
public:
  BatteryLink(HardwareSerial& serial, int rx, int tx);
  void begin(int b);
  void switchBaud(int nb);

  bool sendAndReceive(const char* cmd, char* outBuf, size_t bufSize, unsigned long timeoutMs=6000);
  bool sendAndReceivePrompt(const char* cmd, char* outBuf, size_t bufSize, unsigned long timeoutMs=12000);

  int  available() const;
  void logIncoming(circular_log<7000>* log);

private:
  HardwareSerial& port;
  int rxPin, txPin;
  int baud = 0;

  volatile bool m_busy = false;
  bool lock(uint32_t waitMs);
  void unlock();
  int  readUntil(char* buf, const char* term, size_t maxLen, unsigned long timeoutMs);
  void wakeUpConsole(); // <— WICHTIG: Deklaration
};


