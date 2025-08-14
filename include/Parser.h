// Parser.h
#ifndef PARSER_H
#define PARSER_H

#include "batteryStack.h"
#include <circular_log.h>

namespace Parser {
  void init(circular_log<7000>* log);
  bool parsePwr(const char* in, batteryStack* out);
  bool parsePwrsys(const char* in, systemData* out);
}

#endif // PARSER_H
