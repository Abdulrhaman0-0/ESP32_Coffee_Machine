#include "MachineController.h"
#include "Logger.h"

MachineController::MachineController(HAL &halRef, SettingsManager &settingsRef)
    : hal(halRef), settings(settingsRef), state(IDLE) {
  order.mode = MODE_NONE;
  stateStartTime = 0;
  heaterStartTime = 0;
  pumpStartTime = 0;
  stepStartTime = 0;
}

bool MachineController::start(const OrderParams &params) {
  if (!hal.isReady()) {
    setError("NOT_READY");
    return false;
  }

  if (isBusy()) {
    setError("BUSY");
    return false;
  }

  order = params;
  cfg = settings.get();
  errorMsg = "";

  setState(VALIDATE);
  LOG_INFO("FSM", String("Start: ") + String((int)order.mode));
  return true;
}

void MachineController::stop() {
  LOG_ERROR("FSM", "Emergency stop");
  safeStop();
}

void MachineController::setState(MachineState newState) {
  state = newState;
  stateStartTime = millis();

  String stateStr[] = {"IDLE",
                       "VALIDATE",
                       "DISPENSE_SOLIDS",
                       "HEAT_INTERNAL_PREHEAT",
                       "HEAT_INTERNAL_ACTIVE",
                       "HEAT_EXTERNAL",
                       "DISPENSE_LIQUID",
                       "MIX_DOWN",
                       "MIX_RUN",
                       "MIX_UP",
                       "DONE",
                       "ERROR",
                       "SAFE_STOP"};
  LOG_INFO("FSM", String("State: ") + stateStr[newState]);
}

void MachineController::setError(const String &error) {
  errorMsg = error;
  setState(ERROR_STATE);
  safeStop();
  LOG_ERROR("FSM", String("Error: ") + error);
}

void MachineController::safeStop() {
  hal.allRelaysOff();
  currentStep = "Stopped";
}

bool MachineController::checkCup() {
  if (!hal.cupPresent()) {
    if (state == IDLE || state == VALIDATE) {
      setError("NO_CUP");
    } else {
      setError("NO_CUP_DURING_RUN");
      safeStop();
    }
    return false;
  }
  return true;
}

int MachineController::getSizeMultiplier() {
  return (order.size == "Double") ? 2 : 1;
}

int MachineController::getSugarMultiplier() {
  if (order.sugar == "High")
    return 4;
  if (order.sugar == "Medium")
    return 2;
  return 1;
}

String MachineController::getState() const {
  String states[] = {"IDLE",
                     "VALIDATE",
                     "DISPENSE_SOLIDS",
                     "HEAT_INTERNAL_PREHEAT",
                     "HEAT_INTERNAL_ACTIVE",
                     "HEAT_EXTERNAL",
                     "DISPENSE_LIQUID",
                     "MIX_DOWN",
                     "MIX_RUN",
                     "MIX_UP",
                     "DONE",
                     "ERROR",
                     "SAFE_STOP"};
  return states[state];
}

void MachineController::update() {
  if (state == IDLE || state == ERROR_STATE)
    return;

  switch (order.mode) {
  case MODE_COFFEE:
    updateCoffee();
    break;
  case MODE_HOTWATER:
    updateHotWater();
    break;
  case MODE_NESCAFE:
    updateNescafe();
    break;
  case MODE_CLEANING:
    updateCleaning();
    break;
  default:
    setError("BAD_MODE");
  }
}

void MachineController::updateCoffee() {
  switch (state) {
  case VALIDATE:
    if (!checkCup())
      return;
    setState(DISPENSE_SOLIDS);
    break;

  case DISPENSE_SOLIDS:
    runDispenseSolids();
    if (state == DISPENSE_SOLIDS &&
        millis() - stateStartTime > (getSugarMultiplier() * cfg.tank1Time +
                                     getSizeMultiplier() * cfg.tank2Time) *
                                        1000) {
      hal.relayOff(RELAY_TANK1_SUGAR);
      hal.relayOff(RELAY_TANK2_COFFEE);
      setState(DISPENSE_LIQUID);
    }
    break;

  case DISPENSE_LIQUID:
    runDispenseLiquid();
    break;

  case HEAT_EXTERNAL:
    runHeatExternal();
    break;

  case MIX_DOWN:
    runMixDown();
    break;

  case MIX_RUN:
    runMixRun();
    break;

  case MIX_UP:
    runMixUp();
    break;

  case DONE:
    hal.allRelaysOff();
    setState(IDLE);
    currentStep = "";
    LOG_INFO("FSM", "Coffee cycle complete");
    break;
  }
}

