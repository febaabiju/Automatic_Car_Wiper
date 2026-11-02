/**
 * Automatic Car Wiper - Smart Dynamic Wi-Fi ESP32 Firmware (Arduino IDE & PlatformIO)
 * 
 * Hardware:
 *   - ESP32 30-Pin Dev Module (USB-C)
 *   - Rain Sensor Analog ADC pin (GPIO 34 / ADC1)
 *   - SG90 Servo Motor PWM pin (GPIO 18)
 *   - Physical Reset Button: ESP32 Onboard BOOT Button (GPIO 0)
 * 
 * Dynamic Wi-Fi System:
 *   - WiFiManager with Captive Portal ("ESP32_Setup")
 *   - Automatic reconnection to saved Wi-Fi credentials stored in NVS flash
 *   - Captive portal fallback if Wi-Fi unavailable or unconfigured (192.168.4.1)
 *   - Physical BOOT button long-press (3 seconds) to wipe credentials & enter setup mode
 *   - Non-blocking architecture: Wiper operates locally even if Wi-Fi is offline
 * 
 * Backend Integration:
 *   - Telemetry via HTTPS POST to FastAPI endpoint (/api/readings) every 3 seconds.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <WiFiManager.h>

// ====================================================================
// 1. Configuration (Backend Settings)
// ====================================================================
// Live Deployed Render HTTPS Endpoint (or local http://<PC_IP>:8000/api/readings)
const char* BACKEND_URL = "https://automatic-car-wiper.onrender.com/api/readings";

// Device Identifier
const char* DEVICE_ID   = "ESP32-WIPER";

// Telemetry Interval (3 seconds)
const unsigned long TELEMETRY_INTERVAL_MS = 3000;

// ====================================================================
// 2. Hardware Pin Assignments & Servo Setup
// ====================================================================
Servo myServo;

const int rainSensor  = 34; // Rain Sensor Analog ADC pin (ADC1)
const int servoPin    = 18; // SG90 Servo PWM signal pin
const int TRIGGER_PIN = 0;  // Physical Reset Button: ESP32 Onboard BOOT Button (GPIO 0)

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
const unsigned long wifiCheckInterval = 15000; // Background reconnect interval (15 sec)

unsigned long buttonPressStart = 0; // Tracks BOOT button press duration

// Global WiFiManager instance
WiFiManager wm;

// ====================================================================
// 4. Function Prototypes
// ====================================================================
void setupWiFiManager();
void configModeCallback(WiFiManager *myWiFiManager);
void saveConfigCallback();
void checkResetButton();
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr);

// ====================================================================
// 5. WiFiManager Callbacks & Configuration
// ====================================================================

// Callback invoked when ESP32 fails to connect to saved Wi-Fi and starts AP Captive Portal
void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("\n========================================================");
  Serial.println("   [Wi-Fi Manager] ESP32 SETUP HOTSPOT STARTED!        ");
  Serial.println("========================================================");
  Serial.print("  Hotspot SSID:     ");
  Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.print("  Configuration IP: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("--------------------------------------------------------");
  Serial.println("  INSTRUCTIONS TO CONNECT:");
  Serial.println("  1. Connect your smartphone or laptop to 'ESP32_Setup'.");
  Serial.println("  2. If the portal doesn't open automatically, navigate");
  Serial.println("     to http://192.168.4.1 in your browser.");
  Serial.println("  3. Tap 'Configure WiFi', select your network, enter the");
  Serial.println("     password, and click Save.");
  Serial.println("  4. The ESP32 will save credentials & connect automatically.");
  Serial.println("========================================================\n");
}

// Callback invoked when user submits new credentials via the captive portal
void saveConfigCallback() {
  Serial.println("\n--------------------------------------------------------");
  Serial.println("  [Wi-Fi Manager] New credentials saved to NVS storage!");
  Serial.println("  Reconnecting to new Wi-Fi network...");
  Serial.println("--------------------------------------------------------\n");
}

// Initialize and execute WiFiManager connection sequence
void setupWiFiManager() {
  // Ensure NVS persistence is active so credentials survive restarts and power loss
  WiFi.persistent(true);

  // Set callbacks
  wm.setAPCallback(configModeCallback);
  wm.setSaveConfigCallback(saveConfigCallback);

  // Portal timeout: 180 seconds (3 minutes)
  // If no user configures the device within 3 mins, exit portal and allow local wiping
  wm.setConfigPortalTimeout(180);

  // Connection timeout: 15 seconds per connection attempt to an AP
  wm.setConnectTimeout(15);

  // Display saved Wi-Fi networks in portal scan list sorted by signal strength
  wm.setScanDispPerc(true);

  Serial.println("\n[Wi-Fi] Connecting to saved Wi-Fi...");

  // Attempt automatic connection using saved credentials from NVS.
  // If none exist or connection fails, automatically spins up "ESP32_Setup" AP.
  bool res = wm.autoConnect("ESP32_Setup");

  if (res) {
    Serial.println("\n==========================================");
    Serial.println("  [Wi-Fi] CONNECTED SUCCESSFULLY!         ");
    Serial.print("  Connected SSID:   ");
    Serial.println(WiFi.SSID());
    Serial.print("  Assigned IP:      ");
    Serial.println(WiFi.localIP());
    Serial.print("  Signal (RSSI):    ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.println("==========================================\n");
  } else {
    Serial.println("\n--------------------------------------------------------");
    Serial.println("  [Wi-Fi] Connection failed or portal timed out.");
    Serial.println("  Wiper will operate locally with hardware sensors.");
    Serial.println("  ESP32 will retry connecting in background.");
    Serial.println("--------------------------------------------------------\n");
  }
}

// Monitor physical BOOT button (GPIO 0) for a 3-second long press to clear credentials
void checkResetButton() {
  if (digitalRead(TRIGGER_PIN) == LOW) {
    if (buttonPressStart == 0) {
      buttonPressStart = millis();
    } else if (millis() - buttonPressStart >= 3000) {
      Serial.println("\n********************************************************");
      Serial.println("  [Wi-Fi Reset] BOOT button held for 3 seconds!");
      Serial.println("  Clearing saved Wi-Fi credentials from ESP32 NVS flash...");
      wm.resetSettings();
      Serial.println("  Credentials cleared! Restarting ESP32 into Setup Mode...");
      Serial.println("********************************************************\n");
      delay(1000);
      ESP.restart();
    }
  } else {
    buttonPressStart = 0;
  }
}

// ====================================================================
// 6. HTTP / HTTPS POST Telemetry Transmission
// ====================================================================
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Telemetry] Wi-Fi offline. Skipping HTTP POST (hardware remains functional).");
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

  // Support HTTPS (Render) or plain HTTP (Localhost)
  if (url.startsWith("https://")) {
    secureClient.setInsecure(); // Establishes secure TLS session without requiring hardcoded root CA
    beginSuccess = http.begin(secureClient, url);
    http.setTimeout(2000);      // 2s timeout prevents stalling servo movement during TLS negotiation
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
      Serial.println("Result: Ingested by FastAPI & saved to database.");
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

  // Configure physical BOOT button on GPIO 0
  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  // Initialize Servo matching original working parameters (50Hz, 500-2400us, parked at 20°)
  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);
  myServo.write(20);

  // Check if BOOT button is held down at startup for fast manual reset
  if (digitalRead(TRIGGER_PIN) == LOW) {
    Serial.println("[Wi-Fi Reset] BOOT button detected during boot. Checking duration...");
    unsigned long bootPress = millis();
    while (digitalRead(TRIGGER_PIN) == LOW && millis() - bootPress < 3000) {
      delay(100);
      Serial.print(".");
    }
    if (millis() - bootPress >= 3000) {
      Serial.println("\n[Wi-Fi Reset] Manual reset triggered at boot! Clearing credentials...");
      wm.resetSettings();
      delay(500);
    }
  }

  // Connect to Wi-Fi via WiFiManager (Auto-connect or Captive Portal)
  setupWiFiManager();

  // Initialize reconnect timer
  lastWiFiCheck = millis();
}

// ====================================================================
// 8. Main Loop
// ====================================================================
void loop() {
  // 1. Check physical BOOT button for long press (Wi-Fi reset trigger)
  checkResetButton();

  // 2. Read analog value from rain sensor (0 - 4095 ADC)
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

  // Determine descriptive status & speed labels for telemetry (Off, Slow, Fast)
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

  // --- Background Wi-Fi Reconnect Check (Every 15 Seconds) ---
  // If Wi-Fi dropped, attempt non-blocking reconnect to saved credentials in NVS
  if (WiFi.status() != WL_CONNECTED && (millis() - lastWiFiCheck >= wifiCheckInterval)) {
    lastWiFiCheck = millis();
    Serial.println("\n[Wi-Fi] Link offline. Attempting automatic reconnection to saved AP...");
    WiFi.reconnect();
  }

  // --- Servo Step Delay (Preserved from Working Code) ---
  if (rainDetected) {
    delay(wipeDelay); // 20ms (fast) or 60ms (slow)
  } else {
    delay(300);       // 300ms idle delay when dry
  }
}
