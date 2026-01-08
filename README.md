# ESP32 Coffee Machine (Arduino IDE) — README
## 10 Relays Direct • ESP32 DevKit 38‑Pin (CP2102)

This project runs an **ESP32‑controlled coffee machine** with a **captive‑portal** web UI:
- ESP32 creates a Wi‑Fi Access Point (AP)
- Your phone connects to the AP
- Captive portal opens the UI automatically (DNS wildcard → ESP32 IP)
- UI calls a small REST API (`/api/*`) to start cycles and adjust settings
- ESP32 executes the full drink cycle autonomously (FSM / non‑blocking)

**Important requirement (Debugging):** the firmware must print to **Serial Monitor**:
- Every important API call from the UI: `/api/start`, `/api/stop`, `/api/settings (GET/POST)`, `/api/audio`
- The parsed payload values (mode, size, sugar, hotLiquid, milkRatio, etc.)
- Every hardware action (relay ON/OFF, heater enable/disable, pump start/stop, mixer states)
- Current state / step text changes

Example log style:
```
[12345][INFO][API] POST /api/start | mode=HotWater size=Double sugar=High hotLiquid=milk_extra
[12350][INFO][FSM] State | from=IDLE to=VALIDATE
[12360][INFO][HW ] Relay ON | relay=4 device=PUMP_MILK duration=8000ms
```

---

## 1) Your Hardware (as described)

### Actuators (10 Relays)
Relay index → function:
0. Tank1 Sugar  
1. Tank2 Coffee  
2. Tank3 Nescafe  
3. Water Pump  
4. Milk Pump  
5. Internal Heater (Thermoblock)  
6. External Heater  
7. Mixer Rotate  
8. Mixer Up  
9. Mixer Down  

### Sensors / Peripherals
- **MAX6675** thermocouple (internal thermoblock temp) *(optional second MAX6675 is supported if your code has it)*
- **Ultrasonic** cup sensor (HC‑SR04 / similar)
- **DFPlayer Mini** audio

> Safety note: Any relay controlling 220VAC must be **properly isolated**, fused, and enclosed. Use thermal fuses / hardware protection on heaters.

---

## 2) Recommended ESP32 Pinout (Direct Wiring) — ESP32 DevKit 38‑Pin (CP2102)

This mapping targets the common **ESP‑32S DevKit 38‑pin with CP2102**, keeping **USB Serial (UART0)** free for debugging, and using **UART2** for DFPlayer.

### A) Relays (10 outputs)
> Assumption: your relay board may be **ACTIVE‑LOW** (common). Firmware should support `RELAY_ACTIVE_LOW` so you can invert easily.

| Relay | Function | Suggested ESP32 GPIO |
|---:|---|---:|
| 0 | Tank1 Sugar | GPIO23 |
| 1 | Tank2 Coffee | GPIO22 |
| 2 | Tank3 Nescafe | GPIO21 |
| 3 | Water Pump | GPIO25 |
| 4 | Milk Pump | GPIO26 |
| 5 | Internal Heater (Thermoblock) | GPIO27 |
| 6 | External Heater | GPIO32 |
| 7 | Mixer Rotate | GPIO33 |
| 8 | Mixer Up | GPIO13 |
| 9 | Mixer Down | GPIO14 |

**Why these pins?**
- They are commonly available on the 38‑pin dev boards.
- Avoids the flash pins and keeps UART0 free.
- Uses only 2 “strap‑adjacent” pins (GPIO13, GPIO14) for the last two relays (usually stable).

**Practical wiring tips for relays (strongly recommended):**
- Put **330Ω–1kΩ series resistor** from each ESP32 GPIO → relay IN pin.
- Ensure relay board shares **GND** with ESP32.
- If you see random relay toggles at boot: enable **safe defaults** in firmware (set pins to OFF ASAP) and consider moving any problematic relay off strap pins.

---

### B) MAX6675 (SPI‑like, read‑only)
MAX6675 uses **SCK**, **SO(MISO)**, **CS** (no MOSI).