void MachineController::updateHotWater() {
  switch (state) {
  case VALIDATE:
    if (!checkCup())
      return;
    setState(DISPENSE_SOLIDS);
    break;

  case DISPENSE_SOLIDS:
    runDispenseSolids();
    if (state == DISPENSE_SOLIDS &&
        millis() - stateStartTime >
            getSugarMultiplier() * cfg.tank1Time * 1000) {
      hal.relayOff(RELAY_TANK1_SUGAR);
      setState(HEAT_INTERNAL_PREHEAT);
    }
    break;

  case HEAT_INTERNAL_PREHEAT:
    runHeatInternalPreheat();
    break;

  case HEAT_INTERNAL_ACTIVE:
    runHeatInternalActive();
    break;

  case MIX_DOWN:
    runMixDown();
    break;

  case MIX_RUN:
    runMixRun();
    break;

  case MIX_UP:
    runMixUp();
    break;

  case DONE:
    hal.allRelaysOff();
    setState(IDLE);
    currentStep = "";
    LOG_INFO("FSM", "HotWater cycle complete");
    break;
  }
}

void MachineController::updateNescafe() {
  switch (state) {
  case VALIDATE:
    if (!checkCup())
      return;
    setState(DISPENSE_SOLIDS);
    break;

  case DISPENSE_SOLIDS:
    runDispenseSolids();
    if (state == DISPENSE_SOLIDS &&
        millis() - stateStartTime > (getSugarMultiplier() * cfg.tank1Time +
                                     getSizeMultiplier() * cfg.tank3Time) *
                                        1000) {
      hal.relayOff(RELAY_TANK1_SUGAR);
      hal.relayOff(RELAY_TANK3_NESCAFE);
      setState(HEAT_INTERNAL_PREHEAT);
    }
    break;

  case HEAT_INTERNAL_PREHEAT:
    runHeatInternalPreheat();
    break;

  case HEAT_INTERNAL_ACTIVE:
    runHeatInternalActive();
    break;

  case MIX_DOWN:
    runMixDown();
    break;

  case MIX_RUN:
    runMixRun();
    break;

  case MIX_UP:
    runMixUp();
    break;

  case DONE:
    hal.allRelaysOff();
    setState(IDLE);
    currentStep = "";
    LOG_INFO("FSM", "Nescafe cycle complete");
    break;
  }
}

void MachineController::updateCleaning() {
  switch (state) {
  case VALIDATE:
    if (!checkCup())
      return;
    setState(DISPENSE_LIQUID);
    break;

  case DISPENSE_LIQUID:
    runDispenseLiquid();
    break;

  case DONE:
    hal.allRelaysOff();
    setState(IDLE);
    currentStep = "";
    LOG_INFO("FSM", "Cleaning cycle complete");
    break;
  }
}

void MachineController::runDispenseSolids() {
  if (!checkCup())) return;
  currentStep = "Dispensing solids";

  if (order.mode == MODE_COFFEE) {
    hal.relayOn(RELAY_TANK1_SUGAR);
    hal.relayOn(RELAY_TANK2_COFFEE);
  } else if (order.mode == MODE_NESCAFE) {
    hal.relayOn(RELAY_TANK1_SUGAR);
    hal.relayOn(RELAY_TANK3_NESCAFE);
  } else if (order.mode == MODE_HOTWATER) {
    hal.relayOn(RELAY_TANK1_SUGAR);
  }
}

void MachineController::runHeatInternalPreheat() {
  if (!checkCup())
    return;
  currentStep = "Preheating";

  if (heaterStartTime == 0) {
    heaterStartTime = millis();
    hal.relayOn(RELAY_HEATER_INTERNAL);
    preheatTarget = cfg.intHeaterTemp - 5.0;
  }

  unsigned long elapsed = millis() - heaterStartTime;
  if (elapsed > cfg.intHeaterTime * 1000) {
    setError("HEAT_TIMEOUT");
    hal.relayOff(RELAY_HEATER_INTERNAL);
    return;
  }

  float temp = hal.readInternalTemp();
  if (!isnan(temp) && temp >= preheatTarget) {
    setState(HEAT_INTERNAL_ACTIVE);
    pumpStartTime = 0;
  }
}

