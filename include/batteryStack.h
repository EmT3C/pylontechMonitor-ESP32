#ifndef BATTERYSTACK_H
#define BATTERYSTACK_H

#include <cstring>   // strcmp, strstr ...

#ifndef MAX_PYLON_BATTERIES
#define MAX_PYLON_BATTERIES 7
#endif

// Eine einzelne Pylontech-Batterie
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

  char baseState[9]     = {0}; // "Charge","Dischg","Idle","Balance",...
  char voltageState[9]  = {0}; // "Normal"/Alarm
  char currentState[9]  = {0};
  char tempState[9]     = {0};
  char time[20]         = {0};
  char b_v_st[9]        = {0};
  char b_t_st[9]        = {0};

  bool balancing = false;

  bool isCharging()    const { return std::strcmp(baseState, "Charge") == 0; }
  bool isDischarging() const { return std::strcmp(baseState, "Dischg") == 0; }
  bool isIdle()        const { return std::strcmp(baseState, "Idle")   == 0; }
  bool isBalancing()   const { return balancing; }

  bool isNormal() const {
    if (!isCharging() && !isDischarging() && !isIdle() && !isBalancing())
      return false;
    return  std::strcmp(voltageState, "Normal") == 0 &&
            std::strcmp(currentState, "Normal") == 0 &&
            std::strcmp(tempState,    "Normal") == 0 &&
            std::strcmp(b_v_st,       "Normal") == 0 &&
            std::strcmp(b_t_st,       "Normal") == 0;
  }
};

// Aggregierte Systemwerte
struct systemData {
  int  soc        = 0;
  int  soh        = 0;
  long voltage    = 0;  // mV
  long current    = 0;  // mA

  long temp_high  = 0;  // m°C
  long temp_avg   = 0;  // m°C
  long temp_low   = 0;  // m°C

  long volt_high  = 0;  // mV
  long volt_avg   = 0;  // mV
  long volt_low   = 0;  // mV

  long rc         = 0;  // mAh
  long fcc        = 0;  // mAh
};

// Gesamter Stack
struct batteryStack {
  int  batteryCount = 0;
  int  soc          = 0;     // %
  int  temp         = 0;     // m°C
  int  tempr        = 0;     // m°C
  long currentDC    = 0;     // mA
  long avgVoltage   = 0;     // mV
  char baseState[9] = {0};

  pylonBattery batts[MAX_PYLON_BATTERIES];

  bool anyBalancing = false;

  bool isNormal() const {
    for (int i = 0; i < MAX_PYLON_BATTERIES; ++i) {
      if (batts[i].isPresent && !batts[i].isNormal()) return false;
    }
    return true;
  }

  long getPowerDC() const {
    // (mA -> A) * (mV -> V)
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

// Nur Deklarationen:
//extern batteryStack g_stack;
//extern systemData   g_systemStack;

// Optional: nur Deklaration, Implementierung kommt in Parser.cpp
bool parsePwrsysResponse(const char* in);

#endif // BATTERYSTACK_H
