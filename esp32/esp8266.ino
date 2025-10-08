#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 2
#endif

// --- INCLUDES AND GLOBALS (must be before all function usage) ---
#ifdef ESP32
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#endif
#include <PubSubClient.h>
#include <ArduinoJson.h>

// Conditional includes for storage and watchdog
#ifdef ESP32
  #include <Preferences.h>
  #include <esp_task_wdt.h>
  Preferences prefs;
  #define USE_PREFERENCES
#elif defined(ESP8266)
  #include <EEPROM.h>
  #define USE_EEPROM
#endif

#include <map>
#include "config_esp8266.h"


// --- Connection State Enum and Status LED ---
enum ConnState {
  WIFI_DISCONNECTED,
  WIFI_ONLY,
  BACKEND_CONNECTED
};
ConnState connState = WIFI_DISCONNECTED;

void blinkStatus() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  unsigned long now = millis();
  int pattern = 0;

  if (connState == WIFI_DISCONNECTED) {
    // Fast blink (250ms on, 250ms off)
    pattern = (now % 500) < 250;
  } else if (connState == WIFI_ONLY) {
    // Slow blink (1s on, 1s off)
    pattern = (now % 2000) < 1000;
  } else if (connState == BACKEND_CONNECTED) {
    // LED constantly ON
    pattern = 1;
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, pattern ? HIGH : LOW);
}

// --- Offline event buffering and storage persistence ---
struct OfflineEvent {
  int gpio;
  bool previousState;
  bool newState;
  unsigned long timestamp;
  bool valid;
};

#define MAX_OFFLINE_EVENTS 30  // Reduced for ESP8266 memory
std::vector<OfflineEvent> offlineEvents;

// Conditional storage functions for ESP32/ESP8266 compatibility
#ifdef ESP8266
// EEPROM address mapping for ESP8266
#define EEPROM_SIZE 512
#define EEPROM_SWITCH_CFG_START 0    // Switch config data
#define EEPROM_OFFLINE_EVENTS_START 200  // Offline events data
#endif

void beginStorage(const char* ns, bool readOnly = true) {
#ifdef ESP32
  prefs.begin(ns, readOnly);
#endif
  // ESP8266 EEPROM doesn't need namespace begin/end calls
}

void endStorage() {
#ifdef ESP32
  prefs.end();
#endif
  // ESP8266 EEPROM doesn't need begin/end calls
}

int getIntStorage(const char* key, int defaultValue = 0) {
#ifdef ESP32
  return prefs.getInt(key, defaultValue);
#elif defined(ESP8266)
  // Deterministic per-switch layout in EEPROM to avoid collisions and garbage
  // Layout per-switch (block size = 16 bytes):
  // offset +0 : int relayGpio (4 bytes)
  // offset +4 : int manualGpio (4 bytes)
  // offset +8 : uint8_t defaultState
  // offset +9 : uint8_t savedState
  // offset +10: uint8_t momentary
  // Keys expected: "relayN", "manualN", other keys will fall back to defaults
  if (strncmp(key, "relay", 5) == 0 || strncmp(key, "manual", 6) == 0) {
    int idx = 0;
    const char* p = key + ((key[0] == 'r') ? 5 : 6);
    idx = atoi(p);
    if (idx < 0 || idx >= NUM_SWITCHES) return defaultValue;
    int base = EEPROM_SWITCH_CFG_START + idx * 16;
    int addr = (strncmp(key, "relay", 5) == 0) ? (base + 0) : (base + 4);
    if (addr < 0 || addr + (int)sizeof(int) > EEPROM_SIZE) return defaultValue;
    int value = 0;
    EEPROM.get(addr, value);
    // EEPROM default is 0xFF; for signed int that becomes -1. Treat -1 as unset.
    return (value == -1) ? defaultValue : value;
  }
  // Fallback: return default for unknown keys
  return defaultValue;
#endif
}

void putIntStorage(const char* key, int value) {
#ifdef ESP32
  prefs.putInt(key, value);
#elif defined(ESP8266)
  // Write into deterministic per-switch layout if key matches expected patterns
  if (strncmp(key, "relay", 5) == 0 || strncmp(key, "manual", 6) == 0) {
    int idx = 0;
    const char* p = key + ((key[0] == 'r') ? 5 : 6);
    idx = atoi(p);
    if (idx < 0 || idx >= NUM_SWITCHES) return;
    int base = EEPROM_SWITCH_CFG_START + idx * 16;
    int addr = (strncmp(key, "relay", 5) == 0) ? (base + 0) : (base + 4);
    if (addr < 0 || addr + (int)sizeof(int) > EEPROM_SIZE) return;
    EEPROM.put(addr, value);
    EEPROM.commit();
    return;
  }
  // Unknown key: ignore for now
#endif
}

bool getBoolStorage(const char* key, bool defaultValue = false) {
#ifdef ESP32
  return prefs.getBool(key, defaultValue);
#elif defined(ESP8266)
  // Handle per-switch boolean keys: defN, stateN, momentaryN
  if (strncmp(key, "def", 3) == 0 || strncmp(key, "state", 5) == 0 || strncmp(key, "momentary", 9) == 0) {
    int idx = 0;
    const char* p = key + ((key[0] == 'd') ? 3 : (key[0] == 's' ? 5 : 9));
    idx = atoi(p);
    if (idx < 0 || idx >= NUM_SWITCHES) return defaultValue;
    int base = EEPROM_SWITCH_CFG_START + idx * 16;
    int addr = base + 8; // defaultState at +8
    if (strncmp(key, "state", 5) == 0) addr = base + 9;
    if (strncmp(key, "momentary", 9) == 0) addr = base + 10;
    if (addr < 0 || addr >= EEPROM_SIZE) return defaultValue;
    uint8_t value = 0xFF;
    EEPROM.get(addr, value);
    return value == 0xFF ? defaultValue : (value != 0);
  }
  return defaultValue;
#endif
}

