#ifndef CONFIG_EXAMPLE_H
#define CONFIG_EXAMPLE_H

// ====================================================================
// Automatic Car Wiper - Hardware & Wi-Fi Configuration (Example)
// ====================================================================
// Copy this file to "config.h" and insert your network credentials.

#define WIFI_SSID               "YOUR_PRIMARY_WIFI_SSID"
#define WIFI_PASSWORD           "YOUR_PRIMARY_WIFI_PASS"

#define BACKUP_SSID             "YOUR_BACKUP_WIFI_SSID"
#define BACKUP_PASSWORD         "YOUR_BACKUP_WIFI_PASS"

#define BACKEND_URL             "https://automatic-car-wiper.onrender.com/api/readings"
#define DEVICE_ID               "ESP32-WIPER"

#define PIN_RAIN_SENSOR         34
#define PIN_SERVO               18
#define PIN_STATUS_LED          2
#define PIN_BOOT_BUTTON         0

#define RAIN_DRY_THRESHOLD      3500
#define RAIN_WET_THRESHOLD      3000
#define RAIN_HEAVY_THRESHOLD    1700

#define SERVO_MIN_ANGLE         20
#define SERVO_MAX_ANGLE         120
#define SERVO_STEP_ANGLE        5
#define WIPE_DELAY_SLOW_MS      60
#define WIPE_DELAY_FAST_MS      20

#define TELEMETRY_INTERVAL_MS   3000
#define WIFI_CONNECT_TIMEOUT_MS 8000
#define WIFI_RETRY_INTERVAL_MS  5000
#define SENSOR_SAMPLE_INTERVAL  50

#define SOFTAP_SSID             "AutoWiper_Setup"
#define SOFTAP_PASS             ""
#define SOFTAP_TIMEOUT_MS       180000

#endif // CONFIG_EXAMPLE_H
