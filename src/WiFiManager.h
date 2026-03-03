// ════════════════════════════════════════════════════════════════════════════
// WiFiManager.h  — Web Admin Portal for ESP32-S3 NFC Attendance
//
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

    void init(const String& deviceId, WiFiConfig* cfg) {
        _deviceId = deviceId;
        _cfg      = cfg;
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
    WiFiConfig* _cfg = nullptr;

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

        _srv.on("/api/attendance",       HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiAttendance(); });
        _srv.on("/api/attendance/dates", HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiAttendanceDates(); });
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
    // TAB 2 — ATTENDANCE LOG (NEW: Raw Editor added to fix timestamps)
    // ════════════════════════════════════════════════════════════════════════
    void _handleAttendance() {
        // Get today's real date for the default option label
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
    <button class="btn btn-ghost btn-sm" onclick="editCsv()">&#9998; Edit Raw Log</button>
  </div>
  <div id="dateLabel" style="font-size:.78rem;color:#64748b;margin-bottom:10px"></div>
  <div id="tableArea"><p style="color:#64748b;font-size:.82rem">Loading...</p></div>
  <div id="editArea" style="display:none;">
    <textarea id="csvEditor" style="width:100%;height:300px;background:#000;color:#a3e635;font-family:monospace;padding:10px;border:1px solid var(--border);border-radius:6px;line-height:1.5" spellcheck="false"></textarea>
    <div style="margin-top:8px;display:flex;gap:8px">
      <button class="btn btn-primary" onclick="saveCsv()">Save Changes</button>
      <button class="btn btn-ghost" onclick="cancelEdit()">Cancel</button>
    </div>
    <p style="color:var(--dim);font-size:0.75rem;margin-top:8px;">Format: timestamp,nfc_uid,employee_uid,employee_name,department,event_type,device_id</p>
  </div>
</div>
<script>
var currentFile = '';
var currentDate = '';

// Load date list from /api/attendance/dates
fetch('/api/attendance/dates').then(r=>r.json()).then(function(d){
  var sel = document.getElementById('dateSelect');
  sel.innerHTML = '';
  // Today option first
  var todayOpt = document.createElement('option');
  todayOpt.value = 'today';
  todayOpt.textContent = d.today_label || 'Today';
  sel.appendChild(todayOpt);
  // Past dates from SD, newest first
  (d.dates||[]).forEach(function(entry){
    var o = document.createElement('option');
    o.value = entry.file;
    o.textContent = entry.label;  // e.g. "Mon Mar 03, 2026"
    sel.appendChild(o);
  });
  loadCsv('today');
});

function loadCsv(val){
  currentFile = (val==='today') ? '__today__' : '/attendance/'+val;
  var url='/api/attendance?f='+encodeURIComponent(val);
  document.getElementById('dlCsvBtn').href='/api/sd/dl?f='+encodeURIComponent(currentFile);

  // Show selected date label
  var sel = document.getElementById('dateSelect');
  var selText = sel.options[sel.selectedIndex] ? sel.options[sel.selectedIndex].textContent : '';
  document.getElementById('dateLabel').textContent = selText ? ('Showing records for: ' + selText) : '';

  fetch(url).then(r=>r.json()).then(function(d){
    if(!d.rows||d.rows.length===0){
      document.getElementById('tableArea').innerHTML='<p style="color:#64748b;font-size:.82rem">No records for this date.</p>';
      return;
    }
    var h='<div style="overflow-x:auto"><table><thead><tr>';
    // Inject Date as first column header, then existing headers
    h += '<th style="color:#22d3ee">Date</th>';
    (d.headers||[]).forEach(function(hd){h+='<th>'+hd+'</th>';});
    h+='</tr></thead><tbody>';
    d.rows.forEach(function(row){
      h+='<tr>';
      // Date cell — use d.date from API (the file's date), or parse from timestamp
      var dateStr = d.date || '';
      h+='<td style="color:#22d3ee;white-space:nowrap;font-size:.78rem">'+dateStr+'</td>';
      row.forEach(function(cell,i){
        if(i===5){
          var cls=cell==='check-in'?'badge-in':cell==='check-out'?'badge-out':'badge-denied';
          h+='<td><span class="badge '+cls+'">'+cell+'</span></td>';
        } else {h+='<td>'+cell+'</td>';}
      });
      h+='</tr>';
    });
    h+='</tbody></table></div>';
    document.getElementById('tableArea').innerHTML=h;
  });
}

function editCsv() {
  document.getElementById('tableArea').style.display='none';
  document.getElementById('editArea').style.display='block';
  document.getElementById('csvEditor').value = 'Loading...';
  fetch('/api/sd/read?f='+encodeURIComponent(currentFile))
    .then(r=>r.text()).then(t=>{ document.getElementById('csvEditor').value=t; });
}

function cancelEdit() {
  document.getElementById('tableArea').style.display='block';
  document.getElementById('editArea').style.display='none';
}

function saveCsv() {
  var t = document.getElementById('csvEditor').value;
  if(!confirm("Are you sure you want to overwrite the log file?")) return;
  fetch('/api/sd/write?f='+encodeURIComponent(currentFile), {
    method: 'POST', body: t, headers: {'Content-Type':'text/plain'}
  }).then(r=>r.json()).then(d=>{
    if(d.success){ 
        alert('Saved successfully!'); 
        cancelEdit(); 
        loadCsv(document.getElementById('dateSelect').value); 
    }
    else alert('Failed to save file.');
  });
}

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

    // Resolves __today__ macro for read/write endpoints
    String _resolveTodayPath(String p) {
        if (p == "__today__") {
            unsigned long day = millis() / 86400000UL;
            char buf[40]; snprintf(buf, sizeof(buf), "/attendance/day_%06lu.csv", day);
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
        DynamicJsonDocument doc(8192);
        JsonArray headers = doc.createNestedArray("headers");
        JsonArray rows    = doc.createNestedArray("rows");

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
        String out; serializeJson(doc,out);
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