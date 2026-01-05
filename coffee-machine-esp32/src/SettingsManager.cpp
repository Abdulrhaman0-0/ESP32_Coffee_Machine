#include "SettingsManager.h"
#include "Logger.h"

SettingsManager::SettingsManager() { loadDefaults(); }

void SettingsManager::begin() {
  prefs.begin("coffee", false);

  // Load or create defaults
  if (!prefs.isKey("tank1Time")) {
    LOG_INFO("SETTINGS", "No saved settings, using defaults");
    setDefaults();
  } else {
    current.tank1Time = prefs.getInt("tank1Time", 2);
    current.tank2Time = prefs.getInt("tank2Time", 3);
    current.tank3Time = prefs.getInt("tank3Time", 3);
    current.waterPumpTime = prefs.getInt("waterPumpTime", 5);
    current.milkPumpTime = prefs.getInt("milkPumpTime", 4);
    current.intHeaterTime = prefs.getInt("intHeaterTime", 30);
    current.intHeaterTemp = prefs.getInt("intHeaterTemp", 95);
    current.extHeaterTime = prefs.getInt("extHeaterTime", 45);
    current.extHeaterTemp = prefs.getInt("extHeaterTemp", 90);
    current.mixerTime = prefs.getInt("mixerTime", 10);
    LOG_INFO("SETTINGS", "Loaded from NVS");
  }
}

void SettingsManager::loadDefaults() {
  current.tank1Time = 2;
  current.tank2Time = 3;
  current.tank3Time = 3;
  current.waterPumpTime = 5;
  current.milkPumpTime = 4;
  current.intHeaterTime = 30;
  current.intHeaterTemp = 95;
  current.extHeaterTime = 45;
  current.extHeaterTemp = 90;
  current.mixerTime = 10;
}

Settings SettingsManager::get() { return current; }

bool SettingsManager::validate(const Settings &s) {
  return (s.tank1Time >= 0 && s.tank1Time <= 30 && s.tank2Time >= 0 &&
          s.tank2Time <= 30 && s.tank3Time >= 0 && s.tank3Time <= 30 &&
          s.waterPumpTime >= 0 && s.waterPumpTime <= 60 &&
          s.milkPumpTime >= 0 && s.milkPumpTime <= 60 &&
          s.intHeaterTime >= 10 && s.intHeaterTime <= 120 &&
          s.intHeaterTemp >= 60 && s.intHeaterTemp <= 100 &&
          s.extHeaterTime >= 10 && s.extHeaterTime <= 180 &&
          s.extHeaterTemp >= 60 && s.extHeaterTemp <= 100 && s.mixerTime >= 5 &&
          s.mixerTime <= 60);
}

bool SettingsManager::save(const Settings &s) {
  if (!validate(s)) {
    LOG_ERROR("SETTINGS", "Validation failed");
    return false;
  }

  current = s;

  prefs.putInt("tank1Time", s.tank1Time);
  prefs.putInt("tank2Time", s.tank2Time);
  prefs.putInt("tank3Time", s.tank3Time);
  prefs.putInt("waterPumpTime", s.waterPumpTime);
  prefs.putInt("milkPumpTime", s.milkPumpTime);
  prefs.putInt("intHeaterTime", s.intHeaterTime);
  prefs.putInt("intHeaterTemp", s.intHeaterTemp);
  prefs.putInt("extHeaterTime", s.extHeaterTime);
  prefs.putInt("extHeaterTemp", s.extHeaterTemp); // Saved but not used
  prefs.putInt("mixerTime", s.mixerTime);

  LOG_INFO("SETTINGS", "Saved to NVS");
  return true;
}

void SettingsManager::setDefaults() {
  loadDefaults();
  save(current);
}
