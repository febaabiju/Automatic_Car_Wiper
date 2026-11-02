/**
 * Automatic Car Wiper - Clean Production ESP32 Firmware
 * 
 * Hardware:
 *   - ESP32 30-Pin Dev Module
 *   - Rain Sensor: GPIO 34 (Analog ADC1)
 *   - SG90 Servo:  GPIO 18 (PWM)
 *   - Blue LED:    GPIO 2 (Onboard)
 * 
 * Serial Monitor Baud Rate: 115200
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>

#if __has_include("config.h")
  #include "config.h"
#elif __has_include("config.example.h")
  #include "config.example.h"
#endif

#ifndef BACKEND_URL
  #define BACKEND_URL "https://automatic-car-wiper.onrender.com/api/readings"
#endif
#ifndef DEVICE_ID
  #define DEVICE_ID "ESP32-WIPER"
#endif

// Known Hotspots (Cycles to whichever is actively broadcasting)
const char* HOTSPOT_1_SSID = "narzo30";
const char* HOTSPOT_1_PASS = "Narzo12345";

const char* HOTSPOT_2_SSID = "moto g96 5G_5200";
const char* HOTSPOT_2_PASS = "567895678";

// Active connection targets
const char* activeSSID = HOTSPOT_1_SSID;
const char* activePASS = HOTSPOT_1_PASS;

// Hardware Pins
Servo myServo;
const int rainSensor = 34; // Rain Sensor Analog ADC pin
const int servoPin   = 18; // SG90 Servo PWM signal pin
const int LED_PIN    = 2;  // Onboard Blue Status LED

int angle = 20;
int direction = 1;
bool rainDetected = false;
int wipeDelay = 60;

unsigned long lastSendTime = 0;
unsigned long lastReconnect = 0;
unsigned long lastBlink = 0;
bool ledState = false;

// Cloud Telemetry POST
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  HTTPClient http;
  if (!http.begin(BACKEND_URL)) {
    Serial.println("[HTTP] Begin failed");
    return;
  }

  http.setTimeout(2500);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"device_id\":\"" + String(DEVICE_ID) + "\","
                   "\"rain_value\":" + String(rainVal) + ","
                   "\"rain_status\":\"" + String(statusStr) + "\","
                   "\"wiper_on\":" + String(wiperState ? "true" : "false") + ","
                   "\"wiper_speed\":\"" + String(speedStr) + "\"}";

  int httpCode = http.POST(payload);
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.print("[CLOUD] Streamed to Render: HTTP ");
    Serial.println(httpCode);
  } else {
    Serial.print("[CLOUD] HTTP Error: ");
    Serial.println(httpCode);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n==========================================");
  Serial.println("   Automatic Car Wiper - ESP32 Starting   ");
  Serial.println("==========================================");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Servo Setup (50Hz, 500-2400us, parked at 20°)
  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);
  myServo.write(20);

  // Wi-Fi Scan & Auto-Select
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("[Wi-Fi] Scanning nearby 2.4 GHz networks...");
  int n = WiFi.scanNetworks();
  Serial.print("[Wi-Fi] Found ");
  Serial.print(n);
  Serial.println(" networks:");

  bool foundTarget = false;
  for (int i = 0; i < n; ++i) {
    String foundSSID = WiFi.SSID(i);
    int foundRSSI = WiFi.RSSI(i);
    Serial.print("  [");
    Serial.print(i + 1);
    Serial.print("] ");
    Serial.print(foundSSID);
    Serial.print(" (");
    Serial.print(foundRSSI);
    Serial.println(" dBm)");

    if (foundSSID == HOTSPOT_1_SSID) {
      activeSSID = HOTSPOT_1_SSID;
      activePASS = HOTSPOT_1_PASS;
      foundTarget = true;
    } else if (!foundTarget && foundSSID == HOTSPOT_2_SSID) {
      activeSSID = HOTSPOT_2_SSID;
      activePASS = HOTSPOT_2_PASS;
      foundTarget = true;
    }
  }

  Serial.print("[Wi-Fi] Connecting to: ");
  Serial.println(activeSSID);
  WiFi.begin(activeSSID, activePASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(400);
    Serial.print(".");
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("\n[Wi-Fi] SUCCESS! CONNECTED & GOT IP!");
    Serial.print("[Wi-Fi] IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("[Wi-Fi] Signal:     ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("\n[Wi-Fi] Still connecting in background...");
  }
}

void loop() {
  // LED Status & Auto-Reconnect
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH); // Solid ON when online
  } else {
    // Blink LED when searching/connecting
    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }

    // Retry connection every 8 seconds
    if (millis() - lastReconnect >= 8000) {
      lastReconnect = millis();
      Serial.print("[Wi-Fi] Retrying: ");
      Serial.println(activeSSID);
      WiFi.disconnect();
      WiFi.begin(activeSSID, activePASS);
    }
  }

  // Rain Sensor Read (0 - 4095 ADC)
  int rainValue = analogRead(rainSensor);

  // Rain Detection Thresholds
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
    } else {
      wipeDelay = 60;
      currentRainStatus = "Light Rain";
      currentWiperSpeed = "Slow";
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
    angle = 20;
    direction = 1;
    myServo.write(20);
  }

  // Periodic Telemetry & Serial Logging (Every 3 seconds)
  if (millis() - lastSendTime >= 3000) {
    lastSendTime = millis();
    Serial.print("[SENSOR] Rain: ");
    Serial.print(rainValue);
    Serial.print(" ADC | Condition: ");
    Serial.print(currentRainStatus);
    Serial.print(" | Wiper: ");
    Serial.print(currentWiperSpeed);
    Serial.print(" | Wi-Fi: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "ONLINE" : "SEARCHING");

    sendTelemetry(rainValue, rainDetected, currentRainStatus, currentWiperSpeed);
  }

  // Servo step delay
  if (rainDetected) {
    delay(wipeDelay);
  } else {
    delay(300);
  }
}