void putBoolStorage(const char* key, bool value) {
#ifdef ESP32
  prefs.putBool(key, value);
#elif defined(ESP8266)
  if (strncmp(key, "def", 3) == 0 || strncmp(key, "state", 5) == 0 || strncmp(key, "momentary", 9) == 0) {
    int idx = 0;
    const char* p = key + ((key[0] == 'd') ? 3 : (key[0] == 's' ? 5 : 9));
    idx = atoi(p);
    if (idx < 0 || idx >= NUM_SWITCHES) return;
    int base = EEPROM_SWITCH_CFG_START + idx * 16;
    int addr = base + 8; // defaultState at +8
    if (strncmp(key, "state", 5) == 0) addr = base + 9;
    if (strncmp(key, "momentary", 9) == 0) addr = base + 10;
    if (addr < 0 || addr >= EEPROM_SIZE) return;
    EEPROM.put(addr, (uint8_t)value);
    EEPROM.commit();
    return;
  }
  // Unknown key: ignore
#endif
}

unsigned long getULongStorage(const char* key, unsigned long defaultValue = 0) {
#ifdef ESP32
  return prefs.getULong(key, defaultValue);
#elif defined(ESP8266)
  // EEPROM implementation
  return defaultValue;
#endif
}

void putULongStorage(const char* key, unsigned long value) {
#ifdef ESP32
  prefs.putULong(key, value);
#elif defined(ESP8266)
  // EEPROM implementation
#endif
}

void queueOfflineEvent(int gpio, bool previousState, bool newState) {
  if (offlineEvents.size() >= MAX_OFFLINE_EVENTS) {
    offlineEvents.erase(offlineEvents.begin());
  }
  OfflineEvent event = {gpio, previousState, newState, millis(), true};
  offlineEvents.push_back(event);
  saveOfflineEventsToStorage();
  Serial.printf("[OFFLINE] Queued event: GPIO %d %s -> %s (buffer: %d/%d)\n",
    gpio, previousState ? "ON" : "OFF", newState ? "ON" : "OFF", (int)offlineEvents.size(), MAX_OFFLINE_EVENTS);
}

void saveOfflineEventsToStorage() {
  beginStorage("offline_events", false);
  int numEvents = std::min((int)offlineEvents.size(), MAX_OFFLINE_EVENTS);
  putIntStorage("count", numEvents);
  for (int i = 0; i < numEvents; i++) {
    char gpioKey[16], prevKey[16], newKey[16], tsKey[16], validKey[16];
    sprintf(gpioKey, "gpio%d", i);
    sprintf(prevKey, "prev%d", i);
    sprintf(newKey, "new%d", i);
    sprintf(tsKey, "ts%d", i);
    sprintf(validKey, "valid%d", i);

    putIntStorage(gpioKey, offlineEvents[i].gpio);
    putBoolStorage(prevKey, offlineEvents[i].previousState);
    putBoolStorage(newKey, offlineEvents[i].newState);
    putULongStorage(tsKey, offlineEvents[i].timestamp);
    putBoolStorage(validKey, offlineEvents[i].valid);
  }
  endStorage();
  Serial.printf("[STORAGE] Saved %d offline events\n", numEvents);
}

void loadOfflineEventsFromStorage() {
  beginStorage("offline_events", true);
  int numEvents = getIntStorage("count", 0);
  if (numEvents <= 0 || numEvents > MAX_OFFLINE_EVENTS) {
    endStorage();
    return;
  }
  offlineEvents.clear();
  for (int i = 0; i < numEvents; i++) {
    char gpioKey[16], prevKey[16], newKey[16], tsKey[16], validKey[16];
    sprintf(gpioKey, "gpio%d", i);
    sprintf(prevKey, "prev%d", i);
    sprintf(newKey, "new%d", i);
    sprintf(tsKey, "ts%d", i);
    sprintf(validKey, "valid%d", i);

    OfflineEvent event;
    event.gpio = getIntStorage(gpioKey, -1);
    event.previousState = getBoolStorage(prevKey, false);
    event.newState = getBoolStorage(newKey, false);
    event.timestamp = getULongStorage(tsKey, 0);
    event.valid = getBoolStorage(validKey, false);
    if (event.gpio >= 0 && event.valid) offlineEvents.push_back(event);
  }
  endStorage();
  Serial.printf("[STORAGE] Loaded %d offline events\n", (int)offlineEvents.size());
}

#define MQTT_BROKER "192.168.0.108" // Set to backend IP or broker IP
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASSWORD ""
#define SWITCH_TOPIC "esp8266/switches"
#define STATE_TOPIC "esp8266/state"
#define TELEMETRY_TOPIC "esp8266/telemetry"
#define CONFIG_TOPIC "esp8266/config"

WiFiClient espClient;
PubSubClient mqttClient(espClient);

char mqttClientId[24];

