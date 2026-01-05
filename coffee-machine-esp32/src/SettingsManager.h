#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include <Preferences.h>

struct Settings {
  int tank1Time;
  int tank2Time;
  int tank3Time;
  int waterPumpTime;
  int milkPumpTime;
  int intHeaterTime;
  int intHeaterTemp;
  int extHeaterTime;
  int extHeaterTemp; // Accepted but ignored
  int mixerTime;
};

class SettingsManager {
public:
  SettingsManager();
  void begin();
  Settings get();
  bool save(const Settings &s);
  void setDefaults();

private:
  Preferences prefs;
  Settings current;
  void loadDefaults();
  bool validate(const Settings &s);
};

#endif
