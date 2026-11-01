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

// 2.4 GHz Wi-Fi credentials
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Backend API URL (FastAPI /api/readings endpoint)
// Local:  http://<YOUR_COMPUTER_IP>:8000/api/readings
// Render: https://your-service-name.onrender.com/api/readings
#define BACKEND_URL   "http://192.168.1.XXX:8000/api/readings"

// Device identifier
#define DEVICE_ID     "ESP32-WIPER"

// Telemetry interval in milliseconds (3000ms = 3 seconds)
#define TELEMETRY_INTERVAL_MS 3000

#endif // CONFIG_EXAMPLE_H

