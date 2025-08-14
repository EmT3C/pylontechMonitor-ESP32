#include "Config.h"   // stellt FW_VERSION bereit
#include <stdio.h>
#include "Parser.h"
#include "batteryStack.h"
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string.h>   // für strtok/strtok_r

#ifndef strtok_r
  #define strtok_r(s, delim, saveptr) strtok((s), (delim))
#endif

#ifdef DEBUG_PARSER
  char* tokens[24];
int   tokCount = 0;
char* save = nullptr;
for (char* t = strtok_r(lineCopy, " \t", &save);
     t && tokCount < 24;
     t = strtok_r(nullptr, " \t", &save)) {
  tokens[tokCount++] = t;
}

// <-- HIER die Debug-Ausgabe:
for (int k = 0; k < tokCount; ++k) {
  char dbg[64];
  snprintf(dbg, sizeof(dbg), "tok[%d]=%s", k, tokens[k]);
  if (s_log) s_log->Log(dbg);   // später unter /log sichtbar
}
#endif
extern systemData g_systemStack;  // kommt aus der .ino/.cpp oben

static circular_log<7000>* s_log = nullptr;

void Parser::init(circular_log<7000>* log) { s_log = log; }

static const char* findRow(const char* in, int idx) {
  // suche Zeilenstart: \r\r\n<idx>
  char tag[12];
  snprintf(tag, sizeof(tag), "\r\r\n%d", idx);
  const char* p = strstr(in, tag);
  if (!p) return nullptr;
  p += 3;                       // \r\r\n überspringen
  while (*p==' ') p++;          // führende Spaces
  while (isdigit(*p)) p++;      // die Indexzahl selbst überspringen
  while (*p==' ') p++;          // Spaces nach der Zahl
  return p;
}

