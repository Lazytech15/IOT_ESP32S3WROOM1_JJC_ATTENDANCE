// ════════════════════════════════════════════════════════════════════════════
// WiFiManager.h  — Web Admin Portal for ESP32-S3 NFC Attendance
// 192.168.4.1:8080 in AP mode; local IP in WiFi mode
// Tabs (in order):
//   1. Dashboard  — today's stats + quick actions + LIVE SSE feed
//   2. SD Files   — raw SD card browser: list ALL files, view/download any file
//                 + NEW: Rename, Move, Copy, Delete
//   3. Attendance — today's log table + CSV downloads 
//                 + NEW: Raw Log Editor (Fix clock in/out times)
//   4. Actions    — reboot, clear cache, manage employees
//   5. WiFi       — moved LAST; background scan runs silently
//
// SSE Live Push:
//   - /api/events  — SSE stream (text/event-stream, keep-alive)
//   - broadcastEvent("stats", json)  — call when attendance counters change
//   - broadcastEvent("scan",  json)  — call on every NFC tap with name/type/time
//   - broadcastEvent("ping")         — call every ~20s to keep connection alive
//
// Login: admin / jjcadmin
// Port:  8080
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include "WiFiConfig.h"
#include "sd_database.h"
#include "sd_file_manager.h" // Ensure file manager tools are included
#include "attendance_http_service.h"

// Some cores don't expose WIFI_SCAN_RUNNING; ensure we have a fallback
#ifndef WIFI_SCAN_RUNNING
#define WIFI_SCAN_RUNNING (-1)
#endif

// ── Auth ──────────────────────────────────────────────────────────────────────
#define WM_USER     "admin"
#define WM_PASS     "jjcadmin"
#define WM_SESS     "jjc_ok"
#define WM_PORT     8080

// ── Background WiFi scan state ────────────────────────────────────────────────
static int16_t    _scanResult    = WIFI_SCAN_RUNNING;
static bool       _scanRunning   = false;
static unsigned long _scanStart  = 0;

// ── SSE (Server-Sent Events) live push ───────────────────────────────────────
// Tracks the one active SSE client. ESP32 WebServer handles one stream at a time.
static WiFiClient _sseClient;
static bool       _sseConnected = false;

// ════════════════════════════════════════════════════════════════════════════
class WiFiManager {
public:
    WiFiManager() : _srv(WM_PORT) {}

    void init(const String& deviceId, WiFiConfig* cfg,
              const String& serverURL = "",
              AttendanceHTTPService* attSvc = nullptr) {
        _deviceId  = deviceId;
        _cfg       = cfg;
        _serverURL = serverURL;
        _attSvc    = attSvc;
        _setupRoutes();
        // Must register Cookie header BEFORE begin() so _authed() works.
        static const char* _hdrs[] = {"Cookie"};
        _srv.collectHeaders(_hdrs, 1);
        _srv.begin();
        Serial.printf("[WM] Portal at http://x.x.x.x:%d\n", WM_PORT);
        _startScan();
    }

    void handleClient() {
        _srv.handleClient();
        // Poll async scan result
        if (_scanRunning) {
            int16_t n = WiFi.scanComplete();
            if (n != WIFI_SCAN_RUNNING) {
                _scanResult  = n;
                _scanRunning = false;
                Serial.printf("[WiFi] Scan done: %d networks\n", n);
            }
        }
    }

    // ── Live Push: call this from main.cpp whenever attendance or stats change ──
    // Sends an SSE event to the browser so the dashboard updates without refresh.
    // eventType: "scan"  → new NFC scan recorded
    //            "stats" → attendance counters changed
    //            "ping"  → keepalive (call every ~20s to keep connection alive)
    void broadcastEvent(const String& eventType, const String& jsonPayload = "{}") {
        if (!_sseConnected || !_sseClient.connected()) {
            _sseConnected = false;
            return;
        }
        // SSE frame: write directly to avoid chunked buffering
        _sseClient.print("event: ");
        _sseClient.print(eventType);
        _sseClient.print("\ndata: ");
        _sseClient.print(jsonPayload);
        _sseClient.print("\n\n");
        Serial.printf("[SSE] >> %s\n", eventType.c_str());
    }

private:
    WebServer  _srv;
    String     _deviceId;
    String     _serverURL;
    WiFiConfig* _cfg = nullptr;
    AttendanceHTTPService* _attSvc = nullptr;

    // ── Start async background scan ───────────────────────────────────────────
    void _startScan() {
        if (_scanRunning) return;
        WiFi.scanNetworks(true /*async*/);
        _scanRunning = true;
        _scanResult  = WIFI_SCAN_RUNNING;
        _scanStart   = millis();
        Serial.println("[WiFi] Background scan started");
    }

    bool _authed() {
        if (_srv.hasHeader("Cookie")) {
            String c = _srv.header("Cookie");
            if (c.indexOf(WM_SESS) >= 0) return true;
        }
        return false;
    }

    String _ip() {
        if (_cfg && _cfg->isConnected()) return _cfg->getIPAddress();
        return WiFi.softAPIP().toString();
    }

    // ── Shared HTML head + nav ─────────────────────────────────────────────────
    String _head(const String& title, int activeTab) {
        String h = R"(<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>)" + title + R"(</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0a0e1a;--card:#111827;--border:#1e2d3d;--text:#e2e8f0;--dim:#64748b;
      --teal:#0d9488;--cyan:#22d3ee;--orange:#f97316;--red:#ef4444;--green:#10b981;
      --accent:linear-gradient(135deg,#ea580c,#f97316,#fbbf24)}
body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh}
body::before{content:'';position:fixed;inset:0;
  background:radial-gradient(ellipse 80% 50% at 20% 20%,rgba(13,148,136,.06) 0%,transparent 60%),
             radial-gradient(ellipse 60% 40% at 80% 80%,rgba(249,115,22,.05) 0%,transparent 60%);
  pointer-events:none;z-index:0}
.wrap{position:relative;z-index:1;max-width:960px;margin:0 auto;padding:12px}
.topbar{display:flex;align-items:center;justify-content:space-between;
  background:var(--card);border-radius:10px;padding:10px 16px;
  border:1px solid var(--border);margin-bottom:12px}
.topbar-title{font-size:1.1rem;font-weight:700;background:var(--accent);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent}
.ip-pill{font-size:.75rem;background:rgba(13,148,136,.15);color:var(--cyan);
  border:1px solid rgba(13,148,136,.3);padding:3px 10px;border-radius:20px}
.tabs{display:flex;gap:4px;margin-bottom:14px;flex-wrap:wrap}
.tab{padding:7px 14px;border-radius:8px;text-decoration:none;font-size:.82rem;
  font-weight:600;color:var(--dim);border:1px solid transparent;cursor:pointer;transition:.2s}
.tab:hover{color:var(--text);background:rgba(255,255,255,.05)}
.tab.active{color:#0a0e1a;background:var(--accent);border-color:transparent}
.tab.active{background:linear-gradient(135deg,#ea580c,#f97316)}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;
  padding:16px;margin-bottom:14px}
.card-title{font-size:.85rem;font-weight:700;color:var(--dim);
  text-transform:uppercase;letter-spacing:.05em;margin-bottom:12px}
.stat-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px;margin-bottom:14px}
.stat{background:rgba(255,255,255,.03);border:1px solid var(--border);
  border-radius:8px;padding:12px;text-align:center}
.stat-n{font-size:2rem;font-weight:800;color:var(--cyan)}
.stat-l{font-size:.7rem;color:var(--dim);text-transform:uppercase;margin-top:2px}
table{width:100%;border-collapse:collapse;font-size:.82rem}
th{background:rgba(255,255,255,.04);color:var(--dim);font-weight:600;
  padding:8px 10px;text-align:left;border-bottom:1px solid var(--border)}
td{padding:7px 10px;border-bottom:1px solid rgba(255,255,255,.04);color:var(--text)}
tr:hover td{background:rgba(255,255,255,.02)}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:.72rem;font-weight:600}
.badge-in {background:rgba(34,211,238,.15);color:var(--cyan)}
.badge-out{background:rgba(249,115,22,.15);color:var(--orange)}
.badge-denied{background:rgba(239,68,68,.15);color:var(--red)}
.btn{display:inline-block;padding:8px 18px;border-radius:7px;font-weight:600;
  font-size:.82rem;cursor:pointer;border:none;transition:.2s;text-decoration:none}
.btn-primary{background:linear-gradient(135deg,#ea580c,#f97316);color:#0a0e1a}
.btn-primary:hover{background:linear-gradient(135deg,#f97316,#fbbf24)}
.btn-danger{background:rgba(239,68,68,.2);color:var(--red);border:1px solid rgba(239,68,68,.3)}
.btn-ghost{background:rgba(255,255,255,.06);color:var(--text);border:1px solid var(--border)}
.btn-ghost:hover{background:rgba(255,255,255,.1)}
.btn-sm{padding:4px 10px;font-size:.75rem}
input,select{background:rgba(255,255,255,.06);border:1px solid var(--border);
  border-radius:6px;color:var(--text);padding:8px 12px;font-size:.85rem;width:100%}
input:focus,select:focus{outline:none;border-color:var(--teal)}
.form-row{margin-bottom:10px}
label{font-size:.8rem;color:var(--dim);display:block;margin-bottom:4px}
.tag-wifi{display:flex;align-items:center;gap:8px;padding:8px 10px;
  background:rgba(255,255,255,.03);border:1px solid var(--border);
  border-radius:7px;margin-bottom:6px;cursor:pointer;transition:.15s}
.tag-wifi:hover{background:rgba(255,255,255,.07)}
.tag-wifi .ssid{font-weight:600;font-size:.85rem;flex:1}
.tag-wifi .rssi{font-size:.75rem;color:var(--dim)}
.dot{width:8px;height:8px;border-radius:50%;flex-shrink:0}
.alert{padding:10px 14px;border-radius:7px;font-size:.83rem;margin-top:10px;display:none}
.alert-ok{background:rgba(16,185,129,.1);color:var(--green);border:1px solid rgba(16,185,129,.2)}
.alert-err{background:rgba(239,68,68,.1);color:var(--red);border:1px solid rgba(239,68,68,.2)}
.file-row{display:flex;align-items:center;gap:8px;padding:6px 10px;
  background:rgba(255,255,255,.02);border:1px solid var(--border);
  border-radius:6px;margin-bottom:5px;flex-wrap:wrap;}
.file-row .fn{font-size:.82rem;flex:1;font-family:monospace;color:var(--cyan);word-break:break-all}
.file-row .fsz{font-size:.72rem;color:var(--dim);white-space:nowrap;margin-right:10px;}
.file-content{background:#000;border:1px solid var(--border);border-radius:6px;
  padding:12px;font-family:monospace;font-size:.78rem;color:#a3e635;
  white-space:pre-wrap;word-break:break-all;max-height:400px;overflow-y:auto;margin-top:10px}
.dir-row{background:rgba(13,148,136,.07);border-color:rgba(13,148,136,.2)}
.dir-row .fn{color:var(--teal)}
</style></head><body><div class="wrap">
<div class="topbar">
  <span class="topbar-title">&#9670; JJC Attendance</span>
  <span class="ip-pill">)" + _ip() + ":" + String(WM_PORT) + R"(</span>
  <a href="/logout" class="btn btn-ghost" style="font-size:.75rem;padding:4px 10px">Logout</a>
</div>
<div class="tabs">
  <a href="/portal"       class="tab )" + (activeTab==0?"active":"") + R"(">Dashboard</a>
  <a href="/portal/files" class="tab )" + (activeTab==1?"active":"") + R"(">SD Files</a>
  <a href="/portal/attendance" class="tab )" + (activeTab==2?"active":"") + R"(">Attendance</a>
  <a href="/portal/actions" class="tab )" + (activeTab==3?"active":"") + R"(">Actions</a>
  <a href="/portal/wifi"  class="tab )" + (activeTab==4?"active":"") + R"(">WiFi</a>
</div>
)";
        return h;
    }

    static String _foot() {
        return "</div><script>"
            "function req(u,m,b,cb){"
            "  fetch(u,{method:m||'GET',headers:{'Content-Type':'application/json'},body:b})"
            "  .then(r=>r.json()).then(cb).catch(e=>console.error(e))}"
            "</script></body></html>";
    }

    // ════════════════════════════════════════════════════════════════════════
    void _setupRoutes() {
        // Static probes
        _srv.on("/generate_204",        [this](){ _srv.send(204,"",""); });
        _srv.on("/hotspot-detect.html", [this](){ _srv.send(204,"",""); });
        _srv.on("/ncsi.txt",            [this](){ _srv.send(200,"text/plain","Microsoft NCSI"); });

        // Auth
        _srv.on("/",       HTTP_GET,  [this](){ _srv.sendHeader("Location","/portal"); _srv.send(302); });
        _srv.on("/login",  HTTP_GET,  [this](){ _handleLoginPage(); });
        _srv.on("/login",  HTTP_POST, [this](){ _handleLoginPost(); });
        _srv.on("/logout", HTTP_GET,  [this](){
            _srv.sendHeader("Set-Cookie","jjcsess=;Max-Age=0;Path=/");
            _srv.sendHeader("Location","/login"); _srv.send(302); });

        // Portal tabs
        _srv.on("/portal",            HTTP_GET, [this](){ if(!_authed()){_redir();}else _handleDash(); });
        _srv.on("/portal/files",      HTTP_GET, [this](){ if(!_authed()){_redir();}else _handleFiles(); });
        _srv.on("/portal/attendance", HTTP_GET, [this](){ if(!_authed()){_redir();}else _handleAttendance(); });
        _srv.on("/portal/actions",    HTTP_GET, [this](){ if(!_authed()){_redir();}else _handleActions(); });
        _srv.on("/portal/wifi",       HTTP_GET, [this](){ if(!_authed()){_redir();}else _handleWifi(); });

        // SSE live stream
        _srv.on("/api/events", HTTP_GET, [this](){ if(!_authed()){_srv.send(401);}else _handleSSE(); });

        // API
        _srv.on("/api/status",     HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiStatus(); });
        
        // File APIs
        _srv.on("/api/sd/list",    HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiSdList(); });
        _srv.on("/api/sd/read",    HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiSdRead(); });
        _srv.on("/api/sd/dl",      HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiSdDownload(); });
        _srv.on("/api/sd/tree",    HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiSdTree(); });
        
        // NEW Edit/Delete APIs
        _srv.on("/api/sd/delete",  HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiSdDelete(); });
        _srv.on("/api/sd/rename",  HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiSdRename(); });
        _srv.on("/api/sd/copy",    HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiSdCopy(); });
        _srv.on("/api/sd/write",   HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiSdWrite(); });

        _srv.on("/api/attendance",              HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiAttendance(); });
        _srv.on("/api/attendance/dates",        HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiAttendanceDates(); });
        _srv.on("/api/attendance/update",       HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiAttendanceUpdate(); });
        _srv.on("/api/attendance/deleterow",    HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiAttendanceDeleteRow(); });
        _srv.on("/api/wifi/scan",  HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiScan(); });
        _srv.on("/api/wifi/info",  HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiWifiInfo(); });
        _srv.on("/api/wifi/connect",    HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiConnect(); });
        _srv.on("/api/wifi/disconnect", HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiDisconnect(); });
        _srv.on("/api/reboot",     HTTP_POST, [this](){ if(!_authed()){_srv.send(401);}else _apiReboot(); });
    }

    void _redir() { _srv.sendHeader("Location","/login"); _srv.send(302); }

    // ════════════════════════════════════════════════════════════════════════
    // LOGIN
    // ════════════════════════════════════════════════════════════════════════
    void _handleLoginPage() {
        String html = R"(<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>JJC Login</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0e1a;color:#e2e8f0;font-family:'Segoe UI',system-ui,sans-serif;
  min-height:100vh;display:flex;align-items:center;justify-content:center;
  background-image:radial-gradient(ellipse at 20% 30%,rgba(13,148,136,.08),transparent 60%),
  radial-gradient(ellipse at 80% 70%,rgba(249,115,22,.06),transparent 60%)}
