/**
 * Automatic Car Wiper - PlatformIO ESP32 IoT Firmware
 * Hardware: ESP32 30-Pin Dev Module + Rain Sensor (GPIO 34) + SG90 Servo Motor (GPIO 18)
 * Backend Integration: Telemetry via HTTP POST to FastAPI (/api/readings) every 3 seconds.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>

// ====================================================================
// 1. Configuration (Local Credentials Loaded Safely)
// ====================================================================
#if __has_include("config.h")
  #include "config.h"
#elif __has_include("config.example.h")
  #include "config.example.h"
#else
  #define WIFI_SSID             "YOUR_WIFI_SSID"
  #define WIFI_PASSWORD         "YOUR_WIFI_PASSWORD"
  #define BACKEND_URL           "http://192.168.1.XXX:8000/api/readings"
  #define DEVICE_ID             "ESP32-WIPER"
  #define TELEMETRY_INTERVAL_MS 3000
#endif

// ====================================================================
// 2. Hardware Pin Assignments & Variables (Original Working Logic)
// ====================================================================
Servo myServo;

const int rainSensor = 34; // Rain Sensor Analog ADC pin (ADC1)
const int servoPin   = 18; // SG90 Servo PWM signal pin

int angle = 20;
int direction = 1;

bool rainDetected = false;
int wipeDelay = 60;

// ====================================================================
// 3. Non-Blocking Telemetry Timing Variables
// ====================================================================
unsigned long lastSendTime = 0;
const unsigned long sendInterval = TELEMETRY_INTERVAL_MS;

unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 10000; // Background check every 10 seconds

// ====================================================================
// 4. Function Prototypes
// ====================================================================
void connectToWiFi();
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr);

// ====================================================================
// 5. Wi-Fi Helper Functions
// ====================================================================
void connectToWiFi() {
  Serial.print("[Wi-Fi] Connecting to: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Prevents modem sleep from dropping EAPOL handshake
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Allow up to 10 seconds for initial connection; does not permanently block wiper operation
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] Connected successfully!");
    Serial.print("[Wi-Fi] ESP32 IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[Wi-Fi] Initial connection timeout. Wiper will operate offline and auto-reconnect.");
  }
}

// ====================================================================
// 6. HTTP POST Telemetry Transmission
// ====================================================================
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Telemetry] Wi-Fi offline. Skipping HTTP POST (hardware remains functional).");
    return;
  }

  HTTPClient http;
  http.begin(BACKEND_URL);
  http.setTimeout(1500); // 1.5s timeout prevents stalling servo movement
  http.addHeader("Content-Type", "application/json");

  // Construct JSON payload matching FastAPI SensorReadingCreate schema
  String jsonPayload = "{";
  jsonPayload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  jsonPayload += "\"rain_value\":" + String(rainVal) + ",";
  jsonPayload += "\"rain_status\":\"" + String(statusStr) + "\",";
  jsonPayload += "\"wiper_on\":" + String(wiperState ? "true" : "false") + ",";
  jsonPayload += "\"wiper_speed\":\"" + String(speedStr) + "\"";
  jsonPayload += "}";

  Serial.println("\n--- [Telemetry Packet] ---");
  Serial.print("Payload: ");
  Serial.println(jsonPayload);

  int httpCode = http.POST(jsonPayload);

  if (httpCode > 0) {
    Serial.print("HTTP Status Code: ");
    Serial.println(httpCode);
    if (httpCode == HTTP_CODE_CREATED || httpCode == HTTP_CODE_OK) {
      Serial.println("Result: Ingested by FastAPI & saved to Supabase.");
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
  Serial.println("==========================================");

  // Initialize Servo matching original working parameters (50Hz, 500-2400us, parked at 20°)
  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);
  myServo.write(20);

  // Connect to Wi-Fi
  connectToWiFi();
}

// ====================================================================
// 8. Main Loop
// ====================================================================
void loop() {
  // Read analog value from rain sensor (0 - 4095 ADC)
  int rainValue = analogRead(rainSensor);

  Serial.print("Rain Value: ");
  Serial.println(rainValue);

  // --- Rain Threshold & State Logic (Preserved Exactly) ---
  // Dry condition
  if (rainValue > 3500) {
    rainDetected = false;
  }
  // Rain detected
  else if (rainValue < 3000) {
    rainDetected = true;
  }

  // Determine current descriptive status & speed labels for telemetry (Off, Slow, Fast)
  const char* currentRainStatus;
  const char* currentWiperSpeed;

  if (rainDetected) {
    // Heavy rain: fast wiping below 1700
    if (rainValue < 1700) {
      wipeDelay = 20;
      currentRainStatus = "Heavy Rain";
      currentWiperSpeed = "Fast";
      Serial.println("HEAVY RAIN - FAST WIPING");
    }
    // Light rain: slow wiping
    else {
      wipeDelay = 60;
      currentRainStatus = "Light Rain";
      currentWiperSpeed = "Slow";
      Serial.println("LIGHT RAIN - SLOW WIPING");
    }

    // Step servo sweep between 20° and 120°
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
  }
  else {
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
  }

  // --- Background Wi-Fi Reconnect Check (Every 10 Seconds) ---
  if (WiFi.status() != WL_CONNECTED && (millis() - lastWiFiCheck >= wifiCheckInterval)) {
    lastWiFiCheck = millis();
    Serial.println("[Wi-Fi] Connection lost. Attempting auto-reconnect...");
    WiFi.reconnect();
  }

  // --- Servo Step Delay (Preserved from Working Code) ---
  if (rainDetected) {
    delay(wipeDelay); // 20ms (fast) or 60ms (slow)
  } else {
    delay(300);       // 300ms idle delay when dry
  }
}
