#include "Config.h"
#include <stdio.h>
#include "Parser.h"
#include "batteryStack.h"
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string.h>
#include <Arduino.h>
#include <limits.h>

#ifndef strtok_r
  #define strtok_r(s, delim, saveptr) strtok((s), (delim))
#endif

static circular_log<16384>* s_log = nullptr;

static void setBatteryAlarmText(pylonBattery& b);

void Parser::init(circular_log<16384>* log) {
  s_log = log;
}

static const char* findRow(const char* in, int idx) {
  if (!in) return nullptr;

  const char* p = in;
  while (*p) {
    while (*p == '\r' || *p == '\n') ++p;
    if (!*p) break;

    const char* lineStart = p;
    while (*p == ' ' || *p == '\t') ++p;

    const char* q = p;
    int len = 0;
    while (isdigit((unsigned char)*q) && len < 7) {
      ++q;
      ++len;
    }

    if (len > 0) {
      char numBuf[8];
      memcpy(numBuf, p, len);
      numBuf[len] = 0;

      if (atoi(numBuf) == idx && (*q == ' ' || *q == '\t')) {
        p = q;
        while (*p == ' ' || *p == '\t') ++p;
        return p;
      }
    }

    while (*lineStart && *lineStart != '\n') ++lineStart;
    p = lineStart;
  }

  return nullptr;
}

