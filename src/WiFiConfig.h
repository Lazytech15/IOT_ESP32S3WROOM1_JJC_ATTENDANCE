#ifndef WIFICONFIG_H
#define WIFICONFIG_H

#include <FS.h>
using namespace fs;   // expose fs::FS as bare 'FS' before WebServer.h needs it
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <vector>

// One remembered network. `dropCount` is a rolling "how often did this
// network drop / fail to connect" counter — it's the stand-in we use for
// "connection quality" since the ESP32 has no cheap way to measure real
// throughput per-network. A high-signal network that keeps dropping will
// still lose to a weaker-but-solid one once enough drops accumulate — see
// WiFiConfig::scoreNetwork() in the .cpp.
struct SavedNetwork {
  String ssid;
  String password;
  int32_t dropCount   = 0;   // unexpected disconnects recorded while this SSID was active
  int32_t connectFails = 0;  // times an attemptConnection() to this SSID timed out
  int32_t lastRSSI     = 0;  // most recent RSSI seen for this SSID (dBm, e.g. -55)
};

class WiFiConfig {
public:
  // Constructor — pass AP credentials explicitly (defined in main.cpp via AP_SSID / AP_PASSWORD)
  WiFiConfig(const char* apSSID, const char* apPassword);
  
  // Initialize the WiFi configuration system
  void begin();
  
  // Handle web server requests (call this in loop)
  void handleClient();
  
  // Check if connected to a WiFi network
  bool isConnected();
  
  // Get current SSID
  String getSSID();
  
  // Get current IP address
  String getIPAddress();
  
  // Get AP IP address
  String getAPIPAddress();
  
  // Manually connect to WiFi
  bool connectToWiFi(String ssid, String password);
  
  // Disconnect from WiFi
  void disconnect();

  // Record/persist credentials for a connection that's already established
  // (WiFi.status()==WL_CONNECTED) elsewhere — e.g. WiFiManager's portal
  // /api/wifi/connect handler, which does its own WiFi.begin()+wait. Skips
  // the redundant second WiFi.begin()/wait that connectToWiFi() would do.
  void adoptCurrentConnection(String ssid, String password);

  // Turn the device's own setup hotspot (softAP) on/off without touching
  // the STA connection. Used to keep the hotspot off outside admin hours.
  void startAP();
  void stopAP();
  bool isAPActive();

  // ── Multi-network auto-connect ──────────────────────────────────────────

  // Remember a network (adds it if new, updates the password if it already
  // exists). Does NOT connect — just persists it to the saved list.
  void addNetwork(String ssid, String password);

  // Forget a specific saved network (does nothing if it isn't saved).
  void removeNetwork(String ssid);

  // All currently-remembered networks, most-recently-used/best-scoring first.
  std::vector<SavedNetwork> getSavedNetworks();

  // Scan for nearby networks, match them against the saved list, and
  // connect to whichever match scores best (see scoreNetwork() — this is
  // signal strength *and* recorded stability, not just "loudest bar count").
  // Tries up to a few of the best-scoring candidates (see MAX_BOOT_ATTEMPTS
  // in the .cpp) before giving up and leaving the device on AP-only/offline.
  // Meant to be called once, at boot — it does a blocking scan + connect
  // sequence, so it's deliberately NOT re-run periodically from loop() (that
  // would load both cores for no real benefit outside a fresh boot).
  // Returns true if it ended up connected to something.
  bool autoConnectBest();

private:
  WebServer* server;  // unused (legacy port-80 UI removed); kept null, header stays for ABI stability
  Preferences preferences;
  
  String ssid;
  String password;
  String apSSID;
  String apPassword;
  bool isConnectedToWiFi;
  bool apActive = false;

  std::vector<SavedNetwork> networks;
  bool expectedDisconnect = false;   // true while disconnect() is intentional — skip drop-counting it

  // Helper functions
  void loadCredentials();
  void saveCredentials();
  void clearCredentials();
  bool attemptConnection(String ssid, String password);
  void monitorConnection();

  // Multi-network helpers
  void loadNetworks();
  void saveNetworks();
  SavedNetwork* findNetwork(const String& ssid);          // nullptr if not saved
  int scoreNetwork(const SavedNetwork& net, int32_t rssi); // higher = better pick
  
  static WiFiConfig* instance;
};

#endif