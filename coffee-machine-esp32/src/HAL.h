#ifndef HAL_H
#define HAL_H

#include "Config.h"
#include <max6675.h>


class HAL {
public:
  HAL();
  void begin();

  // Relay control (GPIO)
  void relayOn(uint8_t relay);
  void relayOff(uint8_t relay);
  void allRelaysOff();

  // Sensors
  bool cupPresent();
  float readInternalTemp();
  float readExternalTemp(); // Telemetry only
  bool readLimitUpper();
  bool readLimitLower();

  bool isReady() { return ready; }

private:
  bool ready;

  MAX6675 internalThermocouple;
  MAX6675 externalThermocouple;

  float readThermocouple(MAX6675 &sensor);

  // Debounce
  bool debounceRead(uint8_t pin);
  uint8_t debounceState[2]; // Upper, Lower
  uint8_t debounceCount[2];
};

#endif