void MachineController::runHeatInternalActive() {
  if (!checkCup())
    return;
  currentStep = "Heating and pumping";

  if (pumpStartTime == 0) {
    pumpStartTime = millis();

    // Calculate durations based on mode
    if (order.mode == MODE_HOTWATER) {
      // HotWater: exclusive water OR milk
      if (order.hotLiquid == "water") {
        pumpDuration = getSizeMultiplier() * cfg.waterPumpTime * 1000;
        hal.relayOn(RELAY_PUMP_WATER);
        LOG_INFO("HW", "Water only");
      } else if (order.hotLiquid == "milk_medium") {
        pumpDuration = getSizeMultiplier() * cfg.milkPumpTime * 1000;
        hal.relayOn(RELAY_PUMP_MILK);
        LOG_INFO("HW", "Milk medium");
      } else if (order.hotLiquid == "milk_extra") {
        pumpDuration = getSizeMultiplier() * cfg.milkPumpTime * 2000; // 2x
        hal.relayOn(RELAY_PUMP_MILK);
        LOG_INFO("HW", "Milk extra");
      }

    } else if (order.mode == MODE_NESCAFE) {
      // Nescafe: ratio mixing with separate base times
      int waterTime = getSizeMultiplier() * cfg.waterPumpTime * 1000;
      int milkTime = getSizeMultiplier() * cfg.milkPumpTime * 1000;

      if (order.milkRatio == "none") {
        waterDuration = waterTime;
        milkDuration = 0;
      } else if (order.milkRatio == "medium") {
        waterDuration = waterTime * 0.75;
        milkDuration = milkTime * 0.25;
      } else if (order.milkRatio == "extra") {
        waterDuration = waterTime * 0.5;
        milkDuration = milkTime * 0.5;
      }

      hal.relayOn(RELAY_PUMP_WATER);
      pumpDuration = waterDuration + milkDuration;
      LOG_INFO("NES", String("Water:") + String(waterDuration) +
                          " Milk:" + String(milkDuration));
    }
  }

  unsigned long pumpElapsed = millis() - pumpStartTime;
  unsigned long heatElapsed = millis() - heaterStartTime;

  // Heat timeout check
  if (heatElapsed > cfg.intHeaterTime * 1000) {
    setError("HEAT_TIMEOUT");
    hal.relayOff(RELAY_HEATER_INTERNAL);
    hal.relayOff(RELAY_PUMP_WATER);
    hal.relayOff(RELAY_PUMP_MILK);
    return;
  }

  // Bang-bang heater control
  float temp = hal.readInternalTemp();
  if (!isnan(temp)) {
    if (temp < cfg.intHeaterTemp - 2.0) {
      hal.relayOn(RELAY_HEATER_INTERNAL);
    } else if (temp > cfg.intHeaterTemp + 2.0) {
      hal.relayOff(RELAY_HEATER_INTERNAL);
    }

    // Absolute safety check
    if (temp > INTERNAL_HEATER_ABS_MAX) {
      setError("SENSOR_FAIL");
      hal.relayOff(RELAY_HEATER_INTERNAL);
      return;
    }
  }

  // Nescafe milk switch
  if (order.mode == MODE_NESCAFE && pumpElapsed > waterDuration &&
      milkDuration > 0) {
    hal.relayOff(RELAY_PUMP_WATER);
    hal.relayOn(RELAY_PUMP_MILK);
  }

  // Pump complete
  if (pumpElapsed >= pumpDuration) {
    hal.relayOff(RELAY_PUMP_WATER);
    hal.relayOff(RELAY_PUMP_MILK);
    hal.relayOff(RELAY_HEATER_INTERNAL);
    heaterStartTime = 0;
    setState(MIX_DOWN);
  }
}