// Forward declarations
void publishManualSwitchEvent(int gpio, bool state);
void sendStateUpdate(bool force);
int relayNameToGpio(const char* relay);
String normalizeMac(String mac);

// Conditional watchdog functions
void initWatchdog() {
#ifdef ESP32
  esp_task_wdt_config_t wdt_config = { .timeout_ms = 10000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
#elif defined(ESP8266)
  ESP.wdtEnable(10000);  // 10 second timeout
#endif
}

void resetWatchdog() {
#ifdef ESP32
  esp_task_wdt_reset();
#elif defined(ESP8266)
  ESP.wdtFeed();
#endif
}

// Conditional heap monitoring
void getHeapInfo(size_t& freeHeap, size_t& minFreeHeap) {
#ifdef ESP32
  freeHeap = ESP.getFreeHeap();
  minFreeHeap = ESP.getMinFreeHeap();
#elif defined(ESP8266)
  freeHeap = ESP.getFreeHeap();
  minFreeHeap = ESP.getMaxFreeBlockSize();  // ESP8266 equivalent
#endif
}

void sendQueuedOfflineEvents() {
  if (offlineEvents.empty() || !mqttClient.connected()) return;
  Serial.printf("[OFFLINE] Sending %d queued events to backend...\n", (int)offlineEvents.size());
  for (auto &event : offlineEvents) {
    publishManualSwitchEvent(event.gpio, event.newState);
    delay(50); // Small delay to avoid overwhelming broker
  }
  offlineEvents.clear();
  saveOfflineEventsToStorage();
  Serial.println("[OFFLINE] All queued events sent to backend successfully");
}

// --- Status LED patterns ---
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 2
#endif

// Topics:
//   - "esp32/switches" (subscribe): backend -> ESP32 switch commands
//   - "esp32/state" (publish): ESP32 -> backend state updates
//   - "esp32/telemetry" (publish): ESP32 -> backend telemetry (optional)

// --- GLOBALS AND MACROS (must be before all function usage) ---

// Number of switches (relays)
#ifndef NUM_SWITCHES
#define NUM_SWITCHES (sizeof(relayPins) / sizeof(relayPins[0]))
#endif

// Local switch state array
struct SwitchState {
  int relayGpio;           // Relay control GPIO (output)
  int manualGpio;          // Manual switch GPIO (input)
  bool state;              // Logical ON/OFF state
  bool manualOverride;     // Whether this switch was manually overridden
  bool manualEnabled;      // Whether manual input is active
  bool manualActiveLow;    // Input polarity (true = active low)
  bool manualMomentary;    // true = momentary, false = maintained
  int lastManualLevel;     // Last raw digitalRead level
  unsigned long lastManualChangeMs; // Last time raw level flipped
  int stableManualLevel;   // Debounced level
  bool lastManualActive;   // Previous debounced logical active level
  bool defaultState;       // Default state for offline mode
  int gpio;                // For compatibility with publishState
};
SwitchState switchesLocal[NUM_SWITCHES];

// Track if manual pins have been set up
bool pinSetup[16] = {false}; // Support up to 16 switches

// Command struct for queue
struct Command {
  int gpio;
  bool state;
  bool valid;
  unsigned long timestamp;
};

#define MAX_COMMAND_QUEUE 16
Command commandQueue[MAX_COMMAND_QUEUE];
int commandQueueHead = 0;
int commandQueueTail = 0;

// Timer for periodic state sending
unsigned long lastStateSend = 0;

// --- END GLOBALS ---

// Add a command to the queue
void queueSwitchCommand(int gpio, bool state) {
  int nextTail = (commandQueueTail + 1) % MAX_COMMAND_QUEUE;
  if (nextTail == commandQueueHead) {
    Serial.println("[CMD] Command queue full, dropping command");
    return;
  }
  commandQueue[commandQueueTail] = {gpio, state, true, millis()};
  commandQueueTail = nextTail;
  Serial.printf("[CMD] Queued command: GPIO %d -> %s\n", gpio, state ? "ON" : "OFF");
}

// Process commands in the queue (rate limited)
void processCommandQueue() {
  static unsigned long lastProcess = 0;
  unsigned long now = millis();
  if (now - lastProcess < 10) return; // 10ms interval (reduced from 25ms)
  lastProcess = now;

  int processed = 0;
  while (commandQueueHead != commandQueueTail && processed < 5) {
    Command &cmd = commandQueue[commandQueueHead];
    // Find the switch and apply state
    for (int i = 0; i < NUM_SWITCHES; i++) {
      if (switchesLocal[i].relayGpio == cmd.gpio) {
        switchesLocal[i].state = cmd.state;
        switchesLocal[i].manualOverride = false;
        digitalWrite(switchesLocal[i].relayGpio, cmd.state ? (RELAY_ACTIVE_HIGH ? HIGH : LOW) : (RELAY_ACTIVE_HIGH ? LOW : HIGH));
        switchesLocal[i].gpio = switchesLocal[i].relayGpio; // for publishState
        saveSwitchConfigToStorage();  // Save state change
        Serial.printf("[CMD] Relay GPIO %d set to %s (from queue)\n", cmd.gpio, cmd.state ? "ON" : "OFF");
        break;
      }
    }
    commandQueueHead = (commandQueueHead + 1) % MAX_COMMAND_QUEUE;
    processed++;
  }
}

// --- Dynamic config loading/saving ---
void loadSwitchConfigFromStorage() {
  beginStorage("switch_cfg", true);
  for (int i = 0; i < NUM_SWITCHES; i++) {
    char relayKey[16], manualKey[16], defKey[16], stateKey[16], momentaryKey[16];
    sprintf(relayKey, "relay%d", i);
    sprintf(manualKey, "manual%d", i);
    sprintf(defKey, "def%d", i);
    sprintf(stateKey, "state%d", i);
    sprintf(momentaryKey, "momentary%d", i);

    int relay = getIntStorage(relayKey, relayPins[i]);
    int manual = getIntStorage(manualKey, manualSwitchPins[i]);
    bool defState = getBoolStorage(defKey, false);
    bool savedState = getBoolStorage(stateKey, false);
    bool momentary = getBoolStorage(momentaryKey, false);  // Load momentary setting
    switchesLocal[i].relayGpio = relay;
    switchesLocal[i].manualGpio = manual;
    switchesLocal[i].defaultState = defState;
    switchesLocal[i].state = savedState;  // Load saved state
    switchesLocal[i].manualMomentary = momentary;  // Load momentary setting
  }
  endStorage();
  Serial.println("[STORAGE] Loaded switch config and state from storage");
  // Diagnostic dump to help detect garbage/invalid GPIOs after EEPROM load
  for (int i = 0; i < NUM_SWITCHES; i++) {
    Serial.printf("[STORAGE] Switch %d -> relayGpio=%d, manualGpio=%d, state=%d, momentary=%d\n",
      i, switchesLocal[i].relayGpio, switchesLocal[i].manualGpio, switchesLocal[i].state ? 1 : 0, switchesLocal[i].manualMomentary ? 1 : 0);
  }
}

void saveSwitchConfigToStorage() {
  beginStorage("switch_cfg", false);
  for (int i = 0; i < NUM_SWITCHES; i++) {
    char relayKey[16], manualKey[16], defKey[16], stateKey[16], momentaryKey[16];
    sprintf(relayKey, "relay%d", i);
    sprintf(manualKey, "manual%d", i);
    sprintf(defKey, "def%d", i);
    sprintf(stateKey, "state%d", i);
    sprintf(momentaryKey, "momentary%d", i);

    putIntStorage(relayKey, switchesLocal[i].relayGpio);
    putIntStorage(manualKey, switchesLocal[i].manualGpio);
    putBoolStorage(defKey, switchesLocal[i].defaultState);
    putBoolStorage(stateKey, switchesLocal[i].state);  // Save current state
    putBoolStorage(momentaryKey, switchesLocal[i].manualMomentary);  // Save momentary setting
  }
  endStorage();
  Serial.println("[STORAGE] Saved switch config and state to storage");
}

// Initialize switch state array with mapping, loading from storage if available
void initSwitches() {
  for (int i = 0; i < NUM_SWITCHES; i++) {
    switchesLocal[i].relayGpio = relayPins[i];  // Use const defaults
    switchesLocal[i].manualGpio = manualSwitchPins[i];  // Use const defaults
    switchesLocal[i].state = false;
    switchesLocal[i].manualOverride = false;
    switchesLocal[i].manualEnabled = true;
    switchesLocal[i].manualActiveLow = MANUAL_ACTIVE_LOW;
    switchesLocal[i].manualMomentary = true;  // Default to momentary
    switchesLocal[i].lastManualLevel = -1;
    switchesLocal[i].lastManualChangeMs = 0;
    switchesLocal[i].stableManualLevel = -1;
    switchesLocal[i].lastManualActive = false;
    switchesLocal[i].defaultState = false;
  }
  loadSwitchConfigFromStorage();  // This will override with saved config
  // Apply loaded/saved states to relays
  for (int i = 0; i < NUM_SWITCHES; i++) {
    pinMode(switchesLocal[i].relayGpio, OUTPUT);
    digitalWrite(switchesLocal[i].relayGpio, switchesLocal[i].state ? (RELAY_ACTIVE_HIGH ? HIGH : LOW) : (RELAY_ACTIVE_HIGH ? LOW : HIGH));
    pinSetup[i] = false;  // Force re-setup of manual pins
  }
}

// Debounce and manual switch handling constants
#define MANUAL_DEBOUNCE_MS 20  // Reduced from 80ms for faster response
#define MANUAL_REPEAT_IGNORE_MS 100  // Reduced from 200ms

void handleManualSwitches() {
  unsigned long now = millis();
  for (int i = 0; i < NUM_SWITCHES; i++) {
    SwitchState &sw = switchesLocal[i];
    if (!sw.manualEnabled || sw.manualGpio < 0) continue;

    // Setup pinMode if not already done (first call)
    if (!pinSetup[i]) {
      pinMode(sw.manualGpio, INPUT_PULLUP);
      pinMode(sw.relayGpio, OUTPUT);
      digitalWrite(sw.relayGpio, sw.state ? (RELAY_ACTIVE_HIGH ? HIGH : LOW) : (RELAY_ACTIVE_HIGH ? LOW : HIGH)); // Apply current state

      // Initialize with current pin reading
      int initialLevel = digitalRead(sw.manualGpio);
      sw.lastManualLevel = initialLevel;
      sw.stableManualLevel = initialLevel;
      sw.lastManualActive = sw.manualActiveLow ? (initialLevel == LOW) : (initialLevel == HIGH);
      sw.lastManualChangeMs = now;

      pinSetup[i] = true;
      Serial.printf("[MANUAL] Setup GPIO %d (manual pin %d), initial raw=%d\n", sw.relayGpio, sw.manualGpio, initialLevel);
    }

    int rawLevel = digitalRead(sw.manualGpio);
    bool active = sw.manualActiveLow ? (rawLevel == LOW) : (rawLevel == HIGH);

    // Debug: Log pin readings periodically
    static unsigned long lastDebug[16] = {0};
    if (now - lastDebug[i] > 5000) { // Log every 5 seconds
      Serial.printf("[MANUAL] GPIO %d manual pin %d: raw=%d, active=%d, state=%d, lastActive=%d\n",
        sw.relayGpio, sw.manualGpio, rawLevel, active, sw.state, sw.lastManualActive);
      lastDebug[i] = now;
    }

    // Detect level change
    if (rawLevel != sw.lastManualLevel) {
      sw.lastManualChangeMs = now;
      sw.lastManualLevel = rawLevel;
      Serial.printf("[MANUAL] GPIO %d pin change detected: raw=%d\n", sw.relayGpio, rawLevel);
    }

    // Debounce: wait for stable reading
    if ((now - sw.lastManualChangeMs) > MANUAL_DEBOUNCE_MS) {
      // Check if the stable level changed
      if (rawLevel != sw.stableManualLevel) {
        sw.stableManualLevel = rawLevel;

        // Recalculate active state after debounce
        bool currentActive = sw.manualActiveLow ? (rawLevel == LOW) : (rawLevel == HIGH);

        Serial.printf("[MANUAL] GPIO %d stable change: momentary=%d, active=%d->%d\n",
          sw.relayGpio, sw.manualMomentary, sw.lastManualActive, currentActive);

        if (sw.manualMomentary) {
          // Momentary: toggle on button press (active transition)
          if (currentActive && !sw.lastManualActive) {
            // Button pressed (inactive -> active transition)
            bool prevState = sw.state;
            sw.state = !sw.state;
            sw.manualOverride = true;
            digitalWrite(sw.relayGpio, sw.state ? (RELAY_ACTIVE_HIGH ? HIGH : LOW) : (RELAY_ACTIVE_HIGH ? LOW : HIGH));
            saveSwitchConfigToStorage();
            Serial.printf("[MANUAL] Momentary PRESS: Relay GPIO %d toggled to %s\n", sw.relayGpio, sw.state ? "ON" : "OFF");

            // Send manual switch event for logging
            if (mqttClient.connected()) {
              publishManualSwitchEvent(sw.relayGpio, sw.state);
            }
            // Buffer event if offline
            if (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) {
              queueOfflineEvent(sw.relayGpio, prevState, sw.state);
            }
            // Send immediate state update for UI
            sendStateUpdate(true);
          }
        } else {
          // Maintained: switch state follows manual input
          if (sw.state != currentActive) {
            bool prevState = sw.state;
            sw.state = currentActive;
            sw.manualOverride = true;
            digitalWrite(sw.relayGpio, sw.state ? (RELAY_ACTIVE_HIGH ? HIGH : LOW) : (RELAY_ACTIVE_HIGH ? LOW : HIGH));
            saveSwitchConfigToStorage();
            Serial.printf("[MANUAL] Maintained: Relay GPIO %d set to %s\n", sw.relayGpio, sw.state ? "ON" : "OFF");

            // Send manual switch event for logging
            if (mqttClient.connected()) {
              publishManualSwitchEvent(sw.relayGpio, sw.state);
            }
            // Buffer event if offline
            if (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) {
              queueOfflineEvent(sw.relayGpio, prevState, sw.state);
            }
            // Send immediate state update for UI
            sendStateUpdate(true);
          }
        }

        // Update last active state
        sw.lastManualActive = currentActive;
      }
    }
  }
}

// --- Heartbeat logic ---
void sendHeartbeat() {
  static unsigned long lastHeartbeat = 0;
  unsigned long now = millis();
  if (now - lastHeartbeat < 30000) return; // 30s interval (increased from 10s)
  lastHeartbeat = now;
  DynamicJsonDocument doc(128);
  doc["mac"] = WiFi.macAddress();
  doc["userId"] = "default_user"; // Add user ID to heartbeat
  doc["status"] = "heartbeat";
  size_t freeHeap, minFreeHeap;
  getHeapInfo(freeHeap, minFreeHeap);
  doc["heap"] = freeHeap;
  char buf[128];
  size_t n = serializeJson(doc, buf);
  mqttClient.publish(TELEMETRY_TOPIC, buf, n);
  Serial.println("[HEARTBEAT] Sent heartbeat telemetry");
}

bool pendingState = false;

void checkSystemHealth() {
  static unsigned long lastHealthCheck = 0;
  unsigned long now = millis();
  if (now - lastHealthCheck < 10000) return; // 10s interval
  lastHealthCheck = now;

  // Heap monitoring
  size_t freeHeap, minFreeHeap;
  getHeapInfo(freeHeap, minFreeHeap);
  Serial.printf("[HEALTH] Heap: %u bytes free, Min: %u\n", freeHeap, minFreeHeap);
  if (freeHeap < 10000) {  // Lower threshold for ESP8266
    Serial.printf("[CRITICAL] Very low heap memory: %u bytes free!\n", freeHeap);
  }

  // Command queue monitoring
  int queueItems = (commandQueueTail - commandQueueHead + MAX_COMMAND_QUEUE) % MAX_COMMAND_QUEUE;
  if (queueItems > MAX_COMMAND_QUEUE / 2) {
    Serial.printf("[WARNING] Command queue backing up: %d/%d items\n", queueItems, MAX_COMMAND_QUEUE);
  }

  // Switch state summary
  int activeSwitches = 0;
  for (int i = 0; i < NUM_SWITCHES; i++) {
    if (switchesLocal[i].state) activeSwitches++;
  }
  Serial.printf("[HEALTH] Active switches: %d/%d\n", activeSwitches, NUM_SWITCHES);
}

// --- END: Stubs for missing functions/variables ---

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    connState = WIFI_ONLY;
    // Set unique MQTT client ID based on MAC address
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    sprintf(mqttClientId, "ESP_%s", mac.c_str());
    Serial.printf("MQTT Client ID: %s\n", mqttClientId);
    
    // Display full connection status
    displayIPAddress();
  } else {
    Serial.println("\nWiFi connection failed, running in offline mode");
    connState = WIFI_DISCONNECTED;
  }
}