bool Parser::parsePwr(const char* in, batteryStack* out) {
  if (!in || !out) return false;

  long oldCycleTimes[MAX_PYLON_BATTERIES] = {0};
  for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
    oldCycleTimes[i] = out->batts[i].cycleTimes;
  }

  memset(out, 0, sizeof(*out));

  for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
    out->batts[i].cycleTimes = oldCycleTimes[i];
  }

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
    for (; s[j]; ++j) {
      if (!isdigit((unsigned char)s[j])) return false;
    }
    return true;
  };

  auto atoli = [](const char* s)->long {
    return s ? atol(s) : 0L;
  };

  for (int idx = 1; idx <= MAX_PYLON_BATTERIES; ++idx) {
    const char* row = findRow(in, idx);
    if (!row) continue;

    char line[320];
    size_t n = 0;
    const char* p = row;
    while (*p && *p != '\n' && *p != '\r' && n < sizeof(line) - 1) {
      line[n++] = *p++;
    }
    line[n] = 0;

    const int MAXTOK = 64;
    char* tokens[MAXTOK];
    int tokCount = 0;

    char* save = nullptr;
    char* t = strtok_r(line, " \t", &save);
    while (t && tokCount < MAXTOK) {
      tokens[tokCount++] = t;
      t = strtok_r(nullptr, " \t", &save);
    }

    if (tokCount < 16) continue;
    if (!isNum(tokens[0]) || !isNum(tokens[1]) || !isNum(tokens[2])) continue;

    pylonBattery& b = out->batts[idx - 1];
    long savedCycleTimes = b.cycleTimes;
    memset(&b, 0, sizeof(b));
    b.cycleTimes = savedCycleTimes;

    b.isPresent = true;
    b.voltage      = atoli(tokens[0]);
    b.current      = atoli(tokens[1]);
    b.tempr        = atoli(tokens[2]);
    b.cellTempLow  = (tokCount > 3  && isNum(tokens[3])) ? atoli(tokens[3]) : 0;
    b.cellTempHigh = (tokCount > 5  && isNum(tokens[5])) ? atoli(tokens[5]) : 0;
    b.cellVoltLow  = (tokCount > 7  && isNum(tokens[7])) ? atoli(tokens[7]) : 0;
    b.cellVoltHigh = (tokCount > 9  && isNum(tokens[9])) ? atoli(tokens[9]) : 0;

    strncpy(b.baseState,    (tokCount > 11 ? tokens[11] : "Unknown"), sizeof(b.baseState) - 1);
    strncpy(b.voltageState, (tokCount > 12 ? tokens[12] : "Unknown"), sizeof(b.voltageState) - 1);
    strncpy(b.currentState, (tokCount > 13 ? tokens[13] : "Unknown"), sizeof(b.currentState) - 1);
    strncpy(b.tempState,    (tokCount > 14 ? tokens[14] : "Unknown"), sizeof(b.tempState) - 1);

    if (tokCount > 15) {
      char socBuf[16];
      strncpy(socBuf, tokens[15], sizeof(socBuf) - 1);
      socBuf[sizeof(socBuf) - 1] = 0;
      char* pct = strchr(socBuf, '%');
      if (pct) *pct = 0;
      b.soc = atol(socBuf);
    }

    strcpy(b.b_v_st, "Normal");
    strcpy(b.b_t_st, "Normal");

    if (tokCount > 18) strncpy(b.b_v_st, tokens[18], sizeof(b.b_v_st) - 1);
    if (tokCount > 19) strncpy(b.b_t_st, tokens[19], sizeof(b.b_t_st) - 1);

    b.balancing = b.isBalancing();
    setBatteryAlarmText(b);

    presentCnt++;
    out->currentDC  += b.current;
    out->avgVoltage += b.voltage;
    tempSum         += b.tempr;
    socSum          += b.soc;

    if (b.soc >= 0 && b.soc <= 100 && b.soc < socLow) socLow = b.soc;

    bool subOk =
      strcmp(b.voltageState, "Normal") == 0 &&
      strcmp(b.currentState, "Normal") == 0 &&
      strcmp(b.tempState,    "Normal") == 0 &&
      strcmp(b.b_v_st,       "Normal") == 0 &&
      strcmp(b.b_t_st,       "Normal") == 0;

    bool faultState = !subOk || b.isAlarm() || b.isProtect();

    if (b.isBalancing()) out->anyBalancing = true;

    if (faultState)             alarmCnt++;
    else if (b.isCharging())    chargeCnt++;
    else if (b.isDischarging()) dischargeCnt++;
    else if (b.isIdle())        idleCnt++;

    if (s_log && !b.isNormal()) {
      char dbg[200];
      snprintf(dbg, sizeof(dbg),
               "PWR idx=%d V=%ldmV I=%ldmA T=%ldmC SoC=%ld%% base=%s Vst=%s Ist=%s Tst=%s BV=%s BT=%s",
               idx, b.voltage, b.current, b.tempr, b.soc,
               b.baseState, b.voltageState, b.currentState, b.tempState, b.b_v_st, b.b_t_st);
      s_log->Log(dbg);
    }
  }

  out->batteryCount = presentCnt;

  if (presentCnt > 0) {
    out->avgVoltage /= presentCnt;
    out->temp        = (int)(tempSum / presentCnt);
    out->tempr       = out->temp;
  }

  if (presentCnt > 0) {
    if (chargeCnt == presentCnt) out->soc = (int)(socSum / presentCnt);
    else                         out->soc = (socLow == 101 ? 0 : (int)socLow);
  }

  if (alarmCnt > 0) {
    strcpy(out->baseState, "Alarm!");
  } else if (out->anyBalancing) {
    strcpy(out->baseState, "Balance");
  } else if (chargeCnt > 0 && dischargeCnt == 0) {
    strcpy(out->baseState, "Charge");
  } else if (dischargeCnt > 0 && chargeCnt == 0) {
    strcpy(out->baseState, "Dischg");
  } else if (idleCnt == presentCnt && presentCnt > 0) {
    strcpy(out->baseState, "Idle");
  } else if (out->currentDC > 200) {
    strcpy(out->baseState, "Charge");
  } else if (out->currentDC < -200) {
    strcpy(out->baseState, "Dischg");
  } else {
    strcpy(out->baseState, "Idle");
  }

  out->valid = (presentCnt > 0);
  if (out->valid) out->lastUpdateMs = millis();

  return out->valid;
}

// ---------- pwrsys helpers ----------

