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

// Configured Phone Hotspot Credentials (from config.h)
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  #define HAS_PRIMARY_CREDENTIALS 1
  const char* PRIMARY_SSID = WIFI_SSID;
  const char* PRIMARY_PASS = WIFI_PASSWORD;
#else
  #define HAS_PRIMARY_CREDENTIALS 0
  const char* PRIMARY_SSID = "";
  const char* PRIMARY_PASS = "";
#endif

#if defined(BACKUP_SSID) && defined(BACKUP_PASSWORD)
  #define HAS_BACKUP_CREDENTIALS 1
  const char* SECONDARY_SSID = BACKUP_SSID;
  const char* SECONDARY_PASS = BACKUP_PASSWORD;
#else
  #define HAS_BACKUP_CREDENTIALS 0
  const char* SECONDARY_SSID = "";
  const char* SECONDARY_PASS = "";
#endif

// ====================================================================
// 2. Global Instances & State Variables
// ====================================================================
Servo myServo;
WiFiManager wm;

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

// Wi-Fi Connection Strategy Flags & LED State Machine
bool isWiFiManagerActive = false;
bool isStationConnected = false;

enum WiFiStatusState {
  STATE_CONNECTING,
  STATE_PORTAL,
  STATE_CONNECTED,
  STATE_OFFLINE
};

volatile WiFiStatusState currentWifiState = STATE_CONNECTING;

