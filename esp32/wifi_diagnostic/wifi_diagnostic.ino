/**
 * ESP32 Wi-Fi Diagnostic Tool
 * Used to diagnose Wi-Fi association and authentication events (Reason codes).
 * 
 * Features:
 * - Station mode (WIFI_STA)
 * - Disabled modem sleep (WiFi.setSleep(false)) for reliable 4-way handshake
 * - Detailed Wi-Fi event logging (Layer 2 association, IP acquisition, disconnect reason codes)
 * - Does NOT erase NVS flash credentials
 */

#include <WiFi.h>

// ====================================================================
// Wi-Fi Configuration (Enter your Realme Hotspot Credentials Here)
// ====================================================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Translate disconnect reason codes to human-readable explanations
const char* getDisconnectReasonText(uint8_t reason) {
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
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT (Phone dropped EAPOL key exchange)";
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
    case 201: return "NO_AP_FOUND (Hotspot not detected)";
    case 202: return "AUTH_FAIL (Password mismatch, PMF required, or WPA3/Wi-Fi 6 conflict)";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 206: return "AP_TSF_RESET";
    case 207: return "ROAMING";
    default:  return "OTHER_REASON";
  }
}

// Wi-Fi Event Handler Callback
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_READY:
      Serial.println("[Wi-Fi Event] Wi-Fi interface ready");
      break;

    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("[Wi-Fi Event] Station mode started");
      break;

    case ARDUINO_EVENT_WIFI_STA_STOP:
      Serial.println("[Wi-Fi Event] Station mode stopped");
      break;

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("\n[Wi-Fi Event] SUCCESS: Associated with AP (Layer 2 Link Established)!");
      Serial.print("             Channel: ");
      Serial.println(info.wifi_sta_connected.channel);
      break;

    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      Serial.println("[Wi-Fi Event] Authentication mode changed by AP");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.println("\n**************************************************");
      Serial.println("  [Wi-Fi Event] SUCCESS: CONNECTED & GOT IP!");
      Serial.print("  IP Address: ");
      Serial.println(WiFi.localIP());
      Serial.print("  Gateway:    ");
      Serial.println(WiFi.gatewayIP());
      Serial.print("  Subnet:     ");
      Serial.println(WiFi.subnetMask());
      Serial.print("  Signal:     ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      Serial.println("**************************************************\n");
      break;

    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Serial.println("[Wi-Fi Event] Lost IP address");
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t reason = info.wifi_sta_disconnected.reason;
      Serial.println("\n--------------------------------------------------");
      Serial.println("  [Wi-Fi Event] DISCONNECTED / AUTH FAILED");
      Serial.print("  Reason Code: ");
      Serial.println(reason);
      Serial.print("  Meaning:     ");
      Serial.println(getDisconnectReasonText(reason));
      Serial.println("--------------------------------------------------\n");
      break;
    }

    default:
      Serial.print("[Wi-Fi Event] Event ID: ");
      Serial.println(event);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n==================================================");
  Serial.println("       ESP32 Wi-Fi Diagnostic Test Utility       ");
  Serial.println("==================================================");

  // 1. Attach event listener before starting Wi-Fi
  WiFi.onEvent(onWiFiEvent);

  // 2. Set pure Station Mode
  WiFi.mode(WIFI_STA);

  // 3. Disable modem sleep so radio stays awake during EAPOL handshake
  WiFi.setSleep(false);

  Serial.print("Target SSID: ");
  Serial.println(WIFI_SSID);
  Serial.println("Initiating connection...\n");

  // 4. Begin connection without clearing NVS
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  static unsigned long lastHeartbeat = 0;

  // Print heartbeat status every 5 seconds
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();

    wl_status_t status = WiFi.status();
    Serial.print("[Status Check] WiFi.status() = ");
    Serial.print(status);

    switch (status) {
      case WL_CONNECTED:
        Serial.println(" (WL_CONNECTED: Online)");
        break;
      case WL_NO_SSID_AVAIL:
        Serial.println(" (WL_NO_SSID_AVAIL: Hotspot not found)");
        break;
      case WL_CONNECT_FAILED:
        Serial.println(" (WL_CONNECT_FAILED: Authentication failure)");
        break;
      case WL_CONNECTION_LOST:
        Serial.println(" (WL_CONNECTION_LOST)");
        break;
      case WL_DISCONNECTED:
        Serial.println(" (WL_DISCONNECTED: Connecting or retrying)");
        break;
      case WL_IDLE_STATUS:
        Serial.println(" (WL_IDLE_STATUS)");
        break;
      default:
        Serial.println(" (UNKNOWN)");
        break;
    }
  }

  delay(100);
}