static long readLongAfter(const char* s, const char* label) {
  const char* p = strstr(s, label);
  if (!p) return LONG_MIN;

  p = strchr(p, ':');
  if (!p) return LONG_MIN;
  ++p;

  while (*p == ' ' || *p == '\t') ++p;

  char num[32];
  size_t n = 0;

  if (*p == '+' || *p == '-') {
    num[n++] = *p++;
  }

  while (isdigit((unsigned char)*p) && n < sizeof(num) - 1) {
    num[n++] = *p++;
  }

  num[n] = 0;

  if (n == 0 || (n == 1 && (num[0] == '+' || num[0] == '-'))) return LONG_MIN;
  return atol(num);
}

static long readLongAfterMulti(const char* s, std::initializer_list<const char*> labels) {
  for (const char* lbl : labels) {
    long v = readLongAfter(s, lbl);
    if (v != LONG_MIN) return v;
  }
  return LONG_MIN;
}

// Wie readLongAfterMulti, aber überspringt Treffer die direkt von badPrefix eingeleitet werden.
// Verhindert dass "Recommend chg voltage" den Wert von "system Recommend chg voltage" liest.
static long readLongAfterExcluded(const char* s,
                                   std::initializer_list<const char*> labels,
                                   const char* badPrefix) {
  const size_t badLen = badPrefix ? strlen(badPrefix) : 0;
  for (const char* label : labels) {
    const size_t llen = strlen(label);
    const char* p = s;
    while ((p = strstr(p, label)) != nullptr) {
      if (badLen > 0 && (size_t)(p - s) >= badLen &&
          memcmp(p - badLen, badPrefix, badLen) == 0) {
        p += llen;
        continue;
      }
      const char* q = strchr(p, ':');
      if (!q) { p += llen; continue; }
      ++q;
      while (*q == ' ' || *q == '\t') ++q;
      char num[32];
      size_t n = 0;
      if (*q == '+' || *q == '-') num[n++] = *q++;
      while (isdigit((unsigned char)*q) && n < sizeof(num) - 1) num[n++] = *q++;
      num[n] = 0;
      if (n == 0 || (n == 1 && (num[0] == '+' || num[0] == '-'))) {
        p += llen; continue;
      }
      return atol(num);
    }
  }
  return LONG_MIN;
}

static bool readWordAfterLabel(const char* s, const char* label, char* out, size_t outSize) {
  if (!s || !label || !out || outSize == 0) return false;

  const char* p = strstr(s, label);
  if (!p) return false;

  p = strchr(p, ':');
  if (!p) return false;
  ++p;

  while (*p == ' ' || *p == '\t') ++p;
  if (!*p) return false;

  size_t n = 0;
  while (*p && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t' && n < outSize - 1) {
    out[n++] = *p++;
  }
  out[n] = 0;
  return n > 0;
}

static bool containsIgnoreCase(const char* text, const char* needle) {
  if (!text || !needle || !*needle) return false;

  size_t needleLen = strlen(needle);
  for (const char* p = text; *p; ++p) {
    size_t i = 0;
    while (i < needleLen &&
           p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
      ++i;
    }
    if (i == needleLen) return true;
  }
  return false;
}

static void setBatteryAlarmText(pylonBattery& b) {
  if (b.isProtect()) {
    strncpy(b.alarmText, "Protect", sizeof(b.alarmText) - 1);
    return;
  }
  if (b.isAlarm()) {
    strncpy(b.alarmText, "Alarm", sizeof(b.alarmText) - 1);
    return;
  }
  if (strcmp(b.voltageState, "Normal") != 0) {
    snprintf(b.alarmText, sizeof(b.alarmText), "Voltage: %s", b.voltageState);
    return;
  }
  if (strcmp(b.currentState, "Normal") != 0) {
    snprintf(b.alarmText, sizeof(b.alarmText), "Current: %s", b.currentState);
    return;
  }
  if (strcmp(b.tempState, "Normal") != 0) {
    snprintf(b.alarmText, sizeof(b.alarmText), "Temp: %s", b.tempState);
    return;
  }
  if (strcmp(b.b_v_st, "Normal") != 0) {
    snprintf(b.alarmText, sizeof(b.alarmText), "Cell Volt: %s", b.b_v_st);
    return;
  }
  if (strcmp(b.b_t_st, "Normal") != 0) {
    snprintf(b.alarmText, sizeof(b.alarmText), "Cell Temp: %s", b.b_t_st);
    return;
  }
  strncpy(b.alarmText, "Normal", sizeof(b.alarmText) - 1);
}

