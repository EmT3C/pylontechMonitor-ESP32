#include "PylonLink.h"
#include <string.h>   // strstr, strchr
#include <Arduino.h>  // millis, delay
#include <ctype.h>    // tolower

static inline void pushWindow(char* window, size_t capacity, size_t& len, char c) {
  if (capacity < 2) return;

  if (len < capacity - 1) {
    window[len++] = c;
  } else {
    memmove(window, window + 1, capacity - 2);
    window[capacity - 2] = c;
    len = capacity - 1;
  }
  window[len] = '\0';
}

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
  if (!outBuf || bufSize < 2) return false;
  if (!lock(6000)) return false;

  bool ok = false;
  outBuf[0] = '\0';
  port.flush();

  wakeUpConsole();
  while (port.available()) { port.read(); }

  if (cmd && *cmd) port.print(cmd);
  port.print('\n');

  // kein fester Terminator, readUntil erkennt pylon> und pylon_debug> selbst
  int n = readUntil(outBuf, nullptr, bufSize, timeoutMs);
  ok = (n > 0);

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
  const size_t termLen = (term && *term) ? strlen(term) : 0;
  buf[0] = '\0';

  // kurze Vorwartezeit
  while (!port.available() && (millis() - t0 < timeoutMs)) {
    delay(1);
  }

  // Sliding Windows
  char last6[7]    = {0};  // "pylon>"
  char last12[13]  = {0};  // "pylon_debug>"
  char last96[97]  = {0};  // Pagination etc. in lowercase
  size_t last6Len = 0;
  size_t last12Len = 0;
  size_t last96Len = 0;

  while ((millis() - t0) < timeoutMs && len < maxLen - 1) {
    while (port.available() && len < maxLen - 1) {
      int ci = port.read();
      if (ci < 0) break;
      char c = (char)ci;

      // Puffer füllen
      buf[len++] = c;
      buf[len] = '\0';

      // Sliding windows aktualisieren
      pushWindow(last6, sizeof(last6), last6Len, c);
      pushWindow(last12, sizeof(last12), last12Len, c);
      pushWindow(last96, sizeof(last96), last96Len, (char)tolower((unsigned char)c));

      // 1) expliziten Terminator nur am Pufferende pruefen statt den ganzen Buffer jedes Mal zu durchsuchen
      if (termLen > 0 && len >= termLen) {
        if (memcmp(buf + len - termLen, term, termLen) == 0) {
          found = true;
          break;
        }
      }

      // 2) bekannte Prompts immer erkennen
      if (last6Len == 6 && memcmp(last6, "pylon>", 6) == 0) {
        found = true;
        break;
      }
      if (last12Len == 12 && memcmp(last12, "pylon_debug>", 12) == 0) {
        found = true;
        break;
      }

      // 3) Pagination heuristik (case-insensitive)
      // Varianten wie:
      // "press [enter] to be continued", "press enter to continue", "press any key to continue"
      bool needEnter = false;
      if (strstr(last96, "press") && (strstr(last96, "[enter]") || strstr(last96, " enter"))) {
        if (strstr(last96, "continue") || strstr(last96, "continued") || strstr(last96, "to be continued"))
          needEnter = true;
      }
      if (!needEnter && strstr(last96, "press any key") && strstr(last96, "continue")) {
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