void updateConnectionStatus() {
  static ConnState lastConnState = WIFI_DISCONNECTED;
  static unsigned long lastStatusChange = 0;
  static bool offlineLogged = false;
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    connState = WIFI_DISCONNECTED;
  } else if (!mqttClient.connected()) {
    connState = WIFI_ONLY;
  } else {
    connState = BACKEND_CONNECTED;
  }
  if (connState != lastConnState) {
    Serial.printf("[STATUS] Connection state changed: %d -> %d\n", lastConnState, connState);
    lastConnState = connState;
    lastStatusChange = now;
    sendStateUpdate(true);
    if (!offlineEvents.empty()) {
      Serial.printf("[STATUS] %d offline events queued for backend transmission\n", (int)offlineEvents.size());
    }
    if (connState == BACKEND_CONNECTED) {
      offlineLogged = false;  // Reset when connected
    }
  }
  if (!offlineLogged && connState != BACKEND_CONNECTED && (now - lastStatusChange > 30000)) {
    Serial.println("[STATUS] Confirmed offline - no backend connection for 30+ seconds");
    Serial.printf("[STATUS] Manual switches will work independently. %d events queued.\n", (int)offlineEvents.size());
    offlineLogged = true;
  }
}

void reconnect_mqtt() {
  static unsigned long lastReconnectAttempt = 0;
  unsigned long now = millis();
  if (now - lastReconnectAttempt < 5000) return;
  lastReconnectAttempt = now;
  Serial.print("Attempting MQTT connection...");
  if (mqttClient.connect(mqttClientId, MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("connected");
    mqttClient.subscribe(SWITCH_TOPIC);
    mqttClient.subscribe(CONFIG_TOPIC);
    sendStateUpdate(true);
    sendQueuedOfflineEvents();
    connState = BACKEND_CONNECTED;
    Serial.println("[MQTT] Successfully reconnected - sent current state and queued events");
  } else {
    Serial.print("failed, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" will retry later");
    connState = WIFI_ONLY;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length == 0 || payload == nullptr) {
    Serial.println("[MQTT] Empty payload received, ignoring");
    return;
  }
  String message;
  if (length > 512) {
    Serial.println("[MQTT] Payload too large, ignoring");
    return;
  }
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.printf("[MQTT] Message arrived [%s]: %s\n", topic, message.c_str());
  if (String(topic) == SWITCH_TOPIC) {
    if (message.startsWith("{")) {
      DynamicJsonDocument doc(256);
      DeserializationError err = deserializeJson(doc, message);
      if (!err && doc["mac"].is<const char*>() && doc["secret"].is<const char*>() && doc["userId"].is<const char*>() && doc["gpio"].is<int>() && doc["state"].is<bool>()) {
        String targetMac = doc["mac"];
        String targetSecret = doc["secret"];
        String targetUserId = doc["userId"];
        String myMac = WiFi.macAddress();
        String normalizedTarget = normalizeMac(targetMac);
        String normalizedMy = normalizeMac(myMac);
        if (normalizedTarget.equalsIgnoreCase(normalizedMy)) {
          if (targetSecret == String(DEVICE_SECRET)) {
            // Check user authorization (you can add more complex logic here)
            if (targetUserId == "default_user" || targetUserId == "admin") {
              int gpio = doc["gpio"];
              bool state = doc["state"];
              bool validGpio = false;
              for (int i = 0; i < NUM_SWITCHES; i++) {
                if (switchesLocal[i].relayGpio == gpio) { validGpio = true; break; }
              }
              if (validGpio) {
                queueSwitchCommand(gpio, state);
                processCommandQueue();
                Serial.printf("[MQTT] Processed command for user %s: GPIO %d -> %s\n", targetUserId.c_str(), gpio, state ? "ON" : "OFF");
              } else {
                Serial.printf("[MQTT] Invalid GPIO %d, ignoring command\n", gpio);
              }
            } else {
              Serial.printf("[MQTT] Unauthorized user %s, ignoring command\n", targetUserId.c_str());
            }
          } else {
            Serial.println("[MQTT] Invalid secret, ignoring command");
          }
        } else {
          Serial.printf("[MQTT] Ignored command for different device: %s (my MAC: %s)\n", targetMac.c_str(), myMac.c_str());
        }
      } else {
        Serial.println("[MQTT] Invalid JSON format or missing required fields");
      }
    } else {
      int colon = message.indexOf(':');
      if (colon > 0) {
        String relay = message.substring(0, colon);
        String stateStr = message.substring(colon + 1);
        bool state = (stateStr == "on");
        int gpio = relayNameToGpio(relay.c_str());
        if (gpio >= 0) {
          queueSwitchCommand(gpio, state);
          processCommandQueue();
        }
      }
    }
  } else if (String(topic) == CONFIG_TOPIC) {
    if (message.startsWith("{")) {
      DynamicJsonDocument doc(512);
      DeserializationError err = deserializeJson(doc, message);
      if (!err && doc["mac"].is<const char*>() && doc["secret"].is<const char*>() && doc["userId"].is<const char*>() && doc["switches"].is<JsonArray>()) {
        String targetMac = doc["mac"];
        String targetSecret = doc["secret"];
        String targetUserId = doc["userId"];
        String myMac = WiFi.macAddress();
        String normalizedTarget = normalizeMac(targetMac);
        String normalizedMy = normalizeMac(myMac);
        if (normalizedTarget.equalsIgnoreCase(normalizedMy)) {
          if (targetSecret == String(DEVICE_SECRET)) {
            // Check user authorization for config changes
            if (targetUserId == "admin") {  // Only admin can change config
              JsonArray switches = doc["switches"].as<JsonArray>();
              int n = switches.size();
              if (n > 0 && n <= 6) {
                Serial.printf("[CONFIG] Processing config for %d switches by user %s\n", n, targetUserId.c_str());
                for (int i = 0; i < n; i++) {
                  JsonObject sw = switches[i];
                  switchesLocal[i].relayGpio = sw["gpio"] | 0;
                  switchesLocal[i].manualGpio = sw.containsKey("manualGpio") ? (int)sw["manualGpio"] : -1;
                  // Handle momentary/maintained mode
                  if (sw.containsKey("manualMode")) {
                    String mode = sw["manualMode"];
                    switchesLocal[i].manualMomentary = (mode == "momentary");
                    Serial.printf("[CONFIG] Switch %d: gpio=%d, manualGpio=%d, manualMode=%s, momentary=%d\n",
                      i, switchesLocal[i].relayGpio, switchesLocal[i].manualGpio, mode.c_str(), switchesLocal[i].manualMomentary);
                  } else {
                    Serial.printf("[CONFIG] Switch %d: no manualMode field found\n", i);
                  }
                }
                saveSwitchConfigToStorage();
                // Reinitialize switches with new configuration
                for (int i = 0; i < NUM_SWITCHES; i++) {
                  pinMode(switchesLocal[i].relayGpio, OUTPUT);
                  digitalWrite(switchesLocal[i].relayGpio, switchesLocal[i].state ? (RELAY_ACTIVE_HIGH ? HIGH : LOW) : (RELAY_ACTIVE_HIGH ? LOW : HIGH));
                  pinSetup[i] = false;  // Force re-setup of manual pins
                }
                Serial.println("[CONFIG] Pin configuration and switch modes updated from server and applied.");
              }
            } else {
              Serial.printf("[CONFIG] User %s not authorized for config changes\n", targetUserId.c_str());
            }
          } else {
            Serial.println("[CONFIG] Invalid secret, ignoring config update");
          }
        }
      }
    }
  }
}

// Publish state update to backend
void publishState() {
  DynamicJsonDocument doc(1024);
  doc["mac"] = WiFi.macAddress();
  doc["secret"] = DEVICE_SECRET; // Include device secret for authentication
  doc["userId"] = "default_user"; // Add user ID for authentication
  JsonArray arr = doc.createNestedArray("switches");
  for (int i = 0; i < NUM_SWITCHES; i++) {
    JsonObject o = arr.createNestedObject();
    o["gpio"] = switchesLocal[i].relayGpio;
    o["state"] = switchesLocal[i].state;
    o["manual_override"] = switchesLocal[i].manualOverride;
  }
  char buf[1024];
  size_t n = serializeJson(doc, buf);
  mqttClient.publish(STATE_TOPIC, buf, n);
  Serial.println("[MQTT] Published state update");
}

// Publish manual switch event for logging
void publishManualSwitchEvent(int gpio, bool state) {
  DynamicJsonDocument doc(128);
  doc["mac"] = WiFi.macAddress();
  doc["secret"] = DEVICE_SECRET; // Include device secret for authentication
  doc["userId"] = "default_user"; // Add user ID for authentication
  doc["type"] = "manual_switch";
  doc["gpio"] = gpio;
  doc["state"] = state;
  doc["timestamp"] = millis();
  char buf[128];
  size_t n = serializeJson(doc, buf);
  mqttClient.publish(TELEMETRY_TOPIC, buf, n);
  Serial.printf("[MQTT] Published manual switch event: GPIO %d -> %s\n", gpio, state ? "ON" : "OFF");
}

// --- State update debounce logic ---
void sendStateUpdate(bool force) {
  static unsigned long lastStateSent = 0;
  static uint32_t lastStateHash = 0;
  unsigned long now = millis();
  // Compute a simple hash of switch states
  uint32_t stateHash = 0;
  for (int i = 0; i < NUM_SWITCHES; i++) {
    stateHash ^= (switchesLocal[i].state ? (1 << i) : 0);
    stateHash ^= (switchesLocal[i].manualOverride ? (1 << (i+8)) : 0);
  }
  bool changed = (stateHash != lastStateHash);
  if (force || changed || (now - lastStateSent > 5000)) {
    publishState();
    lastStateSent = now;
    lastStateHash = stateHash;
    pendingState = false;
  } else {
    pendingState = true;
  }
}

// Map relay name to GPIO (implement as needed)
int relayNameToGpio(const char* relay) {
  // Example: "relay1" -> 16, "relay2" -> 17, etc.
  if (strcmp(relay, "relay1") == 0) return 16;
  if (strcmp(relay, "relay2") == 0) return 17;
  if (strcmp(relay, "relay3") == 0) return 18;
  if (strcmp(relay, "relay4") == 0) return 19;
  if (strcmp(relay, "relay5") == 0) return 21;
  if (strcmp(relay, "relay6") == 0) return 22;
  return -1;
}

// Normalize MAC address: remove colons and make lowercase
String normalizeMac(String mac) {
  String normalized = "";
  for (char c : mac) {
    if (isAlphaNumeric(c)) {
      normalized += tolower(c);
    }
  }
  return normalized;
}

// Function to display ESP8266 IP address and connection status
void displayIPAddress() {
  Serial.println("\n=== ESP8266 CONNECTION STATUS ===");
  
  // WiFi Status
  Serial.print("WiFi Status: ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("CONNECTED");
    Serial.print("WiFi SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Subnet Mask: ");
    Serial.println(WiFi.subnetMask());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());
    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());
    Serial.print("Signal Strength (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("DISCONNECTED");
    Serial.print("WiFi Status Code: ");
    Serial.println(WiFi.status());
  }
  
  // MQTT Status
  Serial.print("MQTT Status: ");
  if (mqttClient.connected()) {
    Serial.println("CONNECTED");
    Serial.print("MQTT Broker: ");
    Serial.print(MQTT_BROKER);
    Serial.print(":");
    Serial.println(MQTT_PORT);
    Serial.print("MQTT Client ID: ");
    Serial.println(mqttClientId);
  } else {
    Serial.println("DISCONNECTED");
  }
  
  // Connection State
  Serial.print("Connection State: ");
  switch (connState) {
    case WIFI_DISCONNECTED:
      Serial.println("WIFI_DISCONNECTED (No WiFi)");
      break;
    case WIFI_ONLY:
      Serial.println("WIFI_ONLY (WiFi connected, MQTT disconnected)");
      break;
    case BACKEND_CONNECTED:
      Serial.println("BACKEND_CONNECTED (Fully connected)");
      break;
  }
  
  // Device Info
  Serial.print("Device: ");
  #ifdef ESP32
    Serial.println("ESP32");
    Serial.print("Chip Model: ");
    Serial.println(ESP.getChipModel());
    Serial.print("EFuse MAC: ");
    {
      uint64_t mac = ESP.getEfuseMac();
      char macStr[20];
      sprintf(macStr, "%04x%08x", (uint16_t)(mac >> 32), (uint32_t)(mac & 0xFFFFFFFF));
      Serial.println(macStr);
    }
    Serial.print("Flash Size: ");
    Serial.print(ESP.getFlashChipSize() / 1024 / 1024);
    Serial.println(" MB");
    Serial.print("Free Heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
  #elif defined(ESP8266)
    Serial.println("ESP8266");
    Serial.print("Chip ID (ESP.getChipId is not available on ESP32 builds): ");
    // On ESP8266 builds, ESP.getChipId() exists. For cross-compilation we avoid calling it on ESP32.
    Serial.println(ESP.getChipId());
    Serial.print("Flash Size: ");
    Serial.print(ESP.getFlashChipSize() / 1024 / 1024);
    Serial.println(" MB");
    Serial.print("Free Heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
  #else
    Serial.println("Unknown MCU");
  #endif
  
  Serial.println("===================================\n");
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP Classroom Automation System (MQTT)");
#ifdef ESP8266
  EEPROM.begin(EEPROM_SIZE);
#endif
  initWatchdog();  // Use conditional watchdog
  initSwitches();
  loadOfflineEventsFromStorage();
  setup_wifi();
  Serial.println("WiFi setup complete, initializing MQTT...");
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(256);  // Reduced for ESP8266 memory
  mqttClient.setCallback(mqttCallback);
  mqttClient.subscribe(CONFIG_TOPIC);
}

void loop() {
  resetWatchdog();  // Use conditional watchdog reset
  
  // Handle serial commands
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.equalsIgnoreCase("ip") || command.equalsIgnoreCase("status") || command.equalsIgnoreCase("info")) {
      displayIPAddress();
    } else if (command.equalsIgnoreCase("help")) {
      Serial.println("\n=== ESP8266 Serial Commands ===");
      Serial.println("ip      - Display IP address and connection status");
      Serial.println("status  - Display IP address and connection status");
      Serial.println("info    - Display IP address and connection status");
      Serial.println("help    - Show this help message");
      Serial.println("===========================\n");
    }
  }
  
  unsigned long now = millis();
  handleManualSwitches();
  updateConnectionStatus();
  if (millis() - lastStateSend > 30000) {
    sendStateUpdate(true);
    lastStateSend = millis();
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) reconnect_mqtt();
    if (mqttClient.connected()) {
      mqttClient.loop();
      processCommandQueue();
      static unsigned long lastOfflineSend = 0;
      if (now - lastOfflineSend > 10000) {
        sendQueuedOfflineEvents();
        lastOfflineSend = now;
      }
    }
    sendHeartbeat();
  }
  blinkStatus();
  if (pendingState) sendStateUpdate(false);
  checkSystemHealth();
  delay(10);
}