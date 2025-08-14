#include "PylonLink.h"
#include <string.h>   // strstr, strchr
#include <Arduino.h>  // millis, delay
#include <ctype.h>   // tolower

BatteryLink::BatteryLink(HardwareSerial& serial, int rx, int tx)
  : port(serial), rxPin(rx), txPin(tx) {}

void BatteryLink::begin(int b) {
  baud = b;
  port.begin(baud, SERIAL_8N1, rxPin, txPin);
  delay(50);
  // Eingang puffern leeren (alte Bytes loswerden)
  while (port.available()) { port.read(); }
}

void BatteryLink::switchBaud(int nb) {
  if (baud == nb) return;
  port.flush();         // wartet TX leer
  delay(20);
  port.end();
  delay(20);
  port.begin(nb, SERIAL_8N1, rxPin, txPin);
  baud = nb;
  delay(20);
  while (port.available()) { port.read(); }
}

bool BatteryLink::lock(uint32_t waitMs) {
  uint32_t t0 = millis();
  while (m_busy) {
    if (millis() - t0 > waitMs) return false;
    delay(1);
  }
  m_busy = true;
  return true;
}

void BatteryLink::unlock() { m_busy = false; }

bool BatteryLink::sendAndReceive(const char* cmd, char* outBuf, size_t bufSize, unsigned long timeoutMs) {
  if (!outBuf || bufSize == 0) return false;
  if (!lock(6000)) return false;

  bool ok = false;                         // <— statt frühem return
  port.flush();
  wakeUpConsole();

  // Rx leeren, damit nur Antwort auf DIESEN Befehl kommt
  while (port.available()) { port.read(); }

  if (cmd && *cmd) port.print(cmd);
  port.print('\n');

  // Akzeptiere sowohl ">" als auch "pylon>" als Terminator
  ok = (readUntil(outBuf, ">", bufSize, timeoutMs) > 0);
  if (!ok && outBuf && outBuf[0] == '\0') {
    ok = (readUntil(outBuf, "pylon>", bufSize, timeoutMs) > 0);
  }

  unlock();                                // <— wird jetzt IMMER erreicht
  return ok;
}

bool BatteryLink::sendAndReceivePrompt(const char* cmd, char* outBuf, size_t bufSize, unsigned long timeoutMs) {
  if (!outBuf || bufSize == 0) return false;
  if (!lock(6000)) return false;

  bool ok = false;
  port.flush();
  wakeUpConsole();
  while (port.available()) { port.read(); }

  if (cmd && *cmd) port.print(cmd);
  port.print('\n');

  // Für „help“ & lange Ausgaben: direkt auf explizites Prompt warten
  ok = (readUntil(outBuf, "pylon>", bufSize, timeoutMs) > 0);

  unlock();
  return ok;
}


int BatteryLink::available() const {
  return port.available();
}

void BatteryLink::logIncoming(circular_log<7000>* log) {
  if (!log) return;
  while (port.available()) {
    char c = (char)port.read();
    char s[2] = { c, 0 };
    log->Log(s);
    // Nicht blockieren, WLAN atmen lassen
    delay(0);
  }
}

// Liest bis Terminator (z.B. ">"), erkennt außerdem pylon> und "Press [Enter]..."
int BatteryLink::readUntil(char* buf, const char* term, size_t maxLen, unsigned long timeoutMs) {
  if (!buf || maxLen < 2) return 0;

  size_t len = 0;
  bool found = false;
  const uint32_t t0 = millis();
  buf[0] = '\0';

  // kurze Vorwartezeit
  while (!port.available() && (millis() - t0 < timeoutMs)) {
    delay(1);
  }

  // Sliding Windows
  char last6[7] = {0};     // für "pylon>"
  char last96[97] = {0};   // für case-insensitive Suchmuster (Pagination etc.)

  auto push_last6 = [&](char c){
    memmove(last6, last6+1, 5);
    last6[5] = c; last6[6] = 0;
  };
  auto push_last96 = [&](char c){
    size_t L = strlen(last96);
    if (L < sizeof(last96)-1) { last96[L] = c; last96[L+1] = 0; }
    else { memmove(last96, last96+1, sizeof(last96)-2); last96[sizeof(last96)-2] = c; last96[sizeof(last96)-1] = 0; }
  };
  auto toLowerInPlace = [&](char* s){
    for (char* p=s; *p; ++p) *p = (char)tolower((unsigned char)*p);
  };

  while ((millis() - t0) < timeoutMs && len < maxLen - 1) {
    while (port.available() && len < maxLen - 1) {
      int ci = port.read();
      if (ci < 0) break;
      char c = (char)ci;

      // Puffer füllen
      buf[len++] = c;
      buf[len] = '\0';

      // Sliding windows aktualisieren
      push_last6(c);
      push_last96(c);

      // 1) Terminator check
      if (term && *term) {
        if (strstr(buf, term)) { found = true; break; }   // z.B. "pylon>"
      }
      // 2) Immer auch explizit "pylon>" erkennen
      if (memcmp(last6, "pylon>", 6) == 0) { found = true; break; }

      // 3) Pagination heuristik (case-insensitive)
      // Wir checken im kleinen Fenster die häufigsten Varianten
      char low[97]; strncpy(low, last96, sizeof(low)-1); low[sizeof(low)-1]=0;
      toLowerInPlace(low);

      // Varianten wie:
      // "press [enter] to be continued", "press enter to continue", "press any key to continue"
      bool needEnter = false;
      if (strstr(low, "press") && (strstr(low, "[enter]") || strstr(low, " enter"))) {
        if (strstr(low, "continue") || strstr(low, "continued") || strstr(low, "to be continued"))
          needEnter = true;
      }
      if (!needEnter && strstr(low, "press any key") && strstr(low, "continue")) {
        needEnter = true;
      }

      if (needEnter) {
        port.write('\r');   // nächste Seite anfordern
      }
    }

    if (found) break;
    delay(0);
  }

  if (!found && len == 0) return 0; // gar nichts empfangen
  return (int)len;                  // ggf. teilgefüllt (bei Pufferlimit)
}


// Kurzer Weckversuch wie in deiner Ursprungsversion
void BatteryLink::wakeUpConsole() {
  // Minimal-invasiv: ein paar Newlines auf aktueller Baudrate
  for (int i = 0; i < 3; i++) {
    port.write('\n');
    delay(10);
  }

  // Optional: harte Sequenz (deine ältere Folge). Nur nutzen, wenn lange nichts kommt.
  // Hier NICHT automatisch senden, um unnötige Frames zu vermeiden.
  // -> Wenn du die harte Sequenz brauchst: auskommentieren und aufrufen.
  /*
  int original = baud ? baud : 115200;
  switchBaud(1200);
  port.write("~20014682C0048520FCC3\r");
  delay(100);
  byte nl[] = {0x0E, 0x0A};
  switchBaud(original);
  for (int i=0; i<5; i++) {
    port.write(nl, sizeof(nl));
    delay(20);
    if (port.available()) { while (port.available()) port.read(); break; }
    delay(0);
  }
  */
}
