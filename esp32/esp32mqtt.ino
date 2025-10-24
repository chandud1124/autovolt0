#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 2
#endif

// --- INCLUDES AND GLOBALS (must be before all function usage) ---
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <map>
#include "config.h"
#include "blink_status.h"

// Define global connection state variable
ConnState connState = WIFI_DISCONNECTED;

Preferences prefs;

// --- Connection State Enum and Status LED ---
// Moved to blink_status.h


#define MQTT_BROKER "172.16.3.171" // Set to backend IP or broker IP
#define MQTT_PORT 1883
#define MQTT_USER "0b47b538cb6f184724eef4e2f3f14a6c39ab65a956e652ee"
#define MQTT_PASSWORD ""
#define SWITCH_TOPIC "esp32/switches"
#define STATE_TOPIC "esp32/state"
#define TELEMETRY_TOPIC "esp32/telemetry"
#define CONFIG_TOPIC "esp32/config"

WiFiClient espClient;
PubSubClient mqttClient(espClient);

char mqttClientId[24];

// Forward declarations
void publishManualSwitchEvent(int gpio, bool state, int physicalPin = -1);
void sendStateUpdate(bool force);
int relayNameToGpio(const char* relay);
String normalizeMac(String mac);


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
        saveSwitchConfigToNVS();  // Save state change
        Serial.printf("[CMD] Relay GPIO %d set to %s (from queue)\n", cmd.gpio, cmd.state ? "ON" : "OFF");
        break;
      }
    }
    commandQueueHead = (commandQueueHead + 1) % MAX_COMMAND_QUEUE;
    processed++;
  }
}





// --- Dynamic config loading/saving ---
void loadSwitchConfigFromNVS() {
  prefs.begin("switch_cfg", true);
  for (int i = 0; i < NUM_SWITCHES; i++) {
    String keyRelay = "relay" + String(i);
    String keyManual = "manual" + String(i);
    String keyDefault = "def" + String(i);
    String keyState = "state" + String(i);
    String keyMomentary = "momentary" + String(i);
    int relay = prefs.getInt(keyRelay.c_str(), relayPins[i]);
    int manual = prefs.getInt(keyManual.c_str(), manualSwitchPins[i]);
    bool defState = prefs.getBool(keyDefault.c_str(), false);
    bool savedState = prefs.getBool(keyState.c_str(), false);
    bool momentary = prefs.getBool(keyMomentary.c_str(), true);  // Load momentary setting (default to momentary)
    switchesLocal[i].relayGpio = relay;
    switchesLocal[i].manualGpio = manual;
    switchesLocal[i].defaultState = defState;
    switchesLocal[i].state = savedState;  // Load saved state
    switchesLocal[i].manualMomentary = momentary;  // Load momentary setting
  }
  prefs.end();
  Serial.println("[NVS] Loaded switch config and state from NVS");
}

void saveSwitchConfigToNVS() {
  prefs.begin("switch_cfg", false);
  for (int i = 0; i < NUM_SWITCHES; i++) {
    String keyRelay = "relay" + String(i);
    String keyManual = "manual" + String(i);
    String keyDefault = "def" + String(i);
    String keyState = "state" + String(i);
    String keyMomentary = "momentary" + String(i);
    prefs.putInt(keyRelay.c_str(), switchesLocal[i].relayGpio);
    prefs.putInt(keyManual.c_str(), switchesLocal[i].manualGpio);
    prefs.putBool(keyDefault.c_str(), switchesLocal[i].defaultState);
    prefs.putBool(keyState.c_str(), switchesLocal[i].state);  // Save current state
    prefs.putBool(keyMomentary.c_str(), switchesLocal[i].manualMomentary);  // Save momentary setting
  }
  prefs.end();
  Serial.println("[NVS] Saved switch config and state to NVS");
}

