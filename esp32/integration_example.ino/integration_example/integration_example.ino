/**
 * Automatic Car Wiper - ESP32 IoT Firmware
 * Hardware: ESP32 + Rain Sensor (GPIO 34) + SG90 Servo Motor (GPIO 18)
 * Backend Integration: Sends telemetry via HTTP POST to FastAPI (/api/readings) every 3 seconds.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>

// ====================================================================
// 1. User Configuration (Enter your Wi-Fi & Backend Details Here)
// ====================================================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";      // Must be a 2.4 GHz Wi-Fi network
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";  // Your Wi-Fi password

// Backend API URL:
// - If running FastAPI locally, use your computer's local Wi-Fi IP address (e.g., http://192.168.1.150:8000/api/readings)
//   (Do NOT use "localhost" or "127.0.0.1" because ESP32 cannot resolve your PC's localhost).
// - If deployed on Render/Cloud, use your public URL (e.g., https://your-app.onrender.com/api/readings)
const char* BACKEND_URL   = "http://192.168.1.XXX:8000/api/readings";

const char* DEVICE_ID     = "ESP32-WIPER";

// ====================================================================
// 2. Hardware Pin Assignments & Variables (Preserved from Working Code)
// ====================================================================
Servo myServo;

const int rainSensor = 34; // Rain Sensor Analog ADC pin
const int servoPin   = 18; // SG90 Servo PWM signal pin

int angle = 20;
int direction = 1;

bool rainDetected = false;
int wipeDelay = 60;

// ====================================================================
// 3. Non-Blocking Telemetry Timing Variables
// ====================================================================
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 3000; // Send telemetry every 3000 ms (3 seconds)

unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 10000; // Check Wi-Fi connection every 10 seconds

// ====================================================================
// 4. Wi-Fi Helper Functions
// ====================================================================
void connectToWiFi() {
  Serial.print("[Wi-Fi] Connecting to: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Allow up to 10 seconds for initial connection without permanently hanging setup
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
    Serial.println("\n[Wi-Fi] Initial connection timeout. Wiper will run offline and retry in background.");
  }
}

// ====================================================================
// 5. HTTP POST Telemetry Transmission
// ====================================================================
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Telemetry] Wi-Fi not connected. Skipping HTTP POST.");
    return;
  }

  HTTPClient http;
  http.begin(BACKEND_URL);
  http.setTimeout(1500); // 1.5s timeout prevents long stalls if server is slow
  http.addHeader("Content-Type", "application/json");

  // Construct JSON payload matching FastAPI SensorReadingCreate model
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
// 6. Setup Routine
// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n==========================================");
  Serial.println("   Automatic Car Wiper - ESP32 Starting   ");
  Serial.println("==========================================");

  // Initialize Servo exactly as in working hardware code
  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);
  myServo.write(20);

  // Connect to Wi-Fi
  connectToWiFi();
}

// ====================================================================
// 7. Main Loop
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

  // Determine current descriptive status & speed labels for telemetry
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
    currentWiperSpeed = "OFF";
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
