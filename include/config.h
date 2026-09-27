#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Device identity
// ─────────────────────────────────────────────────────────────────────────────
#define DEVICE_NAME         "meteo-teste"
#define FRIENDLY_NAME       "Meteo Teste"
#define DEVICE_MODEL        "esp32c3_bme280"
#define DEVICE_MANUFACTURER "victorzwk"

// ─────────────────────────────────────────────────────────────────────────────
// Network  (use string literals — passed to WiFi.config / IPAddress::fromString)
// ─────────────────────────────────────────────────────────────────────────────
#define STATIC_IP    "192.168.52.149"
#define GATEWAY_IP   "192.168.52.1"
#define SUBNET_MASK  "255.255.255.0"

// ─────────────────────────────────────────────────────────────────────────────
// Timing
// ─────────────────────────────────────────────────────────────────────────────
// Deep-sleep duration between measurements (microseconds)
#define SLEEP_DURATION_US   (15ULL * 60ULL * 1000000ULL)   // 15 minutes

// Maximum time to wait for WiFi association (ms)
#define WIFI_TIMEOUT_MS     5000

// Maximum time to wait for MQTT broker connection (ms)
#define MQTT_TIMEOUT_MS     3000

// ─────────────────────────────────────────────────────────────────────────────
// Hardware pins  (XIAO ESP32C3)
// ─────────────────────────────────────────────────────────────────────────────
#define I2C_SDA          6    // Pin D4
#define I2C_SCL          7    // Pin D5
#define BATTERY_ADC_PIN  3    // Pin A1 / D1

// BME280
#define BME280_I2C_ADDR  0x76

// BME280 oversampling settings
// Temperature: 16× — used internally by BME280 to compensate pressure and
// humidity readings. High oversampling improves accuracy of all three sensors.
// Pressure/Humidity: 4× — sufficient for weather station resolution.
#define BME280_TEMP_OVERSAMPLING   Adafruit_BME280::SAMPLING_X16
#define BME280_PRESS_OVERSAMPLING  Adafruit_BME280::SAMPLING_X4
#define BME280_HUM_OVERSAMPLING    Adafruit_BME280::SAMPLING_X4

// ─────────────────────────────────────────────────────────────────────────────
// ADC / battery
// ─────────────────────────────────────────────────────────────────────────────
// Number of ADC samples to average — reduces RF-noise spikes dramatically.
// WiFi is fully associated during measurement so RF noise is lower than at
// initial connect time; 8 samples with calibration is sufficient.
#define ADC_SAMPLES          8
#define ADC_SAMPLE_GAP_US    100

// ESP32C3 ADC full-scale voltage at 12dB (ADC_11db) attenuation.
// WARNING: CAL_RAW_LO and CAL_RAW_HI below were measured with this value
// set to 3.10f. Changing ADC_VREF_V requires re-measuring both calibration
// points with a bench PSU or multimeter, otherwise battery readings will
// be wrong and may trigger false battery protection shutdowns.
#define ADC_VREF_V           3.10f

// The voltage divider on the circuit halves the battery voltage before the ADC pin.
#define ADC_DIVIDER_RATIO    2.0f

// Calibration is a two-point linear map over ADC pin voltages (post-divider):
// map raw (post-divider) ADC voltage → true battery voltage.
// Tune CAL_RAW_LO / CAL_RAW_HI in config.h using a bench PSU or multimeter to 
// match actual hardware.
// Format: (raw_lo, true_lo), (raw_hi, true_hi)

#define CAL_RAW_LO    1.65f    // ADC reads this …
#define CAL_TRUE_LO   3.30f    // … when battery is actually this
#define CAL_RAW_HI    2.07f    // ADC reads this …
#define CAL_TRUE_HI   4.14f    // … when battery is actually this

// Battery percentage endpoints (true battery voltage)
#define BATTERY_PCT_MIN_V    3.30f    //  0 %
#define BATTERY_PCT_MAX_V    4.14f    // 100 %

// Enter "infinite" deep-sleep below this true voltage to protect the cell
#define BATTERY_PROTECT_V    3.30f

// ─────────────────────────────────────────────────────────────────────────────
// MQTT publish pacing
// ─────────────────────────────────────────────────────────────────────────────
// All 7 readings go out as one combined JSON publish (see publish_sensors in
// main.cpp), so there's no inter-publish gap to tune — a single small
// payload never fills the TCP send window the way 7 back-to-back publishes
// used to. These two only apply if that one publish fails outright:
// drain first (to clear the send window), then retry once.
#define MQTT_PUB_DRAIN_COUNT      2
#define MQTT_PUB_DRAIN_DELAY_MS   5

// MQTT broker TCP port
#define MQTT_PORT   1883

// Must be ≥ largest single publish payload (~370B for discovery). 1024 is safe.
#define MQTT_BUFFER_BYTES   1024