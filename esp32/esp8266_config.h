// config.h - ESP8266 Configuration
// Compatible with both ESP32 and ESP8266

#ifndef CONFIG_H
#define CONFIG_H

// WiFi Configuration
#define WIFI_SSID "I am Not A Witch I am Your Wifi"           // Your WiFi SSID
#define WIFI_PASSWORD "Whoareu@0000"   

// Device Authentication
#define DEVICE_SECRET "7a9aa8ccac979310a8ace9b4a1beedf78439af3ea91ccd5f"
#define MQTT_BROKER_IP "172.16.3.171"
#define MQTT_BROKER 1883
// Relay Configuration (ESP8266-safe GPIO pins)
// Reduced to 4 relays and use the safest GPIOs that avoid boot-mode conflicts.
// Chosen pins: GPIO4 (D2), GPIO5 (D1), GPIO12 (D6), GPIO13 (D7)
const int relayPins[] = {4, 5, 12, 13};
// Manual Switch Configuration: enable four manual inputs. Note: GPIO0 and GPIO2
// affect boot mode and must be wired as active-low with pull-ups. Avoid pressing
// these buttons during reset or boot.
// Mapping: {GPIO14, GPIO16, GPIO0, GPIO2}
const int manualSwitchPins[] = {14, 16, 0, 2};

// Number of switches
#define NUM_SWITCHES 4

// Relay Active Level (HIGH = active high, LOW = active low)
#define RELAY_ACTIVE_HIGH true

// Manual Switch Active Level (true = active low with pullup, false = active high)
#define MANUAL_ACTIVE_LOW true

// Status LED Pin (ESP8266-safe)
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 2  // GPIO 2 (safe for ESP8266)
#endif

#endif // CONFIG_H