#ifndef PARSER_H
#define PARSER_H

#include "batteryStack.h"
#include <circular_log.h>

namespace Parser {
  void init(circular_log<16384>* log);
  bool parsePwr(const char* in, batteryStack* out);
  bool parsePwrsys(const char* in, systemData* out);
  bool parseStat(const char* in, pylonBattery* batt);
}

#endif // PARSER_H