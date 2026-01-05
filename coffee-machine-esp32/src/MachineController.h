#ifndef MACHINE_CONTROLLER_H
#define MACHINE_CONTROLLER_H

#include "HAL.h"
#include "SettingsManager.h"
#include <Arduino.h>

enum MachineState {
  IDLE,
  VALIDATE,
  DISPENSE_SOLIDS,
  HEAT_INTERNAL_PREHEAT,
  HEAT_INTERNAL_ACTIVE,
  HEAT_EXTERNAL,
  DISPENSE_LIQUID,
  MIX_DOWN,
  MIX_RUN,
  MIX_UP,
  DONE,
  ERROR_STATE,
  SAFE_STOP
};

enum DrinkMode {
  MODE_NONE,
  MODE Caffee,
  MODE_HOTWATER,
  MODE_NESCAFE,
  MODE_CLEANING
};

struct OrderParams {
  DrinkMode mode;

  // Coffee
  String brewBase; // Water | Milk

  // HotWater
  String hotLiquid; // water | milk_medium | milk_extra

  // Nescafe
  String milkRatio; // none | medium | extra

  // Common
  String size;  // Single | Double
  String sugar; // Low | Medium | High

  // Cleaning
  bool cleanMilk;
  bool cleanWater;
};

class MachineController {
public:
  MachineController(HAL &hal, SettingsManager &settings);

  void update(); // Non-blocking FSM update
  bool start(const OrderParams &params);
  void stop();

  bool isBusy() const { return state != IDLE && state != ERROR_STATE; }
  String getState() const;
  String getStep() const { return currentStep; }
  String getError() const { return errorMsg; }

private:
  HAL &hal;
  SettingsManager &settings;

  MachineState state;
  OrderParams order;
  Settings cfg;

  String currentStep;
  String errorMsg;

  unsigned long stateStartTime;
  unsigned long heaterStartTime;
  unsigned long pumpStartTime;
  unsigned long stepStartTime;

  float preheatTarget;
  int pumpDuration;
  int waterDuration;
  int milkDuration;

  void setState(MachineState newState);
  void setError(const String &error);
  bool checkCup();
  void safeStop();

  void updateCoffee();
  void updateHotWater();
  void updateNescafe();
  void updateCleaning();

  void runDispenseSolids();
  void runHeatInternalPreheat();
  void runHeatInternalActive();
  void runHeatExternal();
  void runDispenseLiquid();
  void runMixDown();
  void runMixRun();
  void runMixUp();

  int getSizeMultiplier();
  int getSugarMultiplier();
};

#endif
