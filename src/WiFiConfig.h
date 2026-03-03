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

private:
  WebServer* server;
  Preferences preferences;
  
  String ssid;
  String password;
  String apSSID;
  String apPassword;
  bool isConnectedToWiFi;
  
  // Web server handlers
  void setupRoutes();
  void handleRoot();
  void handleWiFiInfo();
  void handleScan();
  void handleConnect();
  void handleDisconnect();
  void handleNotFound();
  
  // Helper functions
  void loadCredentials();
  void saveCredentials();
  void clearCredentials();
  bool attemptConnection(String ssid, String password);
  void monitorConnection();
  
  // Static wrapper functions for web server callbacks
  static WiFiConfig* instance;
  static void staticHandleRoot();
  static void staticHandleWiFiInfo();
  static void staticHandleScan();
  static void staticHandleConnect();
  static void staticHandleDisconnect();
  static void staticHandleNotFound();
};

#endif