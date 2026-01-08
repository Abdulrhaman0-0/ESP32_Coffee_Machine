/*
  CoffeeMachine.ino — ESP32 Coffee Machine (Plan v2.1 Patch Revised)
  ================================================================

  Board (Arduino IDE):
  - Tools > Board: "ESP32 Dev Module"
  - 38-pin CP2102 ESP-32S board supported (same selection)

  Libraries (Arduino IDE Library Manager):
  - ArduinoJson (v6)

  Filesystem (LittleFS):
  - If /index.html exists in LittleFS, it will be served.
  - If not, ESP32 serves a minimal page telling you to upload UI files.

  Captive Portal:
  - AP SSID: "CoffeeMachine"
  - DNS wildcard -> 192.168.4.1
  - Unknown HTTP paths redirect to "/"

  REST API (JSON): { "ok": bool, "data": object|null, "error": string|null }
  - GET  /api/status
  - POST /api/start
  - POST /api/stop
  - GET  /api/settings
  - POST /api/settings
  - POST /api/audio

  IMPORTANT SERIAL LOGS:
  - Logs ALL important API calls: /api/start, /api/stop, /api/settings (POST), /api/audio (POST)
  - Logs EVERY relay ON/OFF + reason
  - Logs FSM transitions + step text
  - /api/status is rate-limited (max 1 log/second)

  NOTE:
  - Some pins below are ESP32 strapping pins (GPIO12, GPIO4, GPIO15, GPIO2). Use opto-isolated relay board,
    series resistors (330Ω–1kΩ), and avoid strong pulls at boot to prevent boot issues.

  WIRING SUMMARY (DIRECT RELAYS, NO MCP23017)
  ------------------------------------------
  Relays (assumed ACTIVE-LOW by default):
    Relay0  Tank1 Sugar     -> GPIO13
    Relay1  Tank2 Coffee    -> GPIO14
    Relay2  Tank3 Nescafe   -> GPIO21
    Relay3  Water Pump      -> GPIO22
    Relay4  Milk Pump       -> GPIO25
    Relay5  Internal Heater -> GPIO26
    Relay6  External Heater -> GPIO27
    Relay7  Mixer Rotate    -> GPIO12  (strap pin)
    Relay8  Mixer Up        -> GPIO4   (strap pin)
    Relay9  Mixer Down      -> GPIO15  (strap pin)

  MAX6675 (internal thermoblock):
    SCK=GPIO18, SO/MISO=GPIO19, CS=GPIO5 (strap pin; CS idle HIGH usually OK)

  Ultrasonic cup sensor:
    TRIG=GPIO2  (strap pin)
    ECHO=GPIO34 (INPUT-only) + voltage divider if sensor ECHO is 5V

  Limit switches (debounced):
    UPPER=GPIO32 (INPUT_PULLUP) pressed=LOW
    LOWER=GPIO33 (INPUT_PULLUP) pressed=LOW

  DFPlayer Mini (Serial2):
    ESP32 TX2=GPIO17 -> DFPlayer RX (series 1k recommended)
    ESP32 RX2=GPIO16 <- DFPlayer TX
    DFPlayer VCC (often 5V), GND common with ESP32
*/

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <FS.h>
#include <LittleFS.h>

// =======================
//  DATA MODELS (MUST BE ABOVE HELPERS)
// =======================

struct Settings {
  int tank1Time;      // sec
  int tank2Time;      // sec
  int tank3Time;      // sec
  int waterPumpTime;  // sec
  int milkPumpTime;   // sec
  int intHeaterTime;  // sec (max heating window wall time)
  int intHeaterTemp;  // C (target)
  int extHeaterTime;  // sec (timer-only)
  int extHeaterTemp;  // C (accepted but ignored)
  int mixerTime;      // sec
  int audioVolume;    // 0..100
  bool audioMuted;    // bool
};

struct Order {
  String mode;       // "Coffee" | "HotWater" | "Nescafe" | "Cleaning"
  String brewBase;   // Coffee: "Water" | "Milk"
  String size;       // "Single" | "Double"
  String sugar;      // "Low" | "Medium" | "High"

  // HotWater:
  String hotLiquid;  // "water" | "milk_medium" | "milk_extra"

  // Nescafe:
  String milkRatio;  // "none" | "medium" | "extra"

  // Cleaning:
  bool cleanWater;
  bool cleanMilk;
};

struct Status {
  bool isBusy;
  String state;
  String step;
  bool cupPresent;
  float intTemp;   // NAN if not available
  String error;    // "" means none
};

enum MachineState {
  ST_IDLE,
  ST_VALIDATE,
  ST_SOLIDS,
  ST_LIQUID,
  ST_HEAT_INTERNAL_PREHEAT,
  ST_HEAT_INTERNAL_ACTIVE,
  ST_HEAT_EXTERNAL,
  ST_MIX_DOWN,
  ST_MIX_RUN,
  ST_MIX_UP,
  ST_DONE,
  ST_ERROR,
  ST_SAFE_STOP
};

// ============================================================
// 0) USER CONFIG TOGGLES
// ============================================================

// Relay polarity (most relay boards are ACTIVE-LOW)
static const bool RELAY_ACTIVE_LOW = true;

// If your ultrasonic is noisy, you can increase interval (ms)
static const uint32_t CUP_SAMPLE_MS = 200;
static const uint32_t TEMP_SAMPLE_MS = 250;

// Cup distance threshold (cm): <= threshold = cup present
static const float CUP_THRESHOLD_CM = 15.0f;

// /api/status log rate limit
static const uint32_t STATUS_LOG_MIN_MS = 1000;

// ============================================================
// 1) PIN MAP (NO MCP23017)
// ============================================================

// Relays (10)
static const int PIN_RELAY_0  = 13;  // Tank1 Sugar
static const int PIN_RELAY_1  = 14;  // Tank2 Coffee
static const int PIN_RELAY_2  = 21;  // Tank3 Nescafe
static const int PIN_RELAY_3  = 22;  // Water Pump
static const int PIN_RELAY_4  = 25;  // Milk Pump
static const int PIN_RELAY_5  = 26;  // Internal Heater (Thermoblock)
static const int PIN_RELAY_6  = 27;  // External Heater
static const int PIN_RELAY_7  = 12;  // Mixer Rotate (strap pin)
static const int PIN_RELAY_8  = 4;   // Mixer Up (strap pin)
static const int PIN_RELAY_9  = 15;  // Mixer Down (strap pin)

// MAX6675 (internal)
static const int PIN_MAX6675_SCK = 18;
static const int PIN_MAX6675_SO  = 19; // MISO
static const int PIN_MAX6675_CS  = 5;  // CS internal

// Ultrasonic
static const int PIN_US_TRIG = 2;   // strap pin
static const int PIN_US_ECHO = 34;  // input-only

// Limit switches
static const int PIN_LIMIT_UPPER = 32; // INPUT_PULLUP
static const int PIN_LIMIT_LOWER = 33; // INPUT_PULLUP

// DFPlayer (UART2)
static const int PIN_DF_TX2 = 17; // ESP32 TX2 -> DFPlayer RX (series 1k)
static const int PIN_DF_RX2 = 16; // ESP32 RX2 <- DFPlayer TX

// ============================================================
// 2) NETWORK / PORTAL
// ============================================================

