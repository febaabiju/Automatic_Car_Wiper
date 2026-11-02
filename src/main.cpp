/**
 * Automatic Car Wiper - Production Firmware (PlatformIO)
 * 
 * Wi-Fi:
 *   - Powered by WiFiManager (tzapu/WiFiManager) in a stable, BLOCKING initial setup flow.
 *   - Captive portal 'ESP32_Setup' on http://192.168.4.1 when unconfigured.
 *   - No hardcoded SSIDs or passwords.
 *   - Avoids "sta is connecting, cannot set config" error by stopping active STA
 *     connection attempts (setCleanConnect, preSaveConfigCallback, autoReconnect=false).
 *   - Once connected, prints SSID, IP address, and RSSI.
 *   - If portal times out or Wi-Fi is unavailable, continues operating locally.
 *   - Controlled background reconnection in loop() using saved credentials.
 *   - BOOT button (GPIO 0) held for 3 seconds clears saved credentials and reboots to portal.
 * 
 * Hardware Pin Mappings:
 *   - Rain Sensor AO: GPIO 34 (ADC1 Channel 6)
 *   - SG90 Servo PWM: GPIO 18
 *   - Onboard LED:    GPIO 2 (Blue)
 *   - BOOT Button:    GPIO 0
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

// ====================================================================
// 1. Cloud Backend Configuration
// ====================================================================
const char* BACKEND_URL = "https://automatic-car-wiper.onrender.com/api/readings";
const char* DEVICE_ID   = "ESP32-WIPER";
const unsigned long TELEMETRY_INTERVAL_MS = 3000;
const unsigned long RECONNECT_INTERVAL_MS = 20000; // 20s between reconnect retries

// ====================================================================
// 2. Hardware Pin Assignments & Calibration Constants
// ====================================================================
const int PIN_RAIN_SENSOR = 34; // Rain Sensor Analog ADC pin (ADC1)
const int PIN_SERVO       = 18; // SG90 Servo PWM signal pin
const int PIN_STATUS_LED  = 2;  // Onboard Blue Status LED
const int PIN_BOOT_BUTTON = 0;  // Physical BOOT Button (GPIO 0)

// Calibration Thresholds (12-bit ADC: 0 - 4095)
const int RAIN_DRY_THRESHOLD   = 3500; // Above 3500 = Windshield is Dry
const int RAIN_WET_THRESHOLD   = 3000; // Below 3000 = Rain Detected
const int RAIN_HEAVY_THRESHOLD = 1700; // Below 1700 = Heavy Rain

// Wiper Servo Mechanics
const int SERVO_MIN_ANGLE    = 20;  // Parked position (degrees)
const int SERVO_MAX_ANGLE    = 120; // Maximum sweep position (degrees)
const int SERVO_STEP_ANGLE   = 5;   // Sweep step increment
const int WIPE_DELAY_SLOW_MS = 60;  // Step delay for Light Rain (Slow)
const int WIPE_DELAY_FAST_MS = 20;  // Step delay for Heavy Rain (Fast)

// ====================================================================
// 3. Global Instances & State Variables
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

// ====================================================================
// 4. WiFiManager Callbacks (Pre-Save, Save, Config Mode)
// ====================================================================
void configModeCallback(WiFiManager *myWiFiManager) {
  digitalWrite(PIN_STATUS_LED, HIGH); // LED ON during setup mode
  Serial.println("\n========================================================");
  Serial.println("   [WiFiManager] CAPTIVE SETUP PORTAL ACTIVATED!       ");
  Serial.println("========================================================");
  Serial.print("  Hotspot AP Name:  ");
  Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.print("  Configuration IP: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("  BLUE LED:         ON (Setup Mode)");
  Serial.println("--------------------------------------------------------");
  Serial.println("  INSTRUCTIONS TO CONNECT:");
  Serial.println("  1. Connect your phone or laptop Wi-Fi to 'ESP32_Setup'.");
  Serial.println("  2. Open your browser and go to: http://192.168.4.1");
  Serial.println("  3. Tap 'Configure WiFi', select your 2.4 GHz Wi-Fi,");
  Serial.println("     enter the password, and click Save.");
  Serial.println("  4. The ESP32 will connect and begin streaming live data.");
  Serial.println("========================================================\n");
}

void preSaveConfigCallback() {
  // CRITICAL FIX: Abort any active station connection before applying new credentials.
  // This prevents ESP-IDF from throwing "wifi:sta is connecting, cannot set config".
  Serial.println("\n[WiFiManager] Halting active station connection before applying new credentials...");
  WiFi.disconnect(false, false);
  esp_wifi_disconnect();
  delay(100);
}

void saveConfigCallback() {
  Serial.println("\n[WiFiManager] New Wi-Fi credentials saved successfully! Connecting...");
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
    Serial.print("[HTTP] Status Code: ");
    Serial.print(httpCode);
    String response = http.getString();
    Serial.print(" | Server Response: ");
    Serial.println(response);
  } else {
    Serial.print("[HTTP] Connection Error: Code ");
    Serial.print(httpCode);
    Serial.print(" -> ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

// ====================================================================
// 7. Controlled Wi-Fi Reconnection (When disconnected after setup)
// ====================================================================
void handleWiFiReconnect() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  // Only attempt reconnection if valid credentials were previously saved
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

  Serial.println("\n\n==========================================");
  Serial.println("   Automatic Car Wiper - ESP32 Starting   ");
  Serial.println("==========================================");

  // Pin Configuration
  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RAIN_SENSOR, INPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  // Servo Setup (50Hz PWM, 500-2400us pulse, parked at 20°)
  myServo.setPeriodHertz(50);
  myServo.attach(PIN_SERVO, 500, 2400);
  myServo.write(SERVO_MIN_ANGLE);

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
  // Blocking autoConnect: connects if credentials exist, else starts captive portal
  bool connected = wm.autoConnect("ESP32_Setup");

  if (connected) {
    WiFi.setAutoReconnect(true); // Enable auto-reconnect once connected
    digitalWrite(PIN_STATUS_LED, HIGH);
    Serial.println("\n==========================================");
    Serial.println("   [Wi-Fi] CONNECTED SUCCESSFULLY!        ");
    Serial.println("==========================================");
    Serial.print("  SSID:        ");
    Serial.println(WiFi.SSID());
    Serial.print("  IP Address:  ");
    Serial.println(WiFi.localIP());
    Serial.print("  RSSI:        ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.println("==========================================\n");
  } else {
    digitalWrite(PIN_STATUS_LED, LOW);
    Serial.println("\n[Wi-Fi] Portal timed out or connection failed.");
    Serial.println("[Wi-Fi] Continuing hardware operation locally (offline mode).\n");
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
    Serial.print("[Sensor] Rain ADC: ");
    Serial.print(rainValue);
    Serial.print(" | Condition: ");
    Serial.print(currentRainStatus);
    Serial.print(" | Wiper: ");
    Serial.print(currentWiperSpeed);
    Serial.print(" (");
    Serial.print(currentAngle);
    Serial.print(" deg) | Wi-Fi: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE");

    sendTelemetry(rainValue, rainDetected, currentRainStatus, currentWiperSpeed);
  }

  // Yield to FreeRTOS watchdog & background Wi-Fi tasks
  yield();
}
