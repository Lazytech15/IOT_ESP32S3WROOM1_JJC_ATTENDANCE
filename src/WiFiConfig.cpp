#include "WiFiConfig.h"
#include <ArduinoJson.h>
#include <algorithm>

// How much one recorded drop/connect-fail costs a network's score, in
// "RSSI dBm equivalent" points. RSSI values here run roughly -30 (excellent)
// to -90 (unusable), so a couple of drops (16-24 points) is enough to make a
// merely-noisy-but-fast network lose to a rock-solid one several bars weaker
// — matching the "full-bars-but-slow vs two-bars-but-fast" scenario: the
// weaker network earns that headroom back over time simply by *not* dropping
// while the strong-but-flaky one keeps accumulating penalties.
static const int32_t DROP_PENALTY   = 8;
static const int32_t FAIL_PENALTY   = 15;
// Caps so one bad night doesn't permanently blacklist a network forever.
static const int32_t MAX_DROP_COUNT = 12;
static const int32_t MAX_FAIL_COUNT = 8;
// How many of the best-scoring in-range saved networks to try, in order,
// before giving up and staying on AP-only/offline for this boot.
static const int MAX_BOOT_ATTEMPTS = 3;

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
  
  // Load saved WiFi credentials (legacy single ssid/password, kept for the
  // rest of the class that still reads `ssid`/`password` directly) and the
  // multi-network list used by auto-connect below.
  loadCredentials();
  loadNetworks();
  
  // Set WiFi mode
  WiFi.mode(WIFI_AP_STA);
  
  // Start Access Point
  WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  apActive = true;
  Serial.println("✓ Access Point started");
  Serial.println("   SSID: " + apSSID);
  Serial.println("   Password: " + apPassword);
  Serial.println("   IP: " + WiFi.softAPIP().toString());
  
  // Auto-connect: scan what's actually in range ONCE (boot only — no
  // periodic re-scan, to keep this off both cores' backs during normal
  // operation) and try, in order, the best-scoring saved networks that are
  // actually in range (see autoConnectBest() — up to MAX_BOOT_ATTEMPTS of
  // them) before giving up and staying on AP-only/offline for this session.
  // Falls back to the legacy single-credential attempt if nothing was saved
  // yet (e.g. first boot after upgrading from the old single-network build).
  if (!networks.empty()) {
    Serial.println("\n🔄 Scanning for available saved networks...");
    autoConnectBest();
  } else if (ssid.length() > 0) {
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
  if (isConnectedToWiFi) {
    saveCredentials();
    addNetwork(newSSID, newPassword);   // remember it for future auto-connect too
    SavedNetwork* n = findNetwork(newSSID);
    if (n) { n->lastRSSI = WiFi.RSSI(); saveNetworks(); }
  }
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
  expectedDisconnect = true;   // tell monitorConnection() not to count this as a drop
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
    addNetwork(newSSID, newPassword);
    SavedNetwork* n = findNetwork(newSSID);
    if (n) { n->lastRSSI = WiFi.RSSI(); saveNetworks(); }
    return true;
  }
  
  isConnectedToWiFi = false;
  // Record the failed attempt against this network's stability score, so a
  // network that's in range but consistently fails to associate (bad
  // password saved, router rejecting it, etc.) naturally sinks in ranking
  // instead of being retried forever ahead of a working one.
  SavedNetwork* n = findNetwork(newSSID);
  if (n) {
    n->connectFails = min(n->connectFails + 1, MAX_FAIL_COUNT);
    saveNetworks();
  }
  return false;
}

void WiFiConfig::monitorConnection() {
  static bool lastConnectionState = false;
  static String lastSSID = "";
  bool currentConnectionState = (WiFi.status() == WL_CONNECTED);
  
  if (currentConnectionState != lastConnectionState) {
    lastConnectionState = currentConnectionState;
    isConnectedToWiFi = currentConnectionState;
    
    if (currentConnectionState) {
      lastSSID = WiFi.SSID();
      Serial.println("✅ WiFi Connected: " + WiFi.SSID());
      Serial.println("   IP: " + WiFi.localIP().toString());
      SavedNetwork* n = findNetwork(lastSSID);
      if (n) { n->lastRSSI = WiFi.RSSI(); saveNetworks(); }
    } else {
      Serial.println("❌ WiFi Disconnected");
      // Only counts as an instability "drop" if we were previously connected
      // and this wasn't a deliberate disconnect() call. This just feeds the
      // score used at the *next boot* — no background rescan/reconnect
      // happens here anymore, that would load both cores continuously.
      if (lastSSID.length() > 0 && !expectedDisconnect) {
        SavedNetwork* n = findNetwork(lastSSID);
        if (n) {
          n->dropCount = min(n->dropCount + 1, MAX_DROP_COUNT);
          saveNetworks();
          Serial.println("   [WiFi] Recorded a drop for " + lastSSID +
                          " (dropCount=" + String(n->dropCount) + ")");
        }
      }
      expectedDisconnect = false;
      lastSSID = "";
    }
  }
}

// ── Multi-network auto-connect ────────────────────────────────────────────

SavedNetwork* WiFiConfig::findNetwork(const String& targetSsid) {
  for (auto& n : networks) {
    if (n.ssid == targetSsid) return &n;
  }
  return nullptr;
}