static const char* AP_SSID = "CoffeeMachine";
static const char* AP_PASS = "";          // open AP
static const int   AP_CHANNEL = 6;        // 1/6/11 recommended
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GW(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

DNSServer dnsServer;
WebServer server(80);

// ============================================================
// 3) LOGGING
// ============================================================

static uint32_t g_lastStatusLogMs = 0;

static void logLine(const char* level, const char* module, const String& msg) {
  uint32_t t = millis();
  Serial.print('['); Serial.print(t); Serial.print("]["); Serial.print(level);
  Serial.print("]["); Serial.print(module); Serial.print("] ");
  Serial.println(msg);
}

#define LOGI(mod, msg) logLine("INFO",  mod, (msg))
#define LOGW(mod, msg) logLine("WARN",  mod, (msg))
#define LOGE(mod, msg) logLine("ERROR", mod, (msg))

// ============================================================
// 4) DATA MODELS
// ============================================================

struct Settings {
  int tank1Time;      // sec
  int tank2Time;      // sec
  int tank3Time;      // sec
  int waterPumpTime;  // sec
  int milkPumpTime;   // sec
  int intHeaterTime;  // sec (max heating window wall time)
  int intHeaterTemp;  // C (target)
  int extHeaterTime;  // sec (timer-only)
  int extHeaterTemp;  // C (accepted but ignored)
  int mixerTime;      // sec
  int audioVolume;    // 0..100
  bool audioMuted;    // bool
};

struct Order {
  String mode;       // "Coffee" | "HotWater" | "Nescafe" | "Cleaning"
  String brewBase;   // Coffee: "Water" | "Milk"
  String size;       // "Single" | "Double"
  String sugar;      // "Low" | "Medium" | "High"

  // HotWater:
  String hotLiquid;  // "water" | "milk_medium" | "milk_extra"

  // Nescafe:
  String milkRatio;  // "none" | "medium" | "extra"

  // Cleaning:
  bool cleanWater;
  bool cleanMilk;
};

struct Status {
  bool isBusy;
  String state;
  String step;
  bool cupPresent;
  float intTemp;   // NAN if not available
  String error;    // "" means none
};

static Settings g_settings;
static Order    g_order;
static Status   g_status;

// ============================================================
// 5) NVS (Preferences) — defaults + validation
// ============================================================

Preferences prefs;

static int clampInt(int v, int mn, int mx) {
  if (v < mn) return mn;
  if (v > mx) return mx;
  return v;
}

static void setDefaults(Settings& s) {
  s.tank1Time     = 2;
  s.tank2Time     = 3;
  s.tank3Time     = 3;
  s.waterPumpTime = 5;
  s.milkPumpTime  = 4;
  s.intHeaterTime = 30;
  s.intHeaterTemp = 95;
  s.extHeaterTime = 45;
  s.extHeaterTemp = 90;  // accepted but ignored in control
  s.mixerTime     = 10;
  s.audioVolume   = 80;
  s.audioMuted    = false;
}

static void validateClamp(Settings& s) {
  s.tank1Time     = clampInt(s.tank1Time,     0, 30);
  s.tank2Time     = clampInt(s.tank2Time,     0, 30);
  s.tank3Time     = clampInt(s.tank3Time,     0, 30);
  s.waterPumpTime = clampInt(s.waterPumpTime, 0, 60);
  s.milkPumpTime  = clampInt(s.milkPumpTime,  0, 60);
  s.intHeaterTime = clampInt(s.intHeaterTime, 10, 120);
  s.intHeaterTemp = clampInt(s.intHeaterTemp, 60, 100);
  s.extHeaterTime = clampInt(s.extHeaterTime, 10, 180);
  s.extHeaterTemp = clampInt(s.extHeaterTemp, 60, 100);
  s.mixerTime     = clampInt(s.mixerTime,     5, 60);
  s.audioVolume   = clampInt(s.audioVolume,   0, 100);
}

static void loadSettings() {
  setDefaults(g_settings);

  prefs.begin("coffee", true);
  g_settings.tank1Time     = prefs.getInt("tank1Time",     g_settings.tank1Time);
  g_settings.tank2Time     = prefs.getInt("tank2Time",     g_settings.tank2Time);
  g_settings.tank3Time     = prefs.getInt("tank3Time",     g_settings.tank3Time);
  g_settings.waterPumpTime = prefs.getInt("waterPumpTime", g_settings.waterPumpTime);
  g_settings.milkPumpTime  = prefs.getInt("milkPumpTime",  g_settings.milkPumpTime);
  g_settings.intHeaterTime = prefs.getInt("intHeaterTime", g_settings.intHeaterTime);
  g_settings.intHeaterTemp = prefs.getInt("intHeaterTemp", g_settings.intHeaterTemp);
  g_settings.extHeaterTime = prefs.getInt("extHeaterTime", g_settings.extHeaterTime);
  g_settings.extHeaterTemp = prefs.getInt("extHeaterTemp", g_settings.extHeaterTemp);
  g_settings.mixerTime     = prefs.getInt("mixerTime",     g_settings.mixerTime);
  g_settings.audioVolume   = prefs.getInt("audioVolume",   g_settings.audioVolume);
  g_settings.audioMuted    = prefs.getBool("audioMuted",   g_settings.audioMuted);
  prefs.end();

  validateClamp(g_settings);
  LOGI("SETTINGS", "Loaded settings from NVS (clamped). extHeaterTemp accepted but ignored in control.");
}

static void saveSettingsAll(const Settings& s) {
  prefs.begin("coffee", false);
  prefs.putInt("tank1Time",     s.tank1Time);
  prefs.putInt("tank2Time",     s.tank2Time);
  prefs.putInt("tank3Time",     s.tank3Time);
  prefs.putInt("waterPumpTime", s.waterPumpTime);
  prefs.putInt("milkPumpTime",  s.milkPumpTime);
  prefs.putInt("intHeaterTime", s.intHeaterTime);
  prefs.putInt("intHeaterTemp", s.intHeaterTemp);
  prefs.putInt("extHeaterTime", s.extHeaterTime);
  prefs.putInt("extHeaterTemp", s.extHeaterTemp); // stored, but ignored by external heater control
  prefs.putInt("mixerTime",     s.mixerTime);
  prefs.putInt("audioVolume",   s.audioVolume);
  prefs.putBool("audioMuted",   s.audioMuted);
  prefs.end();
  LOGI("SETTINGS", "Saved settings to NVS.");
}

// ============================================================
// 6) HAL — relays + sensors + dfplayer
// ============================================================

static const int RELAY_PINS[10] = {
  PIN_RELAY_0, PIN_RELAY_1, PIN_RELAY_2, PIN_RELAY_3, PIN_RELAY_4,
  PIN_RELAY_5, PIN_RELAY_6, PIN_RELAY_7, PIN_RELAY_8, PIN_RELAY_9
};

static inline int relayLevelOn()  { return RELAY_ACTIVE_LOW ? LOW  : HIGH; }
static inline int relayLevelOff() { return RELAY_ACTIVE_LOW ? HIGH : LOW;  }

static void relayWriteIdx(int idx, bool on, const String& reason) {
  if (idx < 0 || idx > 9) return;
  digitalWrite(RELAY_PINS[idx], on ? relayLevelOn() : relayLevelOff());
  LOGI("HW", String("Relay ") + idx + (on ? " ON" : " OFF") + " | " + reason);
}

static void allRelaysOff(const String& reason) {
  for (int i = 0; i < 10; i++) {
    digitalWrite(RELAY_PINS[i], relayLevelOff());
  }
  LOGW("HW", "ALL RELAYS OFF | " + reason);
}

// ---------- Ultrasonic (cup detect) ----------
static float g_lastCupDistanceCm = NAN;
static uint32_t g_lastCupSampleMs = 0;

static float measureUltrasonicCm() {
  // HC-SR04 typical: TRIG pulse 10us, then pulseIn on ECHO.
  digitalWrite(PIN_US_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_US_TRIG, LOW);

  // timeout ~ 25ms => ~4.3m max
  unsigned long duration = pulseIn(PIN_US_ECHO, HIGH, 25000UL);
  if (duration == 0) return NAN;

  // speed of sound ~ 343 m/s => 29.1 us/cm round trip => 58.2 us/cm
  float cm = (float)duration / 58.2f;
  if (cm < 1.0f || cm > 400.0f) return NAN;
  return cm;
}

static void updateCupPresence() {
  uint32_t now = millis();
  if (now - g_lastCupSampleMs < CUP_SAMPLE_MS) return;
  g_lastCupSampleMs = now;

  float cm = measureUltrasonicCm();
  g_lastCupDistanceCm = cm;

  bool present = false;
  if (!isnan(cm) && cm <= CUP_THRESHOLD_CM) present = true;

  g_status.cupPresent = present;
}

// ---------- MAX6675 (bit-bang read, no external lib) ----------
static float g_lastIntTempC = NAN;
static uint32_t g_lastTempSampleMs = 0;
static int g_tempFailCount = 0;

static uint16_t max6675_readRaw() {
  // MAX6675: 16-bit read. D2 = 1 means thermocouple open.
  digitalWrite(PIN_MAX6675_CS, LOW);
  delayMicroseconds(2);

  uint16_t v = 0;
  for (int i = 15; i >= 0; i--) {
    digitalWrite(PIN_MAX6675_SCK, LOW);
    delayMicroseconds(1);
    digitalWrite(PIN_MAX6675_SCK, HIGH);
    delayMicroseconds(1);
    int bit = digitalRead(PIN_MAX6675_SO);
    v |= ((uint16_t)bit << i);
  }

  digitalWrite(PIN_MAX6675_CS, HIGH);
  return v;
}

static float max6675_readCelsius(bool& ok) {
  uint16_t v = max6675_readRaw();
  // If D2 set => thermocouple open
  if (v & 0x0004) {
    ok = false;
    return NAN;
  }
  // Temp is bits [14:3], in 0.25C steps
  uint16_t tempData = (v >> 3) & 0x0FFF;
  float c = tempData * 0.25f;
  if (c < -10.0f || c > 500.0f) {
    ok = false;
    return NAN;
  }
  ok = true;
  return c;
}

static void updateInternalTemp() {
  uint32_t now = millis();
  if (now - g_lastTempSampleMs < TEMP_SAMPLE_MS) return;
  g_lastTempSampleMs = now;

  bool ok = false;
  float c = max6675_readCelsius(ok);
  if (!ok) {
    g_tempFailCount++;
    if (g_tempFailCount >= 3) {
      // Do not force an error here; FSM will check and abort when heater is used.
      // Keep last value as NAN.
      g_lastIntTempC = NAN;
    }
  } else {
    g_tempFailCount = 0;
    g_lastIntTempC = c;
  }

  g_status.intTemp = g_lastIntTempC;
}

// ---------- DFPlayer minimal protocol (optional) ----------
static HardwareSerial& DFSerial = Serial2;
static bool g_dfInited = false;

static uint16_t df_checksum(uint8_t *cmd) {
  uint16_t sum = 0;
  for (int i = 1; i < 7; i++) sum += cmd[i];
  return 0 - sum;
}

static void df_send(uint8_t cmd, uint16_t param) {
  uint8_t pkt[10] = {0x7E, 0xFF, 0x06, cmd, 0x00,
                     (uint8_t)(param >> 8), (uint8_t)(param & 0xFF),
                     0x00, 0x00, 0xEF};
  uint16_t chk = df_checksum(pkt);
  pkt[7] = (uint8_t)(chk >> 8);
  pkt[8] = (uint8_t)(chk & 0xFF);
  DFSerial.write(pkt, 10);
}

static void df_setVolume_0_30(uint8_t vol) {
  if (vol > 30) vol = 30;
  df_send(0x06, vol);
}

static void audioApplyFromSettings() {
  // Map 0..100 => 0..30
  uint8_t vol30 = (uint8_t)roundf((g_settings.audioVolume / 100.0f) * 30.0f);
  if (g_settings.audioMuted) vol30 = 0;

  if (!g_dfInited) return;
  df_setVolume_0_30(vol30);
  LOGI("AUDIO", String("Apply audio | muted=") + (g_settings.audioMuted ? "true" : "false") +
                  " volume=" + g_settings.audioVolume + "% -> " + vol30 + "/30");
}

// ============================================================
// 7) LIMIT SWITCH DEBOUNCE (5 reads @10ms = 50ms)
// ============================================================

static uint32_t g_lastDebounceMs = 0;
static uint8_t g_upperHist = 0;
static uint8_t g_lowerHist = 0;
static bool g_upperStablePressed = false;
static bool g_lowerStablePressed = false;

static inline bool rawUpperPressed() { return digitalRead(PIN_LIMIT_UPPER) == LOW; }
static inline bool rawLowerPressed() { return digitalRead(PIN_LIMIT_LOWER) == LOW; }

static void updateLimitDebounce() {
  uint32_t now = millis();
  if (now - g_lastDebounceMs < 10) return;
  g_lastDebounceMs = now;

  uint8_t up = rawUpperPressed() ? 1 : 0;
  uint8_t lo = rawLowerPressed() ? 1 : 0;

  g_upperHist = ((g_upperHist << 1) | up) & 0x1F;
  g_lowerHist = ((g_lowerHist << 1) | lo) & 0x1F;

  if (g_upperHist == 0x1F) g_upperStablePressed = true;
  else if (g_upperHist == 0x00) g_upperStablePressed = false;

  if (g_lowerHist == 0x1F) g_lowerStablePressed = true;
  else if (g_lowerHist == 0x00) g_lowerStablePressed = false;
}

// ============================================================
// 8) FSM (NON-BLOCKING)
// ============================================================

enum MachineState {
  ST_IDLE,
  ST_VALIDATE,
  ST_SOLIDS,
  ST_LIQUID,
  ST_HEAT_INTERNAL_PREHEAT,
  ST_HEAT_INTERNAL_ACTIVE,
  ST_HEAT_EXTERNAL,
  ST_MIX_DOWN,
  ST_MIX_RUN,
  ST_MIX_UP,
  ST_DONE,
  ST_ERROR,
  ST_SAFE_STOP
};

static MachineState g_state = ST_IDLE;
static uint32_t g_stateStartMs = 0;

// Sub-steps
static int g_solidsSub = 0;
static int g_liquidSub = 0;
static int g_activeSub = 0;

// Timers / durations
static uint32_t g_stepDurationMs = 0;
static uint32_t g_stepStartMs = 0;

static uint32_t g_heaterWindowStartMs = 0;

// Precomputed for Nescafe
static uint32_t g_nescafeWaterMs = 0;
static uint32_t g_nescafeMilkMs  = 0;

// Helper
static const float INTERNAL_ABS_MAX_C = 110.0f;

static String stateName(MachineState s) {
  switch (s) {
    case ST_IDLE: return "IDLE";
    case ST_VALIDATE: return "VALIDATE";
    case ST_SOLIDS: return "DISPENSE_SOLIDS";
    case ST_LIQUID: return "DISPENSE_LIQUID";
    case ST_HEAT_INTERNAL_PREHEAT: return "HEAT_INTERNAL_PREHEAT";
    case ST_HEAT_INTERNAL_ACTIVE: return "HEAT_INTERNAL_ACTIVE";
    case ST_HEAT_EXTERNAL: return "HEAT_EXTERNAL";
    case ST_MIX_DOWN: return "MIX_DOWN";
    case ST_MIX_RUN: return "MIX_RUN";
    case ST_MIX_UP: return "MIX_UP";
    case ST_DONE: return "DONE";
    case ST_ERROR: return "ERROR";
    case ST_SAFE_STOP: return "SAFE_STOP";
  }
  return "UNKNOWN";
}

static void setState(MachineState ns, const String& stepText) {
  String from = stateName(g_state);
  g_state = ns;
  g_stateStartMs = millis();
  g_status.state = stateName(g_state);
  g_status.step = stepText;

  LOGI("FSM", "State | from=" + from + " to=" + g_status.state + " | step=" + stepText);
}

static void abortWithError(const String& err) {
  allRelaysOff("ABORT " + err);
  g_status.error = err;
  g_status.isBusy = false;
  setState(ST_ERROR, "Aborted: " + err);
}

static int sizeMultiplier(const String& size) {
  if (size.equalsIgnoreCase("Double")) return 2;
  return 1; // Single default
}

static int sugarMultiplier(const String& sugar) {
  if (sugar.equalsIgnoreCase("Low")) return 1;
  if (sugar.equalsIgnoreCase("High")) return 4;
  return 2; // Medium default
}

static uint32_t secToMs(float sec) {
  if (sec <= 0) return 0;
  return (uint32_t)(sec * 1000.0f + 0.5f);
}

static bool heaterWindowExceeded() {
  if (g_heaterWindowStartMs == 0) return false;
  uint32_t now = millis();
  uint32_t maxMs = (uint32_t)g_settings.intHeaterTime * 1000UL;
  return (now - g_heaterWindowStartMs) > maxMs;
}

static bool internalTempValid() {
  return !isnan(g_lastIntTempC);
}

static bool internalOverTemp() {
  if (isnan(g_lastIntTempC)) return false;
  float sp = (float)g_settings.intHeaterTemp;
  if (g_lastIntTempC > INTERNAL_ABS_MAX_C) return true;
  if (g_lastIntTempC > (sp + 10.0f)) return true;
  return false;
}

static void internalHeaterSet(bool on, const String& why) {
  relayWriteIdx(5, on, "InternalHeater " + why);
  if (on && g_heaterWindowStartMs == 0) g_heaterWindowStartMs = millis();
}

static void pumpWaterSet(bool on, const String& why) {
  relayWriteIdx(3, on, "WaterPump " + why);
}

static void pumpMilkSet(bool on, const String& why) {
  relayWriteIdx(4, on, "MilkPump " + why);
}

static void mixerRotateSet(bool on, const String& why) {
  relayWriteIdx(7, on, "MixerRotate " + why);
}
static void mixerUpSet(bool on, const String& why) {
  relayWriteIdx(8, on, "MixerUp " + why);
}
static void mixerDownSet(bool on, const String& why) {
  relayWriteIdx(9, on, "MixerDown " + why);
}

static void tankSugarSet(bool on, const String& why) { relayWriteIdx(0, on, "Tank1Sugar " + why); }
static void tankCoffeeSet(bool on, const String& why) { relayWriteIdx(1, on, "Tank2Coffee " + why); }
static void tankNescafeSet(bool on, const String& why) { relayWriteIdx(2, on, "Tank3Nescafe " + why); }

static void externalHeaterSet(bool on, const String& why) {
  relayWriteIdx(6, on, "ExternalHeater " + why + " (timer-only; extHeaterTemp ignored)");
}

// Cup interlock during run (R9)
static void checkCupDuringRunOrAbort() {
  if (g_status.isBusy && !g_status.cupPresent) {
    abortWithError("NO_CUP_DURING_RUN");
  }
}

// Limit invalid check
static bool limitInvalid() {
  return g_upperStablePressed && g_lowerStablePressed;
}

// Mixer timeout tracking
static uint32_t g_mixerMoveStartMs = 0;
static const uint32_t MIXER_TIMEOUT_MS = 10000;

// Main FSM update
static void fsmUpdate() {
  // Always update safety inputs
  updateCupPresence();
  updateLimitDebounce();
  updateInternalTemp();

  // Critical: cup loss mid-run => immediate abort
  checkCupDuringRunOrAbort();
  if (!g_status.isBusy && g_state != ST_ERROR && g_state != ST_SAFE_STOP) {
    // keep status coherent
    if (g_state == ST_IDLE) g_status.step = "";
  }

  if (!g_status.isBusy) {
    // If not busy, nothing to do unless transitioning from SAFE_STOP/DONE
    if (g_state == ST_DONE) setState(ST_IDLE, "");
    if (g_state == ST_SAFE_STOP) setState(ST_IDLE, "");
    return;
  }

  // Internal heater safety checks (only relevant if heater might be used)
  // We enforce it when heater state is active/preheat OR when order is HotWater/Nescafe.
  bool needsInternalHeater = g_order.mode.equalsIgnoreCase("HotWater") || g_order.mode.equalsIgnoreCase("Nescafe");
  if (needsInternalHeater) {
    if (g_tempFailCount >= 3 && !internalTempValid()) {
      internalHeaterSet(false, "SENSOR_FAIL");
      pumpWaterSet(false, "SENSOR_FAIL");
      pumpMilkSet(false, "SENSOR_FAIL");
      abortWithError("SENSOR_FAIL");
      return;
    }
    if (internalOverTemp()) {
      internalHeaterSet(false, "OVERTEMP");
      pumpWaterSet(false, "OVERTEMP");
      pumpMilkSet(false, "OVERTEMP");
      abortWithError("OVERTEMP");
      return;
    }
    if (heaterWindowExceeded()) {
      internalHeaterSet(false, "HEAT_TIMEOUT");
      pumpWaterSet(false, "HEAT_TIMEOUT");
      pumpMilkSet(false, "HEAT_TIMEOUT");
      abortWithError("HEAT_TIMEOUT");
      return;
    }
  }

  uint32_t now = millis();

  switch (g_state) {
    case ST_VALIDATE: {
      // Cup present at start required for ALL modes
      if (!g_status.cupPresent) {
        abortWithError("NO_CUP");
        return;
      }
      // Limit invalid at start (for mixing cycles)
      if (!g_order.mode.equalsIgnoreCase("Cleaning")) {
        if (limitInvalid()) {
          abortWithError("LIMIT_INVALID");
          return;
        }
      }

      // Reset sequencing
      g_solidsSub = 0;
      g_liquidSub = 0;
      g_activeSub = 0;
      g_heaterWindowStartMs = 0;
      g_stepStartMs = now;

      setState(ST_SOLIDS, "Solids start");
      return;
    }

    case ST_SOLIDS: {
      // Dispense solids depends on mode.
      // We'll run sequentially:
      //  - Sugar (Tank1) if applicable
      //  - Coffee (Tank2) or Nescafe (Tank3) if applicable
      int sz = sizeMultiplier(g_order.size);
      int su = sugarMultiplier(g_order.sugar);

      bool sugarApplies = !g_order.mode.equalsIgnoreCase("Cleaning"); // Coffee/HotWater/Nescafe
      bool coffeeApplies = g_order.mode.equalsIgnoreCase("Coffee");
      bool nescafeApplies = g_order.mode.equalsIgnoreCase("Nescafe");

      if (g_solidsSub == 0) {
        if (sugarApplies && g_settings.tank1Time > 0) {
          g_stepDurationMs = secToMs((float)g_settings.tank1Time * su);
          g_stepStartMs = now;
          tankSugarSet(true, String("for ") + (g_stepDurationMs / 1000.0f) + "s");
          g_solidsSub = 1;
          g_status.step = "Sugar dispensing";
        } else {
          g_solidsSub = 2; // skip sugar
        }
      }

      if (g_solidsSub == 1) {
        if (now - g_stepStartMs >= g_stepDurationMs) {
          tankSugarSet(false, "done");
          g_solidsSub = 2;
        } else {
          return;
        }
      }

      if (g_solidsSub == 2) {
        if (coffeeApplies && g_settings.tank2Time > 0) {
          g_stepDurationMs = secToMs((float)g_settings.tank2Time * sz);
          g_stepStartMs = now;
          tankCoffeeSet(true, String("for ") + (g_stepDurationMs / 1000.0f) + "s");
          g_solidsSub = 3;
          g_status.step = "Coffee solids dispensing";
          return;
        }
        if (nescafeApplies && g_settings.tank3Time > 0) {
          g_stepDurationMs = secToMs((float)g_settings.tank3Time * sz);
          g_stepStartMs = now;
          tankNescafeSet(true, String("for ") + (g_stepDurationMs / 1000.0f) + "s");
          g_solidsSub = 4;
          g_status.step = "Nescafe solids dispensing";
          return;
        }
        // HotWater has no extra solids beyond sugar
        g_solidsSub = 99;
      }

      if (g_solidsSub == 3) {
        if (now - g_stepStartMs >= g_stepDurationMs) {
          tankCoffeeSet(false, "done");
          g_solidsSub = 99;
        } else {
          return;
        }
      }

      if (g_solidsSub == 4) {
        if (now - g_stepStartMs >= g_stepDurationMs) {
          tankNescafeSet(false, "done");
          g_solidsSub = 99;
        } else {
          return;
        }
      }

      // Next stage depends on mode:
      if (g_order.mode.equalsIgnoreCase("HotWater") || g_order.mode.equalsIgnoreCase("Nescafe")) {
        setState(ST_HEAT_INTERNAL_PREHEAT, "Preheat internal heater");
      } else {
        setState(ST_LIQUID, "Liquid start");
      }
      return;
    }

    case ST_LIQUID: {
      // Coffee: pump water OR milk based on brewBase
      // Cleaning: pump water and/or milk (can be simultaneous), no mixing, no heating
      int sz = sizeMultiplier(g_order.size);

      if (g_order.mode.equalsIgnoreCase("Cleaning")) {
        if (!g_order.cleanWater && !g_order.cleanMilk) {
          abortWithError("BAD_PARAMS");
          return;
        }
        uint32_t durWater = secToMs((float)g_settings.waterPumpTime);
        uint32_t durMilk  = secToMs((float)g_settings.milkPumpTime);
        // run simultaneously, stop each when time hits
        if (g_liquidSub == 0) {
          g_stepStartMs = now;
          if (g_order.cleanWater && durWater > 0) pumpWaterSet(true, String("clean for ") + (durWater / 1000.0f) + "s");
          if (g_order.cleanMilk  && durMilk  > 0) pumpMilkSet(true,  String("clean for ") + (durMilk  / 1000.0f) + "s");
          g_liquidSub = 1;
          g_status.step = "Cleaning pumps running";
        }
        if (g_liquidSub == 1) {
          uint32_t elapsed = now - g_stepStartMs;
          if (g_order.cleanWater && elapsed >= durWater) pumpWaterSet(false, "clean done");
          if (g_order.cleanMilk  && elapsed >= durMilk)  pumpMilkSet(false, "clean done");
          bool wDone = (!g_order.cleanWater) || (elapsed >= durWater);
          bool mDone = (!g_order.cleanMilk)  || (elapsed >= durMilk);
          if (wDone && mDone) {
            g_liquidSub = 99;
          } else {
            return;
          }
        }
        if (g_liquidSub == 99) {
          setState(ST_DONE, "Cleaning done");
          g_status.isBusy = false;
          return;
        }
        return;
      }

      // Coffee mode liquid dispense
      if (!g_order.mode.equalsIgnoreCase("Coffee")) {
        abortWithError("BAD_MODE");
        return;
      }

      uint32_t dur = 0;
      bool useWater = g_order.brewBase.equalsIgnoreCase("Water");
      bool useMilk  = g_order.brewBase.equalsIgnoreCase("Milk");
      if (!useWater && !useMilk) useWater = true;

      if (useWater) dur = secToMs((float)g_settings.waterPumpTime * sz);
      else          dur = secToMs((float)g_settings.milkPumpTime  * sz);

      if (g_liquidSub == 0) {
        g_stepStartMs = now;
        if (useWater && dur > 0) pumpWaterSet(true, String("Coffee liquid for ") + (dur / 1000.0f) + "s");
        if (useMilk  && dur > 0) pumpMilkSet(true,  String("Coffee liquid for ") + (dur / 1000.0f) + "s");
        g_liquidSub = 1;
        g_status.step = "Coffee liquid pumping";
      }

      if (g_liquidSub == 1) {
        if (now - g_stepStartMs >= dur) {
          if (useWater) pumpWaterSet(false, "Coffee liquid done");
          if (useMilk)  pumpMilkSet(false,  "Coffee liquid done");
          g_liquidSub = 99;
        } else {
          return;
        }
      }

      if (g_liquidSub == 99) {
        // After liquid -> external heater timer-only -> mixing
        setState(ST_HEAT_EXTERNAL, "External heater (timer-only)");
        return;
      }
      return;
    }

    case ST_HEAT_INTERNAL_PREHEAT: {
      // Preheat to (setpoint - 5C) OR until heater window hits intHeaterTime (abort handled globally)
      float target = (float)g_settings.intHeaterTemp - 5.0f;

      if (g_heaterWindowStartMs == 0) {
        internalHeaterSet(true, "preheat start");
        g_status.step = "Internal preheat (heater ON)";
      }

      if (!internalTempValid()) {
        // wait for valid read (or will abort by SENSOR_FAIL after 3 invalids)
        return;
      }

      if (g_lastIntTempC >= target) {
        setState(ST_HEAT_INTERNAL_ACTIVE, "Internal active (pump + bang-bang)");
        // Setup per mode
        g_activeSub = 0;
        g_stepStartMs = now;
        return;
      }

      // keep waiting (non-blocking)
      return;
    }

    case ST_HEAT_INTERNAL_ACTIVE: {
      // HotWater exclusivity (hotLiquid) OR Nescafe ratio mixing
      int sz = sizeMultiplier(g_order.size);

      // Bang-bang control helper
      auto bangBang = [&]() {
        if (!internalTempValid()) return;
        float sp = (float)g_settings.intHeaterTemp;
        if (g_lastIntTempC < (sp - 2.0f)) internalHeaterSet(true, "bang-bang ON");
        if (g_lastIntTempC > (sp + 2.0f)) internalHeaterSet(false, "bang-bang OFF");
      };

      if (g_order.mode.equalsIgnoreCase("HotWater")) {
        // Exclusivity: water OR milk only, never mixed
        String hl = g_order.hotLiquid;
        hl.toLowerCase();

        bool useWater = (hl == "water");
        bool useMilk  = (hl == "milk_medium") || (hl == "milk_extra");

        float milkMult = 1.0f;
        if (hl == "milk_extra") milkMult = 2.0f; // D8

        uint32_t durMs = 0;
        if (useWater) durMs = secToMs((float)g_settings.waterPumpTime * sz);
        if (useMilk)  durMs = secToMs((float)g_settings.milkPumpTime  * sz * milkMult);
        if (!useWater && !useMilk) { useWater = true; durMs = secToMs((float)g_settings.waterPumpTime * sz); }

        if (g_activeSub == 0) {
          g_stepStartMs = now;
          if (useWater && durMs > 0) pumpWaterSet(true, String("HotWater for ") + (durMs / 1000.0f) + "s");
          if (useMilk  && durMs > 0) pumpMilkSet(true,  String("HotMilk for ") + (durMs / 1000.0f) + "s (exclusive)");
          g_activeSub = 1;
          g_status.step = useWater ? "HotWater pumping (exclusive)" : "HotMilk pumping (exclusive)";
        }

        if (g_activeSub == 1) {
          bangBang();
          if (now - g_stepStartMs >= durMs) {
            if (useWater) pumpWaterSet(false, "HotWater done");
            if (useMilk)  pumpMilkSet(false,  "HotMilk done");
            internalHeaterSet(false, "HotWater done");
            g_activeSub = 99;
          } else {
            return;
          }
        }

        if (g_activeSub == 99) {
          // HotWater goes to mixing
          setState(ST_MIX_DOWN, "Mixer down");
          g_mixerMoveStartMs = now;
          mixerDownSet(true, "start");
          return;
        }

        return;
      }

      if (g_order.mode.equalsIgnoreCase("Nescafe")) {
        // Ratio mixing: separate base times
        // totalWaterTime = size * waterPumpTime
        // totalMilkTime  = size * milkPumpTime
        // none: water=100%, milk=0
        // medium: water=75%, milk=25%
        // extra: water=50%, milk=50%
        if (g_activeSub == 0) {
          float totalWater = (float)g_settings.waterPumpTime * sz;
          float totalMilk  = (float)g_settings.milkPumpTime  * sz;

          String mr = g_order.milkRatio;
          mr.toLowerCase();

          float waterFrac = 1.0f;
          float milkFrac  = 0.0f;
          if (mr == "medium") { waterFrac = 0.75f; milkFrac = 0.25f; }
          else if (mr == "extra") { waterFrac = 0.50f; milkFrac = 0.50f; }
          else { waterFrac = 1.0f; milkFrac = 0.0f; }

          g_nescafeWaterMs = secToMs(totalWater * waterFrac);
          g_nescafeMilkMs  = secToMs(totalMilk  * milkFrac);

          g_activeSub = 1;
          g_stepStartMs = now;

          if (g_nescafeWaterMs > 0) {
            pumpWaterSet(true, String("Nescafe water ") + (g_nescafeWaterMs / 1000.0f) + "s");
            g_status.step = "Nescafe: water portion";
          } else {
            g_activeSub = 2; // skip water
          }
          return;
        }

        if (g_activeSub == 1) {
          bangBang();
          if (now - g_stepStartMs >= g_nescafeWaterMs) {
            pumpWaterSet(false, "Nescafe water done");
            g_activeSub = 2;
            g_stepStartMs = now;
            if (g_nescafeMilkMs > 0) {
              pumpMilkSet(true, String("Nescafe milk ") + (g_nescafeMilkMs / 1000.0f) + "s");
              g_status.step = "Nescafe: milk portion";
              return;
            } else {
              g_activeSub = 99;
            }
          } else {
            return;
          }
        }

        if (g_activeSub == 2) {
          // milk portion (if any)
          if (g_nescafeMilkMs > 0) {
            bangBang();
            if (now - g_stepStartMs >= g_nescafeMilkMs) {
              pumpMilkSet(false, "Nescafe milk done");
              g_activeSub = 99;
            } else {
              return;
            }
          } else {
            g_activeSub = 99;
          }
        }

        if (g_activeSub == 99) {
          internalHeaterSet(false, "Nescafe done");
          setState(ST_MIX_DOWN, "Mixer down");
          g_mixerMoveStartMs = now;
          mixerDownSet(true, "start");
          return;
        }

        return;
      }

      abortWithError("BAD_MODE");
      return;
    }

    case ST_HEAT_EXTERNAL: {
      // Coffee external heater: timer-only extHeaterTime; extHeaterTemp ignored
      if (!g_order.mode.equalsIgnoreCase("Coffee")) {
        abortWithError("BAD_MODE");
        return;
      }

      uint32_t durMs = secToMs((float)g_settings.extHeaterTime);

      if (g_liquidSub == 0) {
        g_stepStartMs = now;
        externalHeaterSet(true, String("for ") + (durMs / 1000.0f) + "s");
        g_liquidSub = 1;
        g_status.step = "External heater warming (timer-only)";
        return;
      }

      if (g_liquidSub == 1) {
        if (now - g_stepStartMs >= durMs) {
          externalHeaterSet(false, "done");
          g_liquidSub = 99;
        } else {
          return;
        }
      }

      if (g_liquidSub == 99) {
        // go to mixing
        setState(ST_MIX_DOWN, "Mixer down");
        g_mixerMoveStartMs = now;
        mixerDownSet(true, "start");
        return;
      }
      return;
    }

    case ST_MIX_DOWN: {
      // Debounced limit; timeout 10s; abort on both pressed
      if (limitInvalid()) {
        mixerDownSet(false, "LIMIT_INVALID");
        mixerUpSet(false, "LIMIT_INVALID");
        abortWithError("LIMIT_INVALID");
        return;
      }

      if (g_lowerStablePressed) {
        mixerDownSet(false, "lower limit reached");
        setState(ST_MIX_RUN, "Mixer rotate");
        mixerRotateSet(true, String("for ") + g_settings.mixerTime + "s");
        g_stepStartMs = now;
        return;
      }

      if (now - g_mixerMoveStartMs >= MIXER_TIMEOUT_MS) {
        mixerDownSet(false, "timeout");
        abortWithError("TIMEOUT_LIMIT");
        return;
      }

      return;
    }

    case ST_MIX_RUN: {
      uint32_t durMs = secToMs((float)g_settings.mixerTime);
      if (now - g_stepStartMs >= durMs) {
        mixerRotateSet(false, "mix done");
        setState(ST_MIX_UP, "Mixer up");
        g_mixerMoveStartMs = now;
        mixerUpSet(true, "start");
      }
      return;
    }

    case ST_MIX_UP: {
      if (limitInvalid()) {
        mixerDownSet(false, "LIMIT_INVALID");
        mixerUpSet(false, "LIMIT_INVALID");
        abortWithError("LIMIT_INVALID");
        return;
      }

      if (g_upperStablePressed) {
        mixerUpSet(false, "upper limit reached");
        setState(ST_DONE, "Cycle done");
        g_status.isBusy = false;
        allRelaysOff("DONE");
        return;
      }

      if (now - g_mixerMoveStartMs >= MIXER_TIMEOUT_MS) {
        mixerUpSet(false, "timeout");
        abortWithError("TIMEOUT_LIMIT");
        return;
      }

      return;
    }

    case ST_DONE: {
      // handled at top
      return;
    }

    case ST_ERROR: {
      // wait for /api/stop or new start
      return;
    }

    case ST_SAFE_STOP: {
      // should go idle
      allRelaysOff("SAFE_STOP");
      g_status.isBusy = false;
      setState(ST_IDLE, "");
      return;
    }

    case ST_IDLE:
    default:
      return;
  }
}

// Start a new order
static void startOrder(const Order& o) {
  g_order = o;
  g_status.error = "";
  g_status.isBusy = true;

  // Safety: ensure everything is off before starting
  allRelaysOff("START cycle init");
  g_heaterWindowStartMs = 0;

  setState(ST_VALIDATE, "Validate start conditions");
}

// ============================================================
// 9) HTTP HELPERS — JSON, file serving, captive portal
// ============================================================

static void sendJson(bool ok, const JsonVariantConst data, const char* error) {
  StaticJsonDocument<768> doc;
  doc["ok"] = ok;
  if (ok) {
    doc["error"] = nullptr;
    doc["data"] = data;
  } else {
    doc["data"] = nullptr;
    doc["error"] = error ? error : "UNKNOWN";
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void sendJsonOkObject(const JsonDocument& dataDoc) {
  StaticJsonDocument<768> wrapper;
  wrapper["ok"] = true;
  wrapper["error"] = nullptr;
  wrapper["data"] = dataDoc.as<JsonVariantConst>();
  String out;
  serializeJson(wrapper, out);
  server.send(200, "application/json", out);
}

static String contentTypeFor(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  return "text/plain";
}

static bool tryServeFromLittleFS(String path) {
  if (path.endsWith("/")) path += "index.html";
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  server.streamFile(f, contentTypeFor(path));
  f.close();
  return true;
}

static void handleRoot() {
  if (tryServeFromLittleFS("/index.html")) return;

  // Minimal fallback UI if no LittleFS index.html
  String html =
    "<!doctype html><html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
    "<title>CoffeeMachine</title></head><body style='font-family:Arial;padding:16px;'>"
    "<h2>ESP32 CoffeeMachine</h2>"
    "<p><b>No UI found.</b> Upload your UI files to LittleFS at <code>/index.html</code>.</p>"
    "<p>API is running. Try <code>/api/status</code> in the browser.</p>"
    "</body></html>";
  server.send(200, "text/html", html);
}

// Captive portal helpers
static void redirectToRoot() {
  server.sendHeader("Location", String("http://") + AP_IP.toString() + "/");
  server.send(302, "text/plain", "");
}

static bool isApiPath(const String& uri) {
  return uri.startsWith("/api/");
}

// ============================================================
// 10) API HANDLERS
// ============================================================

static void apiStatus() {
  // Optional log, rate-limited
  uint32_t now = millis();
  if (now - g_lastStatusLogMs >= STATUS_LOG_MIN_MS) {
    g_lastStatusLogMs = now;
    // Do NOT spam. This is enough.
    LOGI("API", String("GET /api/status | isBusy=") + (g_status.isBusy ? "true" : "false") +
               " state=" + g_status.state +
               " cup=" + (g_status.cupPresent ? "1" : "0") +
               (g_status.error.length() ? (" error=" + g_status.error) : ""));
  }

  StaticJsonDocument<384> d;
  d["isBusy"] = g_status.isBusy;
  d["state"] = g_status.state;
  d["step"] = g_status.step;
  d["cupPresent"] = g_status.cupPresent;
  if (isnan(g_status.intTemp)) d["intTemp"] = nullptr;
  else d["intTemp"] = g_status.intTemp;
  if (g_status.error.length() == 0) d["error"] = nullptr;
  else d["error"] = g_status.error;

  sendJsonOkObject(d);
}

static void apiGetSettings() {
  StaticJsonDocument<512> d;
  d["tank1Time"] = g_settings.tank1Time;
  d["tank2Time"] = g_settings.tank2Time;
  d["tank3Time"] = g_settings.tank3Time;
  d["waterPumpTime"] = g_settings.waterPumpTime;
  d["milkPumpTime"] = g_settings.milkPumpTime;
  d["intHeaterTime"] = g_settings.intHeaterTime;
  d["intHeaterTemp"] = g_settings.intHeaterTemp;
  d["extHeaterTime"] = g_settings.extHeaterTime;
  d["extHeaterTemp"] = g_settings.extHeaterTemp; // accepted but ignored
  d["mixerTime"] = g_settings.mixerTime;
  d["audioVolume"] = g_settings.audioVolume;
  d["audioMuted"] = g_settings.audioMuted;
  sendJsonOkObject(d);
}

static bool readJsonBody(DynamicJsonDocument& doc, String& errOut) {
  if (!server.hasArg("plain")) {
    errOut = "BAD_PARAMS";
    return false;
  }
  String body = server.arg("plain");
  DeserializationError e = deserializeJson(doc, body);
  if (e) {
    errOut = "BAD_PARAMS";
    return false;
  }
  return true;
}

static void apiPostSettings() {
  LOGI("API", "POST /api/settings");

  DynamicJsonDocument doc(1024);
  String err;
  if (!readJsonBody(doc, err)) {
    sendJson(false, JsonVariantConst(), err.c_str());
    return;
  }

  Settings s = g_settings; // start from current
  bool any = false;

  auto updInt = [&](const char* key, int& field, int mn, int mx) -> bool {
    if (!doc.containsKey(key)) return true;
    any = true;
    int v = doc[key].as<int>();
    if (v < mn || v > mx) {
      StaticJsonDocument<192> data;
      data["field"] = key;
      data["reason"] = String("Out of range [") + mn + ".." + mx + "]";
      StaticJsonDocument<384> wrapper;
      wrapper["ok"] = false;
      wrapper["error"] = "INVALID_VALUE";
      wrapper["data"] = data.as<JsonVariant>();
      String out;
      serializeJson(wrapper, out);
      server.send(200, "application/json", out);
      return false;
    }
    field = v;
    return true;
  };

  auto updBool = [&](const char* key, bool& field) -> bool {
    if (!doc.containsKey(key)) return true;
    any = true;
    field = doc[key].as<bool>();
    return true;
  };

  if (!updInt("tank1Time", s.tank1Time, 0, 30)) return;
  if (!updInt("tank2Time", s.tank2Time, 0, 30)) return;
  if (!updInt("tank3Time", s.tank3Time, 0, 30)) return;
  if (!updInt("waterPumpTime", s.waterPumpTime, 0, 60)) return;
  if (!updInt("milkPumpTime", s.milkPumpTime, 0, 60)) return;
  if (!updInt("intHeaterTime", s.intHeaterTime, 10, 120)) return;
  if (!updInt("intHeaterTemp", s.intHeaterTemp, 60, 100)) return;
  if (!updInt("extHeaterTime", s.extHeaterTime, 10, 180)) return;
  if (!updInt("extHeaterTemp", s.extHeaterTemp, 60, 100)) return; // accepted but ignored
  if (!updInt("mixerTime", s.mixerTime, 5, 60)) return;
  if (!updInt("audioVolume", s.audioVolume, 0, 100)) return;
  if (!updBool("audioMuted", s.audioMuted)) return;

  if (!any) {
    sendJson(false, JsonVariantConst(), "BAD_PARAMS");
    return;
  }

  validateClamp(s);
  g_settings = s;
  saveSettingsAll(g_settings);

  // Apply audio to DFPlayer if initialized
  audioApplyFromSettings();

  StaticJsonDocument<64> ok;
  ok["message"] = "Settings saved";
  sendJsonOkObject(ok);
}

static void apiStop() {
  LOGI("API", "POST /api/stop");

  allRelaysOff("API STOP");
  g_status.isBusy = false;
  g_status.error = ""; // user stop clears error
  setState(ST_SAFE_STOP, "Stopped by user");

  StaticJsonDocument<64> d;
  d["message"] = "Stopped";
  sendJsonOkObject(d);
}

static void apiAudio() {
  LOGI("API", "POST /api/audio");

  DynamicJsonDocument doc(512);
  String err;
  if (!readJsonBody(doc, err)) {
    sendJson(false, JsonVariantConst(), err.c_str());
    return;
  }

  bool changed = false;
  if (doc.containsKey("volume")) {
    g_settings.audioVolume = clampInt(doc["volume"].as<int>(), 0, 100);
    changed = true;
  }
  if (doc.containsKey("muted")) {
    g_settings.audioMuted = doc["muted"].as<bool>();
    changed = true;
  }

  if (!changed) {
    sendJson(false, JsonVariantConst(), "BAD_PARAMS");
    return;
  }

  saveSettingsAll(g_settings);
  audioApplyFromSettings();

  StaticJsonDocument<128> d;
  d["audioVolume"] = g_settings.audioVolume;
  d["audioMuted"] = g_settings.audioMuted;
  sendJsonOkObject(d);
}

static void apiStart() {
  LOGI("API", "POST /api/start");

  if (g_status.isBusy) {
    sendJson(false, JsonVariantConst(), "BUSY");
    return;
  }

  DynamicJsonDocument doc(1024);
  String err;
  if (!readJsonBody(doc, err)) {
    sendJson(false, JsonVariantConst(), err.c_str());
    return;
  }

  // Parse mode
  if (!doc.containsKey("mode")) {
    sendJson(false, JsonVariantConst(), "BAD_PARAMS");
    return;
  }

  Order o;
  o.mode = doc["mode"].as<String>();
  o.mode.replace(" ", ""); // tolerate "Hot Water"
  // normalize: "Nescafé" -> "Nescafe"
  o.mode.replace("Nescafé", "Nescafe");

  o.brewBase = doc.containsKey("brewBase") ? doc["brewBase"].as<String>() : "Water";
  o.size     = doc.containsKey("size") ? doc["size"].as<String>() : "Single";
  o.sugar    = doc.containsKey("sugar") ? doc["sugar"].as<String>() : "Medium";
  o.hotLiquid = doc.containsKey("hotLiquid") ? doc["hotLiquid"].as<String>() : "water";
  o.milkRatio = doc.containsKey("milkRatio") ? doc["milkRatio"].as<String>() : "none";
  o.cleanWater = doc.containsKey("cleanWater") ? doc["cleanWater"].as<bool>() : false;
  o.cleanMilk  = doc.containsKey("cleanMilk")  ? doc["cleanMilk"].as<bool>() : false;

  // Validate mode + required fields
  String m = o.mode;
  m.toLowerCase();

  if (!(m == "coffee" || m == "hotwater" || m == "nescafe" || m == "cleaning")) {
    sendJson(false, JsonVariantConst(), "BAD_MODE");
    return;
  }

  if (m == "cleaning") {
    if (!o.cleanMilk && !o.cleanWater) {
      sendJson(false, JsonVariantConst(), "BAD_PARAMS");
      return;
    }
  }

  if (m == "coffee") {
    if (!(o.brewBase.equalsIgnoreCase("Water") || o.brewBase.equalsIgnoreCase("Milk"))) {
      o.brewBase = "Water";
    }
  }

  if (m == "hotwater") {
    // hotLiquid must be one of: water|milk_medium|milk_extra
    String hl = o.hotLiquid; hl.toLowerCase();
    if (!(hl == "water" || hl == "milk_medium" || hl == "milk_extra")) {
      o.hotLiquid = "water";
    }
  }

  if (m == "nescafe") {
    // milkRatio must be none|medium|extra
    String mr = o.milkRatio; mr.toLowerCase();
    if (!(mr == "none" || mr == "medium" || mr == "extra")) {
      o.milkRatio = "none";
    }
  }

  // Immediately sample cup before start
  updateCupPresence();
  if (!g_status.cupPresent) {
    sendJson(false, JsonVariantConst(), "NO_CUP");
    LOGE("FSM", "Start rejected: NO_CUP");
    return;
  }

  // Log order summary
  {
    String summary = "Start | mode=" + o.mode;
    if (m == "coffee") summary += " brewBase=" + o.brewBase;
    if (m == "hotwater") summary += " hotLiquid=" + o.hotLiquid;
    if (m == "nescafe") summary += " milkRatio=" + o.milkRatio;
    if (m == "cleaning") summary += String(" cleanWater=") + (o.cleanWater ? "1" : "0") + " cleanMilk=" + (o.cleanMilk ? "1" : "0");
    summary += " size=" + o.size + " sugar=" + o.sugar;
    LOGI("API", summary);
  }

  startOrder(o);

  StaticJsonDocument<128> d;
  d["message"] = "Cycle started";
  sendJsonOkObject(d);
}

// ============================================================
// 11) ROUTES SETUP
// ============================================================

static void setupRoutes() {
  // API
  server.on("/api/status", HTTP_GET, apiStatus);
  server.on("/api/start", HTTP_POST, apiStart);
  server.on("/api/stop", HTTP_POST, apiStop);
  server.on("/api/settings", HTTP_GET, apiGetSettings);
  server.on("/api/settings", HTTP_POST, apiPostSettings);
  server.on("/api/audio", HTTP_POST, apiAudio);

  // Captive portal common endpoints
  server.on("/", HTTP_GET, handleRoot);
  server.on("/generate_204", HTTP_GET, [](){ redirectToRoot(); }); // Android
  server.on("/fwlink", HTTP_GET, [](){ redirectToRoot(); });       // Windows
  server.on("/hotspot-detect.html", HTTP_GET, [](){ redirectToRoot(); }); // iOS
  server.on("/library/test/success.html", HTTP_GET, [](){ redirectToRoot(); }); // iOS

  server.onNotFound([]() {
    String uri = server.uri();

    // Serve static files if present
    if (!isApiPath(uri)) {
      if (tryServeFromLittleFS(uri)) return;
      redirectToRoot();
      return;
    }

    // API not found
    StaticJsonDocument<64> d;
    d["path"] = uri;
    StaticJsonDocument<192> wrapper;
    wrapper["ok"] = false;
    wrapper["data"] = d.as<JsonVariant>();
    wrapper["error"] = "NOT_FOUND";
    String out;
    serializeJson(wrapper, out);
    server.send(404, "application/json", out);
  });
}

// ============================================================
// 12) SETUP / LOOP
// ============================================================

static void initPins() {
  // Relays
  for (int i = 0; i < 10; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], relayLevelOff());
  }

  // Ultrasonic
  pinMode(PIN_US_TRIG, OUTPUT);
  digitalWrite(PIN_US_TRIG, LOW);
  pinMode(PIN_US_ECHO, INPUT);

  // Limit switches
  pinMode(PIN_LIMIT_UPPER, INPUT_PULLUP);
  pinMode(PIN_LIMIT_LOWER, INPUT_PULLUP);

  // MAX6675 pins
  pinMode(PIN_MAX6675_SCK, OUTPUT);
  pinMode(PIN_MAX6675_CS, OUTPUT);
  pinMode(PIN_MAX6675_SO, INPUT);
  digitalWrite(PIN_MAX6675_CS, HIGH);   // idle
  digitalWrite(PIN_MAX6675_SCK, HIGH);  // idle high is fine
}

static void startWiFiAndPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);

  IPAddress ip = WiFi.softAPIP();
  LOGI("NET", String("AP started | SSID=") + AP_SSID + " IP=" + ip.toString());

  dnsServer.start(53, "*", ip);
  LOGI("NET", "DNS wildcard started");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  LOGI("BOOT", "ESP32 Coffee Machine | Plan v2.1 Patch (Revised) | SINGLE FILE");
  LOGW("BOOT", "If boot issues happen: check strap pins GPIO12/4/15/2 + relay inputs. Use series resistors and avoid strong pulls.");

  initPins();
  allRelaysOff("BOOT init");

  // LittleFS
  if (!LittleFS.begin(true)) {
    LOGE("BOOT", "LittleFS mount failed (format attempted). UI fallback will still work.");
  } else {
    LOGI("BOOT", "LittleFS mounted");
  }

  // Settings
  loadSettings();

  // DFPlayer init (optional)
  DFSerial.begin(9600, SERIAL_8N1, PIN_DF_RX2, PIN_DF_TX2);
  delay(600); // DFPlayer often needs boot delay
  g_dfInited = true;
  audioApplyFromSettings();

  // Initial status
  g_status.isBusy = false;
  g_status.state = "IDLE";
  g_status.step = "";
  g_status.error = "";
  g_status.cupPresent = false;
  g_status.intTemp = NAN;

  // Start AP + captive portal + server
  startWiFiAndPortal();
  setupRoutes();
  server.begin();
  LOGI("NET", "HTTP server started");

  setState(ST_IDLE, "");
  LOGI("BOOT", "System ready");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // FSM tick
  fsmUpdate();

  // Keep loop responsive
  delay(1);
}
