/**
 * Automatic Car Wiper - Clean Production ESP32 Firmware
 * 
 * Hardware:
 *   - ESP32 30-Pin Dev Module
 *   - Rain Sensor Analog ADC pin: GPIO 34
 *   - SG90 Servo PWM pin:         GPIO 18
 *   - Status Blue LED:            GPIO 2
 * 
 * Wi-Fi:
 *   - Auto-connects to saved network from include/config.h
 *   - Streams telemetry to Render every 3 seconds
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

#ifndef WIFI_SSID
  #define WIFI_SSID     "narzo30"
#endif
#ifndef WIFI_PASSWORD
  #define WIFI_PASSWORD "Narzo12345"
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

// Hardware Instances & Pins
Servo myServo;
const int rainSensor = 34; // Rain Sensor Analog (ADC1)
const int servoPin   = 18; // SG90 Servo PWM
const int LED_PIN    = 2;  // Onboard Blue Status LED

int angle = 20;
int direction = 1;
bool rainDetected = false;
int wipeDelay = 60;

unsigned long lastSendTime = 0;
unsigned long lastReconnect = 0;
unsigned long lastBlink = 0;
bool ledState = false;

// HTTP Telemetry Transmission
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
    Serial.print("[CLOUD] Streamed to Render successfully! HTTP ");
    Serial.println(httpCode);
  } else {
    Serial.print("[CLOUD] HTTP POST failed: ");
    Serial.println(httpCode);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n==========================================");
  Serial.println("   Automatic Car Wiper - ESP32 Starting   ");
  Serial.print("   Wi-Fi SSID: ");
  Serial.println(WIFI_SSID);
  Serial.println("==========================================");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Servo Setup (50Hz, 500-2400us, parked at 20°)
  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);
  myServo.write(20);

  // Wi-Fi Connection
  WiFi.mode(WIFI_STA);
  Serial.print("[Wi-Fi] Connecting to: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(400);
    Serial.print(".");
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("\n[Wi-Fi] CONNECTED! IP: " + WiFi.localIP().toString());
    Serial.print("[Wi-Fi] Signal (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("\n[Wi-Fi] Not connected yet, will retry in background...");
  }
}

void loop() {
  // LED & Wi-Fi Management
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH); // Solid ON when online
  } else {
    // Blink LED every 500ms when offline
    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }

    // Auto-reconnect every 8 seconds
    if (millis() - lastReconnect >= 8000) {
      lastReconnect = millis();
      Serial.println("[Wi-Fi] Retrying connection to: " + String(WIFI_SSID));
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
  if (millis() - lastSendTime >= TELEMETRY_INTERVAL_MS) {
    lastSendTime = millis();
    Serial.print("[SENSOR] Rain: ");
    Serial.print(rainValue);
    Serial.print(" ADC | Condition: ");
    Serial.print(currentRainStatus);
    Serial.print(" | Wiper: ");
    Serial.println(currentWiperSpeed);

    sendTelemetry(rainValue, rainDetected, currentRainStatus, currentWiperSpeed);
  }

  // Servo movement delay
  if (rainDetected) {
    delay(wipeDelay);
  } else {
    delay(300);
  }
}
