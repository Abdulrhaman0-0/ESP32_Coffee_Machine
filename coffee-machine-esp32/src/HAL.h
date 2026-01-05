#ifndef HAL_H
#define HAL_H

#include "Config.h"
#include <Adafruit_MCP23X17.h>


class HAL {
public:
  HAL(Adafruit_MCP23X17 &mcp);
  bool initMCP();
  void begin();

  // Relay control (MCP23017)
  void relayOn(uint8_t relay);
  void relayOff(uint8_t relay);
  void allRelaysOff();

  // Sensors
  bool cupPresent();
  float readInternalTemp();
  float readExternalTemp(); // Telemetry only
  bool readLimitUpper();
  bool readLimitLower();

  bool isReady() { return mcpReady; }

private:
  Adafruit_MCP23X17 &mcp;
  bool mcpReady;

  // MAX6675 SPI read
  float readMAX6675(uint8_t cs);

  // Debounce
  bool debounceRead(uint8_t pin);
  uint8_t debounceState[2]; // Upper, Lower
  uint8_t debounceCount[2];
};

#endif
