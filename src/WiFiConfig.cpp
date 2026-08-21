#include "WiFiConfig.h"

// Static instance pointer for callback functions
WiFiConfig* WiFiConfig::instance = nullptr;

// Constructor
WiFiConfig::WiFiConfig(const char* apSSID, const char* apPassword) {
  this->apSSID = String(apSSID);
  this->apPassword = String(apPassword);
  // NOTE: the legacy port-80 "Device Settings" web UI (this class's own
  // WebServer + setupRoutes()/handleRoot() etc.) has been removed — it was
  // an unused duplicate of the WiFi tab already served by WiFiManager on
  // port 8080. `server` is left null; WiFi connection management below
  // (AP/STA, credentials, isConnected(), etc.) is untouched.
  this->server = nullptr;
  this->isConnectedToWiFi = false;
  instance = this;
}

// Initialize the WiFi configuration system
void WiFiConfig::begin() {
  Serial.println("\n\n╔════════════════════════════════════════╗");
  Serial.println("║   ESP32 WiFi Configuration System     ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  // Load saved WiFi credentials
  loadCredentials();
  
  // Set WiFi mode
  WiFi.mode(WIFI_AP_STA);
  
  // Start Access Point
  WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  apActive = true;
  Serial.println("✓ Access Point started");
  Serial.println("   SSID: " + apSSID);
  Serial.println("   Password: " + apPassword);
  Serial.println("   IP: " + WiFi.softAPIP().toString());
  
  // Try to connect to saved WiFi if available
  if (ssid.length() > 0) {
    Serial.println("\n🔄 Attempting to connect to saved network: " + ssid);
    attemptConnection(ssid, password);
  }
  
  // Port-80 "Device Settings" web server intentionally not started — unused,
  // superseded by the WIFI tab in the main dashboard (port 8080).
}

// Handle web server requests (call this in loop)
void WiFiConfig::handleClient() {
  // No-op for the (removed) port-80 server; connection monitoring still runs.
  monitorConnection();
}

// Check if connected to a WiFi network
bool WiFiConfig::isConnected() {
  return isConnectedToWiFi;
}

// Get current SSID
String WiFiConfig::getSSID() {
  return ssid;
}

// Get current IP address
String WiFiConfig::getIPAddress() {
  return WiFi.localIP().toString();
}

// Get AP IP address
String WiFiConfig::getAPIPAddress() {
  return WiFi.softAPIP().toString();
}

// Manually connect to WiFi
bool WiFiConfig::connectToWiFi(String newSSID, String newPassword) {
  return attemptConnection(newSSID, newPassword);
}

// Record credentials for a connection WiFi.begin() already established
// elsewhere (see WiFiManager::_apiConnect) — no second WiFi.begin()/wait.
void WiFiConfig::adoptCurrentConnection(String newSSID, String newPassword) {
  ssid = newSSID;
  password = newPassword;
  isConnectedToWiFi = (WiFi.status() == WL_CONNECTED);
  if (isConnectedToWiFi) saveCredentials();
}

// Turn the setup hotspot (softAP) off without touching the STA connection —
// used to keep it off outside admin working hours. wifioff=true actually
// drops the AP interface (not just kicks clients), so WiFi.mode() falls
// back to STA-only until startAP() is called again.
void WiFiConfig::stopAP() {
  if (!apActive) return;
  WiFi.softAPdisconnect(true);
  apActive = false;
  Serial.println("[WiFi] Hotspot (" + apSSID + ") stopped — outside admin hours");
}

void WiFiConfig::startAP() {
  if (apActive) return;
  WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  apActive = true;
  Serial.println("[WiFi] Hotspot (" + apSSID + ") started — IP: " + WiFi.softAPIP().toString());
}

bool WiFiConfig::isAPActive() {
  return apActive;
}

// Disconnect from WiFi
void WiFiConfig::disconnect() {
  Serial.println("\n🔌 Disconnecting from WiFi...");
  WiFi.disconnect();
  isConnectedToWiFi = false;
  ssid = "";
  password = "";
  clearCredentials();
  Serial.println("✓ Disconnected and credentials cleared\n");
}

// Helper functions
void WiFiConfig::loadCredentials() {
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  preferences.end();
}

void WiFiConfig::saveCredentials() {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
}

void WiFiConfig::clearCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
}

bool WiFiConfig::attemptConnection(String newSSID, String newPassword) {
  WiFi.begin(newSSID.c_str(), newPassword.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) { 
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    ssid = newSSID;
    password = newPassword;
    isConnectedToWiFi = true;
    saveCredentials();
    return true;
  }
  
  isConnectedToWiFi = false;
  return false;
}

void WiFiConfig::monitorConnection() {
  static bool lastConnectionState = false;
  bool currentConnectionState = (WiFi.status() == WL_CONNECTED);
  
  if (currentConnectionState != lastConnectionState) {
    lastConnectionState = currentConnectionState;
    isConnectedToWiFi = currentConnectionState;
    
    if (currentConnectionState) {
      Serial.println("✅ WiFi Connected: " + WiFi.SSID());
      Serial.println("   IP: " + WiFi.localIP().toString());
    } else {
      Serial.println("❌ WiFi Disconnected");
    }
  }
}