// FreeRTOS Background LED Status Task (Runs on Core 0 independently of setup/loop)
void ledStatusTask(void *param) {
  pinMode(PIN_STATUS_LED, OUTPUT);
  for (;;) {
    switch (currentWifiState) {
      case STATE_PORTAL:
        // Rapid Flash (100ms ON / 100ms OFF) during captive portal setup mode
        digitalWrite(PIN_STATUS_LED, HIGH);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        digitalWrite(PIN_STATUS_LED, LOW);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        break;

      case STATE_CONNECTING:
        // Slow Blink (500ms ON / 500ms OFF) while attempting connection
        digitalWrite(PIN_STATUS_LED, HIGH);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        digitalWrite(PIN_STATUS_LED, LOW);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        break;

      case STATE_CONNECTED:
        // Solid Bright Blue ON when connected to Wi-Fi and online
        digitalWrite(PIN_STATUS_LED, HIGH);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        break;

      case STATE_OFFLINE:
      default:
        // Short pulse every 2s when running locally offline
        digitalWrite(PIN_STATUS_LED, LOW);
        vTaskDelay(1900 / portTICK_PERIOD_MS);
        digitalWrite(PIN_STATUS_LED, HIGH);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        break;
    }
  }
}

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
    case 1: // WIFI_REASON_UNSPECIFIED
      Serial.println("UNSPECIFIED. AP deauthenticated client without specifying a reason.");
      break;
    case 2: // WIFI_REASON_AUTH_EXPIRE
      Serial.println("AUTH_EXPIRE. Authentication session expired.");
      break;
    case 3: // WIFI_REASON_AUTH_LEAVE
      Serial.println("AUTH_LEAVE. Deauthenticated because sending station is leaving.");
      break;
    case 4: // WIFI_REASON_ASSOC_EXPIRE
      Serial.println("ASSOC_EXPIRE. Disassociated due to inactivity.");
      break;
    case 5: // WIFI_REASON_ASSOC_TOOMANY
      Serial.println("ASSOC_TOOMANY. Hotspot has reached maximum client associations.");
      break;
    case 6: // WIFI_REASON_NOT_AUTHED
      Serial.println("NOT_AUTHED. Class 2 frame received from non-authenticated station.");
      break;
    case 7: // WIFI_REASON_NOT_ASSOCED
      Serial.println("NOT_ASSOCED. Class 3 frame received from non-associated station.");
      break;
    case 8: // WIFI_REASON_ASSOC_LEAVE
      Serial.println("ASSOC_LEAVE. Disassociated because sending station is leaving.");
      break;
    case 13: // WIFI_REASON_IE_INVALID
      Serial.println("IE_INVALID. Invalid Information Element received from AP.");
      break;
    case 14: // WIFI_REASON_MIC_FAILURE
      Serial.println("MIC_FAILURE. Message Integrity Code check failed. Possible encryption key mismatch.");
      break;
    case 15: // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
      Serial.println("4WAY_HANDSHAKE_TIMEOUT. WPA2 4-Way Key Handshake timed out.");
      Serial.println("     Root Cause Analysis for Reason 15:");
      Serial.println("     1. PASSPHRASE MISMATCH (Most Common): The password entered does NOT match the AP's key.");
      Serial.println("        The AP silently discards Message 2 when MIC verification fails, leading to timeout.");
      Serial.println("        -> Check for mobile keyboard auto-capitalization, trailing space, or wrong character.");
      Serial.println("     2. SAME-DEVICE CONFLICT: If connecting to ESP32_Setup with the same phone that provides");
      Serial.println("        the hotspot, Android may pause/disable the hotspot uplink during Wi-Fi setup.");
      Serial.println("     3. MODEM SLEEP PACKET LOSS: ESP32 radio went to sleep during key exchange.");
      Serial.println("        -> WiFi.setSleep(false) is now enforced to keep the radio awake.");
      Serial.println("     4. PMF / CIPHER INCOMPATIBILITY: Hotspot may be enforcing WPA3-SAE or PMF Required.");
      Serial.println("        -> Ensure hotspot Security is set to 'WPA2-Personal' with PMF Optional/Off.");
      break;
    case 16: // WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT
      Serial.println("GROUP_KEY_UPDATE_TIMEOUT. Group key handshake timed out.");
      break;
    case 17: // WIFI_REASON_IE_IN_4WAY_DIFFERS
      Serial.println("IE_IN_4WAY_DIFFERS. Information Elements in 4-way handshake differ from beacon/probe response.");
      Serial.println("     Root Causes: WPA2/WPA3 mixed transition mode conflict or PMF capability mismatch.");
      break;
    case 18: // WIFI_REASON_GROUP_CIPHER_INVALID
      Serial.println("GROUP_CIPHER_INVALID. Invalid group cipher suite.");
      break;
    case 19: // WIFI_REASON_PAIRWISE_CIPHER_INVALID
      Serial.println("PAIRWISE_CIPHER_INVALID. Invalid pairwise cipher suite (CCMP vs TKIP).");
      break;
    case 20: // WIFI_REASON_AKMP_INVALID
      Serial.println("AKMP_INVALID. Invalid AKMP (Authentication Key Management Protocol, e.g. SAE vs PSK).");
      break;
    case 22: // WIFI_REASON_INVALID_RSN_IE_CAP
      Serial.println("INVALID_RSN_IE_CAP. Invalid RSN IE capabilities (PMF / Management Frame Protection mismatch).");
      break;
    case 24: // WIFI_REASON_CIPHER_SUITE_REJECTED
      Serial.println("CIPHER_SUITE_REJECTED. Cipher suite rejected by AP.");
      break;
    case 36: // WIFI_REASON_STA_LEAVING
      Serial.println("STA_LEAVING. Station interface disconnected / left BSSID to return to captive portal.");
      break;
    case 200: // WIFI_REASON_BEACON_TIMEOUT
      Serial.println("BEACON_TIMEOUT. Lost AP beacon frames.");
      Serial.println("     Root Causes: Signal lost, device moved out of range, or AP powered off.");
      break;
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
    case ARDUINO_EVENT_WIFI_READY:
      Serial.println("[WiFi-Diag] Wi-Fi Hardware & Radio Ready.");
      break;
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("[WiFi-Diag] Station Mode Initialized.");
      break;
    case ARDUINO_EVENT_WIFI_STA_STOP:
      Serial.println("[WiFi-Diag] Station Mode Stopped.");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[WiFi-Diag] STA Associated with AP on Channel %d (Layer 2 Connected).\n", info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      Serial.printf("[WiFi-Diag] AP Auth Mode Changed from %s to %s.\n",
                    getAuthModeName(info.wifi_sta_authmode_change.old_mode),
                    getAuthModeName(info.wifi_sta_authmode_change.new_mode));
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      isStationConnected = true;
      currentWifiState = STATE_CONNECTED;
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
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      isStationConnected = false;
      currentWifiState = STATE_CONNECTING;
      Serial.println("[WiFi-Diag] Warning: STA Lost IP Address (DHCP Lease Expired).");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      isStationConnected = false;
      if (isWiFiManagerActive) {
        currentWifiState = STATE_PORTAL;
      }
      uint8_t reason = info.wifi_sta_disconnected.reason;
      const char* reasonName = WiFi.disconnectReasonName((wifi_err_reason_t)reason);
      Serial.printf("\n[WiFi-Diag] STA Disconnected! Reason Code: %d (%s)\n",
                    reason, reasonName ? reasonName : "UNKNOWN");
      explainDisconnectReason(reason);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_START:
      currentWifiState = STATE_PORTAL;
      Serial.printf("[WiFi-Diag] Captive SoftAP Started. SSID: ESP32_Setup | IP: %s\n",
                    WiFi.softAPIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      if (WiFi.status() == WL_CONNECTED) {
        currentWifiState = STATE_CONNECTED;
      } else {
        currentWifiState = STATE_OFFLINE;
      }
      Serial.println("[WiFi-Diag] Captive SoftAP Stopped.");
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
  currentWifiState = STATE_PORTAL;
  Serial.println("\n========================================================");
  Serial.println("   [WiFiManager] CAPTIVE SETUP PORTAL ACTIVATED!       ");
  Serial.println("========================================================");
  Serial.printf("  Hotspot AP Name:  %s\n", myWiFiManager->getConfigPortalSSID().c_str());
  Serial.printf("  Configuration IP: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("  BLUE LED:         RAPID FLASHING (100ms Setup Mode)");
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
  currentWifiState = STATE_CONNECTING;
  Serial.println("\n========================================================");
  Serial.println("   [WiFiManager] Processing New Wi-Fi Configuration     ");
  Serial.println("========================================================");

  // Diagnostic credential verification without exposing the plaintext secret
  if (wm.server && wm.server->hasArg("s")) {
    String subSsid = wm.server->arg("s");
    String subPass = wm.server->arg("p");
    int passLen = subPass.length();
    bool hasLeadingSpace = passLen > 0 && isspace((unsigned char)subPass[0]);
    bool hasTrailingSpace = passLen > 0 && isspace((unsigned char)subPass[passLen - 1]);
    bool hasWhitespace = false;
    int digitCount = 0;
    int upperCount = 0;
    int lowerCount = 0;
    int symbolCount = 0;

    for (int i = 0; i < passLen; i++) {
      char c = subPass[i];
      if (isspace((unsigned char)c)) hasWhitespace = true;
      else if (isdigit((unsigned char)c)) digitCount++;
      else if (isupper((unsigned char)c)) upperCount++;
      else if (islower((unsigned char)c)) lowerCount++;
      else symbolCount++;
    }

    // Masked password preview (shows first and last character only)
    String maskedPass = "";
    if (passLen == 0) {
      maskedPass = "[EMPTY / OPEN NETWORK]";
    } else if (passLen <= 2) {
      maskedPass = "**";
    } else {
      maskedPass += subPass[0];
      for (int i = 1; i < passLen - 1; i++) maskedPass += '*';
      maskedPass += subPass[passLen - 1];
    }

    Serial.printf("  Target SSID:        '%s' (Length: %d)\n", subSsid.c_str(), (int)subSsid.length());
    Serial.printf("  Password Length:    %d characters\n", passLen);
    Serial.printf("  Password Masked:    %s\n", maskedPass.c_str());
    Serial.printf("  Character Makeup:   %d digits, %d uppercase, %d lowercase, %d symbols\n",
                  digitCount, upperCount, lowerCount, symbolCount);
    Serial.printf("  Leading Space:      %s\n", hasLeadingSpace ? "YES [WARNING! CHECK KEYBOARD]" : "NO");
    Serial.printf("  Trailing Space:     %s\n", hasTrailingSpace ? "YES [WARNING! CHECK KEYBOARD]" : "NO");
    Serial.printf("  Internal Spaces:    %s\n", (hasWhitespace && !hasLeadingSpace && !hasTrailingSpace) ? "YES" : "NO");

    if (hasTrailingSpace || hasLeadingSpace) {
      Serial.println("\n  [DIAGNOSTIC ALERT] Leading or trailing space detected in password!");
      Serial.println("  Mobile keyboards frequently append a space after typing words.");
      Serial.println("  This causes Reason Code 15 (4WAY_HANDSHAKE_TIMEOUT) because the MIC fails!");
    }
  }

  // Gracefully disconnect if station was actively in progress
  if (WiFi.status() == WL_CONNECTED || WiFi.status() == WL_IDLE_STATUS) {
    WiFi.disconnect(false, false);
  }
  Serial.println("========================================================\n");
}

void saveConfigCallback() {
  isWiFiManagerActive = false;
  currentWifiState = STATE_CONNECTING;
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
    Serial.println("[HTTP] Wi-Fi not connected. Skipping telemetry transmission.");
    return;
  }

  String url = String(BACKEND_URL);

  // Construct JSON payload matching FastAPI SensorReadingCreate model
  String payload = "{\"device_id\":\"" + String(DEVICE_ID) + "\","
                   "\"rain_value\":" + String(rainVal) + ","
                   "\"rain_status\":\"" + String(statusStr) + "\","
                   "\"wiper_on\":" + String(wiperState ? "true" : "false") + ","
                   "\"wiper_speed\":\"" + String(speedStr) + "\"}";

  Serial.printf("[HTTP] Sending telemetry to %s...\n", url.c_str());

  if (url.startsWith("https://")) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure(); // Connect securely via TLS without root CA certificate bundle overhead
    {
      HTTPClient http;
      http.setTimeout(4000);
      if (http.begin(secureClient, url)) {
        http.addHeader("Content-Type", "application/json");
        int httpCode = http.POST(payload);
        if (httpCode > 0) {
          Serial.printf("[HTTP] Status Code: %d | Server Response: %s\n", httpCode, http.getString().c_str());
        } else {
          Serial.printf("[HTTP] Connection Error: Code %d -> %s\n", httpCode, http.errorToString(httpCode).c_str());
        }
        http.end();
      } else {
        Serial.println("[HTTP] Error: Failed to initialize HTTPS connection.");
      }
    }
  } else {
    WiFiClient plainClient;
    {
      HTTPClient http;
      http.setTimeout(3000);
      if (http.begin(plainClient, url)) {
        http.addHeader("Content-Type", "application/json");
        int httpCode = http.POST(payload);
        if (httpCode > 0) {
          Serial.printf("[HTTP] Status Code: %d | Server Response: %s\n", httpCode, http.getString().c_str());
        } else {
          Serial.printf("[HTTP] Connection Error: Code %d -> %s\n", httpCode, http.errorToString(httpCode).c_str());
        }
        http.end();
      } else {
        Serial.println("[HTTP] Error: Failed to initialize HTTP connection.");
      }
    }
  }
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
  if (WiFi.status() == WL_CONNECTED) {
    currentWifiState = STATE_CONNECTED;
  } else if (isWiFiManagerActive) {
    currentWifiState = STATE_PORTAL;
  } else {
    currentWifiState = STATE_OFFLINE;
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

  // Start FreeRTOS background LED status task on Core 0 (keeps LED blinking during blocking portal)
  xTaskCreatePinnedToCore(
    ledStatusTask,
    "LED_Status",
    2048,
    NULL,
    1,
    NULL,
    0
  );

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
  delay(50);

  // CRITICAL: Disable modem sleep so radio stays awake during EAPOL handshake (prevents Reason 15)
  WiFi.setSleep(false);

  // Active all-channel scan and signal-based AP selection (matches working firmware)
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // Allow OPEN networks as well as WPA, WPA2, WPA3 transition
  WiFi.setMinSecurity(WIFI_AUTH_OPEN);

  // PMF Configuration for Android 5G Hotspots (WPA2/WPA3 transition compatibility)
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    conf.sta.pmf_cfg.capable = true;
    conf.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
  }

  // Run 2.4 GHz Wi-Fi Environment Scan Diagnostics
  runNetworkScanDiagnostics();

  bool connected = false;

  // Tier 1: Connect directly to Primary Phone Hotspot (narzo30)
  #if HAS_PRIMARY_CREDENTIALS
  if (!connected && strlen(PRIMARY_SSID) > 0 && strcmp(PRIMARY_SSID, "YOUR_PRIMARY_WIFI_SSID") != 0) {
    bool isOpenNetwork = (strlen(PRIMARY_PASS) == 0);
    Serial.printf("\n[Wi-Fi] Attempting direct connection to Primary Phone Hotspot: '%s' (%s)...\n",
                  PRIMARY_SSID, isOpenNetwork ? "Open Network" : "Secured");
    currentWifiState = STATE_CONNECTING;
    WiFi.disconnect(false, false);
    delay(100);
    if (isOpenNetwork) {
      wifi_config_t sta_conf;
      memset(&sta_conf, 0, sizeof(sta_conf));
      strncpy((char*)sta_conf.sta.ssid, PRIMARY_SSID, sizeof(sta_conf.sta.ssid) - 1);
      sta_conf.sta.threshold.authmode = WIFI_AUTH_OPEN;
      sta_conf.sta.pmf_cfg.capable = true;
      sta_conf.sta.pmf_cfg.required = false;
      esp_wifi_set_config(WIFI_IF_STA, &sta_conf);
      WiFi.begin(PRIMARY_SSID, "");
    } else {
      WiFi.begin(PRIMARY_SSID, PRIMARY_PASS);
    }
    unsigned long startPrimary = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startPrimary < 8000) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.println("[Wi-Fi] Connected successfully to Primary Phone Hotspot!");
    } else if (strcmp(PRIMARY_SSID, "narzo30") == 0 && !isOpenNetwork) {
      // If secured attempt failed, try open network fallback for narzo30
      Serial.println("[Wi-Fi] Retrying 'narzo30' as Open network...");
      WiFi.disconnect(false, false);
      delay(100);
      WiFi.begin("narzo30", "");
      unsigned long startAlt = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startAlt < 8000) {
        delay(300);
        Serial.print(".");
      }
      Serial.println();
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        Serial.println("[Wi-Fi] Connected successfully to 'narzo30' (Open)!");
      }
    }
  }
  #endif

  // Tier 2: Check for existing saved credentials in NVS flash
  if (!connected && WiFi.SSID().length() > 0) {
    Serial.printf("[Wi-Fi] Attempting connection to saved network from NVS: '%s'...\n", WiFi.SSID().c_str());
    currentWifiState = STATE_CONNECTING;
    WiFi.begin();
    unsigned long startSaved = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startSaved < 6000) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.println("[Wi-Fi] Connected to saved network from NVS!");
    }
  }

  // Tier 3: Connect directly to Backup Phone Hotspot (narzo30)
  #if HAS_BACKUP_CREDENTIALS
  if (!connected && strlen(SECONDARY_SSID) > 0 && strcmp(SECONDARY_SSID, "YOUR_BACKUP_WIFI_SSID") != 0) {
    Serial.printf("\n[Wi-Fi] Attempting direct connection to Backup Phone Hotspot: '%s'...\n", SECONDARY_SSID);
    currentWifiState = STATE_CONNECTING;
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(SECONDARY_SSID, SECONDARY_PASS);
    unsigned long startBackup = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startBackup < 8000) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.println("[Wi-Fi] Connected successfully to Backup Phone Hotspot!");
    }
  }
  #endif

  // Tier 4: Fallback to WiFiManager Captive Setup Portal (ESP32_Setup)
  if (!connected) {
    Serial.println("\n[Wi-Fi] No configured hotspot connected. Activating Captive Setup Portal...");
    currentWifiState = STATE_PORTAL;
    wm.setCleanConnect(true);
    wm.setAPCallback(configModeCallback);
    wm.setPreSaveConfigCallback(preSaveConfigCallback);
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setConfigPortalTimeout(180); // 3 minutes timeout
    wm.setConnectTimeout(20);       // 20s connection timeout
    wm.setScanDispPerc(true);
    wm.setTitle("Automatic Car Wiper - Setup");

    connected = wm.autoConnect("ESP32_Setup");
  }

  if (connected || WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true); // Enable auto-reconnect once successfully connected
    currentWifiState = STATE_CONNECTED;
    Serial.println("\n[Wi-Fi] Online & Ready for Cloud Telemetry.");
  } else {
    currentWifiState = STATE_OFFLINE;
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
