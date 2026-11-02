#ifndef CONFIG_EXAMPLE_H
#define CONFIG_EXAMPLE_H

// ====================================================================
// Automatic Car Wiper - ESP32 Firmware Configuration Template
// ====================================================================
// Instructions:
// 1. Copy this file to "include/config.h"
// 2. Enter your real Wi-Fi credentials and backend URL
// 3. "include/config.h" is ignored by Git to keep your secrets private
// ====================================================================

// 2.4 GHz Wi-Fi credentials (supports multiple hotspots)
#define WIFI_SSID_1     "YOUR_PRIMARY_SSID"
#define WIFI_PASSWORD_1 "YOUR_PRIMARY_PASSWORD"

#define WIFI_SSID_2     "YOUR_SECONDARY_SSID"
#define WIFI_PASSWORD_2 "YOUR_SECONDARY_PASSWORD"

#define WIFI_SSID       "YOUR_PRIMARY_SSID"
#define WIFI_PASSWORD   "YOUR_PRIMARY_PASSWORD"

// Backend API URL (FastAPI /api/readings endpoint)
// Local:  http://<YOUR_COMPUTER_IP>:8000/api/readings
// Render: https://automatic-car-wiper.onrender.com/api/readings
#define BACKEND_URL "https://automatic-car-wiper.onrender.com/api/readings"

// Device identifier
#define DEVICE_ID "ESP32-WIPER"

// Telemetry interval in milliseconds (3000ms = 3 seconds)
#define TELEMETRY_INTERVAL_MS 3000

#endif // CONFIG_EXAMPLE_H
