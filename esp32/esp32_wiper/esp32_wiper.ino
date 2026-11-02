/**
 * Automatic Car Wiper - Smart Dynamic Wi-Fi ESP32 Firmware (Arduino IDE Sketch)
 * 
 * Hardware:
 *   - ESP32 30-Pin Dev Module (USB-C)
 *   - Rain Sensor Analog ADC pin (GPIO 34 / ADC1)
 *   - SG90 Servo Motor PWM pin (GPIO 18)
 *   - Status LED: ESP32 Onboard Blue LED (GPIO 2)
 *   - Reset Button: ESP32 Onboard BOOT Button (GPIO 0)
 * 
 * Status LED Indications:
 *   - Rapid Flash (100ms ON / 100ms OFF) : Hotspot "ESP32_Setup" is active (Captive Portal)
 *   - Slow Blink  (500ms ON / 500ms OFF) : Connecting to Wi-Fi network
 *   - Solid ON    (Continuous Bright Blue): Connected to Wi-Fi successfully & Online
 *   - Short Pulse (Brief flash every 2s) : Offline (Operating wiper locally)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <WiFiManager.h>
#include <esp_wifi.h>

// ====================================================================
// 1. Configuration (Backend Settings)
// ====================================================================
const char* BACKEND_URL = "https://automatic-car-wiper.onrender.com/api/readings";
const char* DEVICE_ID   = "ESP32-WIPER";
const unsigned long TELEMETRY_INTERVAL_MS = 3000;

// ====================================================================
// 2. Hardware Pin Assignments & LED Status States
// ====================================================================
Servo myServo;

const int rainSensor  = 34; // Rain Sensor Analog ADC pin (ADC1)
const int servoPin    = 18; // SG90 Servo PWM signal pin
const int LED_PIN     = 2;  // Onboard Blue Status LED (GPIO 2)
const int TRIGGER_PIN = 0;  // Physical Reset Button: ESP32 Onboard BOOT Button (GPIO 0)

int angle = 20;
int direction = 1;

bool rainDetected = false;
int wipeDelay = 60;

enum WiFiStatusState {
  STATE_CONNECTING,
  STATE_PORTAL,
  STATE_CONNECTED,
  STATE_OFFLINE
};

volatile WiFiStatusState currentWifiState = STATE_CONNECTING;

// ====================================================================
// 3. Timing & State Tracking Variables
// ====================================================================
unsigned long lastSendTime = 0;
const unsigned long sendInterval = TELEMETRY_INTERVAL_MS;

unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 15000;

unsigned long buttonPressStart = 0;

WiFiManager wm;

// ====================================================================
// 4. Function Prototypes
// ====================================================================
void ledStatusTask(void *param);
void setupWiFiManager();
void configModeCallback(WiFiManager *myWiFiManager);
void saveConfigCallback();
void checkResetButton();
void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr);

// ====================================================================
// 5. FreeRTOS Background LED Status Task
// ====================================================================
void ledStatusTask(void *param) {
  pinMode(LED_PIN, OUTPUT);

  for (;;) {
    switch (currentWifiState) {
      case STATE_PORTAL:
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        break;

      case STATE_CONNECTING:
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        break;

      case STATE_CONNECTED:
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        break;

      case STATE_OFFLINE:
      default:
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(1900 / portTICK_PERIOD_MS);
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        break;
    }
  }
}

// ====================================================================
// 6. WiFiManager Callbacks & Configuration
// ====================================================================
void configModeCallback(WiFiManager *myWiFiManager) {
  currentWifiState = STATE_PORTAL;

  Serial.println("\n========================================================");
  Serial.println("   [Wi-Fi Manager] ESP32 SETUP HOTSPOT STARTED!        ");
  Serial.println("========================================================");
  Serial.print("  Hotspot SSID:     ");
  Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.print("  Configuration IP: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("  BLUE LED:         RAPID BLINKING (Setup Mode)");
  Serial.println("--------------------------------------------------------");
  Serial.println("  INSTRUCTIONS TO CONNECT:");
  Serial.println("  1. Connect your smartphone or laptop to 'ESP32_Setup'.");
  Serial.println("  2. Open your browser and navigate to http://192.168.4.1");
  Serial.println("  3. Tap 'Configure WiFi', select your network, enter the");
  Serial.println("     password, and click Save.");
  Serial.println("  4. Once connected, the Blue LED will turn SOLID ON.");
  Serial.println("========================================================\n");
}

void saveConfigCallback() {
  currentWifiState = STATE_CONNECTING;
  Serial.println("\n[Wi-Fi Manager] New credentials saved! Connecting...");
}

void setupWiFiManager() {
  WiFi.persistent(true);
  WiFi.setSleep(false);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    conf.sta.pmf_cfg.capable = true;
    conf.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
  }

  wm.setAPCallback(configModeCallback);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(300); // 5 minutes
  wm.setConnectTimeout(15);
  wm.setScanDispPerc(true);
  wm.setTitle("Automatic Car Wiper - Wi-Fi Setup");

  Serial.println("\n[Wi-Fi] Connecting to saved Wi-Fi...");
  currentWifiState = STATE_CONNECTING;

  bool res = wm.autoConnect("ESP32_Setup");

  if (res) {
    currentWifiState = STATE_CONNECTED;
    Serial.println("\n[Wi-Fi] CONNECTED SUCCESSFULLY!");
  } else {
    currentWifiState = STATE_OFFLINE;
    Serial.println("\n[Wi-Fi] Connection failed or portal timed out.");
  }
}

void checkResetButton() {
  if (digitalRead(TRIGGER_PIN) == LOW) {
    if (buttonPressStart == 0) {
      buttonPressStart = millis();
    } else if (millis() - buttonPressStart >= 3000) {
      Serial.println("\n[Wi-Fi Reset] BOOT button held for 3 seconds! Resetting...");
      wm.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else {
    buttonPressStart = 0;
  }
}

void sendTelemetry(int rainVal, bool wiperState, const char* statusStr, const char* speedStr) {
  if (WiFi.status() != WL_CONNECTED) {
    currentWifiState = STATE_OFFLINE;
    return;
  }

  currentWifiState = STATE_CONNECTED;
  String url = String(BACKEND_URL);
  HTTPClient http;

  String jsonPayload = "{";
  jsonPayload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  jsonPayload += "\"rain_value\":" + String(rainVal) + ",";
  jsonPayload += "\"rain_status\":\"" + String(statusStr) + "\",";
  jsonPayload += "\"wiper_on\":" + String(wiperState ? "true" : "false") + ",";
  jsonPayload += "\"wiper_speed\":\"" + String(speedStr) + "\"";
  jsonPayload += "}";

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  bool beginSuccess = false;

  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    beginSuccess = http.begin(secureClient, url);
    http.setTimeout(2000);
  } else {
    beginSuccess = http.begin(plainClient, url);
    http.setTimeout(1500);
  }

  if (!beginSuccess) return;

  http.addHeader("Content-Type", "application/json");
  http.POST(jsonPayload);
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  xTaskCreatePinnedToCore(
    ledStatusTask,
    "LED_Status",
    2048,
    NULL,
    1,
    NULL,
    0
  );

  myServo.setPeriodHertz(50);
  myServo.attach(servoPin, 500, 2400);
  myServo.write(20);

  if (digitalRead(TRIGGER_PIN) == LOW) {
    unsigned long bootPress = millis();
    while (digitalRead(TRIGGER_PIN) == LOW && millis() - bootPress < 3000) {
      delay(100);
    }
    if (millis() - bootPress >= 3000) {
      wm.resetSettings();
      delay(500);
    }
  }

  setupWiFiManager();
  lastWiFiCheck = millis();
}

void loop() {
  checkResetButton();

  int rainValue = analogRead(rainSensor);

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

  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();
    sendTelemetry(rainValue, rainDetected, currentRainStatus, currentWiperSpeed);
  }

  if (WiFi.status() != WL_CONNECTED && (millis() - lastWiFiCheck >= wifiCheckInterval)) {
    lastWiFiCheck = millis();
    currentWifiState = STATE_CONNECTING;
    WiFi.reconnect();
  }

  if (WiFi.status() == WL_CONNECTED) {
    currentWifiState = STATE_CONNECTED;
  }

  if (rainDetected) {
    delay(wipeDelay);
  } else {
    delay(300);
  }
}
