/**
 * Automatic Car Wiper - Production Firmware & Wi-Fi Diagnostic Engine (PlatformIO)
 * 
 * Hardware Pin Mappings:
 *   - Rain Sensor AO: GPIO 34 (ADC1 Channel 6)
 *   - SG90 Servo PWM: GPIO 18
 *   - Onboard LED:    GPIO 2 (Blue)
 *   - BOOT Button:    GPIO 0
 * 
 * Wi-Fi Architecture:
 *   - Event-driven diagnostic system logging disconnect reason codes & network states.
 *   - Stable WiFiManager provisioning portal 'ESP32_Setup' on http://192.168.4.1.
 *   - Single active connection strategy at any time.
 *   - Clean station disconnect prior to credential application (avoids ESP_ERR_WIFI_STATE).
 *   - Optional development credentials fallback (from config.h) if enabled.
 *   - Controlled non-blocking reconnection in loop() using saved credentials.
 *   - BOOT button (GPIO 0) 3-second hold to reset credentials and reopen portal.
 *   - Completely non-blocking rain sensor and SG90 servo motor local operations.
 * 
 * Telemetry:
 *   - HTTPS POST every 3 seconds to:
 *     https://automatic-car-wiper.onrender.com/api/readings
 *   - Prints HTTP status codes, server responses, and error strings.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <WiFiManager.h>
#include <esp_wifi.h>

#if __has_include("config.h")
  #include "config.h"
#endif

// ====================================================================
// 1. Cloud Backend & Calibration Constants
// ====================================================================
#ifndef BACKEND_URL
  #define BACKEND_URL "https://automatic-car-wiper.onrender.com/api/readings"
#endif

#ifndef DEVICE_ID
  #define DEVICE_ID "ESP32-WIPER"
#endif

#ifndef TELEMETRY_INTERVAL_MS
  #define TELEMETRY_INTERVAL_MS 3000
#endif
const unsigned long RECONNECT_INTERVAL_MS = 20000; // 20s between reconnection retries

// Hardware Pins (Preserved strictly)
#ifndef PIN_RAIN_SENSOR
  #define PIN_RAIN_SENSOR 34
#endif
#ifndef PIN_SERVO
  #define PIN_SERVO 18
#endif
#ifndef PIN_STATUS_LED
  #define PIN_STATUS_LED 2
#endif
#ifndef PIN_BOOT_BUTTON
  #define PIN_BOOT_BUTTON 0
#endif

// Calibration Thresholds (12-bit ADC: 0 - 4095)
#ifndef RAIN_DRY_THRESHOLD
  #define RAIN_DRY_THRESHOLD 3500
#endif
#ifndef RAIN_WET_THRESHOLD
  #define RAIN_WET_THRESHOLD 3000
#endif
#ifndef RAIN_HEAVY_THRESHOLD
  #define RAIN_HEAVY_THRESHOLD 1700
#endif

// Wiper Servo Mechanics
#ifndef SERVO_MIN_ANGLE
  #define SERVO_MIN_ANGLE 20
#endif
#ifndef SERVO_MAX_ANGLE
  #define SERVO_MAX_ANGLE 120
#endif
#ifndef SERVO_STEP_ANGLE
  #define SERVO_STEP_ANGLE 5
#endif
#ifndef WIPE_DELAY_SLOW_MS
  #define WIPE_DELAY_SLOW_MS 60
#endif
#ifndef WIPE_DELAY_FAST_MS
  #define WIPE_DELAY_FAST_MS 20
#endif

// Optional Dev Credentials Fallback (Enabled only if WIFI_SSID is defined in config.h and not a dummy)
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  #define HAS_DEV_CREDENTIALS 1
  const char* DEV_SSID = WIFI_SSID;
  const char* DEV_PASS = WIFI_PASSWORD;
#else
  #define HAS_DEV_CREDENTIALS 0
  const char* DEV_SSID = "";
  const char* DEV_PASS = "";
#endif

// ====================================================================
// 2. Global Instances & State Variables
// ====================================================================
Servo myServo;

// Wiper & Sensor State
int currentAngle = SERVO_MIN_ANGLE;
int servoDirection = 1;
unsigned long lastServoStepTime = 0;
int currentWipeDelay = WIPE_DELAY_SLOW_MS;

int rainValue = 4095;
bool rainDetected = false;
const char* currentRainStatus = "Dry";
const char* currentWiperSpeed = "Off";
unsigned long lastSensorSampleTime = 0;

// Timing & Indicator Variables
unsigned long lastTelemetryTime = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastLedBlinkTime = 0;
bool ledState = false;

// BOOT Button Press Tracking
unsigned long bootButtonPressStart = 0;
bool bootButtonArmed = false;

// Wi-Fi Connection Strategy Flags
bool isWiFiManagerActive = false;
bool isStationConnected = false;

// ====================================================================
// 3. Wi-Fi Diagnostic Engine & Event Handlers
// ====================================================================
const char* getAuthModeName(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:         return "Open";
    case WIFI_AUTH_WEP:          return "WEP";
    case WIFI_AUTH_WPA_PSK:      return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:     return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:     return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3 Transition";
    default:                     return "Unknown";
  }
}

void explainDisconnectReason(uint8_t reason) {
  Serial.print("  -> Diagnostic Analysis: ");
  switch (reason) {
    case 201: // WIFI_REASON_NO_AP_FOUND
      Serial.println("NO_AP_FOUND. The target SSID was not detected on any 2.4 GHz channel.");
      Serial.println("     Root Causes: Hotspot is broadcasting on 5 GHz only, phone went to sleep,");
      Serial.println("     or phone auto-disabled hotspot when connecting to another Wi-Fi.");
      break;
    case 202: // WIFI_REASON_AUTH_FAIL
      Serial.println("AUTH_FAIL. 4-Way WPA2 handshake rejected by AP.");
      Serial.println("     Root Causes: Password was mistyped, auto-capitalization error, or trailing space.");
      break;
    case 204: // WIFI_REASON_HANDSHAKE_TIMEOUT
      Serial.println("HANDSHAKE_TIMEOUT. Key negotiation timed out.");
      Serial.println("     Root Causes: Security mismatch (e.g. WPA3 SAE or PMF Protected Management Frames required).");
      break;
    case 200: // WIFI_REASON_BEACON_TIMEOUT
      Serial.println("BEACON_TIMEOUT. Lost AP beacon frames.");
      Serial.println("     Root Causes: Signal lost, device moved out of range, or AP powered off.");
      break;
    case 15: // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
      Serial.println("4WAY_HANDSHAKE_TIMEOUT. WPA2 key exchange timed out.");
      break;
    case 205: // WIFI_REASON_CONNECTION_FAIL
      Serial.println("CONNECTION_FAIL. General 802.11 association failed.");
      break;
    default:
      Serial.printf("Standard 802.11 disconnection/deauthentication event (Code %d).\n", reason);
      break;
  }
}

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("[WiFi-Diag] STA Interface Started.");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[WiFi-Diag] STA Associated with AP on Channel %d.\n", info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      isStationConnected = true;
      Serial.println("\n========================================================");
      Serial.println("   [WiFi-Diag] DHCP SUCCESS: IP Address Assigned!       ");
      Serial.println("========================================================");
      Serial.printf("  SSID:        %s\n", WiFi.SSID().c_str());
      Serial.printf("  IP Address:  %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("  Subnet Mask: %s\n", WiFi.subnetMask().toString().c_str());
      Serial.printf("  Gateway:     %s\n", WiFi.gatewayIP().toString().c_str());
      Serial.printf("  DNS Server:  %s\n", WiFi.dnsIP().toString().c_str());
      Serial.printf("  Signal RSSI: %d dBm\n", WiFi.RSSI());
      Serial.println("========================================================\n");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      isStationConnected = false;
      uint8_t reason = info.wifi_sta_disconnected.reason;
      const char* reasonName = WiFi.disconnectReasonName((wifi_err_reason_t)reason);
      Serial.printf("\n[WiFi-Diag] STA Disconnected! Reason Code: %d (%s)\n",
                    reason, reasonName ? reasonName : "UNKNOWN");
      explainDisconnectReason(reason);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.printf("[WiFi-Diag] Captive SoftAP Started. SSID: ESP32_Setup | IP: %s\n",
                    WiFi.softAPIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("[WiFi-Diag] Setup Client (Phone/Laptop) connected to ESP32_Setup!");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.println("[WiFi-Diag] Setup Client disconnected from ESP32_Setup.");
      break;
    default:
      break;
  }
}

void runNetworkScanDiagnostics() {
  Serial.println("\n========================================================");
  Serial.println("       ESP32 2.4 GHz RF Environment Diagnostic Scan     ");
  Serial.println("========================================================");
  int n = WiFi.scanNetworks(false, true); // Active scan, includes hidden SSIDs
  if (n <= 0) {
    Serial.println("  [Scan] No 2.4 GHz networks detected. Verify RF environment.");
  } else {
    Serial.printf("  [Scan] Discovered %d 2.4 GHz wireless network(s):\n", n);
    for (int i = 0; i < n; ++i) {
      int rssi = WiFi.RSSI(i);
      int quality = constrain(2 * (rssi + 100), 0, 100);
      Serial.printf("  [%02d] SSID: %-22s | %4d dBm (%3d%%) | Ch: %2d | Sec: %s\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    rssi,
                    quality,
                    (int)WiFi.channel(i),
                    getAuthModeName(WiFi.encryptionType(i)));
    }
  }
  WiFi.scanDelete();
  Serial.println("========================================================\n");
}

// ====================================================================
// 4. WiFiManager Callbacks
// ====================================================================
void configModeCallback(WiFiManager *myWiFiManager) {
  isWiFiManagerActive = true;
  digitalWrite(PIN_STATUS_LED, HIGH);
  Serial.println("\n========================================================");
  Serial.println("   [WiFiManager] CAPTIVE SETUP PORTAL ACTIVATED!       ");
  Serial.println("========================================================");
  Serial.printf("  Hotspot AP Name:  %s\n", myWiFiManager->getConfigPortalSSID().c_str());
  Serial.printf("  Configuration IP: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("  BLUE LED:         ON (Setup Mode)");
  Serial.println("--------------------------------------------------------");
  Serial.println("  INSTRUCTIONS TO CONNECT:");
  Serial.println("  1. Connect your phone or laptop Wi-Fi to 'ESP32_Setup'.");
  Serial.println("  2. Open your browser and navigate to: http://192.168.4.1");
  Serial.println("  3. Tap 'Configure WiFi', select your 2.4 GHz Wi-Fi,");
  Serial.println("     enter the password, and click Save.");
  Serial.println("  4. The ESP32 will connect and stream live telemetry.");
  Serial.println("========================================================\n");
}

void preSaveConfigCallback() {
  // CRITICAL: Halt active station state before WiFiManager applies new credentials.
  // Prevents ESP-IDF from returning ESP_ERR_WIFI_STATE ("sta is connecting, cannot set config").
  Serial.println("\n[WiFiManager] Halting station state before applying new credentials...");
  WiFi.disconnect(false, false);
  esp_wifi_disconnect();
  delay(100);
}

void saveConfigCallback() {
  isWiFiManagerActive = false;
  Serial.println("\n[WiFiManager] New Wi-Fi credentials saved. Initiating connection...");
}

// ====================================================================
// 5. Non-Blocking Sensor Reading & Wiper Control
// ====================================================================
void updateSensorAndWiper() {
  unsigned long now = millis();

  // 1. Sample Rain Sensor ADC (Every 50ms)
  if (now - lastSensorSampleTime >= 50) {
    lastSensorSampleTime = now;
    rainValue = analogRead(PIN_RAIN_SENSOR);

    // Apply Hysteresis Calibration Thresholds
    if (rainValue > RAIN_DRY_THRESHOLD) {
      rainDetected = false;
      currentRainStatus = "Dry";
      currentWiperSpeed = "Off";
    } else if (rainValue < RAIN_WET_THRESHOLD) {
      rainDetected = true;
      if (rainValue < RAIN_HEAVY_THRESHOLD) {
        currentRainStatus = "Heavy Rain";
        currentWiperSpeed = "Fast";
        currentWipeDelay = WIPE_DELAY_FAST_MS;
      } else {
        currentRainStatus = "Light Rain";
        currentWiperSpeed = "Slow";
        currentWipeDelay = WIPE_DELAY_SLOW_MS;
      }
    }
  }

  // 2. Non-Blocking Servo Stepping State Machine
  if (rainDetected) {
    if (now - lastServoStepTime >= currentWipeDelay) {
      lastServoStepTime = now;
      currentAngle += servoDirection * SERVO_STEP_ANGLE;

      if (currentAngle >= SERVO_MAX_ANGLE) {
        currentAngle = SERVO_MAX_ANGLE;
        servoDirection = -1;
      } else if (currentAngle <= SERVO_MIN_ANGLE) {
        currentAngle = SERVO_MIN_ANGLE;
        servoDirection = 1;
      }

      myServo.write(currentAngle);
    }
  } else {
    // Park wiper at baseline position if windshield is dry
    if (currentAngle != SERVO_MIN_ANGLE) {
      currentAngle = SERVO_MIN_ANGLE;
      servoDirection = 1;
      myServo.write(SERVO_MIN_ANGLE);
    }
  }
}

// ====================================================================
// 6. Cloud Telemetry HTTPS Transmission (Every 3 seconds)
// ====================================================================
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Wi-Fi not connected. Skipping cloud telemetry.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Connect securely via TLS without root CA certificate bundle overhead

  HTTPClient http;
  if (!http.begin(client, BACKEND_URL)) {
    Serial.println("[HTTP] Error: Failed to initialize HTTP client connection.");
    return;
  }

  http.setTimeout(2500); // 2.5s maximum timeout
  http.addHeader("Content-Type", "application/json");

  // Construct JSON payload matching FastAPI SensorReadingCreate model
  String payload = "{\"device_id\":\"" + String(DEVICE_ID) + "\","
                   "\"rain_value\":" + String(rainVal) + ","
                   "\"rain_status\":\"" + String(statusStr) + "\","
                   "\"wiper_on\":" + String(wiperState ? "true" : "false") + ","
                   "\"wiper_speed\":\"" + String(speedStr) + "\"}";

  Serial.println("[HTTP] Sending telemetry to Render...");
  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    Serial.printf("[HTTP] Status Code: %d | Server Response: %s\n", httpCode, http.getString().c_str());
  } else {
    Serial.printf("[HTTP] Connection Error: Code %d -> %s\n", httpCode, http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ====================================================================
// 7. Controlled Wi-Fi Reconnection (When disconnected after setup)
// ====================================================================
void handleWiFiReconnect() {
  if (WiFi.status() == WL_CONNECTED || isWiFiManagerActive) {
    return;
  }

  // Only attempt reconnection if valid credentials exist
  if (WiFi.SSID().length() == 0) {
    return;
  }

  unsigned long now = millis();
  if (now - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = now;
    Serial.println("[Wi-Fi] Disconnected from network. Attempting non-blocking reconnect...");
    WiFi.reconnect();
  }
}

// ====================================================================
// 8. Status LED Indicator Logic (GPIO 2)
// ====================================================================
void updateStatusLED() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_STATUS_LED, HIGH); // Solid ON when connected and online
  } else {
    // Slow blink (500ms ON / 500ms OFF) when offline or searching
    if (now - lastLedBlinkTime >= 500) {
      lastLedBlinkTime = now;
      ledState = !ledState;
      digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
    }
  }
}

// ====================================================================
// 9. Onboard BOOT Button Handler (GPIO 0 - Reset Wi-Fi on 3s hold)
// ====================================================================
void checkResetButton() {
  int reading = digitalRead(PIN_BOOT_BUTTON);

  // Require button to be released (HIGH) first before arming (protects against serial DTR/RTS)
  if (!bootButtonArmed) {
    if (reading == HIGH) {
      bootButtonArmed = true;
    }
    return;
  }

  if (reading == LOW) { // Button is being pressed
    if (bootButtonPressStart == 0) {
      bootButtonPressStart = millis();
    } else if (millis() - bootButtonPressStart >= 3000) {
      Serial.println("\n========================================================");
      Serial.println("  [Wi-Fi Reset] BOOT button held for 3 seconds!        ");
      Serial.println("  [Wi-Fi Reset] Erasing saved Wi-Fi credentials...     ");
      WiFiManager wmReset;
      wmReset.resetSettings();
      WiFi.disconnect(true, true);
      Serial.println("  [Wi-Fi Reset] Credentials cleared! Restarting ESP32... ");
      Serial.println("========================================================\n");
      delay(500);
      ESP.restart();
    }
  } else {
    bootButtonPressStart = 0;
  }
}

// ====================================================================
// 10. Arduino Setup Entry Point
// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n\n========================================================");
  Serial.println("   Automatic Car Wiper - ESP32 Production & Diagnostic  ");
  Serial.println("========================================================");

  // Pin Configuration
  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RAIN_SENSOR, INPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  // Servo Setup (50Hz PWM, 500-2400us pulse, parked at 20°)
  myServo.setPeriodHertz(50);
  myServo.attach(PIN_SERVO, 500, 2400);
  myServo.write(SERVO_MIN_ANGLE);

  // Register Core Wi-Fi Event Diagnostics Callback
  WiFi.onEvent(onWiFiEvent);

  // Configure Wi-Fi Stack Cleanly
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // Disable auto-reconnect during WiFiManager setup
  WiFi.disconnect(false, false); // Abort any active background connection
  esp_wifi_disconnect();
  delay(100);

  // PMF Configuration for Android 5G Hotspots (WPA2/WPA3 transition compatibility)
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    conf.sta.pmf_cfg.capable = true;
    conf.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
  }

  // Run 2.4 GHz Wi-Fi Environment Scan Diagnostics
  runNetworkScanDiagnostics();

  // Initialize WiFiManager in Stable Blocking Mode
  WiFiManager wm;
  wm.setCleanConnect(true); // Always disconnect before attempting new connection
  wm.setAPCallback(configModeCallback);
  wm.setPreSaveConfigCallback(preSaveConfigCallback);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180); // 3 minutes timeout
  wm.setConnectTimeout(20);       // 20s connection timeout
  wm.setScanDispPerc(true);
  wm.setTitle("Automatic Car Wiper - Setup");

  Serial.println("[Wi-Fi] Connecting via WiFiManager...");
  bool connected = wm.autoConnect("ESP32_Setup");

  // Optional Fallback: If WiFiManager times out and Dev Credentials exist
  #if HAS_DEV_CREDENTIALS
  if (!connected && strlen(DEV_SSID) > 0 && strcmp(DEV_SSID, "YOUR_PRIMARY_WIFI_SSID") != 0) {
    Serial.printf("\n[Wi-Fi] WiFiManager portal timed out. Testing Dev Credentials fallback: '%s'...\n", DEV_SSID);
    WiFi.disconnect(false, false);
    esp_wifi_disconnect();
    delay(100);
    WiFi.begin(DEV_SSID, DEV_PASS);
    unsigned long startDev = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startDev < 10000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.println("[Wi-Fi] Connected via Dev Credentials fallback!");
    }
  }
  #endif

  if (connected || WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true); // Enable auto-reconnect once successfully connected
    digitalWrite(PIN_STATUS_LED, HIGH);
    Serial.println("\n[Wi-Fi] Online & Ready for Cloud Telemetry.");
  } else {
    digitalWrite(PIN_STATUS_LED, LOW);
    Serial.println("\n[Wi-Fi] Portal timed out or connection unestablished.");
    Serial.println("[Wi-Fi] Running sensor and wiper hardware in local offline mode.\n");
  }
}

// ====================================================================
// 11. Arduino Main Execution Loop
// ====================================================================
void loop() {
  // 1. Monitor physical BOOT button for 3-second reset hold
  checkResetButton();

  // 2. Drive non-blocking rain sensor ADC reading & smooth wiper sweep
  updateSensorAndWiper();

  // 3. Update status LED indicator
  updateStatusLED();

  // 4. Controlled reconnection if disconnected
  handleWiFiReconnect();

  // 5. Periodic HTTPS telemetry to Render (every 3 seconds)
  unsigned long now = millis();
  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTime = now;

    Serial.println("\n------------------------------------");
    Serial.printf("[Sensor] Rain ADC: %4d | Condition: %-10s | Wiper: %-4s (%3d deg) | Wi-Fi: %s\n",
                  rainValue,
                  currentRainStatus,
                  currentWiperSpeed,
                  currentAngle,
                  WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE");

    sendTelemetry(rainValue, rainDetected, currentRainStatus, currentWiperSpeed);
  }

  // Yield to FreeRTOS watchdog & background Wi-Fi tasks
  yield();
}
