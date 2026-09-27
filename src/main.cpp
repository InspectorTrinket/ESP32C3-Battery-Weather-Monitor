/*
 * meteo-sensor — Simplified PlatformIO firmware for XIAO ESP32C3 + BME280
 * ─────────────────────────────────────────────────────────────────────────
 * Design goals
 *   • Minimise active time → maximise battery life
 *   • Publish temperature, pressure, humidity, battery voltage/%, RSSI,
 *     wake duration via MQTT
 *   • Full Home Assistant auto-discovery (retained topics)
 *   • RTC-cached WiFi channel + BSSID → skip AP scan on every wake
 *   • Direct esp_deep_sleep_start() — no framework scheduler overhead
 *
 * Intentionally omitted:
 *   • No OTA / ArduinoOTA
 *   • No maintenance mode (admin/disable_sleep)
 *   • No Status binary sensor
 *   • No MQTT subscribe — device is purely a publisher
 *   • Flash via USB only (hold BOOT button during power-on)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "config.h"
#include "secrets.h"

// ─────────────────────────────────────────────────────────────────────────────
// RTC memory — survives deep sleep; cleared only on cold power-on
//
// RTC_NOINIT_ATTR is safer than RTC_DATA_ATTR for the XIAO ESP32C3: some units
// trigger a USB_UART_CHIP_RESET that clears DATA but not NOINIT memory.
// A magic word guards against reading garbage on a true cold boot.
// Increment this value whenever the rtc struct layout changes, a new
// discovery entity is added, the device metadata block changes, OR any
// network configuration changes (IP, broker address, WiFi credentials).
// This forces a clean cold-boot re-initialisation on the next wake after
// flashing, so the updated discovery payload actually reaches the broker.
#define RTC_MAGIC  0xBEEF123DUL   // bumped: entities now share one state topic + val_tpl

RTC_NOINIT_ATTR struct {
    uint32_t magic;           // == RTC_MAGIC when struct is valid
    uint8_t  bssid[6];       // cached AP BSSID
    uint8_t  channel;        // cached WiFi channel
    uint32_t boot_count;     // wake-cycle counter since power-on
    bool     discovery_done; // skip re-publishing discovery after first boot
} rtc;

// ─────────────────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────────────────
WiFiClient      tcp_client;
PubSubClient    mqtt(tcp_client);
Adafruit_BME280 bme;

// ─────────────────────────────────────────────────────────────────────────────
// WiFi — fast reconnect using RTC-cached channel + BSSID
//
// On first boot (RTC invalid): full association scan (~800 ms).
// Subsequent boots: skip scan, connect directly on known channel/BSSID (~200 ms).
// If the cached AP info becomes stale (router rebooted / channel changed),
// the fast path times out and automatically fall back to a full scan.
// ─────────────────────────────────────────────────────────────────────────────
bool wifi_connect() {
    WiFi.persistent(false);            // never write credentials to NVS flash
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);              // power_save_mode: none — no RF duty-cycling
                                       // during the active window; saves ~50 ms
                                       // association time and improves reliability.
                                       // Irrelevant to battery life since the radio
                                       // is off entirely during deep sleep.
    WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);

    // Static IP avoids DHCP exchange (~100–200 ms saved)
    IPAddress ip, gw, sn;
    ip.fromString(STATIC_IP);
    gw.fromString(GATEWAY_IP);
    sn.fromString(SUBNET_MASK);
    WiFi.config(ip, gw, sn);

    bool cache_valid = (rtc.magic == RTC_MAGIC);

    if (cache_valid) {
        // Direct connect — no channel scan
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD, rtc.channel, rtc.bssid, true);
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > WIFI_TIMEOUT_MS) {
            if (cache_valid) {
                // Cached data stale — fall back to full scan once
                cache_valid = false;
                WiFi.disconnect(false);
                delay(50);
                WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
                t = millis();
                while (WiFi.status() != WL_CONNECTED) {
                    if (millis() - t > WIFI_TIMEOUT_MS) return false;
                    delay(5);
                }
                break;
            }
            return false;
        }
        delay(5);
    }

    // Refresh RTC cache with current AP info for the next boot
    rtc.channel = WiFi.channel();
    memcpy(rtc.bssid, WiFi.BSSID(), 6);
    rtc.magic = RTC_MAGIC;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT — init once, connect on every wake
// No subscribe — device is a pure publisher.
// ─────────────────────────────────────────────────────────────────────────────
void mqtt_init() {
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setBufferSize(MQTT_BUFFER_BYTES);
}

bool mqtt_connect() {
    unsigned long t = millis();
    while (!mqtt.connect(DEVICE_NAME, MQTT_USER, MQTT_PASSWORD)) {
        if (millis() - t > MQTT_TIMEOUT_MS) return false;
        delay(10);
    }
    tcp_client.setNoDelay(true);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Home Assistant MQTT discovery
//
// Published with retain=true — the broker stores these so HA receives them
// on (re)start, even while the sensor is sleeping.
//
// Only published on the first boot after power-on (rtc.discovery_done flag).
// HA retains the discovery messages on the broker, so there is no need to
// re-send them on every wake cycle. This saves ~2 KB of traffic and several
// hundred milliseconds per cycle.
//
// KEY: mqtt.loop() is called after EVERY publish. Without it, PubSubClient
// cannot process TCP ACKs between writes. On a slow or busy broker, the TCP
// send window fills up, WiFiClient::write() returns 0, and PubSubClient marks
// itself disconnected — silently dropping all subsequent publish() calls.
// ─────────────────────────────────────────────────────────────────────────────
void publish_discovery() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Lowercase, colon-separated — HA's format_mac() validator rejects a
    // "cns" mac connection unless it already matches this exact canonical
    // form (it does not case-fold the value for you). ids/mac_str above is
    // unrelated and stays uppercase/unseparated — changing it would change
    // the device identity and orphan the existing entities.
    char mac_colon[18];
    snprintf(mac_colon, sizeof(mac_colon), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char dev[200];
    snprintf(dev, sizeof(dev),
             ",\"dev\":{\"ids\":[\"%s\"],\"cns\":[[\"mac\",\"%s\"]],\"name\":\"%s\","
             "\"mdl\":\"%s\",\"mf\":\"%s\"}",
             mac_str, mac_colon, FRIENDLY_NAME, DEVICE_MODEL, DEVICE_MANUFACTURER);

    struct Entity {
        const char* id;
        const char* name;
        const char* dev_cla;
        const char* unit;
        const char* ent_cat;
    };
    static const Entity entities[] = {
        { "temperature",       "Temperature",       "temperature",          "°C",  nullptr      },
        { "pressure",          "Pressure",          "atmospheric_pressure", "hPa", nullptr      },
        { "relative_humidity", "Relative Humidity", "humidity",             "%",   nullptr      },
        { "battery_voltage",   "Battery Voltage",   "voltage",              "V",   "diagnostic" },
        { "battery_level",     "Battery Level",     "battery",              "%",   "diagnostic" },
        { "net_rssi",          "Net RSSI",          "signal_strength",      "dBm", "diagnostic" },
        { "wake_duration",     "Wake Duration",     "duration",             "ms",  "diagnostic" },
    };

    char cfg_topic[100];
    char state_topic[60];
    snprintf(state_topic, sizeof(state_topic), "%s/state", DEVICE_NAME);
    char ent_cat_field[30];
    char payload[512];

    for (const auto& e : entities) {
        snprintf(cfg_topic,   sizeof(cfg_topic),
                 "homeassistant/sensor/%s/%s/config", DEVICE_NAME, e.id);

        if (e.ent_cat) {
            snprintf(ent_cat_field, sizeof(ent_cat_field),
                     ",\"ent_cat\":\"%s\"", e.ent_cat);
        } else {
            ent_cat_field[0] = '\0';
        }

        // All entities share one state topic (see publish_sensors) — val_tpl
        // picks this entity's field out of the combined JSON payload.
        // Abbreviated HA MQTT discovery keys — standard short forms
        snprintf(payload, sizeof(payload),
                 "{"
                 "\"name\":\"%s\","
                 "\"stat_t\":\"%s\","
                 "\"val_tpl\":\"{{ value_json.%s }}\","
                 "\"uniq_id\":\"%s_%s\","
                 "\"dev_cla\":\"%s\","
                 "\"unit_of_meas\":\"%s\","
                 "\"stat_cla\":\"measurement\"%s%s"
                 "}",
                 e.name, state_topic, e.id,
                 mac_str, e.id,
                 e.dev_cla,
                 e.unit,
                 ent_cat_field, dev);

        mqtt.publish(cfg_topic, payload, /*retain=*/true);
        mqtt.loop();   // process ACKs and keep connection alive
        delay(10);     // brief yield — lets the broker process each message
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Sensor data publishing
//
// All 7 readings are bundled into one retained JSON message on a single
// topic. The original design published each value as its own message —
// firing several of those back-to-back in one wake cycle is what exhausted
// the ESP32's TCP send window and dropped the later ones. A single ~150
// byte payload never triggers that, so the inter-publish gap/drain scheme
// this used to need is gone; only the failure-retry path remains.
// ─────────────────────────────────────────────────────────────────────────────
void publish_sensors(float temp, float pres, float humi,
                     float vbat, float pct, int rssi, uint32_t wake_ms) {
    char topic[60];
    snprintf(topic, sizeof(topic), "%s/state", DEVICE_NAME);

    char payload[220];
    snprintf(payload, sizeof(payload),
             "{\"temperature\":%.2f,\"pressure\":%.2f,\"relative_humidity\":%.3f,"
             "\"battery_voltage\":%.4f,\"battery_level\":%.0f,\"net_rssi\":%d,"
             "\"wake_duration\":%lu}",
             temp, pres, humi, vbat, pct, rssi, wake_ms);

    // mqtt.publish() returns false silently if the TCP write fails. Checking
    // connected() alone is insufficient because the socket can break during
    // the write itself, leaving connected() still true until the next
    // mqtt.loop() detects the dead socket.
    if (!mqtt.connected()) mqtt_connect();
    if (!mqtt.publish(topic, payload, /*retain=*/true)) {
        // Drain first to clear the TCP send window, then retry once. Only
        // reconnect if the connection actually dropped.
        for (int i = 0; i < MQTT_PUB_DRAIN_COUNT; i++) {
            mqtt.loop();
            delay(MQTT_PUB_DRAIN_DELAY_MS);
        }
        if (!mqtt.connected()) mqtt_connect();
        mqtt.publish(topic, payload, /*retain=*/true);
    }
    mqtt.loop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Battery ADC
//
// The ESP32C3 ADC is sensitive to WiFi RF noise. Averaging ADC_SAMPLES
// readings suppresses noise. WiFi is fully associated by the time 
// the ADC is read so noise is lower than at boot;
// ─────────────────────────────────────────────────────────────────────────────
float read_battery_voltage() {
    // ADC_11db ≈ 12 dB attenuation; full-scale input on ESP32C3
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    uint32_t sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        sum += analogRead(BATTERY_ADC_PIN);
        delayMicroseconds(ADC_SAMPLE_GAP_US);
    }
    
    // ADC counts → voltage at the ADC pin (this is what the divider outputs,
    // i.e. approximately battery_voltage / ADC_DIVIDER_RATIO)    
    float v_pin = (static_cast<float>(sum) / ADC_SAMPLES / 4095.0f) * ADC_VREF_V;
    
    // Two-point linear calibration.
    // CAL_RAW_LO / CAL_RAW_HI are ADC PIN voltages (post-divider).
    // CAL_TRUE_LO / CAL_TRUE_HI are the true battery voltages at those points.
    // The divider ratio is implicitly encoded in the calibration constants,
    // so calibration is done in the v_pin domain and mapped directly to true 
    // battery voltage.    
    float slope = (CAL_TRUE_HI - CAL_TRUE_LO) / (CAL_RAW_HI - CAL_RAW_LO);
    return CAL_TRUE_LO + slope * (v_pin - CAL_RAW_LO);
}