.card{background:#111827;border:1px solid #1e2d3d;border-radius:14px;
  padding:32px 28px;width:340px;max-width:94vw}
.logo{text-align:center;margin-bottom:24px}
.logo h2{font-size:1.3rem;font-weight:800;background:linear-gradient(135deg,#ea580c,#f97316,#fbbf24);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent}
.logo p{font-size:.78rem;color:#64748b;margin-top:4px}
.ip{display:inline-block;margin:0 auto 20px;background:rgba(13,148,136,.12);
  color:#22d3ee;border:1px solid rgba(13,148,136,.25);border-radius:20px;
  padding:4px 14px;font-size:.78rem;display:block;text-align:center}
label{font-size:.8rem;color:#64748b;display:block;margin-bottom:4px}
input{width:100%;background:rgba(255,255,255,.06);border:1px solid #1e2d3d;
  border-radius:6px;color:#e2e8f0;padding:10px 12px;font-size:.9rem;margin-bottom:14px}
input:focus{outline:none;border-color:#0d9488}
button{width:100%;padding:11px;border-radius:7px;border:none;
  background:linear-gradient(135deg,#ea580c,#f97316);color:#0a0e1a;
  font-weight:700;font-size:.9rem;cursor:pointer;transition:.2s}
button:hover{background:linear-gradient(135deg,#f97316,#fbbf24)}
</style></head><body><div class="card">
<div class="logo"><h2>&#9670; JJC Attendance</h2><p>Admin Portal</p></div>
<div class="ip">)" + _ip() + ":" + String(WM_PORT) + R"(</div>
<form method="POST" action="/login">
<label>Username</label><input name="u" type="text" autocomplete="username" required>
<label>Password</label><input name="p" type="password" autocomplete="current-password" required>
<button type="submit">Sign In</button>
</form>
</div></body></html>)";
        _srv.send(200, "text/html", html);
    }

    void _handleLoginPost() {
        String u = _srv.arg("u"), p = _srv.arg("p");
        if (u == WM_USER && p == WM_PASS) {
            _srv.sendHeader("Set-Cookie","jjcsess=" WM_SESS ";Path=/");
            _srv.sendHeader("Location","/portal"); _srv.send(302);
        } else {
            _srv.sendHeader("Location","/login?err=1"); _srv.send(302);
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // TAB 0 — DASHBOARD
    // ════════════════════════════════════════════════════════════════════════
    void _handleDash() {
        int ins  = max(0, SDDatabase::countTodayCheckIns());
        int outs = max(0, SDDatabase::countTodayCheckOuts());
        uint64_t freeMB = SDDatabase::freeBytes() / 1048576;
        bool wOk = _cfg && _cfg->isConnected();

        String html = _head("Dashboard", 0);
        // IDs targeted by SSE listener for live updates
        html += "<div class='stat-row'>";
        html += "<div class='stat'><div class='stat-n' style='color:#10b981' id='live-ins'>" + String(ins) + "</div><div class='stat-l'>Clock-ins today</div></div>";
        html += "<div class='stat'><div class='stat-n' style='color:#f97316' id='live-outs'>" + String(outs) + "</div><div class='stat-l'>Clock-outs today</div></div>";
        html += "<div class='stat'><div class='stat-n' style='color:#22d3ee' id='live-mb'>" + String(freeMB) + "<span style='font-size:1rem'>MB</span></div><div class='stat-l'>SD Free</div></div>";
        html += "<div class='stat'><div class='stat-n' style='color:" + String(wOk?"#10b981":"#ef4444") + ";font-size:1rem' id='live-wifi'>" + (wOk ? "Online" : "Offline") + "</div><div class='stat-l'>WiFi</div></div>";
        html += "</div>";
        // Live scan feed
        html += "<div class='card'><div class='card-title' style='display:flex;align-items:center;gap:8px'>";
        html += "<span id='live-dot' style='display:inline-block;width:8px;height:8px;border-radius:50%;background:#64748b'></span>";
        html += "LIVE FEED";
        html += "<span id='live-status' style='font-size:.72rem;color:#64748b;margin-left:auto'>Connecting...</span></div>";
        html += "<div id='live-log' style='font-family:monospace;font-size:.8rem;color:#94a3b8;min-height:40px'></div></div>";
        html += R"SSE(<script>
(function(){
  var es=new EventSource('/api/events');
  var dot=document.getElementById('live-dot');
  var st=document.getElementById('live-status');
  var log=document.getElementById('live-log');
  var lines=[];
  es.onopen=function(){dot.style.background='#10b981';st.textContent='Live';};
  es.onerror=function(){dot.style.background='#ef4444';st.textContent='Reconnecting...';};
  es.addEventListener('stats',function(e){
    try{
      var d=JSON.parse(e.data);
      if(d.ins!==undefined)document.getElementById('live-ins').textContent=d.ins;
      if(d.outs!==undefined)document.getElementById('live-outs').textContent=d.outs;
      if(d.free_mb!==undefined)document.getElementById('live-mb').innerHTML=d.free_mb+'<span style="font-size:1rem">MB</span>';
      if(d.wifi!==undefined){var w=document.getElementById('live-wifi');w.textContent=d.wifi?'Online':'Offline';w.style.color=d.wifi?'#10b981':'#ef4444';}
    }catch(err){}
  });
  es.addEventListener('scan',function(e){
    try{
      var d=JSON.parse(e.data);
      var col=d.type==='check-in'?'#22d3ee':d.type==='check-out'?'#f97316':'#ef4444';
      lines.unshift('<div style="color:'+col+';margin-bottom:4px">['+( d.time||'--:--')+'] <strong>'+(d.name||'?')+'</strong> &mdash; '+d.type.toUpperCase()+'</div>');
      if(lines.length>8)lines.length=8;
      log.innerHTML=lines.join('');
    }catch(err){}
  });
  es.addEventListener('ping',function(){
    dot.style.background='#10b981';
    st.textContent='Live \u2022 '+new Date().toLocaleTimeString();
  });
})();
</script>)SSE";

        if (wOk) {
            html += "<div class='card'><div class='card-title'>Network</div>";
            html += "<p style='font-size:.85rem;color:#94a3b8'>Connected to: <strong style='color:#22d3ee'>" + _cfg->getSSID() + "</strong></p>";
            html += "<p style='font-size:.85rem;color:#94a3b8;margin-top:6px'>IP: <strong>" + _cfg->getIPAddress() + "</strong></p>";
            html += "</div>";
        }
        html += _foot();
        _srv.send(200, "text/html", html);
    }

    // ════════════════════════════════════════════════════════════════════════
    // TAB 1 — SD FILES (NEW: Move/Copy/Rename/Delete)
    // ════════════════════════════════════════════════════════════════════════
    void _handleFiles() {
        String html = _head("SD Files", 1);
        html += R"HTML(
<div class="card">
  <div class="card-title">SD Card Browser</div>
  <div style="display:flex;gap:8px;align-items:center;margin-bottom:12px;flex-wrap:wrap">
    <input id="pathInput" value="/" style="flex:1;min-width:180px" placeholder="Path (e.g. /attendance)">
    <button class="btn btn-primary btn-sm" onclick="browseDir()">Browse</button>
    <button class="btn btn-ghost btn-sm" onclick="document.getElementById('pathInput').value='/';browseDir()">Root</button>
  </div>
  <div id="filelist"></div>
  <div id="fileview" style="display:none">
    <div style="display:flex;justify-content:space-between;align-items:center;margin-top:14px">
      <span id="viewTitle" style="font-size:.8rem;font-family:monospace;color:#22d3ee"></span>
      <div style="display:flex;gap:6px">
        <button class="btn btn-ghost btn-sm" onclick="closeView()">Close</button>
        <a id="dlBtn" class="btn btn-primary btn-sm" download>Download</a>
      </div>
    </div>
    <div id="viewContent" class="file-content"></div>
  </div>
</div>
<script>
function browseDir(){
  var p=document.getElementById('pathInput').value||'/';
  fetch('/api/sd/tree?path='+encodeURIComponent(p)).then(r=>r.json()).then(renderTree);
}
function renderTree(data){
  if(!data.success){
    document.getElementById('filelist').innerHTML='<p style="color:#ef4444;font-size:.82rem">'+data.error+'</p>';
    return;
  }
  var h='';
  var items=data.items||[];
  if(items.length===0){h='<p style="color:#64748b;font-size:.82rem">Empty directory</p>';}
  items.forEach(function(item){
    var isDir=item.type==='dir';
    var icon=isDir?'&#128193;':'&#128196;';
    var sz=isDir?'DIR':fmtSize(item.size);
    h+='<div class="file-row '+(isDir?'dir-row':'')+'">';
    h+='<span style="font-size:1rem">'+icon+'</span>';
    h+='<span class="fn">'+item.path+'</span>';
    h+='<span class="fsz">'+sz+'</span>';
    h+='<div style="display:flex;gap:4px;flex-wrap:wrap;">';
    if(isDir){
      h+='<button class="btn btn-ghost btn-sm" onclick="navDir(\''+item.path+'\')">Open</button>';
      h+='<button class="btn btn-danger btn-sm" onclick="delFile(\''+item.path+'\')">Del</button>';
    } else {
      h+='<button class="btn btn-ghost btn-sm" onclick="viewFile(\''+item.path+'\')">View</button>';
      h+='<button class="btn btn-ghost btn-sm" onclick="renFile(\''+item.path+'\')">Ren/Mv</button>';
      h+='<button class="btn btn-ghost btn-sm" onclick="cpFile(\''+item.path+'\')">Cp</button>';
      h+='<button class="btn btn-danger btn-sm" onclick="delFile(\''+item.path+'\')">Del</button>';
    }
    h+='</div></div>';
  });
  document.getElementById('filelist').innerHTML=h;
}

// Actions
function delFile(p){
  if(confirm('Delete '+p+'? This cannot be undone.')){
    req('/api/sd/delete','POST',JSON.stringify({path:p}),(d)=>{
      if(d.success) browseDir(); else alert('Delete failed');
    });
  }
}
function renFile(p){
  var t=prompt('Rename or Move to path:', p);
  if(t && t!==p){
    req('/api/sd/rename','POST',JSON.stringify({from:p,to:t}),(d)=>{
      if(d.success) browseDir(); else alert('Rename failed');
    });
  }
}
function cpFile(p){
  var t=prompt('Copy to path:', p+'_copy');
  if(t && t!==p){
    req('/api/sd/copy','POST',JSON.stringify({from:p,to:t}),(d)=>{
      if(d.success) browseDir(); else alert('Copy failed');
    });
  }
}

// Navigation
function navDir(path){ document.getElementById('pathInput').value=path; browseDir(); }
function fmtSize(b){
  if(b===undefined)return'?';
  if(b<1024)return b+'B';
  if(b<1048576)return(b/1024).toFixed(1)+'KB';
  return(b/1048576).toFixed(2)+'MB';
}
function viewFile(path){
  document.getElementById('viewContent').textContent='Loading...';
  document.getElementById('viewTitle').textContent=path;
  document.getElementById('dlBtn').href='/api/sd/dl?f='+encodeURIComponent(path);
  document.getElementById('dlBtn').download=path.split('/').pop();
  document.getElementById('fileview').style.display='block';
  fetch('/api/sd/read?f='+encodeURIComponent(path))
  .then(r=>r.text()).then(function(t){
    document.getElementById('viewContent').textContent=t;
  });
}
function closeView(){document.getElementById('fileview').style.display='none';}
browseDir();
</script>
)HTML";
        html += _foot();
        _srv.send(200, "text/html", html);
    }

    // ════════════════════════════════════════════════════════════════════════
    // TAB 2 — ATTENDANCE LOG  (Rich row editor + server sync)
    // ════════════════════════════════════════════════════════════════════════
    void _handleAttendance() {
        struct tm ti = {}; getLocalTime(&ti, 0);
        char todayLabel[32];
        strftime(todayLabel, sizeof(todayLabel), "Today (%a %b %d, %Y)", &ti);

        String html = _head("Attendance", 2);
        html += R"HTML(
<div class="card">
  <div class="card-title">Attendance Log</div>
  <div style="display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap;align-items:center">
    <select id="dateSelect" onchange="loadCsv(this.value)" style="width:auto;flex:1"></select>
    <a id="dlCsvBtn" href="#" class="btn btn-primary btn-sm" download="attendance.csv">Download CSV</a>
    <button class="btn btn-ghost btn-sm" onclick="openEditor()">&#9998; Edit Raw Log</button>
  </div>
  <div id="dateLabel" style="font-size:.78rem;color:#64748b;margin-bottom:10px"></div>
  <div id="tableArea"><p style="color:#64748b;font-size:.82rem">Loading...</p></div>
</div>

<!-- ── Row Editor Modal ── -->
<div id="editorModal" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,.75);z-index:999;overflow-y:auto;padding:20px">
  <div style="max-width:800px;margin:0 auto;background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px">
    <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:16px">
      <h3 style="font-size:1rem;font-weight:700">&#9998; Edit Attendance Records</h3>
      <div style="display:flex;gap:8px">
        <button class="btn btn-primary btn-sm" onclick="saveAllRows()">&#10003; Save &amp; Sync</button>
        <button class="btn btn-ghost btn-sm" onclick="closeEditor()">&#10005; Close</button>
      </div>
    </div>
    <div id="syncStatus" style="display:none;padding:8px 12px;border-radius:6px;font-size:.82rem;margin-bottom:12px"></div>
    <div style="font-size:.78rem;color:var(--dim);margin-bottom:12px">
      Edit any field below. Changes are saved to the SD card and synced to the server.
    </div>
    <div id="editorRows"></div>
    <div style="margin-top:14px;display:flex;gap:8px;flex-wrap:wrap">
      <button class="btn btn-primary" onclick="saveAllRows()">&#10003; Save &amp; Sync to Server</button>
      <button class="btn btn-ghost" onclick="closeEditor()">Cancel</button>
    </div>
  </div>
</div>

<style>
.erow{background:rgba(255,255,255,.03);border:1px solid var(--border);border-radius:8px;
  padding:12px;margin-bottom:10px;position:relative}
.erow:hover{border-color:var(--teal)}
.erow-header{display:flex;align-items:center;gap:8px;margin-bottom:10px}
.erow-num{font-size:.7rem;color:var(--dim);min-width:24px}
.erow-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.erow-field label{font-size:.72rem;color:var(--dim);display:block;margin-bottom:3px}
.erow-field input,.erow-field select{background:rgba(255,255,255,.06);border:1px solid var(--border);
  border-radius:5px;color:var(--text);padding:6px 8px;font-size:.8rem;width:100%}
.erow-field input:focus,.erow-field select:focus{outline:none;border-color:var(--teal)}
.del-row-btn{position:absolute;top:10px;right:10px;background:rgba(239,68,68,.15);
  color:var(--red);border:1px solid rgba(239,68,68,.3);border-radius:5px;
  padding:3px 8px;font-size:.72rem;cursor:pointer}
.del-row-btn:hover{background:rgba(239,68,68,.3)}
.badge-morning_in,.badge-morning_out,.badge-afternoon_in,.badge-afternoon_out,
.badge-evening_in,.badge-evening_out{display:inline-block;padding:2px 8px;
  border-radius:4px;font-size:.72rem;font-weight:600}
.badge-morning_in,.badge-afternoon_in,.badge-evening_in{background:rgba(34,211,238,.15);color:#22d3ee}
.badge-morning_out,.badge-afternoon_out,.badge-evening_out{background:rgba(249,115,22,.15);color:#f97316}
</style>

<script>
var currentFile = '';
var currentDate = '';
var _editorRows = [];
var _serverIds  = [];  // parallel to _editorRows: server DB id for each row, 0 if unknown

// ── SSE live-refresh: auto-reload when a scan or reseed arrives ──
(function(){
  var _sseAtt = new EventSource('/api/events');
  var _refreshTimer = null;
  function _scheduleRefresh(){
    if(_refreshTimer) clearTimeout(_refreshTimer);
    _refreshTimer = setTimeout(function(){
      var sel = document.getElementById('dateSelect');
      if(sel) loadCsv(sel.value || 'today');
    }, 1500);
  }
  _sseAtt.addEventListener('scan',  function(){ _scheduleRefresh(); });
  _sseAtt.addEventListener('stats', function(){ _scheduleRefresh(); });
  _sseAtt.onerror = function(){ /* silent reconnect */ };
})();

fetch('/api/attendance/dates').then(r=>r.json()).then(function(d){
  var sel = document.getElementById('dateSelect');
  sel.innerHTML = '';
  var todayOpt = document.createElement('option');
  todayOpt.value = 'today';
  todayOpt.textContent = d.today_label || 'Today';
  sel.appendChild(todayOpt);
  (d.dates||[]).forEach(function(entry){
    var o = document.createElement('option');
    o.value = entry.file;
    o.textContent = entry.label;
    sel.appendChild(o);
  });
  loadCsv('today');
});

var _autoRetryTimer = null;

function loadCsv(val){
  currentFile = (val==='today') ? '__today__' : '/attendance/'+val;
  var url = '/api/attendance?f='+encodeURIComponent(val);
  document.getElementById('dlCsvBtn').href = '/api/sd/dl?f='+encodeURIComponent(currentFile);
  var sel = document.getElementById('dateSelect');
  var selText = sel.options[sel.selectedIndex] ? sel.options[sel.selectedIndex].textContent : '';
  document.getElementById('dateLabel').textContent = selText ? ('Showing records for: '+selText) : '';

  // Show spinner while fetching
  document.getElementById('tableArea').innerHTML =
    '<div style="display:flex;align-items:center;gap:10px;padding:12px 0;color:#64748b;font-size:.82rem">'
    +'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#22d3ee" stroke-width="2"'
    +' style="animation:spin 1s linear infinite"><circle cx="12" cy="12" r="10" stroke-opacity=".25"/>'
    +'<path d="M12 2a10 10 0 0 1 10 10"/></svg>'
    +'Fetching attendance data...</div>'
    +'<style>@keyframes spin{to{transform:rotate(360deg)}}</style>';

  fetch(url).then(r=>r.json()).then(function(d){
    // Cancel any pending auto-retry (previous empty-state timer)
    if(_autoRetryTimer){ clearTimeout(_autoRetryTimer); _autoRetryTimer=null; }

    // If ESP32 cleaned up server-deleted rows, reload transparently
    if(d.rows_removed && d.rows_removed > 0){
      console.log('[SD] '+d.rows_removed+' server-deleted row(s) purged — reloading');
      loadCsv(val);
      return;
    }

    if(!d.rows || d.rows.length===0){
      // If this is today's view, schedule one auto-retry in 5s.
      // The SD sync may still be running in the background at boot time.
      if(val==='today'){
        document.getElementById('tableArea').innerHTML=
          '<div style="display:flex;align-items:center;gap:10px;padding:12px 0;color:#64748b;font-size:.82rem">'
          +'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#f97316" stroke-width="2"'
          +' style="animation:spin 1s linear infinite"><circle cx="12" cy="12" r="10" stroke-opacity=".25"/>'
          +'<path d="M12 2a10 10 0 0 1 10 10"/></svg>'
          +'SD sync in progress — retrying in 5 s…</div>';
        _autoRetryTimer = setTimeout(function(){ loadCsv('today'); }, 5000);
      } else {
        document.getElementById('tableArea').innerHTML=
          '<p style="color:#64748b;font-size:.82rem">No records for this date.</p>';
      }
      return;
    }

    // ── Source badge (shown in dateLabel when data came live from server) ──
    var srcBadge = '';
    if(d.pulled_from_server){
      srcBadge = ' <span style="display:inline-block;padding:2px 8px;border-radius:4px;'
               + 'font-size:.7rem;font-weight:600;background:rgba(34,211,238,.15);'
               + 'color:#22d3ee;border:1px solid rgba(34,211,238,.2);margin-left:6px">'
               + '&#8659; live from server</span>';
    }
    var lbl = document.getElementById('dateLabel');
    lbl.innerHTML = (selText ? ('Showing records for: '+selText) : '') + srcBadge;

    var h='<div style="overflow-x:auto"><table><thead><tr>';
    h+='<th style="color:#22d3ee">Date</th>';
    (d.headers||[]).forEach(function(hd){h+='<th>'+hd+'</th>';});
    h+='<th></th>';  // edit button column
    h+='</tr></thead><tbody>';
    d.rows.forEach(function(row, idx){
      h+='<tr>';
      h+='<td style="color:#22d3ee;white-space:nowrap;font-size:.78rem">'+(d.date||'')+'</td>';
      row.forEach(function(cell,i){
        if(i===5){
          // event_type badge — handles morning_in, afternoon_out, etc.
          var et = cell.trim();
          var isIn = et.endsWith('_in') || et==='check-in';
          var cls = isIn ? 'badge-in' : (et.endsWith('_out')||et==='check-out') ? 'badge-out' : 'badge-denied';
          h+='<td><span class="badge '+cls+'">'+et+'</span></td>';
        } else {
          h+='<td>'+cell+'</td>';
        }
      });
      // Only show edit button for SD-backed rows (server-live rows have no local ID to edit)
      if(!d.pulled_from_server){
        h+='<td><button class="btn btn-ghost btn-sm" onclick="editRow('+idx+')">&#9998;</button></td>';
      } else {
        h+='<td><span style="font-size:.7rem;color:#64748b" title="Save SD sync first">—</span></td>';
      }
      h+='</tr>';
    });
    h+='</tbody></table></div>';
    if(d.pulled_from_server){
      h+='<p style="font-size:.75rem;color:#64748b;margin-top:8px;padding:0 4px">'
        +'&#9432; Showing live server data. SD sync is still running — '
        +'<a href="#" onclick="loadCsv(\'today\');return false;" '
        +'style="color:#22d3ee">refresh</a> in a moment to edit records.</p>';
    }
    document.getElementById('tableArea').innerHTML = h;
    // Store rows for editor
    window._csvData = d;
  }).catch(function(e){
    document.getElementById('tableArea').innerHTML=
      '<p style="color:#ef4444;font-size:.82rem">Fetch error: '+e+'</p>';
  });
}

function editRow(idx){
  openEditor(idx);
}

function openEditor(focusIdx){
  if(!window._csvData || !window._csvData.rows){
    alert('Load attendance data first.');
    return;
  }
  var d = window._csvData;
  _editorRows = d.rows.map(function(r){ return r.slice(); });
  _serverIds  = (d.serverIds||[]).map(function(id){ return id||0; });
  // Pad to same length in case serverIds is shorter (e.g. offline)
  while (_serverIds.length < _editorRows.length) _serverIds.push(0);

  var html = '';
  _editorRows.forEach(function(row, i){
    var et      = (row[5]||'').trim();
    var isIn    = et.endsWith('_in') || et==='check-in';
    var badgeCls= isIn ? 'badge-in' : (et.endsWith('_out')||et==='check-out') ? 'badge-out' : 'badge-denied';

    // Parse timestamp → HH:MM:SS for the time input
    var rawTs  = (row[0]||'').replace(/"/g,'').trim();
    // If stored as "HH:MM:SS" already; if "YYYY-MM-DD HH:MM:SS" extract time part
    var timePart = rawTs;
    if(rawTs.indexOf(' ') >= 0) timePart = rawTs.split(' ')[1] || rawTs;
    // <input type="time" step="1"> expects HH:MM:SS
    if(timePart.split(':').length === 2) timePart += ':00';

    html += '<div class="erow" id="erow_'+i+'">';
    html += '<div class="erow-header">';
    html += '<span class="erow-num">#'+(i+1)+'</span>';
    html += '<span class="badge '+badgeCls+'" id="badge_'+i+'">'+et+'</span>';
    html += '<span style="font-size:.72rem;color:var(--dim);margin-left:8px">'+
            (row[3]||'').replace(/"/g,'')+'</span>';
    html += '</div>';

    // ── Two-column grid: ONLY editable fields ──────────────────────────────
    html += '<div class="erow-grid">';

    // EDITABLE: Timestamp — time picker
    html += '<div class="erow-field">';
    html += '<label>&#128336; Time</label>';
    html += '<input type="time" step="1" id="f_'+i+'_0" value="'+timePart+'" '+
            'style="color-scheme:dark" onchange="updateBadge('+i+')">';
    html += '</div>';

    // EDITABLE: Clock Type — dropdown
    html += '<div class="erow-field"><label>&#128203; Clock Type</label>';
    html += '<select id="f_'+i+'_5" onchange="updateBadge('+i+')">';
    var types=['morning_in','morning_out','afternoon_in','afternoon_out','evening_in','evening_out'];
    types.forEach(function(t){
      var lbl = t.replace('_',' ').replace(/\b\w/g,function(c){return c.toUpperCase();});
      html += '<option value="'+t+'"'+(et===t?' selected':'')+'>'+lbl+'</option>';
    });
    html += '</select></div>';

    html += '</div>'; // erow-grid

    // ── Read-only info strip ───────────────────────────────────────────────
    html += '<div style="display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:8px;'+
            'background:rgba(0,0,0,.2);border-radius:6px;padding:8px 10px">';

    html += _roField('Employee', (row[3]||'').replace(/"/g,''));
    html += _roField('Department', (row[4]||'').replace(/"/g,''));
    html += _roField('Emp UID', (row[2]||'').replace(/"/g,''));

    html += '</div>'; // read-only strip

    html += '<button class="del-row-btn" onclick="deleteRow('+i+')">&#128465; Delete</button>';
    html += '</div>'; // erow
  });

  document.getElementById('editorRows').innerHTML = html;
  document.getElementById('editorModal').style.display = 'block';
  document.body.style.overflow = 'hidden';

  if(focusIdx !== undefined){
    setTimeout(function(){
      var el = document.getElementById('erow_'+focusIdx);
      if(el) el.scrollIntoView({behavior:'smooth',block:'center'});
    },100);
  }
}

// Helper: read-only label+value cell
function _roField(label, val){
  return '<div><div style="font-size:.68rem;color:var(--dim);margin-bottom:2px">'+label+'</div>'+
         '<div style="font-size:.78rem;color:var(--text);font-weight:500">'+
         (val||'—')+'</div></div>';
}

// Live-update the badge when clock type or time changes
function updateBadge(i){
  var sel = document.getElementById('f_'+i+'_5');
  if(!sel) return;
  var et = sel.value;
  var isIn = et.endsWith('_in');
  var badgeCls = isIn ? 'badge-in' : 'badge-out';
  var badge = document.getElementById('badge_'+i);
  if(badge){
    badge.className = 'badge '+badgeCls;
    badge.textContent = et;
  }
}

function closeEditor(){
  document.getElementById('editorModal').style.display = 'none';
  document.body.style.overflow = '';
  hideSyncStatus();
}

function deleteRow(idx){
  if(!confirm('Delete row #'+(idx+1)+'? This will remove it from SD and the server.')) return;
  var row = _editorRows[idx];
  var payload = {
    file: currentFile,
    rowIndex: idx,
    empUid: (row[2]||'').replace(/"/g,''),
    timestamp: (row[0]||'').replace(/"/g,''),
    eventType: (row[5]||'').replace(/"/g,''),
    serverId: _serverIds[idx]||0        // pass server DB id directly — no range-query needed
  };
  fetch('/api/attendance/deleterow', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify(payload)
  }).then(r=>r.json()).then(function(d){
    if(d.success){
      showSyncStatus('Row deleted. ' + (d.server_ok ? 'Server synced.' : 'SD only (offline).'), d.server_ok);
      _editorRows.splice(idx, 1);
      var sel = document.getElementById('dateSelect');
      loadCsv(sel.value);
      closeEditor();
    } else {
      showSyncStatus('Delete failed: '+(d.error||'unknown'), false);
    }
  }).catch(function(e){ showSyncStatus('Network error: '+e, false); });
}

function saveAllRows(){
  var rows = [];
  var serverIdsOut = [];
  for(var i=0; i<_editorRows.length; i++){
    var el = document.getElementById('erow_'+i);
    if(!el) continue;

    var orig = _editorRows[i];

    // Reconstruct timestamp: keep date prefix if original had one
    var timePicker = document.getElementById('f_'+i+'_0');
    var newTime    = timePicker ? timePicker.value : '';   // "HH:MM" or "HH:MM:SS"
    // Ensure seconds are included
    if(newTime && newTime.split(':').length === 2) newTime += ':00';
    var origTs = (orig[0]||'').replace(/"/g,'').trim();
    var finalTs = newTime;
    if(origTs.indexOf(' ') >= 0){
      // Original was "YYYY-MM-DD HH:MM:SS" — preserve the date portion
      finalTs = origTs.split(' ')[0] + ' ' + newTime;
    }

    var clockSel = document.getElementById('f_'+i+'_5');
    var newClock = clockSel ? clockSel.value : (orig[5]||'');

    var row = [
      finalTs,                              // 0 timestamp  (edited)
      (orig[1]||'').replace(/"/g,''),       // 1 nfc_uid    (locked)
      (orig[2]||'').replace(/"/g,''),       // 2 emp_uid    (locked)
      (orig[3]||'').replace(/"/g,''),       // 3 emp_name   (locked)
      (orig[4]||'').replace(/"/g,''),       // 4 dept       (locked)
      newClock,                             // 5 clock_type (edited)
      (orig[6]||'').replace(/"/g,'')        // 6 device_id  (locked)
    ];
    rows.push(row);
    serverIdsOut.push(_serverIds[i]||0);
  }

  var payload = { file: currentFile, rows: rows, serverIds: serverIdsOut };
  showSyncStatus('Saving to SD and syncing to server...', null);

  fetch('/api/attendance/update', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify(payload)
  }).then(r=>r.json()).then(function(d){
    if(d.success){
      var msg = 'Saved to SD.';
      if(d.server_synced > 0) msg += ' Synced '+d.server_synced+' record(s) to server.';
      else if(d.server_error) msg += ' Server sync failed: '+d.server_error;
      else msg += ' Device offline — SD only.';
      showSyncStatus(msg, d.server_synced > 0);
      var sel = document.getElementById('dateSelect');
      loadCsv(sel.value);
    } else {
      showSyncStatus('Save failed: '+(d.error||'unknown error'), false);
    }
  }).catch(function(e){ showSyncStatus('Network error: '+e, false); });
}

function showSyncStatus(msg, ok){
  var el = document.getElementById('syncStatus');
  el.textContent = msg;
  if(ok === null){
    el.style.cssText='display:block;background:rgba(249,115,22,.1);color:#f97316;border:1px solid rgba(249,115,22,.3);padding:8px 12px;border-radius:6px;font-size:.82rem;margin-bottom:12px';
  } else if(ok){
    el.style.cssText='display:block;background:rgba(16,185,129,.1);color:#10b981;border:1px solid rgba(16,185,129,.3);padding:8px 12px;border-radius:6px;font-size:.82rem;margin-bottom:12px';
  } else {
    el.style.cssText='display:block;background:rgba(239,68,68,.1);color:#ef4444;border:1px solid rgba(239,68,68,.3);padding:8px 12px;border-radius:6px;font-size:.82rem;margin-bottom:12px';
  }
}
function hideSyncStatus(){ document.getElementById('syncStatus').style.display='none'; }

loadCsv('today');
</script>
)HTML";
        html += _foot();
        _srv.send(200, "text/html", html);
    }

    // ════════════════════════════════════════════════════════════════════════
    // TAB 3 — ACTIONS
    // ════════════════════════════════════════════════════════════════════════
    void _handleActions() {
        String html = _head("Actions", 3);
        html += R"HTML(
<div class="card">
  <div class="card-title">Device Actions</div>
  <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px">
    <div style="background:rgba(255,255,255,.03);border:1px solid var(--border);border-radius:8px;padding:14px;text-align:center">
      <div style="font-size:1.6rem;margin-bottom:6px">&#9851;</div>
      <div style="font-size:.82rem;font-weight:600;margin-bottom:8px">Reboot Device</div>
      <button class="btn btn-danger btn-sm" onclick="doReboot()">Reboot</button>
    </div>
    <div style="background:rgba(255,255,255,.03);border:1px solid var(--border);border-radius:8px;padding:14px;text-align:center">
      <div style="font-size:1.6rem;margin-bottom:6px">&#128247;</div>
      <div style="font-size:.82rem;font-weight:600;margin-bottom:8px">Employee Photos</div>
      <a href="/portal/files" class="btn btn-ghost btn-sm">Browse SD</a>
    </div>
    <div style="background:rgba(255,255,255,.03);border:1px solid var(--border);border-radius:8px;padding:14px;text-align:center;opacity:.5">
      <div style="font-size:1.6rem;margin-bottom:6px">&#128465;</div>
      <div style="font-size:.82rem;font-weight:600;margin-bottom:8px">Clear Cache</div>
      <button class="btn btn-ghost btn-sm" disabled>Coming Soon</button>
    </div>
    <div style="background:rgba(255,255,255,.03);border:1px solid var(--border);border-radius:8px;padding:14px;text-align:center;opacity:.5">
      <div style="font-size:1.6rem;margin-bottom:6px">&#128100;</div>
      <div style="font-size:.82rem;font-weight:600;margin-bottom:8px">Manage Users</div>
      <button class="btn btn-ghost btn-sm" disabled>Coming Soon</button>
    </div>
  </div>
  <div id="actionMsg" class="alert"></div>
</div>
<script>
function doReboot(){
  if(!confirm('Reboot the device?'))return;
  fetch('/api/reboot',{method:'POST'}).then(r=>r.json()).then(function(){
    var m=document.getElementById('actionMsg');
    m.className='alert alert-ok';m.textContent='Rebooting...';m.style.display='block';
  });
}
</script>
)HTML";
        html += _foot();
        _srv.send(200, "text/html", html);
    }

    // ════════════════════════════════════════════════════════════════════════
    // TAB 4 — WIFI
    // ════════════════════════════════════════════════════════════════════════
    void _handleWifi() {
        bool wOk = _cfg && _cfg->isConnected();
        String html = _head("WiFi", 4);
        html += R"HTML(
<div class="card">
  <div class="card-title">Current Connection</div>
  <div id="wifiStatus"><p style="color:#64748b;font-size:.82rem">Loading...</p></div>
</div>
<div class="card">
  <div class="card-title">Available Networks
    <button class="btn btn-ghost btn-sm" style="margin-left:10px" onclick="rescan()">&#8635; Rescan</button>
    <span id="scanStatus" style="font-size:.72rem;color:#64748b;margin-left:8px"></span>
  </div>
  <div id="netlist"><p style="color:#64748b;font-size:.82rem">Loading cached scan...</p></div>
</div>
<div class="card" id="connectForm" style="display:none">
  <div class="card-title" id="connectTitle">Connect</div>
  <div class="form-row"><label>SSID</label><input id="cfSSID" type="text"></div>
  <div class="form-row"><label>Password</label><input id="cfPass" type="password" placeholder="Leave blank for open network"></div>
  <div style="display:flex;gap:8px;margin-top:4px">
    <button class="btn btn-primary" onclick="doConnect()">Connect</button>
    <button class="btn btn-ghost" onclick="closeForm()">Cancel</button>
  </div>
  <div id="connectMsg" class="alert"></div>
</div>
<script>
function loadStatus(){
  fetch('/api/wifi/info').then(r=>r.json()).then(function(d){
    var h='<div style="display:flex;align-items:center;gap:10px">';
    var col=d.connected?'#10b981':'#ef4444';
    h+='<span class="dot" style="background:'+col+'"></span>';
    if(d.connected){
      h+='<div><strong style="color:#22d3ee">'+d.ssid+'</strong>';
      h+='<span style="color:#64748b;font-size:.78rem;margin-left:8px">'+d.ip+'</span></div>';
      h+='<button class="btn btn-danger btn-sm" onclick="doDisconnect()">Disconnect</button>';
    } else {
      h+='<span style="color:#ef4444">Not connected (AP: '+d.ap_ip+')</span>';
    }
    h+='</div>';
    document.getElementById('wifiStatus').innerHTML=h;
  });
}
function loadNets(){
  fetch('/api/wifi/scan').then(r=>r.json()).then(function(d){
    var ss=document.getElementById('scanStatus');
    if(d.scanning){ss.textContent='Scanning...';setTimeout(loadNets,2000);return;}
    ss.textContent=d.count+' networks found';
    var h='';
    (d.networks||[]).forEach(function(n){
      var bars=n.rssi>-60?'&#9646;&#9646;&#9646;':n.rssi>-75?'&#9646;&#9646;&#9647;':'&#9646;&#9647;&#9647;';
      var lock=n.open?'':'&#128274;';
      h+='<div class="tag-wifi" onclick="selectNet(\''+n.ssid.replace(/'/g,"\\'")+'\')">';
      h+='<span style="color:#22d3ee;font-size:.85rem">'+bars+'</span>';
      h+='<span class="ssid">'+n.ssid+' '+lock+'</span>';
      h+='<span class="rssi">'+n.rssi+'dBm</span>';
      h+='</div>';
    });
    if(!h)h='<p style="color:#64748b;font-size:.82rem">No networks found. Try rescanning.</p>';
    document.getElementById('netlist').innerHTML=h;
  });
}
function rescan(){
  document.getElementById('scanStatus').textContent='Starting scan...';
  fetch('/api/wifi/scan?rescan=1').then(()=>setTimeout(loadNets,3000));
}
function selectNet(ssid){
  document.getElementById('cfSSID').value=ssid;
  document.getElementById('connectTitle').textContent='Connect to: '+ssid;
  document.getElementById('connectForm').style.display='block';
  document.getElementById('connectMsg').style.display='none';
  window.scrollTo(0,document.body.scrollHeight);
}
function closeForm(){document.getElementById('connectForm').style.display='none';}
function doConnect(){
  var s=document.getElementById('cfSSID').value;
  var p=document.getElementById('cfPass').value;
  if(!s)return;
  var m=document.getElementById('connectMsg');
  m.className='alert alert-ok';m.textContent='Connecting...';m.style.display='block';
  fetch('/api/wifi/connect',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:s,password:p})
  }).then(r=>r.json()).then(function(d){
    if(d.success){m.textContent='Connected! IP: '+d.ip;loadStatus();}
    else{m.className='alert alert-err';m.textContent='Failed: '+(d.error||'Unknown');}
  });
}
function doDisconnect(){
  if(!confirm('Disconnect from WiFi?'))return;
  fetch('/api/wifi/disconnect',{method:'POST'}).then(r=>r.json()).then(function(){
    loadStatus();loadNets();
  });
}
loadStatus();loadNets();
</script>
)HTML";
        html += _foot();
        _srv.send(200, "text/html", html);
    }

    // ════════════════════════════════════════════════════════════════════════
    // API HANDLERS
    // ════════════════════════════════════════════════════════════════════════

    // Resolves __today__ macro to the real NTP-based attendance file path.
    // Uses getLocalTime() — same source as SDDatabase's date provider —
    // so this always matches the file that SDDatabase::logAttendance() writes to.
    String _resolveTodayPath(const String& p) {
        if (p == "__today__" || p == "today") {
            // Try NTP first
            struct tm ti = {};
            if (getLocalTime(&ti, 0)) {
                char buf[40];
                snprintf(buf, sizeof(buf), "/attendance/%04d-%02d-%02d.csv",
                         ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
                return String(buf);
            }
            // Fallback: uptime-based (same as SDDatabase fallback before NTP sync)
            unsigned long day = millis() / 86400000UL;
            char buf[40];
            snprintf(buf, sizeof(buf), "/attendance/day_%06lu.csv", day);
            return String(buf);
        }
        return p;
    }

    // ── SSE Handler ───────────────────────────────────────────────────────────
    // Keeps the HTTP connection open and streams events to the browser.
    void _handleSSE() {
        // Drop stale connection
        if (_sseConnected && _sseClient.connected()) _sseClient.stop();

        // Grab the raw TCP socket BEFORE WebServer can close it
        _sseClient    = _srv.client();
        _sseConnected = (_sseClient && _sseClient.connected());
        if (!_sseConnected) return;

        // Write raw HTTP — bypasses WebServer chunked encoding which breaks SSE.
        // "Transfer-Encoding: identity" explicitly disables chunking.
        _sseClient.print(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Transfer-Encoding: identity\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n"
        );
        _sseClient.setNoDelay(true);

        // Push a stats snapshot immediately so the browser gets data on connect
        _sendStatsSnapshot();

        Serial.println("[SSE] Client connected — raw HTTP stream open");
    }

    // Pushes current stats as the first event on connect
    void _sendStatsSnapshot() {
        if (!_sseConnected || !_sseClient.connected()) return;
        int ins   = max(0, SDDatabase::countTodayCheckIns());
        int outs  = max(0, SDDatabase::countTodayCheckOuts());
        bool wOk  = _cfg && _cfg->isConnected();
        uint64_t freeMB = SDDatabase::freeBytes() / 1048576;

        char payload[128];
        snprintf(payload, sizeof(payload),
            "{\"ins\":%d,\"outs\":%d,\"wifi\":%s,\"free_mb\":%llu}",
            ins, outs, wOk ? "true" : "false", freeMB);

        _sseClient.print("event: stats\ndata: ");
        _sseClient.print(payload);
        _sseClient.print("\n\n");
    }

    void _apiStatus() {
        DynamicJsonDocument doc(512);
        doc["wifi"]    = _cfg && _cfg->isConnected();
        doc["sd"]      = SDDatabase::isReady();
        doc["free_mb"] = (int)(SDDatabase::freeBytes() / 1048576);
        doc["uptime"]  = millis() / 1000;
        String out; serializeJson(doc,out);
        _srv.sendHeader("Access-Control-Allow-Origin","*");
        _srv.send(200,"application/json",out);
    }

    // ── FILE OPERATIONS ───────────────────────────────────────────────────────

    // SD Delete (File or Dir)
    void _apiSdDelete() {
        DynamicJsonDocument req(256); deserializeJson(req, _srv.arg("plain"));
        String p = req["path"] | "";
        if (p.indexOf("..") >= 0 || p == "/" || p.length() == 0) {
            _srv.send(403,"application/json","{\"success\":false}"); return;
        }
        File f = SD_MMC.open(p);
        bool isDir = f && f.isDirectory();
        if (f) f.close();

        bool ok = isDir ? SDFileManager::deleteDir(p) : SDFileManager::deleteFile(p);
        _srv.send(200,"application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
    }

    // SD Rename / Move
    void _apiSdRename() {
        DynamicJsonDocument req(256); deserializeJson(req, _srv.arg("plain"));
        String f = req["from"] | "", t = req["to"] | "";
        if (f.indexOf("..") >= 0 || t.indexOf("..") >= 0 || f.length() == 0 || t.length() == 0) {
            _srv.send(403,"application/json","{\"success\":false}"); return;
        }
        bool ok = SDFileManager::renameFile(f, t);
        _srv.send(200,"application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
    }

    // SD Copy
    void _apiSdCopy() {
        DynamicJsonDocument req(256); deserializeJson(req, _srv.arg("plain"));
        String f = req["from"] | "", t = req["to"] | "";
        if (f.indexOf("..") >= 0 || t.indexOf("..") >= 0 || f.length() == 0 || t.length() == 0) {
            _srv.send(403,"application/json","{\"success\":false}"); return;
        }
        bool ok = SDFileManager::copyFile(f, t);
        _srv.send(200,"application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
    }

    // SD Write / Overwrite Text (Used by Attendance Editor)
    void _apiSdWrite() {
        if (!_srv.hasArg("f")) { _srv.send(400,"application/json","{\"success\":false,\"error\":\"Missing f\"}"); return; }
        String path = _resolveTodayPath(_srv.arg("f"));
        if (path.indexOf("..") >= 0) { _srv.send(403,"application/json","{\"success\":false}"); return; }
        
        String content = _srv.arg("plain"); // Raw body
        bool ok = SDFileManager::writeTextFile(path, content);
        _srv.send(200,"application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
    }

    // ── READ & LIST ───────────────────────────────────────────────────────────

    // SD tree
    void _apiSdTree() {
        String path = _srv.hasArg("path") ? _srv.arg("path") : "/";
        if (!SDDatabase::isReady()) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"SD not ready\"}");
            return;
        }
        File dir = SD_MMC.open(path);
        if (!dir || !dir.isDirectory()) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"Not a directory\"}");
            return;
        }
        DynamicJsonDocument doc(8192);
        doc["success"] = true;
        doc["path"]    = path;
        JsonArray items = doc.createNestedArray("items");
        
        if (path != "/" && path.length() > 1) {
            int ls = path.lastIndexOf('/');
            String parent = (ls > 0) ? path.substring(0, ls) : "/";
            JsonObject p = items.createNestedObject();
            p["name"] = ".."; p["path"] = parent; p["type"] = "dir"; p["size"] = 0;
        }
        File entry = dir.openNextFile();
        while (entry) {
            JsonObject item = items.createNestedObject();
            String ename = String(entry.name());
            String fpath = (path == "/") ? "/" + ename : path + "/" + ename;
            item["name"] = ename;
            item["path"] = fpath;
            if (entry.isDirectory()) {
                item["type"] = "dir";
                item["size"] = 0;
            } else {
                item["type"] = "file";
                item["size"] = (int)entry.size();
            }
            entry.close();
            entry = dir.openNextFile();
            yield();
        }
        dir.close();
        String out; serializeJson(doc, out);
        _srv.sendHeader("Access-Control-Allow-Origin","*");
        _srv.send(200,"application/json",out);
    }

    // SD list CSV files
    void _apiSdList() {
        DynamicJsonDocument doc(2048);
        JsonArray arr = doc.createNestedArray("files");
        if (SDDatabase::isReady()) {
            File d = SD_MMC.open("/attendance");
            if (d) {
                File e = d.openNextFile();
                while (e) {
                    if (!e.isDirectory()) arr.add(String(e.name()));
                    e.close(); e = d.openNextFile();
                }
                d.close();
            }
        }
        String out; serializeJson(doc,out);
        _srv.sendHeader("Access-Control-Allow-Origin","*");
        _srv.send(200,"application/json",out);
    }

    void _apiSdRead() {
        if (!_srv.hasArg("f")) { _srv.send(400,"text/plain","Missing f param"); return; }
        String path = _resolveTodayPath(_srv.arg("f"));
        if (path.indexOf("..") >= 0) { _srv.send(403,"text/plain","Forbidden"); return; }
        if (!SDDatabase::isReady()) { _srv.send(503,"text/plain","SD not ready"); return; }
        if (!SD_MMC.exists(path)) { _srv.send(404,"text/plain","Not found"); return; }
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) { _srv.send(500,"text/plain","Open failed"); return; }

        String ext = path.substring(path.lastIndexOf('.') + 1); ext.toLowerCase();
        bool isBinary = (ext=="jpg"||ext=="jpeg"||ext=="png"||ext=="bin"||ext=="dat");

        if (isBinary) {
            uint8_t buf[512]; size_t n = f.read(buf, sizeof(buf)); f.close();
            String hex = "[Binary file: " + path + " | size preview (hex)]\n";
            for (size_t i=0;i<n;i++) {
                char b[4]; snprintf(b,sizeof(b),"%02X ",buf[i]); hex+=b;
                if ((i+1)%16==0) hex+="\n";
            }
            _srv.sendHeader("Access-Control-Allow-Origin","*");
            _srv.send(200,"text/plain",hex);
        } else {
            String content;
            size_t sz = f.size();
            if (sz > 32768) {
                content = "[File too large to display in browser, showing first 32KB]\n\n";
                content.reserve(32800);
                size_t got = 0;
                while (f.available() && got < 32768) { content += (char)f.read(); got++; }
            } else {
                while (f.available()) content += (char)f.read();
            }
            f.close();
            _srv.sendHeader("Access-Control-Allow-Origin","*");
            _srv.send(200,"text/plain",content);
        }
    }

    void _apiSdDownload() {
        if (!_srv.hasArg("f")) { _srv.send(400,"text/plain","Missing f"); return; }
        String path = _resolveTodayPath(_srv.arg("f"));
        if (path.indexOf("..") >= 0) { _srv.send(403,"text/plain","Forbidden"); return; }
        if (!SDDatabase::isReady()) { _srv.send(503,"text/plain","SD not ready"); return; }

        if (path.length() == 0 || !SD_MMC.exists(path)) {
            _srv.send(404,"text/plain","Not found"); return;
        }
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) { _srv.send(500,"text/plain","Open failed"); return; }

        String fname = path.substring(path.lastIndexOf('/')+1);
        String ext   = fname.substring(fname.lastIndexOf('.')+1);
        String ct    = "application/octet-stream";
        if (ext=="csv")  ct = "text/csv";
        else if (ext=="json") ct = "application/json";
        else if (ext=="txt")  ct = "text/plain";
        else if (ext=="jpg"||ext=="jpeg") ct = "image/jpeg";

        _srv.sendHeader("Content-Disposition","attachment; filename=\""+fname+"\"");
        _srv.sendHeader("Content-Length", String(f.size()));
        _srv.streamFile(f, ct);
        f.close();
    }

    // ── ATTENDANCE UPDATE — save edited rows to SD, PUT/POST server, invalidate summary ──
    void _apiAttendanceUpdate() {
        String body = _srv.arg("plain");
        if (body.length() == 0) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"Empty body\"}");
            return;
        }

        DynamicJsonDocument req(16384);
        if (deserializeJson(req, body) != DeserializationError::Ok) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"JSON parse error\"}");
            return;
        }

        String   fileArg   = req["file"]      | "today";
        String   path      = _resolveTodayPath(fileArg);
        JsonArray rows     = req["rows"].as<JsonArray>();
        JsonArray srvIdArr = req["serverIds"].as<JsonArray>(); // parallel to rows

        Serial.printf("[WM] attendance/update path=%s rows=%d\n", path.c_str(), (int)rows.size());

        if (!SDDatabase::isReady()) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"SD not ready\"}");
            return;
        }

        // Derive YYYY-MM-DD from file path for clock_time construction
        String fileDateStr = "";
        {
            String fn = path.substring(path.lastIndexOf('/') + 1);
            fn.replace(".csv","");
            if (fn.length() == 10 && fn.indexOf('-') == 4) fileDateStr = fn;
        }

        // ── 1. Rebuild CSV on SD ──────────────────────────────────────────────
        if (SD_MMC.exists(path)) SD_MMC.remove(path);
        File f = SD_MMC.open(path, FILE_WRITE);
        if (!f) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"Cannot open SD file\"}");
            return;
        }
        f.println("timestamp,nfc_uid,employee_uid,employee_name,department,event_type,device_id");
        int written = 0;
        for (JsonArray row : rows) {
            String line = "";
            for (int i = 0; i < 7; i++) {
                if (i > 0) line += ",";
                String cell = row[i] | ""; cell.trim();
                if (cell.indexOf(',') >= 0 || cell.indexOf('"') >= 0) {
                    cell.replace("\"","\"\""); cell = "\"" + cell + "\"";
                }
                line += cell;
            }
            f.println(line); written++; yield();
        }
        f.flush(); f.close();
        Serial.printf("[WM] SD OK: %d rows → %s\n", written, path.c_str());

        // ── 2. Sync each row to server ────────────────────────────────────────
        int    synced    = 0;
        String serverErr = "";
        bool   wifiUp    = (_cfg && _cfg->isConnected());

        if (wifiUp && _serverURL.length() > 0) {
            int rowIdx = 0;
            for (JsonArray row : rows) {
                String empUid = (String)(row[2] | ""); empUid.trim();
                String rawTs  = (String)(row[0] | ""); rawTs.trim();
                String evType = (String)(row[5] | ""); evType.trim();

                if (empUid.length() == 0) { rowIdx++; continue; }

                // Split timestamp into date + time
                String dateStr = fileDateStr, timeStr = rawTs;
                int sp = rawTs.indexOf(' ');
                if (sp >= 0) { dateStr = rawTs.substring(0,sp); timeStr = rawTs.substring(sp+1); }
                String clockTimeFull = dateStr + " " + timeStr;

                // Server id passed from JS (0 = no existing record)
                int serverId = (rowIdx < (int)srvIdArr.size()) ? (srvIdArr[rowIdx] | 0) : 0;

                Serial.printf("[WM] row[%d] uid=%s type=%s time=%s srvId=%d\n",
                              rowIdx, empUid.c_str(), evType.c_str(),
                              clockTimeFull.c_str(), serverId);

                DynamicJsonDocument pd(512);
                pd["clock_type"]   = evType;
                pd["clock_time"]   = clockTimeFull;
                pd["date"]         = dateStr;
                pd["employee_uid"] = empUid;
                pd["is_synced"]    = 0;
                String pdStr; serializeJson(pd, pdStr);

                HTTPClient hc; hc.setTimeout(8000);
                int code = 0;

                if (serverId > 0) {
                    // ── PUT /api/attendanceEdit/:id  (update existing) ────────
                    hc.begin(_serverURL + "/api/attendanceEdit/" + String(serverId));
                    hc.addHeader("Content-Type","application/json");
                    hc.addHeader("X-Client-Type","ESP32");
                    code = hc.PUT(pdStr);
                    Serial.printf("[WM] PUT /attendanceEdit/%d → %d\n", serverId, code);
                } else {
                    // ── POST /api/attendanceEdit  (insert new) ────────────────
                    hc.begin(_serverURL + "/api/attendanceEdit");
                    hc.addHeader("Content-Type","application/json");
                    hc.addHeader("X-Client-Type","ESP32");
                    code = hc.POST(pdStr);
                    Serial.printf("[WM] POST /attendanceEdit → %d\n", code);

                    // ── Capture returned server ID → persist to SD map ────────
                    // Prevents future edits from POSTing again (duplicate row bug).
                    if ((code == 200 || code == 201) && fileDateStr.length() == 10) {
                        String postBody = hc.getString();
                        DynamicJsonDocument postResp(512);
                        if (deserializeJson(postResp, postBody) == DeserializationError::Ok) {
                            int newSrvId = postResp["data"]["id"] | postResp["id"] | 0;
                            if (newSrvId > 0) {
                                // Key uses the clock_time from the row (col 0), time portion only
                                String rowTs = (String)(row[0] | ""); rowTs.trim();
                                int sp2 = rowTs.indexOf(' ');
                                String timeOnly = (sp2 >= 0) ? rowTs.substring(sp2 + 1) : rowTs;
                                SDDatabase::saveServerIdMapping(fileDateStr, empUid,
                                                                evType, timeOnly, newSrvId);
                                Serial.printf("[WM] New server_id=%d saved to SD map\n", newSrvId);
                            }
                        }
                    }
                }

                if (code == 200 || code == 201) synced++;
                else if (serverErr.length() == 0)
                    serverErr = "HTTP " + String(code) + " uid=" + empUid;
                hc.end();
                rowIdx++;
                yield();
            }

            // ── 3. Update daily summary: fetch current → patch clock column → PUT ──
            // For each unique employee on this date:
            //   a) GET /api/attendanceEdit/summary?employee_uid=X&date=Y
            //   b) Overwrite only the changed clock_type column(s) with the new time
            //   c) PUT /api/attendanceEdit/summary/:id  → server recalculates hours + fires socket
            if (synced > 0 && fileDateStr.length() == 10) {

                // Build map: empUid → { clockType → newTime } from the edited rows
                // (only include rows that were successfully synced above)
                // HEAP-allocated to avoid ~10 KB stack overflow (316 bytes × 32 = 10,112 bytes).
                // EmpPatch on the stack was triggering the "Stack canary watchpoint" crash.
                struct ClockPatch { String clockType; String newTime; };
                struct EmpPatch   { String uid; ClockPatch clocks[6]; int count; };
                EmpPatch* empPatches = new EmpPatch[32];
                if (!empPatches) {
                    Serial.println("[WM] OOM: empPatches alloc failed");
                    _srv.send(500,"application/json","{\"success\":false,\"error\":\"OOM\"}");
                    return;
                }
                int epCount = 0;

                int rowIdx2 = 0;
                for (JsonArray row : rows) {
                    String eu = (String)(row[2] | ""); eu.trim();
                    String ct = (String)(row[5] | ""); ct.trim();
                    String rawTs = (String)(row[0] | ""); rawTs.trim();
                    // Extract time portion "HH:MM:SS"
                    String timeOnly = rawTs;
                    int sp2 = rawTs.indexOf(' ');
                    if (sp2 >= 0) timeOnly = rawTs.substring(sp2 + 1);

                    if (eu.length() == 0 || ct.length() == 0) { rowIdx2++; continue; }

                    // Find or create patch entry for this employee
                    int pi = -1;
                    for (int k=0;k<epCount;k++) if(empPatches[k].uid==eu){pi=k;break;}
                    if (pi < 0 && epCount < 32) { empPatches[epCount].uid=eu; empPatches[epCount].count=0; pi=epCount++; }
                    if (pi >= 0 && empPatches[pi].count < 6) {
                        empPatches[pi].clocks[empPatches[pi].count++] = {ct, timeOnly};
                    }
                    rowIdx2++;
                }

                // For each employee: fetch summary → patch → PUT
                DynamicJsonDocument sumFetch(4096); // declared once outside loop to reduce heap churn
                for (int pi=0; pi<epCount; pi++) {
                    String empUid = empPatches[pi].uid;
                    sumFetch.clear();

                    // ── a) Fetch current daily summary ────────────────────────
                    int summaryId = 0;
                    bool fetched  = false;
                    {
                        HTTPClient hf; hf.setTimeout(6000);
                        hf.begin(_serverURL + "/api/attendanceEdit/summary?employee_uid="
                                 + empUid + "&date=" + fileDateStr);
                        hf.addHeader("X-Client-Type","ESP32");
                        int fc = hf.GET();
                        Serial.printf("[WM] GET summary emp=%s date=%s → %d\n",
                                      empUid.c_str(), fileDateStr.c_str(), fc);
                        if (fc == 200) {
                            String rb = hf.getString(); hf.end();
                            bool ok = false;
                            if (_attSvc) ok = _attSvc->decryptBody(rb, sumFetch);
                            if (!ok)     ok = (deserializeJson(sumFetch, rb) == DeserializationError::Ok);
                            if (ok) {
                                // Summary may be at top-level or wrapped in "data"
                                JsonObject sumObj;
                                if (sumFetch["data"].is<JsonObject>())
                                    sumObj = sumFetch["data"].as<JsonObject>();
                                else if (sumFetch.as<JsonObject>().containsKey("id"))
                                    sumObj = sumFetch.as<JsonObject>();
                                if (!sumObj.isNull()) {
                                    summaryId = sumObj["id"] | 0;
                                    fetched   = (summaryId > 0);
                                }
                            }
                        } else { hf.end(); }
                    }

                    if (!fetched) {
                        Serial.printf("[WM] No summary found for uid=%s — skipping\n", empUid.c_str());
                        continue;
                    }

                    // ── b) Build PATCH payload: only the changed clock columns ─
                    // We include ALL 6 clock columns so the server can recalculate
                    // regular_hours, overtime_hours, is_late from the full picture.
                    // Unchanged columns are forwarded from the fetched summary.
                    JsonObject existingSum;
                    if (sumFetch["data"].is<JsonObject>())
                        existingSum = sumFetch["data"].as<JsonObject>();
                    else
                        existingSum = sumFetch.as<JsonObject>();

                    const char* clockCols[6] = {
                        "morning_in","morning_out",
                        "afternoon_in","afternoon_out",
                        "evening_in","evening_out"
                    };

                    DynamicJsonDocument putSum(512);
                    putSum["employee_uid"] = empUid;
                    putSum["date"]         = fileDateStr;

                    // Start from existing values
                    for (int c=0;c<6;c++) {
                        String existing = existingSum[clockCols[c]] | "";
                        putSum[clockCols[c]] = existing;
                    }

                    // Apply patches from the edited rows
                    for (int k=0; k<empPatches[pi].count; k++) {
                        String ct  = empPatches[pi].clocks[k].clockType;
                        String val = empPatches[pi].clocks[k].newTime;
                        // Clock column value format: "HH:MM:SS" or "YYYY-MM-DD HH:MM:SS"
                        // Server stores full datetime in summary columns
                        String fullVal = (val.length() == 8 || val.length() == 5)
                                         ? fileDateStr + " " + val
                                         : val;
                        for (int c=0;c<6;c++) {
                            if (ct == String(clockCols[c])) {
                                putSum[clockCols[c]] = fullVal;
                                break;
                            }
                        }
                    }

                    String putSumStr; serializeJson(putSum, putSumStr);
                    Serial.printf("[WM] PUT summary id=%d uid=%s\n", summaryId, empUid.c_str());
                    Serial.println("[WM] Summary payload: " + putSumStr);

                    // ── c) PUT /api/attendanceEdit/summary/:id ─────────────────
                    HTTPClient hps; hps.setTimeout(8000);
                    hps.begin(_serverURL + "/api/attendanceEdit/summary/" + String(summaryId));
                    hps.addHeader("Content-Type","application/json");
                    hps.addHeader("X-Client-Type","ESP32");
                    int pc = hps.PUT(putSumStr);
                    Serial.printf("[WM] PUT summary → %d\n", pc);
                    hps.end();
                    yield();
                }
                delete[] empPatches;  // free heap allocation
                empPatches = nullptr;
            }
        } else {
            serverErr = wifiUp ? "no server URL" : "offline";
        }

        DynamicJsonDocument resp(256);
        resp["success"]       = true;
        resp["rows_written"]  = written;
        resp["server_synced"] = synced;
        if (serverErr.length() > 0) resp["server_error"] = serverErr;
        String out; serializeJson(resp, out);
        Serial.println("[WM] Response: " + out);
        _srv.sendHeader("Access-Control-Allow-Origin","*");
        _srv.send(200,"application/json",out);
    }

    // ── ATTENDANCE DELETE ROW — remove one row from SD and notify server ──────
    void _apiAttendanceDeleteRow() {
        String body = _srv.arg("plain");
        DynamicJsonDocument req(512);
        if (deserializeJson(req, body) != DeserializationError::Ok) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"JSON parse error\"}");
            return;
        }

        String fileArg  = req["file"]      | "today";
        int    rowIndex = req["rowIndex"]   | -1;
        String empUid   = req["empUid"]     | "";
        String evType   = req["eventType"]  | "";
        String rowTs    = req["timestamp"]  | "";  // "HH:MM:SS" or "YYYY-MM-DD HH:MM:SS"
        int    serverId = req["serverId"]   | 0;   // passed from JS _serverIds[idx]

        String path = _resolveTodayPath(fileArg);

        // Derive date string for SD map lookup
        String fileDateStr = "";
        {
            String fn = path.substring(path.lastIndexOf('/') + 1);
            fn.replace(".csv","");
            if (fn.length() == 10 && fn.indexOf('-') == 4) fileDateStr = fn;
        }

        if (!SDDatabase::isReady()) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"SD not ready\"}");
            return;
        }
        if (!SD_MMC.exists(path)) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"File not found\"}");
            return;
        }

        // ── Stream-copy: read source line-by-line → write temp file, skip target row
        // NEVER buffer all lines in a String array — that overflows the stack.
        String tmpPath = path + ".tmp";

        File rf = SD_MMC.open(path, FILE_READ);
        if (!rf) {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"Read failed\"}");
            return;
        }
        if (SD_MMC.exists(tmpPath)) SD_MMC.remove(tmpPath);
        File wf = SD_MMC.open(tmpPath, FILE_WRITE);
        if (!wf) {
            rf.close();
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"Write failed\"}");
            return;
        }

        int lineNum = 0;   // 0 = header; 1..N = data rows (1-based)
        int dataRow = 0;   // 0-based data row counter (increments after header)
        bool skipped = false;

        while (rf.available()) {
            String line = rf.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;

            if (lineNum == 0) {
                // Always keep header
                wf.println(line);
            } else {
                // Data row: skip the target index
                if (dataRow == rowIndex) {
                    skipped = true;
                } else {
                    wf.println(line);
                }
                dataRow++;
            }
            lineNum++;
            yield();
        }
        rf.close();
        wf.flush();
        wf.close();

        // Swap: remove original, rename temp
        SD_MMC.remove(path);
        // SD_MMC has no rename — copy tmp → original then remove tmp
        File src = SD_MMC.open(tmpPath, FILE_READ);
        File dst = SD_MMC.open(path,    FILE_WRITE);
        if (src && dst) {
            uint8_t buf[256];
            while (src.available()) {
                int n = src.read(buf, sizeof(buf));
                if (n > 0) dst.write(buf, n);
                yield();
            }
            src.close(); dst.flush(); dst.close();
            SD_MMC.remove(tmpPath);
            Serial.printf("[WM] Deleted row %d from %s (skipped=%d)\n",
                          rowIndex, path.c_str(), (int)skipped);
        } else {
            if (src) src.close();
            if (dst) dst.close();
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"Rename failed\"}");
            return;
        }

        // ── Notify server via DELETE /api/attendanceEdit/:id ─────────────────
        // serverId is passed directly from JS (no range-query lookup needed)
        bool serverOk = false;
        if (serverId > 0 && _serverURL.length() > 0 && _cfg && _cfg->isConnected()) {
            HTTPClient hd; hd.setTimeout(6000);
            hd.begin(_serverURL + "/api/attendanceEdit/" + String(serverId));
            hd.addHeader("X-Client-Type","ESP32");
            int dc = hd.sendRequest("DELETE", "");
            serverOk = (dc == 200 || dc == 204);
            Serial.printf("[WM] DELETE /attendanceEdit/%d → %d\n", serverId, dc);
            hd.end();
        } else if (serverId == 0) {
            Serial.println("[WM] Delete: no serverId provided, SD only");
        }

        // ── Remove entry from SD server-ID map so future loads don't keep it ──
        if (fileDateStr.length() == 10 && empUid.length() > 0 && evType.length() > 0) {
            // Extract time-only portion from the row timestamp
            String timeOnly = rowTs;
            int sp2 = rowTs.indexOf(' ');
            if (sp2 >= 0) timeOnly = rowTs.substring(sp2 + 1);
            SDDatabase::removeServerIdMapping(fileDateStr, empUid, evType, timeOnly);
        }

        DynamicJsonDocument resp(128);
        resp["success"]   = true;
        resp["server_ok"] = serverOk;
        String out; serializeJson(resp, out);
        _srv.sendHeader("Access-Control-Allow-Origin","*");
        _srv.send(200,"application/json",out);
    }

    // Returns all available attendance dates from SD + today's real label
    void _apiAttendanceDates() {
        DynamicJsonDocument doc(4096);
        JsonArray dates = doc.createNestedArray("dates");

        // Today's label from NTP
        struct tm ti = {};
        char todayLabel[40] = "Today";
        if (getLocalTime(&ti, 0)) {
            strftime(todayLabel, sizeof(todayLabel), "Today (%a %b %d, %Y)", &ti);
        }
        doc["today_label"] = todayLabel;

        // Scan /attendance dir for CSV files, build human-readable labels
        if (SDDatabase::isReady()) {
            File dir = SD_MMC.open("/attendance");
            if (dir && dir.isDirectory()) {
                // Collect filenames first so we can sort newest-first
                String files[64]; int fc = 0;
                File e = dir.openNextFile();
                while (e && fc < 64) {
                    String fn = String(e.name());
                    if (!e.isDirectory() && fn.endsWith(".csv")) {
                        files[fc++] = fn;
                    }
                    e.close(); e = dir.openNextFile();
                }
                dir.close();

                // Simple reverse sort (newest day_XXXXXX.csv last numerically → reverse)
                for (int i = 0; i < fc - 1; i++)
                    for (int j = i+1; j < fc; j++)
                        if (files[i] < files[j]) { String tmp=files[i]; files[i]=files[j]; files[j]=tmp; }

                for (int i = 0; i < fc; i++) {
                    JsonObject entry = dates.createNestedObject();
                    entry["file"] = files[i];

                    // Try to derive a human date from the filename or file content
                    // Format attempt: day_XXXXXX.csv where XXXXXX = day index
                    String label = files[i]; // fallback
                    // If file has a timestamp in first data row, extract date from it
                    String fp = "/attendance/" + files[i];
                    File f = SD_MMC.open(fp, FILE_READ);
                    if (f) {
                        String firstLine, secondLine;
                        bool gotFirst = false;
                        while (f.available()) {
                            char c = f.read();
                            if (c == '\n') {
                                if (!gotFirst) { gotFirst = true; continue; }
                                break;
                            }
                            if (gotFirst) secondLine += c;
                            else firstLine += c;
                        }
                        f.close();
                        // secondLine is first data row: timestamp,nfc_uid,...
                        // timestamp format expected: HH:MM:SS or YYYY-MM-DD HH:MM:SS
                        secondLine.trim();
                        if (secondLine.length() > 0) {
                            // Extract date portion — if it starts with digits, try to parse
                            // Common formats: "08:32:11" (time only) or "2026-03-03 08:32"
                            int comma = secondLine.indexOf(',');
                            String ts = comma > 0 ? secondLine.substring(0, comma) : secondLine;
                            ts.trim();
                            // If it's a full datetime (contains space or dash-date)
                            if (ts.indexOf('-') == 4 && ts.length() >= 10) {
                                // YYYY-MM-DD ... parse
                                int yr = ts.substring(0,4).toInt();
                                int mo = ts.substring(5,7).toInt();
                                int dy = ts.substring(8,10).toInt();
                                if (yr > 2020 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {
                                    struct tm ft = {};
                                    ft.tm_year = yr - 1900; ft.tm_mon = mo - 1; ft.tm_mday = dy;
                                    mktime(&ft);
                                    char lb[32];
                                    strftime(lb, sizeof(lb), "%a %b %d, %Y", &ft);
                                    label = String(lb);
                                }
                            } else {
                                // Time-only timestamp — use file modification is unavailable on SD_MMC
                                // Fall back to filename number as day offset from a base
                                label = files[i]; // keep filename as label
                            }
                        }
                    }
                    entry["label"] = label;
                    yield();
                }
            }
        }
        String out; serializeJson(doc, out);
        _srv.sendHeader("Access-Control-Allow-Origin", "*");
        _srv.send(200, "application/json", out);
    }

    void _apiAttendance() {
        String fileArg = _srv.hasArg("f") ? _srv.arg("f") : "today";
        String csv = (fileArg=="today") ? SDDatabase::readTodayCSV()
                                        : SDDatabase::readCSV(fileArg);

        // ── SERVER FALLBACK: if SD has no records for today, fetch live ──────
        // Handles the boot window where seedTodayAttendanceFromServer() hasn't
        // finished yet (browser opened the Attendance tab before SD sync completed).
        //
        // Strategy:
        //   1. Try GET /api/attendance?date=… (raw per-row endpoint — always fresh)
        //   2. Build a temporary CSV in memory to display in the table.
        //   3. Do NOT write to SD here — that is the job of the background seeder.
        //
        // Using the RAW endpoint instead of esp32-sync because:
        //   • esp32-sync reads daily_attendance_summary which is populated by a
        //     server-side job that may not have run yet → returns empty data.
        //   • The raw endpoint reads the live attendance table directly and always
        //     has the freshest records regardless of the summary job state.
        bool pulledFromServer = false;
        if (fileArg == "today" && csv.length() == 0
            && _attSvc && _serverURL.length() > 0
            && _cfg && _cfg->isConnected()) {

            struct tm _fti = {}; getLocalTime(&_fti, 0);
            char _todayDate[12] = "";
            strftime(_todayDate, sizeof(_todayDate), "%Y-%m-%d", &_fti);

            if (strlen(_todayDate) == 10) {
                Serial.printf("[WM] SD empty — fetching live from server (%s)\n", _todayDate);

                // ── Try raw attendance endpoint ───────────────────────────────
                String _lurl = _serverURL + "/api/attendance?date="
                               + String(_todayDate)
                               + "&limit=500&sort_by=clock_time&sort_order=ASC";
                Serial.println("[WM] Live-fetch URL: " + _lurl);

                HTTPClient _lhc; _lhc.setTimeout(10000);
                _lhc.begin(_lurl);
                _lhc.addHeader("X-Client-Type", "ESP32");
                int _lcode = _lhc.GET();
                Serial.printf("[WM] Live-fetch HTTP %d\n", _lcode);

                if (_lcode == 200) {
                    String _lbody = _lhc.getString(); _lhc.end();
                    Serial.printf("[WM] Live-fetch body: %d bytes\n", (int)_lbody.length());

                    DynamicJsonDocument* _lDoc = psramFound()
                        ? new DynamicJsonDocument(131072)
                        : new DynamicJsonDocument(32768);

                    bool _ldec = false;
                    if (_attSvc) _ldec = _attSvc->decryptBody(_lbody, *_lDoc);
                    if (!_ldec)  _ldec = (deserializeJson(*_lDoc, _lbody) == DeserializationError::Ok);

                    if (_ldec) {
                        // Unwrap: { success, data: { data: [...] } } or { success, data: [...] }
                        JsonArray _larr;
                        if ((*_lDoc)["data"].is<JsonArray>())
                            _larr = (*_lDoc)["data"].as<JsonArray>();
                        else if ((*_lDoc)["data"].is<JsonObject>() &&
                                 (*_lDoc)["data"]["data"].is<JsonArray>())
                            _larr = (*_lDoc)["data"]["data"].as<JsonArray>();

                        if (!_larr.isNull() && _larr.size() > 0) {
                            Serial.printf("[WM] Live-fetch: building CSV from %d records\n",
                                          (int)_larr.size());
                            csv = "timestamp,nfc_uid,employee_uid,employee_name,department,clock_type,device_id\n";
                            for (JsonObject _rec : _larr) {
                                // clock_time may be "YYYY-MM-DD HH:MM:SS" — strip date
                                String _ct = String(_rec["clock_time"] | "");
                                int _sp = _ct.indexOf(' ');
                                if (_sp >= 0) _ct = _ct.substring(_sp + 1);

                                // Build full name from employee_name or first+last
                                String _fn = String(_rec["employee_name"] | "");
                                if (_fn.length() == 0) {
                                    String _f = String(_rec["first_name"] | "");
                                    String _l = String(_rec["last_name"]  | "");
                                    _fn = (_f + " " + _l); _fn.trim();
                                }

                                String _uid  = String(_rec["employee_uid"] | "");
                                String _dept = String(_rec["department"]   | "");
                                String _type = String(_rec["clock_type"]   | "");

                                csv += _ct + ",," + _uid + "," + _fn + ","
                                     + _dept + "," + _type + ",server_live\n";
                            }
                            pulledFromServer = true;
                            Serial.printf("[WM] Live-fetch: %d records loaded for display\n",
                                          (int)_larr.size());
                        } else {
                            Serial.println("[WM] Live-fetch: decoded OK but data array empty/missing");
                            // Log top-level keys for diagnosis
                            Serial.print("[WM] Live-fetch keys: ");
                            for (JsonPair kv : _lDoc->as<JsonObject>())
                                Serial.print(String(kv.key().c_str()) + " ");
                            Serial.println();
                        }
                    } else {
                        Serial.println("[WM] Live-fetch: decrypt/parse FAILED");
                        Serial.println("[WM] Body preview: " +
                                       _lbody.substring(0, min(200, (int)_lbody.length())));
                    }
                    delete _lDoc;
                } else {
                    _lhc.end();
                    Serial.printf("[WM] Live-fetch failed (HTTP %d) — table will be empty\n",
                                  _lcode);
                }
                Serial.flush();
            }
        }

        // ── SNAPSHOT FALLBACK: read esp32_YYYY-MM-DD.json when CSV still empty ─
        // The esp32-sync endpoint saves a per-employee summary JSON even when the
        // daily_attendance_summary aggregation job has not yet populated session
        // times (all nulls).  BUT the raw /api/attendance endpoint always has live
        // per-row data.  If that raw fetch also returned empty (e.g. body=0 after
        // a reboot, TCP not ready, heap pressure), we fall back to the snapshot
        // which at minimum shows which employees have ANY attendance today and
        // what times were recorded once the server aggregation catches up.
        // This is display-only (read from SD, not written to SD here).
        if (fileArg == "today" && csv.length() == 0 && !pulledFromServer
            && SDDatabase::isReady()) {

            struct tm _sti = {}; getLocalTime(&_sti, 0);
            char _snapDate[12] = "";
            strftime(_snapDate, sizeof(_snapDate), "%Y-%m-%d", &_sti);

            String snapPath = "/attendance/esp32_" + String(_snapDate) + ".json";
            if (SD_MMC.exists(snapPath.c_str())) {
                Serial.printf("[WM] CSV empty — trying snapshot: %s\n", snapPath.c_str());
                File sf = SD_MMC.open(snapPath.c_str(), FILE_READ);
                if (sf && sf.size() > 2) {
                    // The snapshot can be up to ~10KB for 43 employees.
                    // We read it in a streaming fashion to avoid large heap allocation.
                    // Build CSV rows directly from the JSON array.
                    // Format: timestamp,nfc_uid,employee_uid,employee_name,department,clock_type,device_id
                    String snapCsv = "timestamp,nfc_uid,employee_uid,employee_name,department,clock_type,device_id\n";
                    bool snapHasRows = false;

                    // Parse in a 16KB doc (snapshot is ~9-10KB for 43 employees)
                    size_t snapSz = sf.size();
                    DynamicJsonDocument* snapDoc = nullptr;
                    if (snapSz < 32768) {
                        snapDoc = new DynamicJsonDocument(max((size_t)16384, snapSz * 2));
                    }
                    if (snapDoc) {
                        DeserializationError snapErr = deserializeJson(*snapDoc, sf);
                        if (!snapErr && snapDoc->is<JsonArray>()) {
                            static const char* SNAP_COLS[] = {
                                "morning_in", "morning_out",
                                "afternoon_in", "afternoon_out",
                                "evening_in", "evening_out",
                                "overtime_in", "overtime_out", nullptr
                            };
                            for (JsonObject emp : snapDoc->as<JsonArray>()) {
                                String empUid  = emp["uid"]  | "";
                                String empName = emp["name"] | "";
                                String dept    = emp["dept"] | "";
                                if (empUid.length() == 0) continue;
                                for (int ci = 0; SNAP_COLS[ci]; ci++) {
                                    const char* col = SNAP_COLS[ci];
                                    if (emp[col].isNull()) continue;
                                    String tv = emp[col] | "";
                                    if (tv.length() == 0 || tv == "null") continue;
                                    // tv is already time-only (HH:MM:SS) from snapshot writer
                                    snapCsv += tv + ",," + empUid + "," + empName
                                             + "," + dept + "," + String(col) + ",snapshot\n";
                                    snapHasRows = true;
                                }
                            }
                        } else if (snapErr) {
                            Serial.printf("[WM] Snapshot parse error: %s\n", snapErr.c_str());
                        }
                        delete snapDoc;
                    }
                    sf.close();

                    if (snapHasRows) {
                        csv = snapCsv;
                        pulledFromServer = true;   // marks as non-editable (no SD row IDs)
                        Serial.println("[WM] Snapshot fallback: loaded for display");
                    } else {
                        Serial.println("[WM] Snapshot exists but has no populated session times yet");
                    }
                }
                if (sf) sf.close();
            }
        }

        DynamicJsonDocument doc(12288);
        JsonArray headers   = doc.createNestedArray("headers");
        JsonArray rows      = doc.createNestedArray("rows");
        JsonArray serverIds = doc.createNestedArray("serverIds"); // parallel to rows

        // Resolve human-readable date for this file
        // "today" → use NTP local time; named file → parse from first data row or filename
        char dateStr[32] = "";
        if (fileArg == "today") {
            struct tm ti = {};
            if (getLocalTime(&ti, 0)) strftime(dateStr, sizeof(dateStr), "%a %b %d, %Y", &ti);
            else strcpy(dateStr, "Today");
        } else {
            // Try to extract date from first data row timestamp
            int nl1 = csv.indexOf('\n');
            int nl2 = nl1 >= 0 ? csv.indexOf('\n', nl1+1) : -1;
            String dataRow = (nl1 >= 0 && nl2 > nl1) ? csv.substring(nl1+1, nl2) : "";
            dataRow.trim();
            int comma = dataRow.indexOf(',');
            String ts = comma > 0 ? dataRow.substring(0, comma) : dataRow;
            ts.trim();
            if (ts.indexOf('-') == 4 && ts.length() >= 10) {
                int yr = ts.substring(0,4).toInt(), mo = ts.substring(5,7).toInt(), dy = ts.substring(8,10).toInt();
                if (yr > 2020 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {
                    struct tm ft = {};
                    ft.tm_year = yr-1900; ft.tm_mon = mo-1; ft.tm_mday = dy;
                    mktime(&ft);
                    strftime(dateStr, sizeof(dateStr), "%a %b %d, %Y", &ft);
                }
            }
            if (strlen(dateStr) == 0) strlcpy(dateStr, fileArg.c_str(), sizeof(dateStr));
        }
        doc["date"] = dateStr;
        doc["pulled_from_server"] = pulledFromServer;

        if (csv.length() > 0) {
            int start=0, lineNum=0;
            while (start < (int)csv.length()) {
                int nl = csv.indexOf('\n',start);
                String line = (nl<0) ? csv.substring(start) : csv.substring(start,nl);
                line.trim();
                if (line.length() > 0) {
                    if (lineNum==0) {
                        int s=0;
                        while(s<=(int)line.length()){
                            int cm=line.indexOf(',',s);
                            headers.add(cm<0?line.substring(s):line.substring(s,cm));
                            if(cm<0)break; s=cm+1;
                        }
                    } else {
                        JsonArray row=rows.createNestedArray();
                        int s=0;
                        while(s<=(int)line.length()){
                            int cm=line.indexOf(',',s);
                            String cell=cm<0?line.substring(s):line.substring(s,cm);
                            row.add(cell);
                            if(cm<0)break; s=cm+1;
                        }
                    }
                    lineNum++;
                }
                if(nl<0)break; start=nl+1;
            }
        }

        // ── Fetch server record IDs for this date so the editor can PUT (not POST) ──
        // ── Build serverIds: SD map first, network lookup as fallback ─────────
        // Key format: "empUid|clockType|HH:MM:SS"  — includes time so duplicate
        // clock_type records (e.g. two morning_in rows) get different IDs.
        bool wifiUp = (_cfg && _cfg->isConnected());

        // Derive YYYY-MM-DD from the resolved file path
        String resolvedPath = _resolveTodayPath(fileArg == "today" ? "__today__" : fileArg);
        String fileDateStr  = "";
        {
            String fn = resolvedPath.substring(resolvedPath.lastIndexOf('/') + 1);
            fn.replace(".csv","");
            if (fn.length() == 10 && fn.indexOf('-') == 4) fileDateStr = fn;
        }

        // ── Step A: Load SD server-ID map (written by NFC scan on first POST) ──
        // This handles records that were synced during the scan — most reliable.
        DynamicJsonDocument sdMap(4096);
        if (fileDateStr.length() == 10) {
            String mapJson = SDDatabase::loadServerIdMapJson(fileDateStr);
            if (mapJson.length() > 2) { // "{}" = 2
                deserializeJson(sdMap, mapJson);
                Serial.printf("[WM] SD server_id map: %d entries\n", sdMap.size());
            }
        }

        // ── Step B: For rows missing from SD map, fetch from server ──────────
        // Build server-side lookup keyed on "empUid|clockType|HH:MM:SS" so
        // duplicate clock_type rows resolve to their correct individual IDs.
        const int MAP_SZ = 128;
        // ⚠ Heap-allocate — 128 Strings on the stack overflows with everything else in this function
        String* sKeys = new String[MAP_SZ];
        int*    sIds  = new int[MAP_SZ];
        memset(sIds, 0, sizeof(int) * MAP_SZ);
        int  sCount = 0;
        bool serverFetchOk = false;  // true only if we got a valid HTTP 200 + parsed response

        if (wifiUp && _serverURL.length() > 0 && rows.size() > 0 && fileDateStr.length() == 10) {
            HTTPClient hc; hc.setTimeout(6000);
            hc.begin(_serverURL + "/api/attendanceEdit/range?start_date=" +
                     fileDateStr + "&end_date=" + fileDateStr);
            hc.addHeader("X-Client-Type","ESP32");
            int code = hc.GET();
            Serial.printf("[WM] serverIds fetch code=%d\n", code);

            if (code == 200) {
                String rbody = hc.getString();
                hc.end();

                DynamicJsonDocument srvDoc(16384);
                bool ok = false;
                if (_attSvc) ok = _attSvc->decryptBody(rbody, srvDoc);
                if (!ok) ok = (deserializeJson(srvDoc, rbody) == DeserializationError::Ok);

                if (ok) {
                    serverFetchOk = true;
                    JsonArray srvArr;
                    if (srvDoc["data"].is<JsonArray>())
                        srvArr = srvDoc["data"].as<JsonArray>();
                    else if (srvDoc["data"]["data"].is<JsonArray>())
                        srvArr = srvDoc["data"]["data"].as<JsonArray>();

                    // Key includes time → no more collision on duplicate clock_types
                    for (JsonObject rec : srvArr) {
                        if (sCount >= MAP_SZ) break;
                        String eu = rec["employee_uid"] | "";
                        String ct = rec["clock_type"]   | "";
                        int    id = rec["id"]           | 0;
                        // clock_time may be "YYYY-MM-DD HH:MM:SS" — extract time only
                        String ct_time = rec["clock_time"] | "";
                        int sp = ct_time.indexOf(' ');
                        if (sp >= 0) ct_time = ct_time.substring(sp + 1);
                        if (eu.length() == 0 || id == 0) continue;
                        // Use time-keyed key; fallback to no-time key for old records
                        String k = eu + "|" + ct + "|" + ct_time;
                        sKeys[sCount] = k; sIds[sCount] = id; sCount++;
                    }
                    Serial.printf("[WM] Server map: %d records (time-keyed)\n", sCount);
                } else { hc.end(); }
            } else { hc.end(); }
        }

        // ── Step B2: Reconcile SD CSV — remove rows deleted on server ─────────
        // If we got a valid server response, any SD row whose server_id is in our
        // SD map but NOT in the server response was deleted on the server.
        // Rule: only remove rows that WERE synced (have entry in SD map).
        //       Offline-only rows (no map entry) are left untouched.
        if (serverFetchOk && fileDateStr.length() == 10) {
            // Build set of active server IDs for fast membership check
            bool anyStale = false;

            // Load SD map
            DynamicJsonDocument reconcileMap(4096);
            String mapJson2 = SDDatabase::loadServerIdMapJson(fileDateStr);
            bool mapLoaded = (mapJson2.length() > 2) &&
                             (deserializeJson(reconcileMap, mapJson2) == DeserializationError::Ok);

            if (mapLoaded && reconcileMap.size() > 0) {
                // Find stale keys: in SD map but server_id not in active sIds[]
                const int MAX_STALE = 32;
                String* staleKeys = new String[MAX_STALE];
                int staleCount = 0;

                for (JsonPair kv : reconcileMap.as<JsonObject>()) {
                    int mapSrvId = kv.value().as<int>();
                    if (mapSrvId <= 0) continue;
                    bool found = false;
                    for (int j = 0; j < sCount; j++) {
                        if (sIds[j] == mapSrvId) { found = true; break; }
                    }
                    if (!found && staleCount < MAX_STALE) {
                        staleKeys[staleCount++] = String(kv.key().c_str());
                        anyStale = true;
                        Serial.printf("[WM] Stale row detected (server_id=%d deleted): %s\n",
                                      mapSrvId, kv.key().c_str());
                    }
                }

                if (anyStale) {
                    // Stream-rewrite CSV, skipping rows whose key matches a stale entry
                    String tmpPath2  = resolvedPath + ".tmp";
                    String csvPath2  = resolvedPath;

                    File rf2 = SD_MMC.open(csvPath2, FILE_READ);
                    File wf2 = SD_MMC.open(tmpPath2, FILE_WRITE);

                    if (rf2 && wf2) {
                        int lineNum2 = 0;
                        int removedCount = 0;
                        while (rf2.available()) {
                            String line2 = rf2.readStringUntil('\n');
                            line2.trim();
                            if (line2.length() == 0) continue;

                            if (lineNum2 == 0) {
                                wf2.println(line2);  // always keep header
                            } else {
                                // Parse empUid (col 2), clockType (col 5), timestamp (col 0)
                                String cols[7]; int ci = 0, s2 = 0;
                                while (s2 <= (int)line2.length() && ci < 7) {
                                    int cm2 = line2.indexOf(',', s2);
                                    cols[ci++] = (cm2 < 0) ? line2.substring(s2) : line2.substring(s2, cm2);
                                    if (cm2 < 0) break; s2 = cm2 + 1;
                                }
                                String rowEu = cols[2]; rowEu.trim();
                                String rowCt = cols[5]; rowCt.trim();
                                String rowTs2 = cols[0]; rowTs2.trim();
                                // Extract time portion only
                                int sp3 = rowTs2.indexOf(' ');
                                String rowTime = (sp3 >= 0) ? rowTs2.substring(sp3 + 1) : rowTs2;
                                String rowKey = rowEu + "|" + rowCt + "|" + rowTime;

                                bool isStale = false;
                                for (int k2 = 0; k2 < staleCount; k2++) {
                                    if (staleKeys[k2] == rowKey) { isStale = true; break; }
                                }
                                if (isStale) {
                                    removedCount++;
                                    Serial.printf("[WM] Removing server-deleted row from SD: %s\n",
                                                  rowKey.c_str());
                                } else {
                                    wf2.println(line2);
                                }
                            }
                            lineNum2++;
                            yield();
                        }
                        rf2.close(); wf2.flush(); wf2.close();

                        // Copy tmp → original (SD_MMC has no rename)
                        SD_MMC.remove(csvPath2);
                        File src2 = SD_MMC.open(tmpPath2, FILE_READ);
                        File dst2 = SD_MMC.open(csvPath2, FILE_WRITE);
                        if (src2 && dst2) {
                            uint8_t buf2[256];
                            while (src2.available()) {
                                int n = src2.read(buf2, sizeof(buf2));
                                if (n > 0) dst2.write(buf2, n);
                                yield();
                            }
                            src2.close(); dst2.flush(); dst2.close();
                            SD_MMC.remove(tmpPath2);
                            Serial.printf("[WM] SD reconcile: removed %d server-deleted rows\n",
                                          removedCount);
                        } else {
                            if (src2) src2.close();
                            if (dst2) dst2.close();
                        }

                        // Remove stale keys from SD map
                        for (int k2 = 0; k2 < staleCount; k2++) {
                            reconcileMap.remove(staleKeys[k2].c_str());
                        }
                        String mapPath = "/attendance/server_ids_" + fileDateStr + ".json";
                        File mf = SD_MMC.open(mapPath, FILE_WRITE);
                        if (mf) { serializeJson(reconcileMap, mf); mf.close(); }

                        // Tell JS how many rows were purged — it will re-fetch automatically
                        if (removedCount > 0) {
                            doc["rows_removed"] = removedCount;
                            Serial.printf("[WM] B2: %d stale rows removed, JS will reload\n",
                                          removedCount);
                        }
                    } else {
                        if (rf2) rf2.close();
                        if (wf2) wf2.close();
                    }
                }
                delete[] staleKeys;
            }
        }

        // ── Step C: Match each CSV row to its server ID ───────────────────
        for (JsonArray row : rows) {
            String eu = (String)(row[2] | ""); eu.trim();
            String ct = (String)(row[5] | ""); ct.trim();
            // Extract time from timestamp (col 0): "YYYY-MM-DD HH:MM:SS" or "HH:MM:SS"
            String ts = (String)(row[0] | ""); ts.trim();
            int sp = ts.indexOf(' ');
            String timeOnly = (sp >= 0) ? ts.substring(sp + 1) : ts;

            int sid = 0;

            // 1. SD map (most reliable — written at NFC scan time)
            String sdKey = eu + "|" + ct + "|" + timeOnly;
            if (sdMap.containsKey(sdKey)) {
                sid = sdMap[sdKey] | 0;
            }

            // 2. Network map fallback (for records synced before this fix)
            if (sid == 0) {
                String netKey = eu + "|" + ct + "|" + timeOnly;
                for (int j = 0; j < sCount; j++) {
                    if (sKeys[j] == netKey) { sid = sIds[j]; break; }
                }
            }

            serverIds.add(sid);
        }


        String out; serializeJson(doc,out);
        delete[] sKeys;
        delete[] sIds;
        _srv.sendHeader("Access-Control-Allow-Origin","*");
        _srv.send(200,"application/json",out);
    }

    void _apiScan() {
        if (_srv.hasArg("rescan")) {
            _startScan();
        }
        DynamicJsonDocument doc(4096);
        if (_scanRunning) {
            doc["scanning"] = true; doc["count"] = 0;
        } else {
            doc["scanning"] = false;
            int n = (_scanResult > 0) ? _scanResult : 0;
            doc["count"] = n;
            JsonArray nets = doc.createNestedArray("networks");
            for (int i=0; i<n; i++) {
                JsonObject net = nets.createNestedObject();
                net["ssid"] = WiFi.SSID(i);
                net["rssi"] = WiFi.RSSI(i);
                net["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
            }
        }
        String out; serializeJson(doc,out);
        _srv.sendHeader("Access-Control-Allow-Origin","*");
        _srv.send(200,"application/json",out);
    }

    void _apiWifiInfo() {
        DynamicJsonDocument doc(256);
        bool con = _cfg && _cfg->isConnected();
        doc["connected"] = con;
        doc["ssid"]      = con ? _cfg->getSSID()      : "";
        doc["ip"]        = con ? _cfg->getIPAddress() : "";
        doc["ap_ip"]     = WiFi.softAPIP().toString();
        String out; serializeJson(doc,out);
        _srv.sendHeader("Access-Control-Allow-Origin","*");
        _srv.send(200,"application/json",out);
    }

    void _apiConnect() {
        String body = _srv.arg("plain");
        DynamicJsonDocument req(256);
        deserializeJson(req, body);
        String ssid = req["ssid"] | "";
        String pass = req["password"] | "";
        if (ssid.length()==0) { _srv.send(200,"application/json","{\"success\":false,\"error\":\"No SSID\"}"); return; }

        Serial.println("[WiFi] Connecting to: " + ssid);
        WiFi.begin(ssid.c_str(), pass.length()>0 ? pass.c_str() : nullptr);
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500); attempts++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            if (_cfg) _cfg->connectToWiFi(ssid, pass);
            DynamicJsonDocument r(128);
            r["success"]=true; r["ip"]=WiFi.localIP().toString();
            String out; serializeJson(r,out);
            _srv.send(200,"application/json",out);
            _startScan();
        } else {
            _srv.send(200,"application/json","{\"success\":false,\"error\":\"Connection failed\"}");
        }
    }

    void _apiDisconnect() {
        WiFi.disconnect();
        _srv.send(200,"application/json","{\"success\":true}");
        _startScan();
    }

    void _apiReboot() {
        _srv.send(200,"application/json","{\"success\":true,\"message\":\"Rebooting...\"}");
        delay(500); ESP.restart();
    }
};