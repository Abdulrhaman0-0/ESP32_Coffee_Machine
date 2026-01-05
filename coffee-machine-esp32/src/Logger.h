#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

#define LOG_INFO(module, msg)                                                  \
  Serial.printf("[%lu][INFO][%s] %s\n", millis(), module, msg)
#define LOG_WARN(module, msg)                                                  \
  Serial.printf("[%lu][WARN][%s] %s\n", millis(), module, msg)
#define LOG_ERROR(module, msg)                                                 \
  Serial.printf("[%lu][ERROR][%s] %s\n", millis(), module, msg)
#define LOG_DEBUG(module, msg)                                                 \
  Serial.printf("[%lu][DEBUG][%s] %s\n", millis(), module, msg)

#endif
