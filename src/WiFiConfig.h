#ifndef WIFICONFIG_H
#define WIFICONFIG_H

#include <FS.h>
using namespace fs;   // expose fs::FS as bare 'FS' before WebServer.h needs it
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

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

private:
  WebServer* server;  // unused (legacy port-80 UI removed); kept null, header stays for ABI stability
  Preferences preferences;
  
  String ssid;
  String password;
  String apSSID;
  String apPassword;
  bool isConnectedToWiFi;
  bool apActive = false;
  
  // Helper functions
  void loadCredentials();
  void saveCredentials();
  void clearCredentials();
  bool attemptConnection(String ssid, String password);
  void monitorConnection();
  
  static WiFiConfig* instance;
};

#endif