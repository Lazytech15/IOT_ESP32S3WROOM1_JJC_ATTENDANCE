#include "WiFiConfig.h"

// Static instance pointer for callback functions
WiFiConfig* WiFiConfig::instance = nullptr;

// HTML Page as a constant
const char* wifiConfigPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>WiFi Manager</title>
  <style>
    :root {
      --primary: #4f46e5; --primary-hover: #4338ca; --bg: #f8fafc;
      --card-bg: #ffffff; --text-main: #1e293b; --text-muted: #64748b;
      --success: #22c55e; --danger: #ef4444; --warning: #f59e0b;
    }
    * { margin: 0; padding: 0; box-sizing: border-box; font-family: 'Inter', -apple-system, sans-serif; }
    
    body {
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      display: flex; justify-content: center; align-items: center;
      min-height: 100vh; padding: 1rem;
    }

    .card {
      background: var(--card-bg); width: 100%; max-width: 450px;
      height: 650px; max-height: 95vh;
      display: flex; flex-direction: column;
      border-radius: 1.5rem; box-shadow: 0 20px 25px rgba(0,0,0,0.2); overflow: hidden;
    }

    .header { background: var(--primary); padding: 1.5rem; text-align: center; color: white; flex-shrink: 0; }
    .content { padding: 1.25rem; flex-grow: 1; display: flex; flex-direction: column; overflow: hidden; position: relative; }
    
    .status-badge { padding: 0.75rem; border-radius: 0.75rem; font-size: 0.85rem; margin-bottom: 0.5rem; flex-shrink: 0; }
    .status-connected { background: #f0fdf4; border: 1px solid #bbf7d0; color: #166534; }
    .status-disconnected { background: #fff7ed; border: 1px solid #ffedd5; color: #9a3412; }

    #networks-list {
      flex-grow: 1; overflow-y: auto; margin: 0.5rem 0;
      padding-right: 5px; border-top: 1px solid #f1f5f9;
    }
    #networks-list::-webkit-scrollbar { width: 4px; }
    #networks-list::-webkit-scrollbar-thumb { background: #cbd5e1; border-radius: 10px; }

    .network-item {
      display: flex; justify-content: space-between; align-items: center;
      padding: 0.85rem; border: 1px solid #e2e8f0; border-radius: 0.75rem;
      margin-bottom: 0.5rem; cursor: pointer; transition: 0.2s;
    }
    .network-item:hover { border-color: var(--primary); background: #f5f3ff; }
    .connected-highlight { border: 2px solid var(--success) !important; background: #f0fdf4 !important; }

    /* LOADING OVERLAY */
    #loading-overlay {
      position: absolute; top: 0; left: 0; right: 0; bottom: 0;
      background: rgba(255,255,255,0.9);
      display: flex; flex-direction: column; justify-content: center; align-items: center;
      z-index: 100;
    }
    .spinner {
      width: 40px; height: 40px;
      border: 4px solid #f3f3f3; border-top: 4px solid var(--primary);
      border-radius: 50%; animation: spin 1s linear infinite;
      margin-bottom: 1rem;
    }
    @keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }

    .btn { width: 100%; padding: 0.8rem; border-radius: 0.75rem; border: none; font-weight: 600; cursor: pointer; margin-top: 0.4rem; }
    .btn-primary { background: var(--primary); color: white; }
    .btn-outline { background: transparent; border: 1px solid #e2e8f0; color: var(--text-muted); }
    .btn-sm-danger { background: transparent; color: var(--danger); font-size: 0.75rem; padding: 4px 8px; border: 1px solid var(--danger); border-radius: 4px; }
    
    .hidden { display: none !important; }
  </style>
</head>
<body>
  <div class="card">
    <div class="header"><h1>Device Settings</h1></div>
    <div class="content">
      <div id="loading-overlay" class="hidden">
        <div class="spinner"></div>
        <p id="loading-text">Connecting...</p>
      </div>

      <div id="status-container" class="status-badge status-disconnected">
        <div id="status-text">Checking...</div>
      </div>

      <div id="scan-section" style="display:flex; flex-direction:column; height:100%; overflow:hidden;">
        <div id="saved-networks-container"></div>
        <div id="networks-list"></div>
        <button class="btn btn-outline" onclick="scanNetworks()">🔄 Refresh List</button>
      </div>

      <div id="connect-section" class="hidden">
        <h3 id="selected-ssid" style="margin-bottom:1rem;"></h3>
        <input type="password" id="password-input" style="width:100%; padding:0.8rem; margin-bottom:1rem; border:1px solid #ddd; border-radius:0.5rem;" placeholder="WiFi Password">
        <button class="btn btn-primary" onclick="connectToWiFi()">Connect</button>
        <button class="btn btn-outline" onclick="showScan()">Back</button>
      </div>
    </div>
  </div>

  <script>
    let selectedSSID = '';
    let currentConnectedSSID = '';
    let lastScannedNetworks = [];

    function toggleLoading(show, text = "Connecting...") {
      const loader = document.getElementById('loading-overlay');
      document.getElementById('loading-text').innerText = text;
      loader.classList.toggle('hidden', !show);
    }

    function updateStatus() {
      fetch('/wifi-info').then(r => r.json()).then(data => {
        currentConnectedSSID = data.connected ? data.ssid : ''; 
        document.getElementById('status-text').innerHTML = data.connected ? 
          `<b>Connected:</b> ${data.ssid}` : `<b>Status:</b> Disconnected`;
        document.getElementById('status-container').className = 'status-badge ' + (data.connected ? 'status-connected' : 'status-disconnected');
        
        const savedContainer = document.getElementById('saved-networks-container');
        savedContainer.innerHTML = (data.saved_ssid && data.saved_ssid !== "") ? `
          <div style="font-size:0.7rem; font-weight:700; color:#64748b; text-transform:uppercase; margin-bottom:0.5rem;">Saved Network</div>
          <div class="saved-item">
            <div><strong>${data.saved_ssid}</strong></div>
            <button class="btn-sm-danger" onclick="deleteSaved()">Forget</button>
          </div>` : '';
        renderNetworkList();
      });
    }

    function scanNetworks() {
      toggleLoading(true, "Scanning...");
      fetch('/scan').then(r => r.json()).then(data => {
        lastScannedNetworks = data;
        renderNetworkList();
        toggleLoading(false);
      }).catch(() => toggleLoading(false));
    }

    function renderNetworkList() {
      const list = document.getElementById('networks-list');
      list.innerHTML = '<div style="font-size:0.7rem; font-weight:700; color:#64748b; text-transform:uppercase; margin:0.5rem 0;">Available Networks</div>';
      lastScannedNetworks.forEach(net => {
        const isConnected = (net.ssid === currentConnectedSSID);
        const item = document.createElement('div');
        item.className = 'network-item' + (isConnected ? ' connected-highlight' : '');
        item.onclick = () => { 
          selectedSSID = net.ssid;
          document.getElementById('selected-ssid').innerText = net.ssid; 
          document.getElementById('scan-section').classList.add('hidden');
          document.getElementById('connect-section').classList.remove('hidden'); 
        };
        item.innerHTML = `<div><b>${net.ssid}</b><br><small>${net.rssi} dBm</small></div><span>${isConnected ? '✅' : '📶'}</span>`;
        list.appendChild(item);
      });
    }

    function connectToWiFi() {
      const pass = document.getElementById('password-input').value;
      if (!pass) return alert('Enter password');
      
      toggleLoading(true, "Establishing Connection...");
      
      fetch('/connect', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: `ssid=${encodeURIComponent(selectedSSID)}&password=${encodeURIComponent(pass)}`
      }).then(r => r.json()).then(data => {
        if(data.success) {
          setTimeout(() => { toggleLoading(false); showScan(); updateStatus(); }, 2000);
        } else {
          toggleLoading(false);
          alert('Connection Failed: Check credentials.');
        }
      }).catch(() => {
        toggleLoading(false);
        alert('Network changed or lost. Check device connection.');
      });
    }

    function deleteSaved() {
      if(confirm('Forget network?')) {
        fetch('/disconnect').then(() => { updateStatus(); scanNetworks(); });
      }
    }

    function showScan() {
      document.getElementById('scan-section').classList.remove('hidden');
      document.getElementById('connect-section').classList.add('hidden');
    }

    window.onload = () => { updateStatus(); scanNetworks(); setInterval(updateStatus, 15000); };
  </script>
</body>
</html>
)rawliteral";