| MAX6675 Signal | ESP32 GPIO |
|---|---:|
| SCK | GPIO18 |
| SO (MISO) | GPIO19 |
| CS (Internal thermoblock) | GPIO5 |
| CS (External, optional) | GPIO15 *(optional)* |

**Wiring:**
- MAX6675 VCC → **3.3V** (preferred)
- MAX6675 GND → GND
- MAX6675 SCK → GPIO18
- MAX6675 SO → GPIO19
- MAX6675 CS → GPIO5

**Notes:**
- GPIO5 and GPIO15 are “boot strap related” pins on many ESP32s. They are usually safe for **chip‑select** because CS idles HIGH, but if you experience boot issues, move CS to a different free GPIO.
- If you only have **one MAX6675**, ignore CS_EXT completely.

---

### C) DFPlayer Mini (UART2)
Use Serial2 so USB serial stays available.

| DFPlayer | ESP32 |
|---|---|
| VCC | 5V (most DFPlayers are happier on 5V) |
| GND | GND |
| TX | GPIO16 (ESP32 RX2) |
| RX | GPIO17 (ESP32 TX2) through **1kΩ** resistor |
| SPK+/SPK- | Speaker output |

**DFPlayer tips:**
- Common failure: TX/RX swapped.
- No common ground = no serial comms.
- DFPlayer may need **500–1000ms boot delay** after power‑up before init.
- SD card should be FAT32; tracks in `/MP3/0001.mp3`, etc. (depends on your firmware/library).

---

### D) Ultrasonic Cup Sensor (HC‑SR04 style)
Recommended pins:
- TRIG: GPIO4 (output)
- ECHO: GPIO36 (input‑only, safe)

| Ultrasonic | ESP32 |
|---|---|
| VCC | 5V (HC‑SR04) |
| GND | GND |
| TRIG | GPIO4 |
| ECHO | GPIO36 **through voltage divider** |

**Voltage divider for ECHO (5V → 3.3V safe):**
- Example: 2.2kΩ (top) + 3.3kΩ (bottom)
- ECHO → 2.2kΩ → node → GPIO36
- node → 3.3kΩ → GND

> If you have a 3.3V ultrasonic module (HC‑SR04P / SR04P), the divider may not be required—verify your module.

---

### E) Optional Mixer Limit Switches (recommended for real operation)
Even if you test without sensors first, you will eventually want limit switches for safe mixer movement.

| Limit Switch | ESP32 GPIO |
|---|---:|
| Upper | GPIO34 (input‑only) |
| Lower | GPIO35 (input‑only) |

GPIO34/35 are input‑only and often **do not have internal pullups**. Use external **10kΩ pull‑ups** to 3.3V if needed.

---

## 3) Arduino IDE Setup (ESP32 Dev Module)

### A) Install ESP32 core
1. Arduino IDE → **File → Preferences**
2. Add ESP32 boards URL (Espressif) to “Additional Board Manager URLs”
3. Tools → Board → Boards Manager → install **“esp32 by Espressif Systems”**
4. Tools → Board → select **ESP32 Dev Module**
5. Tools → Upload Speed: 921600 or 460800 (if stable)
6. Serial Monitor: **115200**

### B) Libraries
Install via Library Manager:
- **ArduinoJson** (v6)
- **MAX6675** library (whichever your code uses; many examples use `max6675`)
- **DFRobotDFPlayerMini** (or alternative DFPlayer library)

Built-in / core:
- WiFi
- WebServer
- DNSServer
- LittleFS (available in ESP32 core; may require enabling in tools)

---

## 4) LittleFS UI Upload (Arduino IDE)

Your UI files (at minimum `index.html`) must be uploaded to LittleFS.

### Option A (Arduino IDE 2.x plugin)
- Put the UI file here:
  - `YourSketchFolder/data/index.html`
- Install a LittleFS upload tool for Arduino IDE 2.x (commonly “ESP32 LittleFS Data Upload”).
- Then run from Tools menu:
  - **“ESP32 LittleFS Data Upload”** (name depends on the plugin)