void WiFiConfig::addNetwork(String newSSID, String newPassword) {
  if (newSSID.length() == 0) return;
  SavedNetwork* existing = findNetwork(newSSID);
  if (existing) {
    existing->password = newPassword;   // refresh password in case it changed
  } else {
    SavedNetwork n;
    n.ssid = newSSID;
    n.password = newPassword;
    networks.push_back(n);
  }
  saveNetworks();
}

void WiFiConfig::removeNetwork(String targetSsid) {
  for (size_t i = 0; i < networks.size(); i++) {
    if (networks[i].ssid == targetSsid) {
      networks.erase(networks.begin() + i);
      saveNetworks();
      return;
    }
  }
}

std::vector<SavedNetwork> WiFiConfig::getSavedNetworks() {
  return networks;
}

// Higher = more desirable. Starts from raw RSSI (dBm, e.g. -40 great to -90
// unusable) and subtracts penalties for a track record of dropping or
// failing to connect. This is the "full-bars-but-slow loses to
// two-bars-but-reliable" behavior: a network doesn't need to be fast to win,
// it just needs to not have a history of falling over — which in practice
// correlates with the flaky router/overloaded AP scenario, without needing
// an actual throughput test the ESP32 can't cheaply run before deciding.
int WiFiConfig::scoreNetwork(const SavedNetwork& net, int32_t rssi) {
  return (int)rssi - (int)(net.dropCount * DROP_PENALTY) - (int)(net.connectFails * FAIL_PENALTY);
}

bool WiFiConfig::autoConnectBest() {
  if (networks.empty()) return isConnectedToWiFi;

  int16_t n = WiFi.scanNetworks(false /*sync*/, false, false, 300);

  // Build a ranked list (best score first) of saved networks that are
  // actually in range right now.
  std::vector<SavedNetwork*> candidates;
  for (int16_t i = 0; i < n; i++) {
    SavedNetwork* match = findNetwork(WiFi.SSID(i));
    if (!match) continue;   // in range but we don't have credentials for it
    match->lastRSSI = WiFi.RSSI(i);
    candidates.push_back(match);
  }
  WiFi.scanDelete();
  saveNetworks();   // persist the lastRSSI updates from the loop above

  if (candidates.empty()) {
    Serial.println("[WiFi] No saved networks currently in range");
    return isConnectedToWiFi;
  }

  std::sort(candidates.begin(), candidates.end(),
            [this](SavedNetwork* a, SavedNetwork* b) {
              return scoreNetwork(*a, a->lastRSSI) > scoreNetwork(*b, b->lastRSSI);
            });

  // Try the best few in order — covers "best-scoring one happens to be
  // rejecting connections right now" (wrong/changed password, router full,
  // etc.) without retrying the same dead network 3 times.
  int attemptsTried = 0;
  for (SavedNetwork* candidate : candidates) {
    if (attemptsTried >= MAX_BOOT_ATTEMPTS) break;
    attemptsTried++;

    Serial.printf("[WiFi] Attempt %d/%d: %s (RSSI %d dBm, score %d)\n",
                  attemptsTried, MAX_BOOT_ATTEMPTS, candidate->ssid.c_str(),
                  (int)candidate->lastRSSI, scoreNetwork(*candidate, candidate->lastRSSI));

    if (WiFi.status() == WL_CONNECTED) {
      WiFi.disconnect(false, false);
      delay(300);
    }
    if (attemptConnection(candidate->ssid, candidate->password)) {
      return true;
    }
  }

  Serial.printf("[WiFi] Gave up after %d attempt(s) — staying offline/AP-only for this boot\n",
                attemptsTried);
  return false;
}

void WiFiConfig::loadNetworks() {
  networks.clear();
  preferences.begin("wifi", true);
  String json = preferences.getString("networks", "");
  preferences.end();

  if (json.length() > 0) {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, json) == DeserializationError::Ok) {
      for (JsonObject o : doc.as<JsonArray>()) {
        SavedNetwork n;
        n.ssid          = o["ssid"]   | "";
        n.password      = o["pass"]   | "";
        n.dropCount     = o["drops"]  | 0;
        n.connectFails  = o["fails"]  | 0;
        n.lastRSSI      = o["rssi"]   | 0;
        if (n.ssid.length() > 0) networks.push_back(n);
      }
    }
  }

  // Migration: first boot after upgrading from the single-network build —
  // fold the legacy ssid/password (already loaded by loadCredentials()) in
  // as the first saved network so auto-connect has something to work with
  // immediately, without the person having to re-enter it via the portal.
  if (networks.empty() && ssid.length() > 0) {
    SavedNetwork n;
    n.ssid = ssid;
    n.password = password;
    networks.push_back(n);
    saveNetworks();
    Serial.println("[WiFi] Migrated legacy saved network into multi-network list: " + ssid);
  }
}

void WiFiConfig::saveNetworks() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto& n : networks) {
    JsonObject o = arr.createNestedObject();
    o["ssid"]  = n.ssid;
    o["pass"]  = n.password;
    o["drops"] = n.dropCount;
    o["fails"] = n.connectFails;
    o["rssi"]  = n.lastRSSI;
  }
  String out;
  serializeJson(doc, out);

  preferences.begin("wifi", false);
  preferences.putString("networks", out);
  preferences.end();
}