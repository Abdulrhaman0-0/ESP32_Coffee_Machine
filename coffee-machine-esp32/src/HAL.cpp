#include "HAL.h"
#include "Logger.h"

HAL::HAL()
    : ready(false), internalThermocouple(SPI_SCK, CS_INTERNAL, SPI_MISO),
      externalThermocouple(SPI_SCK, CS_EXTERNAL, SPI_MISO) {
  debounceState[0] = debounceState[1] = HIGH;
  debounceCount[0] = debounceCount[1] = 0;
}

void HAL::begin() {
  // Init relays
  const uint8_t relayPins[] = {RELAY_TANK1_SUGAR,     RELAY_TANK2_COFFEE,
                               RELAY_TANK3_NESCAFE,   RELAY_PUMP_WATER,
                               RELAY_PUMP_MILK,       RELAY_HEATER_INTERNAL,
                               RELAY_HEATER_EXTERNAL, RELAY_MIXER_ROTATE,
                               RELAY_MIXER_UP,        RELAY_MIXER_DOWN};
  for (uint8_t pin : relayPins) {
    pinMode(pin, OUTPUT);
  }
  ready = true;
  allRelaysOff();

  // Init ultrasonic
  pinMode(ULTRASONIC_TRIG, OUTPUT);
  pinMode(ULTRASONIC_ECHO, INPUT);

  // Init limit switches
  pinMode(LIMIT_UPPER, INPUT_PULLUP);
  pinMode(LIMIT_LOWER, INPUT_PULLUP);

  LOG_INFO("HAL", "Sensors initialized");
}

void HAL::relayOn(uint8_t relay) {
  if (!ready)
    return;

  if (RELAY_ACTIVE_LOW) {
    digitalWrite(relay, LOW);
  } else {
    digitalWrite(relay, HIGH);
  }
}

void HAL::relayOff(uint8_t relay) {
  if (!ready)
    return;

  if (RELAY_ACTIVE_LOW) {
    digitalWrite(relay, HIGH);
  } else {
    digitalWrite(relay, LOW);
  }
}

void HAL::allRelaysOff() {
  if (!ready)
    return;

  relayOff(RELAY_TANK1_SUGAR);
  relayOff(RELAY_TANK2_COFFEE);
  relayOff(RELAY_TANK3_NESCAFE);
  relayOff(RELAY_PUMP_WATER);
  relayOff(RELAY_PUMP_MILK);
  relayOff(RELAY_HEATER_INTERNAL);
  relayOff(RELAY_HEATER_EXTERNAL);
  relayOff(RELAY_MIXER_ROTATE);
  relayOff(RELAY_MIXER_UP);
  relayOff(RELAY_MIXER_DOWN);
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

float HAL::readThermocouple(MAX6675 &sensor) {
  double temp = sensor.readCelsius();
  if (isnan(temp)) {
    return NAN;
  }
  return static_cast<float>(temp);
}

float HAL::readInternalTemp() { return readThermocouple(internalThermocouple); }

float HAL::readExternalTemp() { return readThermocouple(externalThermocouple); }

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