void MachineController::runHeatExternal() {
  if (!checkCup())
    return;
  currentStep = "Cup warming";

  if (stepStartTime == 0) {
    stepStartTime = millis();
    hal.relayOn(RELAY_HEATER_EXTERNAL);
    LOG_INFO("HW", String("External heater ON for ") +
                       String(cfg.extHeaterTime) + "s");
  }

  unsigned long elapsed = millis() - stepStartTime;

  // Timer-only control (extHeaterTemp IGNORED)
  if (elapsed >= cfg.extHeaterTime * 1000) {
    hal.relayOff(RELAY_HEATER_EXTERNAL);
    stepStartTime = 0;
    setState(MIX_DOWN);
    LOG_INFO("HW", "External heater OFF");
  }
}

void MachineController::runDispenseLiquid() {
  if (!checkCup())
    return;

  if (order.mode == MODE_COFFEE) {
    currentStep = "Dispensing liquid";

    if (pumpStartTime == 0) {
      pumpStartTime = millis();

      if (order.brewBase == "Water") {
        pumpDuration = getSizeMultiplier() * cfg.waterPumpTime * 1000;
        hal.relayOn(RELAY_PUMP_WATER);
      } else {
        pumpDuration = getSizeMultiplier() * cfg.milkPumpTime * 1000;
        hal.relayOn(RELAY_PUMP_MILK);
      }
    }

    if (millis() - pumpStartTime >= pumpDuration) {
      hal.relayOff(RELAY_PUMP_WATER);
      hal.relayOff(RELAY_PUMP_MILK);
      pumpStartTime = 0;
      setState(HEAT_EXTERNAL); // Coffee goes to external heater
    }

  } else if (order.mode == MODE_CLEANING) {
    currentStep = "Cleaning";

    if (pumpStartTime == 0) {
      pumpStartTime = millis();

      if (order.cleanWater) {
        hal.relayOn(RELAY_PUMP_WATER);
      }
      if (order.cleanMilk) {
        hal.relayOn(RELAY_PUMP_MILK);
      }

      pumpDuration = max(order.cleanWater ? cfg.waterPumpTime : 0,
                         order.cleanMilk ? cfg.milkPumpTime : 0) *
                     1000;
    }

    if (millis() - pumpStartTime >= pumpDuration) {
      hal.relayOff(RELAY_PUMP_WATER);
      hal.relayOff(RELAY_PUMP_MILK);
      pumpStartTime = 0;
      setState(DONE); // Cleaning does not mix
    }
  }
}

void MachineController::runMixDown() {
  if (!checkCup())
    return;
  currentStep = "Mixer moving down";

  if (stepStartTime == 0) {
    stepStartTime = millis();

    // Check both limits not pressed
    if (hal.readLimitUpper() && hal.readLimitLower()) {
      setError("LIMIT_INVALID");
      return;
    }

    hal.relayOn(RELAY_MIXER_DOWN);
  }

  if (hal.readLimitLower()) {
    hal.relayOff(RELAY_MIXER_DOWN);
    stepStartTime = 0;
    setState(MIX_RUN);
  } else if (millis() - stepStartTime > LIMIT_TIMEOUT_MS) {
    hal.relayOff(RELAY_MIXER_DOWN);
    setError("TIMEOUT_LIMIT");
  }
}

void MachineController::runMixRun() {
  if (!checkCup())
    return;
  currentStep = "Mixing";

  if (stepStartTime == 0) {
    stepStartTime = millis();
    hal.relayOn(RELAY_MIXER_ROTATE);
  }

  if (millis() - stepStartTime >= cfg.mixerTime * 1000) {
    hal.relayOff(RELAY_MIXER_ROTATE);
    stepStartTime = 0;
    setState(MIX_UP);
  }
}

void MachineController::runMixUp() {
  if (!checkCup())
    return;
  currentStep = "Mixer moving up";

  if (stepStartTime == 0) {
    stepStartTime = millis();
    hal.relayOn(RELAY_MIXER_UP);
  }

  if (hal.readLimitUpper()) {
    hal.relayOff(RELAY_MIXER_UP);
    stepStartTime = 0;
    setState(DONE);
  } else if (millis() - stepStartTime > LIMIT_TIMEOUT_MS) {
    hal.relayOff(RELAY_MIXER_UP);
    setError("TIMEOUT_LIMIT");
  }
}
