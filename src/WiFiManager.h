// ════════════════════════════════════════════════════════════════════════════
// WiFiManager.h  — Web Admin Portal for ESP32-S3 NFC Attendance
//
// Tabs (in order):
//   1. Dashboard  — today's stats + quick actions
//   2. SD Files   — raw SD card browser: list ALL files, view/download any file
//   3. Attendance — today's log table + CSV downloads
//   4. Actions    — reboot, clear cache, manage employees
//   5. WiFi       — moved LAST; background scan runs silently
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

// ════════════════════════════════════════════════════════════════════════════
class WiFiManager {
public:
    WiFiManager() : _srv(WM_PORT) {}

    void init(const String& deviceId, WiFiConfig* cfg) {
        _deviceId = deviceId;
        _cfg      = cfg;
        _setupRoutes();
        // Must register Cookie header BEFORE begin() so _authed() works.
        // IMPORTANT: array must be static — WebServer stores a raw pointer,
        // so a local array would dangle after init() returns.
        static const char* _hdrs[] = {"Cookie"};
        _srv.collectHeaders(_hdrs, 1);
        _srv.begin();
        Serial.printf("[WM] Portal at http://x.x.x.x:%d\n", WM_PORT);
        // Kick off first silent background scan
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

    // ── Auth check ────────────────────────────────────────────────────────────
    bool _authed() {
        if (_srv.hasHeader("Cookie")) {
            String c = _srv.header("Cookie");
            if (c.indexOf(WM_SESS) >= 0) return true;
        }
        return false;
    }

    // ── Current device IP ─────────────────────────────────────────────────────
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
.dot-green{background:var(--green)} .dot-red{background:var(--red)}
.dot-yellow{background:#f59e0b} .dot-dim{background:var(--dim)}
.alert{padding:10px 14px;border-radius:7px;font-size:.83rem;margin-top:10px;display:none}
.alert-ok{background:rgba(16,185,129,.1);color:var(--green);border:1px solid rgba(16,185,129,.2)}
.alert-err{background:rgba(239,68,68,.1);color:var(--red);border:1px solid rgba(239,68,68,.2)}
.file-row{display:flex;align-items:center;gap:8px;padding:6px 10px;
  background:rgba(255,255,255,.02);border:1px solid var(--border);
  border-radius:6px;margin-bottom:5px}
.file-row .fn{font-size:.82rem;flex:1;font-family:monospace;color:var(--cyan);word-break:break-all}
.file-row .fsz{font-size:.72rem;color:var(--dim);white-space:nowrap}
.file-content{background:#000;border:1px solid var(--border);border-radius:6px;
  padding:12px;font-family:monospace;font-size:.78rem;color:#a3e635;
  white-space:pre-wrap;word-break:break-all;max-height:400px;overflow-y:auto;margin-top:10px}
#loading{display:none;text-align:center;color:var(--dim);padding:20px;font-size:.85rem}
.spinner{display:inline-block;width:16px;height:16px;border:2px solid var(--border);
  border-top-color:var(--cyan);border-radius:50%;animation:spin .6s linear infinite;margin-right:6px}
@keyframes spin{to{transform:rotate(360deg)}}
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

        // API
        _srv.on("/api/status",     HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiStatus(); });
        _srv.on("/api/sd/list",    HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiSdList(); });
        _srv.on("/api/sd/read",    HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiSdRead(); });
        _srv.on("/api/sd/dl",      HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiSdDownload(); });
        _srv.on("/api/sd/tree",    HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiSdTree(); });
        _srv.on("/api/attendance", HTTP_GET,  [this](){ if(!_authed()){_srv.send(401);}else _apiAttendance(); });
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
.err{color:#ef4444;font-size:.8rem;margin-top:8px;text-align:center;display:none}
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
        html += "<div class='stat-row'>";
        html += "<div class='stat'><div class='stat-n' style='color:#10b981'>" + String(ins) + "</div><div class='stat-l'>Clock-ins today</div></div>";
        html += "<div class='stat'><div class='stat-n' style='color:#f97316'>" + String(outs) + "</div><div class='stat-l'>Clock-outs today</div></div>";
        html += "<div class='stat'><div class='stat-n' style='color:#22d3ee'>" + String(freeMB) + "<span style='font-size:1rem'>MB</span></div><div class='stat-l'>SD Free</div></div>";
        html += "<div class='stat'><div class='stat-n' style='color:" + String(wOk?"#10b981":"#ef4444") + ";font-size:1rem'>" + (wOk ? "Online" : "Offline") + "</div><div class='stat-l'>WiFi</div></div>";
        html += "</div>";

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
    // TAB 1 — SD FILES (full raw browser)
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
    var nameDisplay=item.name;
    h+='<div class="file-row '+(isDir?'dir-row':'')+'">';
    h+='<span style="font-size:1rem">'+icon+'</span>';
    h+='<span class="fn">'+item.path+'</span>';
    h+='<span class="fsz">'+sz+'</span>';
    if(isDir){
      h+='<button class="btn btn-ghost btn-sm" onclick="navDir(\''+item.path+'\')">Open</button>';
    } else {
      h+='<button class="btn btn-ghost btn-sm" onclick="viewFile(\''+item.path+'\')">View</button>';
      h+='<a class="btn btn-primary btn-sm" href="/api/sd/dl?f='+encodeURIComponent(item.path)+'" download="'+item.name+'">DL</a>';
    }
    h+='</div>';
  });
  document.getElementById('filelist').innerHTML=h;
}
function navDir(path){
  document.getElementById('pathInput').value=path;
  browseDir();
}
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
    // TAB 2 — ATTENDANCE LOG
    // ════════════════════════════════════════════════════════════════════════
    void _handleAttendance() {
        String html = _head("Attendance", 2);
        html += R"HTML(
<div class="card">
  <div class="card-title">Today's Log</div>
  <div style="display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap">
    <select id="dateSelect" onchange="loadCsv(this.value)" style="width:auto;flex:1">
      <option value="today">Today</option>
    </select>
    <a id="dlCsvBtn" href="#" class="btn btn-primary btn-sm" download="attendance.csv">Download CSV</a>
  </div>
  <div id="tableArea"><p style="color:#64748b;font-size:.82rem">Loading...</p></div>
</div>
<script>
fetch('/api/sd/list').then(r=>r.json()).then(function(d){
  var sel=document.getElementById('dateSelect');
  (d.files||[]).forEach(function(f){
    var o=document.createElement('option');o.value=f;o.textContent=f;sel.appendChild(o);
  });
});
function loadCsv(val){
  var url='/api/attendance?f='+encodeURIComponent(val);
  document.getElementById('dlCsvBtn').href='/api/sd/dl?f='+encodeURIComponent(val==='today'?'__today__':val);
  fetch(url).then(r=>r.json()).then(function(d){
    if(!d.rows||d.rows.length===0){
      document.getElementById('tableArea').innerHTML='<p style="color:#64748b;font-size:.82rem">No records.</p>';
      return;
    }
    var h='<div style="overflow-x:auto"><table><thead><tr>';
    (d.headers||[]).forEach(function(hd){h+='<th>'+hd+'</th>';});
    h+='</tr></thead><tbody>';
    d.rows.forEach(function(row){
      h+='<tr>';
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
    // TAB 4 — WIFI (moved last; uses background scan result)
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

    // SD tree: list directory entries with type + size
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
        // Add parent directory entry
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
            // entry.name() returns just the name, not full path
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

    // SD list: just the attendance CSV filenames
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

    // Read a file from SD and return as plain text (for view)
    void _apiSdRead() {
        if (!_srv.hasArg("f")) { _srv.send(400,"text/plain","Missing f param"); return; }
        String path = _srv.arg("f");
        // Security: no path traversal
        if (path.indexOf("..") >= 0) { _srv.send(403,"text/plain","Forbidden"); return; }
        if (!SDDatabase::isReady()) { _srv.send(503,"text/plain","SD not ready"); return; }
        if (!SD_MMC.exists(path)) { _srv.send(404,"text/plain","Not found"); return; }
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) { _srv.send(500,"text/plain","Open failed"); return; }

        // Check if it's a binary file by extension — send hex dump for binary
        String ext = path.substring(path.lastIndexOf('.') + 1);
        ext.toLowerCase();
        bool isBinary = (ext=="jpg"||ext=="jpeg"||ext=="png"||ext=="bin"||ext=="dat");

        if (isBinary) {
            // For binary files, show a hex preview (first 512 bytes)
            uint8_t buf[512]; size_t n = f.read(buf, sizeof(buf)); f.close();
            String hex = "[Binary file: " + path + " | size preview (hex)]\n";
            for (size_t i=0;i<n;i++) {
                char b[4]; snprintf(b,sizeof(b),"%02X ",buf[i]); hex+=b;
                if ((i+1)%16==0) hex+="\n";
            }
            _srv.sendHeader("Access-Control-Allow-Origin","*");
            _srv.send(200,"text/plain",hex);
        } else {
            // Stream text file
            String content;
            size_t sz = f.size();
            if (sz > 32768) {
                // Too large — return first 32KB
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

    // Download file from SD (proper Content-Disposition)
    void _apiSdDownload() {
        if (!_srv.hasArg("f")) { _srv.send(400,"text/plain","Missing f"); return; }
        String path = _srv.arg("f");
        if (path.indexOf("..") >= 0) { _srv.send(403,"text/plain","Forbidden"); return; }
        if (!SDDatabase::isReady()) { _srv.send(503,"text/plain","SD not ready"); return; }

        // Special token for today's file
        if (path == "__today__") path = SDDatabase::readTodayCSV().length() > 0
            ? "/attendance/day_" + String(millis()/86400000UL) + ".csv"
            : "";

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

    // Attendance log as JSON rows
    void _apiAttendance() {
        String fileArg = _srv.hasArg("f") ? _srv.arg("f") : "today";
        String csv = (fileArg=="today") ? SDDatabase::readTodayCSV()
                                        : SDDatabase::readCSV(fileArg);
        DynamicJsonDocument doc(8192);
        JsonArray headers = doc.createNestedArray("headers");
        JsonArray rows    = doc.createNestedArray("rows");

        if (csv.length() > 0) {
            int start=0, lineNum=0;
            while (start < (int)csv.length()) {
                int nl = csv.indexOf('\n',start);
                String line = (nl<0) ? csv.substring(start) : csv.substring(start,nl);
                line.trim();
                if (line.length() > 0) {
                    if (lineNum==0) {
                        // header row
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

    // WiFi scan — returns cached result; triggers new scan if requested
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
            // Credentials saved by WiFiConfig internally after connectToWiFi
            if (_cfg) _cfg->connectToWiFi(ssid, pass);
            DynamicJsonDocument r(128);
            r["success"]=true; r["ip"]=WiFi.localIP().toString();
            String out; serializeJson(r,out);
            _srv.send(200,"application/json",out);
            // restart background scan with new connection context
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