bool Parser::parsePwrsys(const char* in, systemData* out) {
  if (!in || !out) return false;

  memset(out, 0, sizeof(*out));
  out->soc = -1;
  out->soh = -1;

  long v;
  int matched = 0;

  if ((v = readLongAfterMulti(in, {"System SOC"})) != LONG_MIN) {
    out->soc = (int)v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"System SOH"})) != LONG_MIN) {
    out->soh = (int)v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"System Volt", "System Voltage"})) != LONG_MIN) {
    out->voltage = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"System Curr", "System Current"})) != LONG_MIN) {
    out->current = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"System RC"})) != LONG_MIN) {
    out->rc = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"System FCC"})) != LONG_MIN) {
    out->fcc = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"Highest voltage", "High voltage"})) != LONG_MIN) {
    out->volt_high = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"Average voltage", "Avg voltage"})) != LONG_MIN) {
    out->volt_avg = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"Lowest voltage", "Low voltage"})) != LONG_MIN) {
    out->volt_low = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"Highest temperature", "High temperature"})) != LONG_MIN) {
    out->temp_high = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"Average temperature", "Avg temperature"})) != LONG_MIN) {
    out->temp_avg = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"Lowest temperature", "Low temperature"})) != LONG_MIN) {
    out->temp_low = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"system Recommend chg voltage"})) != LONG_MIN) {
    out->sys_rec_chg_voltage = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"system Recommend dsg voltage"})) != LONG_MIN) {
    out->sys_rec_dsg_voltage = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"system Recommend chg current"})) != LONG_MIN) {
    out->sys_rec_chg_current = v;
    matched++;
  }

  if ((v = readLongAfterMulti(in, {"system Recommend dsg current"})) != LONG_MIN) {
    out->sys_rec_dsg_current = v;
    matched++;
  }

  if ((v = readLongAfterExcluded(in, {"Recommend chg voltage"}, "system ")) != LONG_MIN) {
    out->rec_chg_voltage = v;
    matched++;
  }

  if ((v = readLongAfterExcluded(in, {"Recommend dsg voltage"}, "system ")) != LONG_MIN) {
    out->rec_dsg_voltage = v;
    matched++;
  }

  if ((v = readLongAfterExcluded(in, {"Recommend chg current"}, "system ")) != LONG_MIN) {
    out->rec_chg_current = v;
    matched++;
  }

  if ((v = readLongAfterExcluded(in, {"Recommend dsg current"}, "system ")) != LONG_MIN) {
    out->rec_dsg_current = v;
    matched++;
  }

  char stateBuf[16] = {0};
  char alarmBuf[16] = {0};

  if (readWordAfterLabel(in, "Alarm status", alarmBuf, sizeof(alarmBuf)) ||
      readWordAfterLabel(in, "Alarm Status", alarmBuf, sizeof(alarmBuf))) {
    if (strcmp(alarmBuf, "Normal") == 0 || strcmp(alarmBuf, "normal") == 0) {
      strncpy(out->alarmState, "Normal", sizeof(out->alarmState) - 1);
    } else {
      strncpy(out->alarmState, "Alarm", sizeof(out->alarmState) - 1);
    }
  } else if (containsIgnoreCase(in, "protect")) {
    strncpy(out->alarmState, "Alarm", sizeof(out->alarmState) - 1);
  } else {
    strncpy(out->alarmState, "Normal", sizeof(out->alarmState) - 1);
  }

  if (readWordAfterLabel(in, "System state", stateBuf, sizeof(stateBuf)) ||
      readWordAfterLabel(in, "System State", stateBuf, sizeof(stateBuf)) ||
      readWordAfterLabel(in, "State", stateBuf, sizeof(stateBuf))) {
    if (strcmp(stateBuf, "Protect") == 0) {
      strncpy(out->state, "Protect", sizeof(out->state) - 1);
    } else if (strcmp(stateBuf, "Balance") == 0) {
      strncpy(out->state, "Balance", sizeof(out->state) - 1);
    } else if (strcmp(stateBuf, "Dischg") == 0 || strcmp(stateBuf, "Discharging") == 0) {
      strncpy(out->state, "Dischg", sizeof(out->state) - 1);
    } else if (strcmp(stateBuf, "Charge") == 0 || strcmp(stateBuf, "Charging") == 0) {
      strncpy(out->state, "Charge", sizeof(out->state) - 1);
    } else if (strcmp(stateBuf, "Idle") == 0) {
      strncpy(out->state, "Idle", sizeof(out->state) - 1);
    } else {
      strncpy(out->state, stateBuf, sizeof(out->state) - 1);
    }
  } else if (containsIgnoreCase(in, "System is discharging")) {
    strncpy(out->state, "Dischg", sizeof(out->state) - 1);
  } else if (containsIgnoreCase(in, "System is charging")) {
    strncpy(out->state, "Charge", sizeof(out->state) - 1);
  } else if (containsIgnoreCase(in, "System is idle")) {
    strncpy(out->state, "Idle", sizeof(out->state) - 1);
  } else if (containsIgnoreCase(in, "System is balancing")) {
    strncpy(out->state, "Balance", sizeof(out->state) - 1);
  } else if (containsIgnoreCase(in, "System is protect")) {
    strncpy(out->state, "Protect", sizeof(out->state) - 1);
  } else {
    strncpy(out->state, "Unknown", sizeof(out->state) - 1);
  }

  out->valid = (matched >= 4);
  if (out->valid) out->lastUpdateMs = millis();

  if (s_log) {
    char dbg[240];
    snprintf(dbg, sizeof(dbg),
             "PWRSYS valid=%d matched=%d SOC=%d SOH=%d U=%ldmV I=%ldmA RC=%ld FCC=%ld Vmax=%ld Vavg=%ld Vmin=%ld Tmax=%ld Tavg=%ld Tmin=%ld state=%s alarm=%s",
             out->valid ? 1 : 0, matched,
             out->soc, out->soh,
             out->voltage, out->current,
             out->rc, out->fcc,
             out->volt_high, out->volt_avg, out->volt_low,
             out->temp_high, out->temp_avg, out->temp_low,
             out->state, out->alarmState);
    s_log->Log(dbg);
  }

  return out->valid;
}
bool Parser::parseStat(const char* in, pylonBattery* batt) {
  if (!in || !batt) return false;

  long v = readLongAfterMulti(in, {
    "CYCLE Times",
    "Cycle Times",
    "Cycle times",
    "cycle times",
    "Cycles",
    "Cycle"
  });

  if (v == LONG_MIN) {
    const char* p = in;
    while (*p) {
      while (*p == '\r' || *p == '\n') ++p;
      if (!*p) break;

      const char* lineStart = p;
      while (*p && *p != '\r' && *p != '\n') ++p;

      char line[128];
      size_t n = (size_t)(p - lineStart);
      if (n >= sizeof(line)) n = sizeof(line) - 1;
      memcpy(line, lineStart, n);
      line[n] = 0;

      if (!containsIgnoreCase(line, "cycle")) continue;

      const char* q = line;
      while (*q && !isdigit((unsigned char)*q) && *q != '-' && *q != '+') ++q;
      if (!*q) continue;

      v = atol(q);
      break;
    }
  }

  if (v == LONG_MIN) return false;

  batt->cycleTimes = v;

  if (s_log) {
    char dbg[96];
    snprintf(dbg, sizeof(dbg), "STAT cycleTimes=%ld", batt->cycleTimes);
    s_log->Log(dbg);
  }

  return true;
}
