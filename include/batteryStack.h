#ifndef BATTERYSTACK_H
#define BATTERYSTACK_H

#include <cstring>

#ifndef MAX_PYLON_BATTERIES
#define MAX_PYLON_BATTERIES 6
#endif

struct pylonBattery {
  bool isPresent = false;

  long soc = 0;        // %
  long voltage = 0;    // mV
  long current = 0;    // mA
  long tempr = 0;      // m°C

  long cellTempLow  = 0; // m°C
  long cellTempHigh = 0; // m°C
  long cellVoltLow  = 0; // mV
  long cellVoltHigh = 0; // mV
  long cycleTimes = 0;
  
  char baseState[16]    = {0};
  char voltageState[16] = {0};
  char currentState[16] = {0};
  char tempState[16]    = {0};
  char time[20]         = {0};
  char b_v_st[16]       = {0};
  char b_t_st[16]       = {0};
  char alarmText[48]    = {0};

  bool balancing = false;

  bool stateEquals(const char* expected) const {
    return std::strcmp(baseState, expected) == 0;
  }

  bool isCharging()    const { return std::strcmp(baseState, "Charge")  == 0; }
  bool isDischarging() const { return std::strcmp(baseState, "Dischg")  == 0; }
  bool isIdle()        const { return std::strcmp(baseState, "Idle")    == 0; }
  bool isBalancing()   const { return balancing || stateEquals("Balance"); }
  bool isProtect()     const { return stateEquals("Protect"); }
  bool isAlarm()       const { return stateEquals("Alarm") || stateEquals("Alarm!"); }
  bool hasAlarm()      const { return !isNormal() || isProtect() || isAlarm(); }

  bool isNormal() const {
    if (!isCharging() && !isDischarging() && !isIdle() && !isBalancing())
      return false;

    return std::strcmp(voltageState, "Normal") == 0 &&
           std::strcmp(currentState, "Normal") == 0 &&
           std::strcmp(tempState,    "Normal") == 0 &&
           std::strcmp(b_v_st,       "Normal") == 0 &&
           std::strcmp(b_t_st,       "Normal") == 0;
  }
};

struct systemData {
  int  soc        = -1;
  int  soh        = -1;
  long voltage    = 0;   // mV
  long current    = 0;   // mA

  long temp_high  = 0;   // m°C
  long temp_avg   = 0;   // m°C
  long temp_low   = 0;   // m°C

  long volt_high  = 0;   // mV
  long volt_avg   = 0;   // mV
  long volt_low   = 0;   // mV

  long rc         = 0;   // mAh
  long fcc        = 0;   // mAh

  long rec_chg_voltage = 0;       // mV
  long rec_dsg_voltage = 0;       // mV
  long rec_chg_current = 0;       // mA
  long rec_dsg_current = 0;       // mA

  long sys_rec_chg_voltage = 0;   // mV
  long sys_rec_dsg_voltage = 0;   // mV
  long sys_rec_chg_current = 0;   // mA
  long sys_rec_dsg_current = 0;   // mA

  char state[16]      = {0};
  char alarmState[16] = {0};

  bool valid = false;
  unsigned long lastUpdateMs = 0;
};

struct batteryStack {
  int  batteryCount = 0;
  int  soc          = 0;     // %
  int  temp         = 0;     // m°C
  int  tempr        = 0;     // m°C
  long currentDC    = 0;     // mA
  long avgVoltage   = 0;     // mV
  char baseState[16] = {0};

  pylonBattery batts[MAX_PYLON_BATTERIES];

  bool anyBalancing = false;
  bool valid = false;
  unsigned long lastUpdateMs = 0;

  bool isNormal() const {
    if (!valid || batteryCount <= 0) return false;
    for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
      if (batts[i].isPresent && !batts[i].isNormal()) return false;
    }
    return true;
  }

  long getPowerDC() const {
    return static_cast<long>((currentDC / 1000.0) * (avgVoltage / 1000.0));
  }

  float powerIN() const {
    return (currentDC > 0)
      ? (currentDC / 1000.0f) * (avgVoltage / 1000.0f)
      : 0.0f;
  }

  float powerOUT() const {
    return (currentDC < 0)
      ? -(currentDC / 1000.0f) * (avgVoltage / 1000.0f)
      : 0.0f;
  }

  long getEstPowerAc() const {
    const double p = getPowerDC();
    if (p == 0.0) return 0;

    if (p < 0.0) {
      if (p < -1000.0)     return static_cast<long>(p * 0.94);
      else if (p < -600.0) return static_cast<long>(p * 0.90);
      else                 return static_cast<long>(p * 0.87);
    } else {
      if (p > 1000.0)      return static_cast<long>(p * 1.06);
      else if (p > 600.0)  return static_cast<long>(p * 1.10);
      else                 return static_cast<long>(p * 1.13);
    }
  }
};

struct dailyEnergyData {
  bool valid = false;
  bool timeSynced = false;
  unsigned long lastUpdateMs = 0;
  unsigned long currentEpoch = 0;
  unsigned long localDayNumber = 0;
  float chargeKWhToday = 0.0f;
  float dischargeKWhToday = 0.0f;
};

#endif // BATTERYSTACK_H
