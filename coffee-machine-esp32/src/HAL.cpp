#include "HAL.h"
#include "Logger.h"
#include <SPI.h>

HAL::HAL(Adafruit_MCP23X17 &mcpRef) : mcp(mcpRef), mcpReady(false) {
  debounceState[0] = debounceState[1] = HIGH;
  debounceCount[0] = debounceCount[1] = 0;
}

bool HAL::initMCP() {
  if (!mcp.begin_I2C(MCP23017_ADDR)) {
    return false;
  }

  // Configure all as outputs
  for (uint8_t i = 0; i < 16; i++) {
    mcp.pinMode(i, OUTPUT);
  }

  // Set all relays OFF (HIGH if active-low)
  allRelaysOff();

  // Verify
  uint16_t check = mcp.readGPIOAB();
  if (check != 0xFFFF) {
    LOG_WARN("HAL", "MCP23017 verification warning");
  }

  mcpReady = true;
  LOG_INFO("HAL", "MCP23017 initialized, all relays OFF");
  return true;
}

void HAL::begin() {
  // Init SPI for MAX6675
  SPI.begin(SPI_SCK, SPI_MISO, -1, -1);
  pinMode(CS_INTERNAL, OUTPUT);
  pinMode(CS_EXTERNAL, OUTPUT);
  digitalWrite(CS_INTERNAL, HIGH);
  digitalWrite(CS_EXTERNAL, HIGH);

  // Init ultrasonic
  pinMode(ULTRASONIC_TRIG, OUTPUT);
  pinMode(ULTRASONIC_ECHO, INPUT);

  // Init limit switches
  pinMode(LIMIT_UPPER, INPUT_PULLUP);
  pinMode(LIMIT_LOWER, INPUT_PULLUP);

  LOG_INFO("HAL", "Sensors initialized");
}

void HAL::relayOn(uint8_t relay) {
  if (!mcpReady || relay > RELAY_MIXER_DOWN)
    return;

  if (RELAY_ACTIVE_LOW) {
    mcp.digitalWrite(relay, LOW);
  } else {
    mcp.digitalWrite(relay, HIGH);
  }
}

void HAL::relayOff(uint8_t relay) {
  if (!mcpReady || relay > RELAY_MIXER_DOWN)
    return;

  if (RELAY_ACTIVE_LOW) {
    mcp.digitalWrite(relay, HIGH);
  } else {
    mcp.digitalWrite(relay, LOW);
  }
}

void HAL::allRelaysOff() {
  if (!mcpReady)
    return;

  for (uint8_t i = 0; i <= RELAY_MIXER_DOWN; i++) {
    relayOff(i);
  }
}

bool HAL::cupPresent() {
  digitalWrite(ULTRASONIC_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG, LOW);

  long duration = pulseIn(ULTRASONIC_ECHO, HIGH, 30000);
  if (duration == 0)
    return false;

  float distance = (duration * 0.034) / 2.0;
  return (distance > 0 && distance < CUP_DETECT_THRESHOLD_CM);
}

float HAL::readMAX6675(uint8_t cs) {
  digitalWrite(cs, LOW);
  delayMicroseconds(1);

  uint16_t data = 0;
  data = SPI.transfer(0x00) << 8;
  data |= SPI.transfer(0x00);

  digitalWrite(cs, HIGH);

  // Check fault bit (D2)
  if (data & 0x0004) {
    return NAN;
  }

  // Extract temp (bits 3-14)
  data >>= 3;
  float temp = data * 0.25;

  return temp;
}

float HAL::readInternalTemp() { return readMAX6675(CS_INTERNAL); }

float HAL::readExternalTemp() { return readMAX6675(CS_EXTERNAL); }

bool HAL::debounceRead(uint8_t pin) {
  bool current = digitalRead(pin);
  uint8_t idx = (pin == LIMIT_UPPER) ? 0 : 1;

  if (current == debounceState[idx]) {
    debounceCount[idx]++;
    if (debounceCount[idx] >= DEBOUNCE_READS) {
      return current;
    }
  } else {
    debounceState[idx] = current;
    debounceCount[idx] = 1;
  }

  return !current; // Return opposite while debouncing
}

bool HAL::readLimitUpper() {
  return debounceRead(LIMIT_UPPER) == LOW; // Active low
}

bool HAL::readLimitLower() { return debounceRead(LIMIT_LOWER) == LOW; }
