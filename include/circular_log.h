#ifndef CIRCULAR_LOG_H
#define CIRCULAR_LOG_H

#include <Arduino.h>
#include <stdio.h>   // für snprintf in C-API (kein std::)

template <size_t Size>
class circular_log {
 private:
  char buffer[Size];
  size_t index = 0;
  bool filled = false;

 public:
  circular_log() {
    memset(buffer, 0, Size);
  }

  void Log(const char* msg) {
    size_t len = strlen(msg);
    for (size_t i = 0; i < len; ++i) {
      buffer[index++] = msg[i];
      if (index >= Size) {
        index = 0;
        filled = true;
      }
    }
  }

  const char* c_str() {
    static char out[Size + 1];
    if (filled) {
      memcpy(out, buffer + index, Size - index);
      memcpy(out + Size - index, buffer, index);
      out[Size] = 0;
    } else {
      memcpy(out, buffer, index);
      out[index] = 0;
    }
    return out;
  }
};

#endif // CIRCULAR_LOG_H
