#pragma once

// WLAN
#define WIFI_SSID      "FRITZ!Box 7530 US"
#define WIFI_PASS      "57734535437779957113"
#define WIFI_HOSTNAME  "PylontechBattery"
// Statische IP
#define USE_STATIC_IP     1
#define IP_ADDRESS  IPAddress(192,168,188,74)
#define IP_GATEWAY  IPAddress(192,168,188,1)
#define IP_SUBNET   IPAddress(255,255,255,0)
#define IP_DNS1     IPAddress(192,168,188,39)
#define IP_DNS2     IPAddress(1,1,1,1)
// MQTT (optional)
#define ENABLE_MQTT    1
#define MQTT_SERVER    "192.168.188.75"
#define MQTT_PORT      1883
#define MQTT_USER      "emtec"
#define MQTT_PASSWORD  "gh3gb14"
#define MQTT_TOPIC_ROOT            "pylontech/sensor/grid_battery/"
#define HA_DISCOVERY_SENSOR_PREFIX "homeassistant/sensor/"
#define HA_DISCOVERY_BINARY_PREFIX  "homeassistant/binary_sensor/"

// NTP/Zeit
#define NTP_SERVER     "pool.ntp.org"
#define GMT_OFFSET_SEC 0
#ifndef GMT
#define GMT           GMT_OFFSET_SEC
#endif

// UART2
#define PIN_RX2       16
#define PIN_TX2       17
#define DEFAULT_BAUD  115200

// Firmware-Layout der Pylon-Ausgabe (1 oder 2)
#ifndef FW_VERSION
#define FW_VERSION    1
#endif

// ESP32TimerInterrupt Settings
#define USING_ESP32_TIMERINTERRUPT   true
#define TIMER_BASE_CLK               80000000  // 80 MHz
#define TIMER_DIVIDER                80        // -> 1 MHz