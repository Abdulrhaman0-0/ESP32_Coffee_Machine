#ifndef CONFIG_H
#define CONFIG_H

// I2C for MCP23017
#define I2C_SDA 21
#define I2C_SCL 22
#define MCP23017_ADDR 0x20

// SPI for MAX6675
#define SPI_SCK 18
#define SPI_MISO 19
#define CS_INTERNAL 25
#define CS_EXTERNAL 26  // Telemetry only

// UART2 for DFPlayer
#define DFPLAYER_TX 17
#define DFPLAYER_RX 16

// Sensors
#define ULTRASONIC_TRIG 27
#define ULTRASONIC_ECHO 34
#define LIMIT_UPPER 32
#define LIMIT_LOWER 33

// MCP23017 Relay Mapping (ACTIVE LOW)
#define RELAY_TANK1_SUGAR     0
#define RELAY_TANK2_COFFEE    1
#define RELAY_TANK3_NESCAFE   2
#define RELAY_PUMP_WATER      3
#define RELAY_PUMP_MILK       4
#define RELAY_HEATER_INTERNAL 5
#define RELAY_HEATER_EXTERNAL 6
#define RELAY_MIXER_ROTATE    7
#define RELAY_MIXER_UP        8
#define RELAY_MIXER_DOWN      9

#define RELAY_ACTIVE_LOW true

// WiFi AP
#define AP_SSID "CoffeeMachine"
#define AP_PASS ""
#define AP_CHANNEL 6

// Timing
#define API_STATUS_POLL_MS 400
#define DEBOUNCE_READS 5
#define DEBOUNCE_INTERVAL_MS 10
#define LIMIT_TIMEOUT_MS 10000
#define CUP_DETECT_THRESHOLD_CM 15.0

// Safety
#define INTERNAL_HEATER_ABS_MAX 110
#define SENSOR_FAIL_RETRIES 3

#endif
