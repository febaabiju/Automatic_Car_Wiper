/**
 * Automatic Car Wiper - Hardcoded Wi-Fi Test Firmware
 * 
 * Hardware:
 *   - ESP32 30-Pin Dev Module (USB-C)
 *   - Rain Sensor Analog ADC pin (GPIO 34 / ADC1)
 *   - SG90 Servo Motor PWM pin (GPIO 18)
 *   - Status LED: ESP32 Onboard Blue LED (GPIO 2)
 * 
 * Wi-Fi Credentials (Hardcoded):
 *   - SSID:     "moto g96 5G_5200"
 *   - Password: "567895678"
 * 
 * Status LED Indications:
 *   - Blinking (500ms ON / 500ms OFF) : Connecting to "moto g96 5G_5200"
 *   - Solid ON (Continuous Blue)      : Connected successfully! Online & streaming to Render
 *   - OFF / Brief Pulse               : Disconnected / Reconnecting
 * 
 * Backend:
 *   - HTTPS POST to FastAPI: https://automatic-car-wiper.onrender.com/api/readings
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <esp_wifi.h>

// ====================================================================
// 1. Hardcoded Configuration
// ====================================================================
#if __has_include("config.h")
  #include "config.h"
#elif __has_include("config.example.h")
  #include "config.example.h"
#endif

#ifndef WIFI_SSID
  #define WIFI_SSID     "YOUR_WIFI_SSID"
#endif
#ifndef WIFI_PASSWORD
  #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

#ifndef BACKEND_URL
  #define BACKEND_URL   "https://automatic-car-wiper.onrender.com/api/readings"
#endif
#ifndef DEVICE_ID
  #define DEVICE_ID     "ESP32-WIPER"
#endif
#ifndef TELEMETRY_INTERVAL_MS
  #define TELEMETRY_INTERVAL_MS 3000
#endif

#if defined(WIFI_SSID_1) && defined(WIFI_SSID_2)
  const char* knownSSIDs[]     = { WIFI_SSID_1, WIFI_SSID_2 };
  const char* knownPasswords[] = { WIFI_PASSWORD_1, WIFI_PASSWORD_2 };
  const int numKnownNetworks   = 2;
#elif defined(WIFI_SSID_1)
  const char* knownSSIDs[]     = { WIFI_SSID_1 };
  const char* knownPasswords[] = { WIFI_PASSWORD_1 };
  const int numKnownNetworks   = 1;
#else
  const char* knownSSIDs[]     = { WIFI_SSID };
  const char* knownPasswords[] = { WIFI_PASSWORD };
  const int numKnownNetworks   = 1;
#endif
int currentNetIndex = 0;

// ====================================================================
// 2. Hardware Pin Assignments & Variables
// ====================================================================
Servo myServo;

const int rainSensor = 34; // Rain Sensor Analog ADC pin (ADC1)
const int servoPin   = 18; // SG90 Servo PWM signal pin
const int LED_PIN    = 2;  // Onboard Blue Status LED (GPIO 2)

int angle = 20;
int direction = 1;

bool rainDetected = false;
int wipeDelay = 60;

// ====================================================================
// 3. Timing & State Tracking Variables
// ====================================================================
unsigned long lastSendTime = 0;
const unsigned long sendInterval = TELEMETRY_INTERVAL_MS;

unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 10000; // Check reconnect every 10 seconds

unsigned long lastLedBlink = 0;
bool ledState = false;

// ====================================================================
// 4. Function Prototypes
// ====================================================================
void connectToWiFi();
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
const char* getWiFiDisconnectReason(uint8_t reason);
void updateStatusLED();
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr);

// ====================================================================
// 5. Wi-Fi Diagnostics & Connection Helpers
// ====================================================================
const char* getWiFiDisconnectReason(uint8_t reason) {
  switch (reason) {
    case 1:   return "UNSPECIFIED";
    case 2:   return "AUTH_EXPIRE";
    case 3:   return "AUTH_LEAVE";
    case 4:   return "ASSOC_EXPIRE";
    case 5:   return "ASSOC_TOOMANY";
    case 6:   return "NOT_AUTHED";
    case 7:   return "NOT_ASSOCED";
    case 8:   return "ASSOC_LEAVE";
    case 9:   return "ASSOC_NOT_AUTHED";
    case 10:  return "DISASSOC_PWRCAP_BAD";
    case 11:  return "DISASSOC_SUPCHAN_BAD";
    case 13:  return "IE_INVALID";
    case 14:  return "MIC_FAILURE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
    case 16:  return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17:  return "IE_IN_4WAY_DIFFERS";
    case 18:  return "GROUP_CIPHER_INVALID";
    case 19:  return "PAIRWISE_CIPHER_INVALID";
    case 20:  return "AKMP_INVALID";
    case 21:  return "UNSUPP_RSN_IE_VERSION";
    case 22:  return "INVALID_RSN_IE_CAP";
    case 23:  return "802_1X_AUTH_FAILED";
    case 24:  return "CIPHER_SUITE_REJECTED";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND (Hotspot not in range or wrong band/SSID)";
    case 202: return "AUTH_FAIL (Password mismatch or WPA3/PMF conflict)";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "UNKNOWN_REASON";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("[Wi-Fi Event] Station started. Connecting to hotspot...");
      break;

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[Wi-Fi Event] Associated with hotspot (Layer 2 link OK)!");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.println("\n==========================================");
      Serial.println("  [Wi-Fi Event] CONNECTED SUCCESSFULLY!   ");
      Serial.print("  Target SSID:  ");
      Serial.println(WIFI_SSID);
      Serial.print("  Assigned IP:  ");
      Serial.println(WiFi.localIP());
      Serial.print("  Signal (RSSI):");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      Serial.println("  STATUS LED:   SOLID ON (Online)");
      Serial.println("==========================================\n");
      digitalWrite(LED_PIN, HIGH);
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t reason = info.wifi_sta_disconnected.reason;
      Serial.println("\n------------------------------------------");
      Serial.println("  [Wi-Fi Event] DISCONNECTED");
      Serial.print("  Reason Code: ");
      Serial.println(reason);
      Serial.print("  Meaning:     ");
      Serial.println(getWiFiDisconnectReason(reason));
      Serial.println("------------------------------------------\n");
      break;
    }

    default:
      break;
  }
}

void applyWiFiEnhancements() {
  // Enable Protected Management Frames (PMF) capability for Android hotspots (WPA2/WPA3 mixed mode)
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    conf.sta.pmf_cfg.capable = true;
    conf.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
  }
  // Optimize RF TX power for close-range stability (reduces receiver saturation)
  WiFi.setTxPower(WIFI_POWER_17dBm);
}

void connectToWiFi() {
  WiFi.persistent(true);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  applyWiFiEnhancements();

  const char* targetSSID = knownSSIDs[currentNetIndex];
  const char* targetPass = knownPasswords[currentNetIndex];

  Serial.println("\n------------------------------------------");
  Serial.print("[Wi-Fi] Connecting to: ");
  Serial.println(targetSSID);
  Serial.print("[Wi-Fi] Password:      ");
  Serial.println(targetPass);
  Serial.println("------------------------------------------");

  WiFi.begin(targetSSID, targetPass);

  // Wait up to 10 seconds for initial connection with serial dots
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
    // Toggle LED while connecting
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("\n[Wi-Fi] Initial check: Connection established!");
  } else {
    Serial.println("\n[Wi-Fi] Still connecting in background... wiper operates locally.");
  }
}

void updateStatusLED() {
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH); // Solid ON when connected
  } else {
    // Blink every 500ms when disconnected/connecting
    if (millis() - lastLedBlink >= 500) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
  }
}

// ====================================================================
// 6. HTTP / HTTPS POST Telemetry Transmission
// ====================================================================
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Telemetry] Wi-Fi offline. Skipping HTTP POST (hardware operates locally).");
    return;
  }

  String url = String(BACKEND_URL);
  HTTPClient http;

  // JSON payload matching FastAPI SensorReadingCreate schema
  String jsonPayload = "{";
  jsonPayload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  jsonPayload += "\"rain_value\":" + String(rainVal) + ",";
  jsonPayload += "\"rain_status\":\"" + String(statusStr) + "\",";
  jsonPayload += "\"wiper_on\":" + String(wiperState ? "true" : "false") + ",";
  jsonPayload += "\"wiper_speed\":\"" + String(speedStr) + "\"";
  jsonPayload += "}";

  Serial.println("\n--- [Telemetry Packet] ---");
  Serial.print("Target URL: ");
  Serial.println(url);
  Serial.print("Payload:    ");
  Serial.println(jsonPayload);

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  bool beginSuccess = false;

  if (url.startsWith("https://")) {
    secureClient.setInsecure(); // Establishes secure TLS session without hardcoded root CA
    beginSuccess = http.begin(secureClient, url);
    http.setTimeout(2000);      // 2s timeout prevents stalling servo movement
  } else {
    beginSuccess = http.begin(plainClient, url);
    http.setTimeout(1500);
  }

  if (!beginSuccess) {
    Serial.println("[HTTP] Error: Failed to initialize connection to endpoint.");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(jsonPayload);

  if (httpCode > 0) {
    Serial.print("HTTP Status Code: ");
    Serial.println(httpCode);
    if (httpCode == HTTP_CODE_CREATED || httpCode == HTTP_CODE_OK) {
      Serial.println("Result: Ingested by FastAPI & saved to Supabase!");
    }
  } else {
    Serial.print("HTTP Error: ");
    Serial.println(http.errorToString(httpCode).c_str());
  }
  Serial.println("--------------------------");

  http.end();
}

// ====================================================================
// 7. Setup Routine
// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n==========================================");
  Serial.println("   Automatic Car Wiper - ESP32 Starting   ");
  Serial.println("   Testing Direct Wi-Fi: moto g96 5G_5200 ");
  Serial.println("==========================================");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Initialize Servo matching original working parameters (50Hz, 500-2400us, parked at 20°)
  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);
  myServo.write(20);

  // Connect to hardcoded Wi-Fi
  connectToWiFi();

  lastWiFiCheck = millis();
}

// ====================================================================
// 8. Main Loop
// ====================================================================
void loop() {
  // Update status LED
  updateStatusLED();

  // Read analog value from rain sensor (0 - 4095 ADC)
  int rainValue = analogRead(rainSensor);

  Serial.print("Rain Value: ");
  Serial.println(rainValue);

  // --- Rain Threshold & State Logic (Preserved Exactly) ---
  if (rainValue > 3500) {
    rainDetected = false;
  } else if (rainValue < 3000) {
    rainDetected = true;
  }

  const char* currentRainStatus;
  const char* currentWiperSpeed;

  if (rainDetected) {
    if (rainValue < 1700) {
      wipeDelay = 20;
      currentRainStatus = "Heavy Rain";
      currentWiperSpeed = "Fast";
      Serial.println("HEAVY RAIN - FAST WIPING");
    } else {
      wipeDelay = 60;
      currentRainStatus = "Light Rain";
      currentWiperSpeed = "Slow";
      Serial.println("LIGHT RAIN - SLOW WIPING");
    }

    angle += direction * 5;
    if (angle >= 120) {
      angle = 120;
      direction = -1;
    }
    if (angle <= 20) {
      angle = 20;
      direction = 1;
    }

    myServo.write(angle);
  } else {
    currentRainStatus = "Dry";
    currentWiperSpeed = "Off";
    Serial.println("DRY - WIPER OFF");

    angle = 20;
    direction = 1;
    myServo.write(20);
  }

  // --- Periodic Non-Blocking Telemetry (Every 3 Seconds) ---
  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();
    sendTelemetry(rainValue, rainDetected, currentRainStatus, currentWiperSpeed);
    
    // Serial Stream Format (for direct USB-to-cloud streaming if Wi-Fi is offline)
    Serial.print("TELEMETRY:{\"device_id\":\"");
    Serial.print(DEVICE_ID);
    Serial.print("\",\"rain_value\":");
    Serial.print(rainValue);
    Serial.print(",\"rain_status\":\"");
    Serial.print(currentRainStatus);
    Serial.print("\",\"wiper_on\":");
    Serial.print(rainDetected ? "true" : "false");
    Serial.print(",\"wiper_speed\":\"");
    Serial.print(currentWiperSpeed);
    Serial.println("\"}");
  }

  // --- Background Wi-Fi Reconnect (Every 10 Seconds) ---
  if (WiFi.status() != WL_CONNECTED && (millis() - lastWiFiCheck >= wifiCheckInterval)) {
    lastWiFiCheck = millis();
    currentNetIndex = (currentNetIndex + 1) % numKnownNetworks;
    const char* targetSSID = knownSSIDs[currentNetIndex];
    const char* targetPass = knownPasswords[currentNetIndex];

    Serial.print("\n[Wi-Fi] Disconnected. Rotating to network [");
    Serial.print(currentNetIndex + 1);
    Serial.print("/");
    Serial.print(numKnownNetworks);
    Serial.print("]: ");
    Serial.println(targetSSID);

    WiFi.disconnect();
    applyWiFiEnhancements();
    WiFi.begin(targetSSID, targetPass);
  }

  // --- Servo Step Delay (Preserved from Working Code) ---
  if (rainDetected) {
    delay(wipeDelay); // 20ms (fast) or 60ms (slow)
  } else {
    delay(300);       // 300ms idle delay when dry
  }
}