// Initialize switch state array with mapping, loading from NVS if available
void initSwitches() {
  for (int i = 0; i < NUM_SWITCHES; i++) {
    switchesLocal[i].relayGpio = relayPins[i];
    switchesLocal[i].manualGpio = manualSwitchPins[i];
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
  loadSwitchConfigFromNVS();
  // Apply loaded states to relays
  for (int i = 0; i < NUM_SWITCHES; i++) {
    pinMode(switchesLocal[i].relayGpio, OUTPUT);
    digitalWrite(switchesLocal[i].relayGpio, switchesLocal[i].state ? (RELAY_ACTIVE_HIGH ? HIGH : LOW) : (RELAY_ACTIVE_HIGH ? LOW : HIGH));
    pinSetup[i] = false;  // Force re-setup of manual pins
  }
}



// Debounce and manual switch handling constants
#define MANUAL_DEBOUNCE_MS 100  // Increased from 20ms for better noise rejection
#define MANUAL_REPEAT_IGNORE_MS 100  // Reduced from 200ms

void handleManualSwitches() {
  unsigned long now = millis();
  for (int i = 0; i < NUM_SWITCHES && i < 16; i++) {  // Add bounds check
    SwitchState &sw = switchesLocal[i];
    if (!sw.manualEnabled || sw.manualGpio < 0 || sw.manualGpio > 39) continue;  // Validate GPIO range

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

    // Special debug logging for GPIO 19
    if (sw.relayGpio == 19) {
      Serial.printf("[DEBUG] GPIO 19: raw=%d, active=%d, state=%d, lastActive=%d\n", rawLevel, active, sw.state, sw.lastManualActive);
    }

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
            saveSwitchConfigToNVS();
            Serial.printf("[MANUAL] Momentary PRESS: Relay GPIO %d toggled to %s\n", sw.relayGpio, sw.state ? "ON" : "OFF");
            
            // Send manual switch event for logging
            if (mqttClient.connected()) {
              publishManualSwitchEvent(sw.relayGpio, sw.state, sw.manualGpio);
            }
            // No offline event buffering: manual switch always controls relay locally
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
            saveSwitchConfigToNVS();
            Serial.printf("[MANUAL] Maintained: Relay GPIO %d set to %s\n", sw.relayGpio, sw.state ? "ON" : "OFF");
            
            // Send manual switch event for logging
            if (mqttClient.connected()) {
              publishManualSwitchEvent(sw.relayGpio, sw.state, sw.manualGpio);
            }
            // No offline event buffering: manual switch always controls relay locally
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
  doc["status"] = "heartbeat";
  doc["heap"] = ESP.getFreeHeap();
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
  size_t freeHeap = ESP.getFreeHeap();
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(2,0,0)
  size_t minFreeHeap = ESP.getMinFreeHeap();
#else
  size_t minFreeHeap = 0;
#endif
  Serial.printf("[HEALTH] Heap: %u bytes free, Min: %u\n", freeHeap, minFreeHeap);

  // CRITICAL: Check for dangerously low heap
  if (freeHeap < 5000) {
    Serial.printf("[CRITICAL] Extremely low heap memory: %u bytes free! System may crash soon.\n", freeHeap);
    // Force garbage collection by restarting MQTT if heap is critical
    if (freeHeap < 3000 && mqttClient.connected()) {
      Serial.println("[CRITICAL] Heap critically low, disconnecting MQTT to free memory");
      mqttClient.disconnect();
    }
  } else if (freeHeap < 10000) {
    Serial.printf("[WARNING] Very low heap memory: %u bytes free!\n", freeHeap);
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
    delay(100);  // Reduced delay to allow more frequent manual switch checks
    handleManualSwitches();  // Handle manual switches during WiFi connection
    esp_task_wdt_reset(); // Reset watchdog during WiFi connection
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
    sprintf(mqttClientId, "ESP32_%s", mac.c_str());
    Serial.printf("MQTT Client ID: %s\n", mqttClientId);
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
    // No offline event reporting
    if (connState == BACKEND_CONNECTED) {
      offlineLogged = false;  // Reset when connected
    }
  }
  // No offline event logging or queue reporting
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
  connState = BACKEND_CONNECTED;
  Serial.println("[MQTT] Successfully reconnected - sent current state");
  } else {
    Serial.print("failed, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" will retry later");
    connState = WIFI_ONLY;
  }
}



void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length == 0 || payload == nullptr || topic == nullptr) {
    Serial.println("[MQTT] Empty or null payload/topic received, ignoring");
    return;
  }

  // Check heap before processing
  if (ESP.getFreeHeap() < 2000) {
    Serial.println("[MQTT] Low heap memory, skipping MQTT message processing");
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
    Serial.printf("[MQTT] Received switch command: %s\n", message.c_str());
    if (message.startsWith("{")) {
      DynamicJsonDocument doc(256);
      DeserializationError err = deserializeJson(doc, message);
      if (!err && doc["mac"].is<const char*>() && doc["secret"].is<const char*>() && doc["gpio"].is<int>() && doc["state"].is<bool>()) {
        String targetMac = doc["mac"];
        String targetSecret = doc["secret"];
        String myMac = WiFi.macAddress();
        String normalizedTarget = normalizeMac(targetMac);
        String normalizedMy = normalizeMac(myMac);
        Serial.printf("[MQTT] Target MAC: %s, My MAC: %s, Normalized Target: %s, Normalized My: %s\n", targetMac.c_str(), myMac.c_str(), normalizedTarget.c_str(), normalizedMy.c_str());
        if (normalizedTarget.equalsIgnoreCase(normalizedMy)) {
          Serial.println("[MQTT] MAC matches");
          if (targetSecret == String(DEVICE_SECRET)) {
            Serial.println("[MQTT] Secret matches");
            int gpio = doc["gpio"];
            bool state = doc["state"];
            bool validGpio = false;
            for (int i = 0; i < NUM_SWITCHES; i++) {
              if (switchesLocal[i].relayGpio == gpio) { validGpio = true; break; }
            }
            if (validGpio) {
              Serial.printf("[MQTT] Valid GPIO %d, queuing command\n", gpio);
              queueSwitchCommand(gpio, state);
              processCommandQueue();
              Serial.printf("[MQTT] Processed command for this device: GPIO %d -> %s\n", gpio, state ? "ON" : "OFF");
            } else {
              Serial.printf("[MQTT] Invalid GPIO %d, ignoring command\n", gpio);
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
      if (!err && doc["mac"].is<const char*>() && doc["secret"].is<const char*>() && doc["switches"].is<JsonArray>()) {
        String targetMac = doc["mac"];
        String targetSecret = doc["secret"];
        String myMac = WiFi.macAddress();
        String normalizedTarget = normalizeMac(targetMac);
        String normalizedMy = normalizeMac(myMac);
        if (normalizedTarget.equalsIgnoreCase(normalizedMy)) {
          if (targetSecret == String(DEVICE_SECRET)) {
            JsonArray switches = doc["switches"].as<JsonArray>();
            int n = switches.size();
            if (n > 0 && n <= 6) {
              Serial.printf("[CONFIG] Processing config for %d switches\n", n);
              for (int i = 0; i < n; i++) {
                JsonObject sw = switches[i];
                relayPins[i] = sw["gpio"] | 0;
                manualSwitchPins[i] = sw.containsKey("manualGpio") ? (int)sw["manualGpio"] : -1;
                // Handle momentary/maintained mode
                if (sw.containsKey("manualMode")) {
                  String mode = sw["manualMode"];
                  switchesLocal[i].manualMomentary = (mode == "momentary");
                  Serial.printf("[CONFIG] Switch %d: gpio=%d, manualGpio=%d, manualMode=%s, momentary=%d\n",
                    i, relayPins[i], manualSwitchPins[i], mode.c_str(), switchesLocal[i].manualMomentary);
                } else {
                  Serial.printf("[CONFIG] Switch %d: no manualMode field found\n", i);
                }
              }
              prefs.begin("switch_cfg", false);
              for (int i = 0; i < n; i++) {
                prefs.putInt(("relay" + String(i)).c_str(), relayPins[i]);
                prefs.putInt(("manual" + String(i)).c_str(), manualSwitchPins[i]);
                // Save momentary setting to NVS
                prefs.putBool(("momentary" + String(i)).c_str(), switchesLocal[i].manualMomentary);
              }
              prefs.end();
              initSwitches();
              Serial.println("[CONFIG] Pin configuration and switch modes updated from server and applied.");
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
  DynamicJsonDocument doc(512);  // Reduced from 1024 to prevent heap fragmentation
  doc["mac"] = WiFi.macAddress();
  doc["secret"] = DEVICE_SECRET; // Include device secret for authentication
  JsonArray arr = doc.createNestedArray("switches");
  for (int i = 0; i < NUM_SWITCHES; i++) {
    JsonObject o = arr.createNestedObject();
    o["gpio"] = switchesLocal[i].relayGpio;
    o["state"] = switchesLocal[i].state;
    o["manual_override"] = switchesLocal[i].manualOverride;
  }
  char buf[512];  // Reduced buffer size to match document
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n > 0 && n < sizeof(buf)) {
    mqttClient.publish(STATE_TOPIC, buf, n);
    Serial.println("[MQTT] Published state update");
  } else {
    Serial.println("[MQTT] Failed to serialize state update - buffer too small");
  }
}

// Publish manual switch event for logging
void publishManualSwitchEvent(int gpio, bool state, int physicalPin) {
  // Check heap before allocating JSON document
  if (ESP.getFreeHeap() < 1000) {
    Serial.println("[MQTT] Low heap memory, skipping manual switch event");
    return;
  }

  DynamicJsonDocument doc(128);
  doc["mac"] = WiFi.macAddress();
  doc["secret"] = DEVICE_SECRET; // Include device secret for authentication
  doc["type"] = "manual_switch";
  doc["gpio"] = gpio;
  doc["state"] = state;
  if (physicalPin >= 0) {
    doc["physicalPin"] = physicalPin;
  }
  doc["timestamp"] = millis();
  char buf[128];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n > 0 && n < sizeof(buf) && mqttClient.connected()) {
    mqttClient.publish(TELEMETRY_TOPIC, buf, n);
    Serial.printf("[MQTT] Published manual switch event: GPIO %d (physical pin %d) -> %s\n", gpio, physicalPin, state ? "ON" : "OFF");
  } else {
    Serial.println("[MQTT] Failed to publish manual switch event");
  }
}

// Replace sendStateUpdate(bool force) to call publishState() when needed

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

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32 Classroom Automation System (MQTT)");

  // Boot recovery check
  Serial.printf("[BOOT] Reset reason: %d\n", esp_reset_reason());
  // Check for abnormal reset (panic, watchdog, or brownout)
  int resetReason = esp_reset_reason();
  if (resetReason == 1 || resetReason == 5 || resetReason == 6) {  // ESP_RST_PANIC=1, ESP_RST_INT_WDT=5, ESP_RST_TASK_WDT=6
    Serial.println("[BOOT] Detected crash recovery - initializing safely");
    delay(1000); // Give serial time to flush
  }

  // Initialize watchdog with 10 second timeout
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 10000,  // 10 seconds
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  initSwitches();
  // No offline event loading: do not persist manual switch events
  setup_wifi();
  Serial.println("WiFi setup complete, initializing MQTT...");
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setCallback(mqttCallback);
  mqttClient.subscribe(CONFIG_TOPIC);
}

void loop() {
  static unsigned long loopStartTime = 0;
  loopStartTime = millis();

  esp_task_wdt_reset();
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
    }
    sendHeartbeat();
  }
  blinkStatus();
  if (pendingState) sendStateUpdate(false);
  checkSystemHealth();

  // Re-apply relay states periodically to counteract hardware drift
  static unsigned long lastRelayCheck = 0;
  if (now - lastRelayCheck > 5000) { // Every 5 seconds
    for (int i = 0; i < NUM_SWITCHES; i++) {
      digitalWrite(switchesLocal[i].relayGpio, switchesLocal[i].state ? (RELAY_ACTIVE_HIGH ? HIGH : LOW) : (RELAY_ACTIVE_HIGH ? LOW : HIGH));
    }
    lastRelayCheck = now;
    Serial.println("[RELAY] Re-applied all relay states");
  }

  // Safety check: ensure loop doesn't take too long (should complete in < 100ms normally)
  unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > 500) {
    Serial.printf("[WARNING] Loop took %lu ms - possible performance issue\n", loopDuration);
  }

  delay(10);
}