float battery_percent(float vbat) {
    float pct = (vbat - BATTERY_PCT_MIN_V)
                / (BATTERY_PCT_MAX_V - BATTERY_PCT_MIN_V) * 100.0f;
    if (pct <   0.0f) pct =   0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}

// ─────────────────────────────────────────────────────────────────────────────
// Deep sleep
// ─────────────────────────────────────────────────────────────────────────────
[[noreturn]] void go_to_sleep(uint64_t duration_us) {
    esp_wifi_stop();
    esp_sleep_enable_timer_wakeup(duration_us);
    esp_deep_sleep_start();
    while (true) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// BME280 direct I2C helpers — trigger and poll measurement without blocking
// ─────────────────────────────────────────────────────────────────────────────
void trigger_bme_measurement() {
    // Construct ctrl_meas (0xF4) directly from config.h oversampling values.
    // Adafruit sampling enum values equal the osrs bit field values directly:
    //   SAMPLING_X1=1, X2=2, X4=3, X8=4, X16=5
    // bits 7:5 = osrs_t, bits 4:2 = osrs_p, bits 1:0 = mode=01 (forced)
    // This is authoritative — no dependency on library internal state or register readback.
    uint8_t ctrl = ((BME280_TEMP_OVERSAMPLING  & 0x07) << 5) |
                   ((BME280_PRESS_OVERSAMPLING & 0x07) << 2) |
                   0x01;   // forced mode
    Wire.beginTransmission(BME280_I2C_ADDR);
    Wire.write(0xF4);
    Wire.write(ctrl);
    Wire.endTransmission();
}

void wait_for_bme_measurement() {
    while (true) {
        Wire.beginTransmission(BME280_I2C_ADDR);
        Wire.write(0xF3);                // status register
        Wire.endTransmission(false);     // repeated start
        // Cast both arguments to uint8_t to avoid overload ambiguity
        Wire.requestFrom((uint8_t)BME280_I2C_ADDR, (uint8_t)1);
        if (Wire.available()) {
            uint8_t status = Wire.read();
            if (!(status & 0x08)) break; // bit 3 = measuring
        }
        delay(1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup() — executes on every wake from deep sleep
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    uint32_t wake_start_ms = millis();  // capture before anything else

    // RTC init
    if (rtc.magic != RTC_MAGIC) {
        rtc.boot_count     = 0;
        rtc.discovery_done = false;  // force discovery on first cold boot
    }
    rtc.boot_count++;

    // I2C
    Wire.begin(I2C_SDA, I2C_SCL);

    // BME280 — trigger measurement early so it runs in parallel with WiFi+MQTT connect
    if (!bme.begin(BME280_I2C_ADDR)) {
        go_to_sleep(SLEEP_DURATION_US); // sensor absent — skip cycle
    }
    bme.setSampling(
        Adafruit_BME280::MODE_FORCED,
        BME280_TEMP_OVERSAMPLING,
        BME280_PRESS_OVERSAMPLING,
        BME280_HUM_OVERSAMPLING,
        Adafruit_BME280::FILTER_OFF,
        Adafruit_BME280::STANDBY_MS_0_5
    );
    // Trigger forced measurement using direct I2C (non‑blocking)
    trigger_bme_measurement();

    // WiFi
    if (!wifi_connect()) {
        go_to_sleep(SLEEP_DURATION_US);
    }

    // MQTT
    mqtt_init();
    if (!mqtt_connect()) {
        go_to_sleep(SLEEP_DURATION_US);
    }

    // Discovery — first boot only
    if (!rtc.discovery_done) {
        publish_discovery();
        rtc.discovery_done = true;
        
        // Full flush after discovery — 7 retained messages (~370 bytes each)
        // need time to clear the broker's ACK queue before publishing sensors.
        // 10 × 10ms = 100ms is sufficient on a local broker.
        for (int i = 0; i < 10; i++) { mqtt.loop(); delay(10); }
        
        // Reconnect if discovery exhausted the connection
        if (!mqtt.connected()) mqtt_connect();
    }

    // Wait for BME280 measurement to complete
    wait_for_bme_measurement();

    // Now read all values (they are ready)
    float temperature = bme.readTemperature();
    float pressure    = bme.readPressure() / 100.0f;
    float humidity    = bme.readHumidity();

    // Battery
    float vbat = read_battery_voltage();
    float pct  = battery_percent(vbat);

    // Cell protection: "indefinite" sleep if voltage critically low
    if (vbat < BATTERY_PROTECT_V) {
        go_to_sleep(8760ULL * 3600ULL * 1000000ULL);   // ~1 year ≡ "off"
    }

    // RSSI
    int rssi = WiFi.RSSI();

    // Publish
    publish_sensors(temperature, pressure, humidity, vbat, pct, rssi,
                    static_cast<uint32_t>(millis() - wake_start_ms));

    // Final drain — gives TCP stack time to send the last publish before WiFi stops
    if (!mqtt.connected()) mqtt_connect();
    for (int i = 0; i < 10; i++) {   // 10 × 5ms = 50ms
        mqtt.loop();
        delay(5);
    }

    go_to_sleep(SLEEP_DURATION_US);
}

// ─────────────────────────────────────────────────────────────────────────────
// loop() — never reached; setup() always ends in go_to_sleep()
// ─────────────────────────────────────────────────────────────────────────────
void loop() {}