bool Parser::parsePwr(const char* in, batteryStack* out)
{
  if (!in || !out) return false;

  memset(out, 0, sizeof(*out));

  int  presentCnt   = 0;
  int  chargeCnt    = 0;
  int  dischargeCnt = 0;
  int  idleCnt      = 0;
  int  alarmCnt     = 0;
  long socSum       = 0;
  long socLow       = 101;
  long tempSum      = 0;

  auto isNum = [](const char* s)->bool {
    if (!s || !*s) return false;
    int j = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    for (; s[j]; ++j) if (!isdigit((unsigned char)s[j])) return false;
    return true;
  };
  auto atoli = [](const char* s)->long { return s ? atol(s) : 0L; };

  for (int idx = 1; idx <= MAX_PYLON_BATTERIES; ++idx) {
    // Zeile zu Batterie idx suchen (z.B. "L1:1 ..." oder "\r\r\n1     ...")
    const char* row = findRow(in, idx);
    if (!row) continue;

    // Zeile kopieren
    char line[256];
    size_t n = 0;
    const char* p = row;
    while (*p && *p!='\n' && *p!='\r' && n < sizeof(line)-1) line[n++] = *p++;
    line[n] = 0;

    // Tokenisieren
    const int MAXTOK = 64;
    const char* tokens[MAXTOK];
    int tokCount = 0;
    char* save = nullptr;
    char* t = strtok_r(line, " \t", &save);
    while (t && tokCount < MAXTOK) {
      tokens[tokCount++] = t;
      t = strtok_r(nullptr, " \t", &save);
    }
    if (tokCount == 0) continue;

    // ersten numerischen Token finden (damit "PWR", "L1:1" etc. egal sind)
    int k = -1;
    for (int ti = 0; ti < tokCount; ++ti) {
      if (isNum(tokens[ti])) { k = ti; break; }
    }
    // Mindestlayout prüfen (bis inkl. baseState vorhanden)
    if (k < 0 || (k + 7) >= tokCount) continue;

    // ---- Werte nach FW1-Layout (relativ zu k) ----
    // k+0: voltage[mV], k+1: current[mA], k+2: temp_avg[m°C],
    // k+3: temp_low[m°C], k+4: temp_high[m°C],
    // k+5: cellVoltLow[mV], k+6: cellVoltHigh[mV],
    // k+7: baseState ("Charge/Dischg/Idle/Balance")
    pylonBattery& b = out->batts[idx-1];
    memset(&b, 0, sizeof(b));

    // Defaults, damit Nicht-Treffer kein „Alarm“ auslösen
    strcpy(b.voltageState, "Normal");
    strcpy(b.currentState, "Normal");
    strcpy(b.tempState,    "Normal");
    strcpy(b.b_v_st,       "Normal");
    strcpy(b.b_t_st,       "Normal");
    b.baseState[0] = '\0';
    b.balancing    = false;
    b.isPresent    = true;

    b.voltage       = atoli(tokens[k+0]);
    b.current       = atoli(tokens[k+1]);
    b.tempr         = atoli(tokens[k+2]);   // m°C (Durchschnitt)
    b.cellTempLow   = atoli(tokens[k+3]);
    b.cellTempHigh  = atoli(tokens[k+4]);
    b.cellVoltLow   = atoli(tokens[k+5]);
    b.cellVoltHigh  = atoli(tokens[k+6]);

    // baseState
    strncpy(b.baseState, tokens[k+7], sizeof(b.baseState)-1);

    // Nachfolgende Sub-States + SoC robust einsammeln
    auto isStateToken = [](const char* s)->bool {
      return (strcmp(s, "Normal")==0 || strcmp(s, "Alarm")==0 ||
              strcmp(s, "Warn")==0   || strcmp(s, "Warning")==0 ||
              strcmp(s, "Fault")==0);
    };

    int stateFillIdx = 0; // 0:voltageState,1:currentState,2:tempState,3:b_v_st,4:b_t_st
    for (int ti = k+8; ti < tokCount; ++ti) {
      const char* sTok = tokens[ti];

      // SoC als "<zahl>%"
      if (strchr(sTok, '%')) {
        char tmp[16];
        strncpy(tmp, sTok, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = 0;
        char* per = strchr(tmp, '%'); if (per) *per = 0;
        b.soc = atol(tmp);
        continue;
      }

      // Erneuter Grundzustand (selten) – ignorieren
      if (!strcmp(sTok,"Charge") || !strcmp(sTok,"Dischg") ||
          !strcmp(sTok,"Idle")   || !strcmp(sTok,"Balance")) {
        if (!b.baseState[0]) strncpy(b.baseState, sTok, sizeof(b.baseState)-1);
        continue;
      }

      // Sub-States in Reihenfolge füllen, aber nur bekannte State-Tokens
      if (isStateToken(sTok) && stateFillIdx < 5) {
        switch (stateFillIdx) {
          case 0: strncpy(b.voltageState, sTok, sizeof(b.voltageState)-1); break;
          case 1: strncpy(b.currentState, sTok, sizeof(b.currentState)-1); break;
          case 2: strncpy(b.tempState,    sTok, sizeof(b.tempState)-1);    break;
          case 3: strncpy(b.b_v_st,       sTok, sizeof(b.b_v_st)-1);       break;
          case 4: strncpy(b.b_t_st,       sTok, sizeof(b.b_t_st)-1);       break;
        }
        ++stateFillIdx;
      }
    }

    b.balancing = (strcmp(b.baseState, "Balance") == 0);

    // ---- Aggregation ----
    presentCnt++;
    out->currentDC  += b.current;     // mA
    out->avgVoltage += b.voltage;     // mV
    tempSum         += b.tempr;       // m°C

    socSum += (long)b.soc;
    if (b.soc > 0 && b.soc < socLow) socLow = (long)b.soc;

    bool subOk =
      strcmp(b.voltageState, "Normal") == 0 &&
      strcmp(b.currentState, "Normal") == 0 &&
      strcmp(b.tempState,    "Normal") == 0 &&
      strcmp(b.b_v_st,       "Normal") == 0 &&
      strcmp(b.b_t_st,       "Normal") == 0;

    if (!subOk)                      alarmCnt++;
    else if (b.isCharging())         chargeCnt++;
    else if (b.isDischarging())      dischargeCnt++;
    else if (b.isIdle())             idleCnt++;

    if (b.balancing) out->anyBalancing = true;

    if (s_log) {
      char dbg[140];
      snprintf(dbg, sizeof(dbg),
               "PWR idx=%d V=%ldmV I=%ldmA T=%ldmC SoC=%ld%% base=%s Vst=%s Ist=%s Tst=%s",
               idx, b.voltage, b.current, b.tempr, b.soc, b.baseState,
               b.voltageState, b.currentState, b.tempState);
      s_log->Log(dbg);
    }
  }

  out->batteryCount = presentCnt;
  if (presentCnt > 0) {
    out->avgVoltage /= presentCnt;               // mV
    out->temp        = (int)(tempSum / presentCnt); // m°C
  }

  // Gesamt-SoC: beim Laden Mittelwert, sonst Minimum
  if (presentCnt > 0) {
    if (chargeCnt == presentCnt) out->soc = (int)(socSum / presentCnt);
    else                         out->soc = (socLow == 101 ? 0 : (int)socLow);
  }

  // Stack-State: zuerst Alarm, sonst Stromrichtung
  if      (alarmCnt > 0)                      strcpy(out->baseState, "Alarm!");
  else if (out->currentDC >  200)             strcpy(out->baseState, "Charge");
  else if (out->currentDC < -200)             strcpy(out->baseState, "Dischg");
  else if (idleCnt == presentCnt && presentCnt>0) strcpy(out->baseState, "Idle");
  else                                          strcpy(out->baseState, "Balance");

  return presentCnt > 0;
}


// ---- robuste Parser-Helfer ----
static long readMilliAfter_single(const char* s, const char* label) {
  const char* p = strstr(s, label);
  if (!p) return LONG_MIN;
  p = strchr(p, ':');
  if (!p) return LONG_MIN;
  while (*p == ':' || *p == ' ' || *p == '\t') ++p;

  // Zahl bis zum nächsten Trennzeichen holen
  char num[32]; size_t n = 0;
  while (*p && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t' && n < sizeof(num)-1) {
    num[n++] = *p++;
  }
  num[n] = '\0';
  if (n == 0) return LONG_MIN;

  // Dezimal -> mEinheit, sonst direkt übernehmen
  if (strchr(num, '.')) {
    double d = strtod(num, nullptr);
    long   v = (long)((d * 1000.0) + (d >= 0 ? 0.5 : -0.5));
    return v;
  }
  return atol(num);
}

static long readMilliAfter_multi(const char* s, std::initializer_list<const char*> labels) {
  for (const char* L : labels) {
    long v = readMilliAfter_single(s, L);
    if (v != LONG_MIN) return v;
  }
  return LONG_MIN;
}

static long readLongAfter(const char* s, const char* label) {
  const char* p = strstr(s, label);
  if (!p) return LONG_MIN;
  p = strchr(p, ':');
  if (!p) return LONG_MIN;
  while (*p == ':' || *p == ' ' || *p == '\t') ++p;
  return atol(p);  // mV/mA für System Volt/Current, plain für %, RC, FCC
}

bool Parser::parsePwrsys(const char* in, systemData* out) {
  if (!in || !out) return false;

  long v;
  if ((v = readLongAfter(in, "System SOC"))  != LONG_MIN) out->soc     = (int)v;
  if ((v = readLongAfter(in, "System SOH"))  != LONG_MIN) out->soh     = (int)v;
  if ((v = readLongAfter(in, "System Volt")) != LONG_MIN) out->voltage = v;   // mV (FW liefert mV)
  if ((v = readLongAfter(in, "System Curr")) != LONG_MIN) out->current = v;   // mA (FW liefert mA)
  if ((v = readLongAfter(in, "System RC"))   != LONG_MIN) out->rc      = v;   // mAh
  if ((v = readLongAfter(in, "System FCC"))  != LONG_MIN) out->fcc     = v;   // mAh

  // Temperaturen in m°C – akzeptiere mehrere Labelvarianten
  if ((v = readMilliAfter_multi(in, {"Average temperature", "Temp Avg"})) != LONG_MIN) out->temp_avg  = v;
  if ((v = readMilliAfter_multi(in, {"Highest temperature", "Temp High"}))!= LONG_MIN) out->temp_high = v;
  if ((v = readMilliAfter_multi(in, {"Lowest  temperature", "Lowest temperature", "Temp Low"})) != LONG_MIN) out->temp_low  = v;

  // Zellspannungen in mV – mehrere Labelvarianten
  if ((v = readMilliAfter_multi(in, {"Average voltage", "Volt Avg"}))     != LONG_MIN) out->volt_avg  = v;
  if ((v = readMilliAfter_multi(in, {"Highest voltage", "Volt High"}))    != LONG_MIN) out->volt_high = v;
  if ((v = readMilliAfter_multi(in, {"Lowest  voltage", "Lowest voltage", "Volt Low"})) != LONG_MIN) out->volt_low  = v;

  return true; // auch wenn nur Teilwerte gefunden wurden
}

bool parsePwrsysResponse(const char* in) {
  return Parser::parsePwrsys(in, &g_systemStack);
}