// Constructor
WiFiConfig::WiFiConfig(const char* apSSID, const char* apPassword) {
  this->apSSID = String(apSSID);
  this->apPassword = String(apPassword);
  this->server = new WebServer(80);
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
  Serial.println("✓ Access Point started");
  Serial.println("   SSID: " + apSSID);
  Serial.println("   Password: " + apPassword);
  Serial.println("   IP: " + WiFi.softAPIP().toString());
  
  // Try to connect to saved WiFi if available
  if (ssid.length() > 0) {
    Serial.println("\n🔄 Attempting to connect to saved network: " + ssid);
    attemptConnection(ssid, password);
  }
  
  // Setup web server routes
  setupRoutes();
  
  // Start web server
  server->begin();
  Serial.println("\n✓ Web server started!");
  Serial.println("\n📱 To configure WiFi:");
  Serial.println("   1. Connect to WiFi: " + apSSID);
  Serial.println("   2. Password: " + apPassword);
  Serial.println("   3. Open browser: http://" + WiFi.softAPIP().toString());
  Serial.println("\n⚠️  NOTE: On mobile devices, you may need to:");
  Serial.println("    - Disable mobile data for this connection");
  Serial.println("    - Select 'Stay connected' if prompted\n");
}

// Handle web server requests (call this in loop)
void WiFiConfig::handleClient() {
  server->handleClient();
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

// Setup web server routes
void WiFiConfig::setupRoutes() {
  server->on("/", staticHandleRoot);
  server->on("/wifi-info", staticHandleWiFiInfo);
  server->on("/scan", staticHandleScan);
  server->on("/connect", HTTP_POST, staticHandleConnect);
  server->on("/disconnect", staticHandleDisconnect);
  server->onNotFound(staticHandleNotFound);
}

// Web server handlers
void WiFiConfig::handleRoot() {
  server->send(200, "text/html", wifiConfigPage);
}

void WiFiConfig::handleWiFiInfo() {
  // Load current saved SSID from Preferences
  preferences.begin("wifi", true);
  String savedSSID = preferences.getString("ssid", "");
  preferences.end();

  String json = "{";
  json += "\"connected\":" + String(isConnectedToWiFi ? "true" : "false");
  json += ",\"ssid\":\"" + ssid + "\""; // Current active SSID
  json += ",\"saved_ssid\":\"" + savedSSID + "\""; // Stored in Flash
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "}";
  server->send(200, "application/json", json);
}

void WiFiConfig::handleScan() {
  Serial.println("\n🔍 Starting WiFi network scan...");
  
  int n = WiFi.scanNetworks();
  
  Serial.println("Scan complete. Found " + String(n) + " networks:");
  
  String json = "[";
  
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    
    String networkSSID = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    
    json += "{";
    json += "\"ssid\":\"" + networkSSID + "\"";
    json += ",\"rssi\":" + String(rssi);
    json += "}";
    
    Serial.print("  ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(networkSSID);
    Serial.print(" (");
    Serial.print(rssi);
    Serial.println(" dBm)");
  }
  
  json += "]";
  
  Serial.println("✓ Sending " + String(n) + " networks to client\n");
  
  server->send(200, "application/json", json);
}

void WiFiConfig::handleConnect() {
  if (server->hasArg("ssid") && server->hasArg("password")) {
    String newSSID = server->arg("ssid");
    String newPassword = server->arg("password");
    
    Serial.println("\n🔄 Attempting to connect to: " + newSSID);
    
    if (attemptConnection(newSSID, newPassword)) {
      String json = "{\"success\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}";
      server->send(200, "application/json", json);
    } else {
      Serial.println("❌ Connection failed!");
      server->send(200, "application/json", "{\"success\":false,\"message\":\"Connection failed\"}");
    }
  } else {
    server->send(400, "application/json", "{\"success\":false,\"message\":\"Missing parameters\"}");
  }
}

void WiFiConfig::handleDisconnect() {
  disconnect();
  server->send(200, "application/json", "{\"success\":true}");
}

void WiFiConfig::handleNotFound() {
  String uri = server->uri();
  
  // Common captive portal detection URLs - return success to prevent spam
  if (uri == "/generate_204" ||           // Android
      uri == "/gen_204" ||                // Android
      uri == "/hotspot-detect.html" ||    // iOS/macOS
      uri == "/library/test/success.html" || // iOS
      uri == "/success.txt" ||            // Firefox
      uri == "/chrome-variations/seed" || // Chrome
      uri == "/connecttest.txt" ||        // Windows
      uri == "/redirect" ||               // Windows
      uri == "/canonical.html" ||         // Ubuntu
      uri == "/chat") {                   // Random requests
    // Return 204 No Content to stop captive portal checks
    server->send(204, "text/plain", "");
    return;
  }
  
  // For actual 404 errors, log and return error
  Serial.println("❌ 404 Error - " + uri);
  server->send(404, "text/plain", "Not Found");
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

// Static wrapper functions
void WiFiConfig::staticHandleRoot() {
  if (instance) instance->handleRoot();
}

void WiFiConfig::staticHandleWiFiInfo() {
  if (instance) instance->handleWiFiInfo();
}

void WiFiConfig::staticHandleScan() {
  if (instance) instance->handleScan();
}

void WiFiConfig::staticHandleConnect() {
  if (instance) instance->handleConnect();
}

void WiFiConfig::staticHandleDisconnect() {
  if (instance) instance->handleDisconnect();
}

void WiFiConfig::staticHandleNotFound() {
  if (instance) instance->handleNotFound();
}