### Option B (Workaround)
If you cannot install the Arduino IDE upload tool, you can:
- temporarily use PlatformIO for `uploadfs`, or
- embed minimal HTML directly in firmware (not recommended for big UIs).

---

## 5) API Quick Reference (what the UI sends)

Your firmware should log all these requests to Serial.

### GET `/api/status`
Returns state, step, isBusy, cupPresent, temps (if enabled).

### POST `/api/start`
Payload examples (latest plan behavior):

**Coffee**
```json
{"mode":"Coffee","brewBase":"Water","size":"Double","sugar":"Medium"}
```

**Hot Water (exclusive: water-only OR milk-only)**
```json
{"mode":"HotWater","hotLiquid":"water","size":"Single","sugar":"Low"}
```
or
```json
{"mode":"HotWater","hotLiquid":"milk_extra","size":"Double","sugar":"High"}
```

**Nescafé (ratio mixing)**
```json
{"mode":"Nescafe","milkRatio":"medium","size":"Double","sugar":"High"}
```

**Cleaning**
```json
{"mode":"Cleaning","cleanMilk":true,"cleanWater":false}
```

### POST `/api/stop`
Immediate safe stop (all relays OFF). Must be logged.

### GET/POST `/api/settings`
Firmware is the source of truth. UI loads settings at page load and POSTs changes on Save.

### POST `/api/audio`
Updates DFPlayer volume/mute; should be logged.

---

## 6) First Power‑On Test (WITHOUT sensors / WITHOUT 220VAC)

You said you want to test first before connecting sensors—do this safely:

1. Do **NOT** connect 220VAC loads yet.
2. Power ESP32 from USB only.
3. Connect relay board to a safe DC supply, and use relay LEDs as indicators.
4. Upload firmware.
5. Upload UI to LittleFS.
6. Connect phone to AP (SSID example: `CoffeeMachine`).
7. Open `http://192.168.4.1`
8. Press Start for a short cycle and confirm in Serial:
   - API received
   - FSM transitions
   - Relay ON/OFF actions

> For pumps/heaters, keep outputs disconnected while validating logic. Only move to high voltage once logic is stable.

---

## 7) Common Issues & Fixes

### A) Relays ON at boot
- Relay board is likely **ACTIVE‑LOW**
- Ensure firmware sets pins OFF immediately at boot
- Add series resistors
- Avoid using “strap pins” for relays if you see boot problems

### B) DFPlayer not responding
- TX/RX swapped
- No common ground
- Needs boot delay
- SD card formatting / file naming mismatch

### C) MAX6675 invalid readings
- Wrong wiring or wrong CS
- Noise / long wires
- Thermocouple polarity reversed
- Module not powered at 3.3V stable

### D) Ultrasonic always NO_CUP
- Missing voltage divider on ECHO
- Wrong pins
- Threshold in firmware too strict/loose

---

## 8) Safety Notes (Must Read)
- Heaters: do not rely on software only—use **hardware thermal cutoffs**.
- External heater in the plan is **timer-only** (temperature reading may be logged but not used for control).
- Enclose all mains wiring and keep AC/DC separation.

---

## 9) Copy/Paste Pin Summary

### Relays
- R0 GPIO23
- R1 GPIO22
- R2 GPIO21
- R3 GPIO25
- R4 GPIO26
- R5 GPIO27
- R6 GPIO32
- R7 GPIO33
- R8 GPIO13
- R9 GPIO14

### MAX6675
- SCK GPIO18
- SO  GPIO19
- CS_INT GPIO5
- CS_EXT GPIO15 (optional)

### DFPlayer (Serial2)
- RX2 GPIO16 (ESP32 receives from DFPlayer TX)
- TX2 GPIO17 (ESP32 sends to DFPlayer RX via 1kΩ)

### Ultrasonic
- TRIG GPIO4
- ECHO GPIO36 (with divider)

### Limit switches (optional)
- UPPER GPIO34
- LOWER GPIO35
