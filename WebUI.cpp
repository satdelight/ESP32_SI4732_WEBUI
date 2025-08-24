#include "WebUI.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>

// Aus deinem bestehenden Sketch:
#include <Wire.h>
#include <SI4735.h>

// Symbole aus ESP32_SI4732_WEBUI.ino
extern SI4735   rx;
extern uint16_t currentFrequency; // FM: 10-kHz-Einheiten; AM/SW/LW: kHz
extern uint8_t  currentMode;      // 0=FM, 1=LSB, 2=USB, 3=AM
extern void     oledShowFrequencyScreen();
extern void     setBand(int8_t up_down);
extern void     doMode(int8_t v);

static AsyncWebServer server(80);

// ---------------- Hilfsfunktionen ----------------
static inline const char* modeToStr(uint8_t m) {
  switch (m) {
    case 0: return "FM";
    case 1: return "LSB";
    case 2: return "USB";
    case 3: return "AM";
    default: return "UNK";
  }
}

// Frequenz-String wie auf OLED, inkl. Einheit
static String makeFreqString() {
  char tmp[8], out[16];
  sprintf(tmp, "%5.5u", currentFrequency);
  if (rx.isCurrentTuneFM()) {
    // 10130 -> "101.3 MHz"
    out[0] = tmp[0];
    out[1] = tmp[1];
    out[2] = tmp[2];
    out[3] = '.';
    out[4] = tmp[3];
    out[5] = '\0';
    String s(out);
    s += " MHz";
    return s;
  } else {
    // kHz (führende Nullen wie OLED)
    out[0] = (tmp[0] == '0') ? ' ' : tmp[0];
    out[1] = tmp[1];
    if (currentFrequency < 1000) { out[1] = ' '; out[2] = tmp[2]; out[3] = tmp[3]; out[4] = tmp[4]; out[5]='\0'; }
    else { out[2] = tmp[2]; out[3] = tmp[3]; out[4] = tmp[4]; out[5]='\0'; }
    String s(out);
    s.trim();
    s += " kHz";
    return s;
  }
}

static void setFrequencySafe(uint16_t v) {
  currentFrequency = v;
  rx.setFrequency(currentFrequency);
  oledShowFrequencyScreen();
}

static void tuneDelta(int delta) {
  int32_t f = (int32_t)currentFrequency + delta;
  if (f < 1) f = 1;
  if (f > 300000) f = 300000; // Schutz
  setFrequencySafe((uint16_t)f);
}

// ---------------- HTML-Seiten ----------------
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 SI4732 Web UI</title>
  <style>
    body{font-family:system-ui,Arial,sans-serif;margin:16px}
    .row{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0}
    button{font-size:16px;padding:8px 12px}
    .stat{font-size:18px;margin:4px 0}
    code{background:#f3f3f3;padding:2px 6px;border-radius:4px}
    small{color:#666}
    a{color:#06c;text-decoration:none}
  </style>
</head>
<body>
  <h1>ESP32 SI4732</h1>
  <div class="stat">Frequenz: <span id="freq">-</span></div>
  <div class="stat">Mode: <span id="mode">-</span></div>
  <div class="stat">RSSI: <span id="rssi">-</span> dBμ, SNR: <span id="snr">-</span> dB</div>

  <div class="row">
    <button id="b1">-</button>
    <button id="b2">-</button>
    <button id="b3">-</button>
    <button id="b4">-</button>
  </div>
  <small id="hint">Hinweis: FM nutzt 10‑kHz‑Einheiten (z. B. 10 =&gt; 100 kHz). AM/SW/LW nutzen kHz.</small>

  <div class="row" style="margin-top:12px">
    <input id="in" type="number" min="1" step="1" style="width:160px" placeholder="Wert">
    <button id="setbtn">Set</button>
  </div>

  <div class="row">
    <button id="modebtn">Mode wechseln</button>
  </div>

  <div class="row">
    <button id="bandL">&lt; Band</button>
    <button id="bandR">Band &gt;</button>
  </div>

  <p><a href="/wifi">WLAN konfigurieren</a></p>

  <p>API: <code>/api/status</code>, <code>/api/tune?delta=10</code>, <code>/api/setfreq?khz=7100</code> oder <code>/api/setfreq?val=7100</code>, <code>/api/mode?next=1</code>, <code>/api/band?dir=1</code></p>

<script>
const btns = [
  document.getElementById('b1'),
  document.getElementById('b2'),
  document.getElementById('b3'),
  document.getElementById('b4'),
];

async function getStatus(){
  try{
    const r = await fetch('/api/status'); const j = await r.json();
    document.getElementById('freq').textContent = j.freq_str || j.freq_khz;
    document.getElementById('mode').textContent = j.mode;
    document.getElementById('rssi').textContent = j.rssi_dbuv;
    document.getElementById('snr').textContent = j.snr_db;
    updateButtons(j.mode);
  }catch(e){}
}

function updateButtons(mode){
  if(mode === 'FM'){
    const defs = [
      {label:'-100 kHz', d:-10},
      {label:'-10 kHz',  d:-1},
      {label:'+10 kHz',  d:+1},
      {label:'+100 kHz', d:+10},
    ];
    defs.forEach((o,i)=>{ btns[i].textContent=o.label; btns[i].dataset.delta=o.d; });
    document.getElementById('hint').textContent = 'Hinweis: FM nutzt 10‑kHz‑Einheiten. AM/SW/LW nutzen kHz.';
  }else{
    const defs = [
      {label:'-100 kHz', d:-100},
      {label:'-10 kHz',  d:-10},
      {label:'+10 kHz',  d:+10},
      {label:'+100 kHz', d:+100},
    ];
    defs.forEach((o,i)=>{ btns[i].textContent=o.label; btns[i].dataset.delta=o.d; });
    document.getElementById('hint').textContent = 'Hinweis: AM/SW/LW nutzen kHz. FM nutzt 10‑kHz‑Einheiten.';
  }
}

async function tuneFromBtn(ev){
  const d = ev.target.dataset.delta;
  if(!d) return;
  await fetch('/api/tune?delta='+encodeURIComponent(d));
  getStatus();
}
btns.forEach(b=>b.addEventListener('click', tuneFromBtn));

document.getElementById('setbtn').addEventListener('click', async ()=>{
  const v = document.getElementById('in').value;
  if(!v) return;
  await fetch('/api/setfreq?val='+encodeURIComponent(v));
  getStatus();
});

document.getElementById('modebtn').addEventListener('click', async ()=>{
  await fetch('/api/mode?next=1'); getStatus();
});
document.getElementById('bandL').addEventListener('click', async ()=>{
  await fetch('/api/band?dir=-1'); getStatus();
});
document.getElementById('bandR').addEventListener('click', async ()=>{
  await fetch('/api/band?dir=1'); getStatus();
});

getStatus(); setInterval(getStatus, 1000);
</script>
</body>
</html>
)HTML";

static const char WIFI_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="de">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>WLAN konfigurieren</title>
  <style>
    body{font-family:system-ui,Arial,sans-serif;margin:16px}
    input,button{font-size:16px;padding:8px}
    form{display:flex;flex-direction:column;gap:8px;max-width:360px}
  </style>
</head>
<body>
  <h1>WLAN konfigurieren</h1>
  <form method="post" action="/wifi">
    <label>SSID<br><input type="text" name="ssid" required></label>
    <label>Passwort<br><input type="password" name="pass"></label>
    <button type="submit">Verbinden & Speichern</button>
  </form>
  <p>Tipp: Nach erfolgreicher Verbindung startet das Gerät neu.</p>
</body>
</html>
)HTML";

// ---------------- Webserver Setup ----------------
static void setupRoutes() {
  // UI
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });

  // WLAN-Formular
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html; charset=utf-8", WIFI_HTML);
  });

  // WLAN speichern (POST oder GET mit Parametern)
  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest* req) {
    String ssid = req->hasParam("ssid", true) ? req->getParam("ssid", true)->value() : "";
    String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    if (ssid.length() == 0) { req->send(400, "text/plain", "Missing SSID"); return; }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true); // Zugangsdaten dauerhaft speichern
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      String msg = "Verbunden mit " + WiFi.SSID() + " - IP: " + WiFi.localIP().toString() + "<br>Neustart...";
      req->send(200, "text/html; charset=utf-8", msg);
      delay(1000);
      ESP.restart();
    } else {
      req->send(200, "text/html; charset=utf-8", "Verbindung fehlgeschlagen. Bitte pruefe SSID/Passwort und versuche es erneut.");
    }
  });

  // Status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    int8_t rssi = 0, snr = 0, mult = 0, freqOfs = 0;
    rx.getCurrentReceivedSignalQuality(&rssi, &snr, &mult, &freqOfs);

    StaticJsonDocument<320> doc;
    doc["freq_khz"] = currentFrequency;     // Rohwert (FM=10kHz Schritte; AM=kHz)
    doc["freq_str"] = makeFreqString();     // Formatierter String
    doc["mode"]     = modeToStr(currentMode);
    doc["rssi_dbuv"]= rssi;
    doc["snr_db"]   = snr;
    doc["freq_ofs"] = freqOfs;
    doc["net_mode"] = (WiFi.getMode() & WIFI_AP) ? "AP" : "STA";
    doc["ip"]       = ((WiFi.getMode() & WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP()).toString();

    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // Tuning
  server.on("/api/tune", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("delta")) { req->send(400, "text/plain", "missing delta"); return; }
    int delta = req->getParam("delta")->value().toInt();
    tuneDelta(delta);
    req->send(200, "text/plain", "OK");
  });

  // Direkt setzen (?khz= oder ?val=)
  server.on("/api/setfreq", HTTP_GET, [](AsyncWebServerRequest* req) {
    uint16_t v = 0;
    if (req->hasParam("khz")) {
      v = (uint16_t) req->getParam("khz")->value().toInt();
    } else if (req->hasParam("val")) {
      v = (uint16_t) req->getParam("val")->value().toInt();
    } else {
      req->send(400, "text/plain", "missing khz/val"); return;
    }
    setFrequencySafe(v);
    req->send(200, "text/plain", "OK");
  });

  // Mode zyklisch weiter
  server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest* req) {
    doMode(1);
    req->send(200, "text/plain", "OK");
  });

  // Band wechseln
  server.on("/api/band", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("dir")) { req->send(400, "text/plain", "missing dir"); return; }
    int dir = req->getParam("dir")->value().toInt();
    setBand((dir >= 0) ? +1 : -1);
    oledShowFrequencyScreen();
    req->send(200, "text/plain", "OK");
  });
}

// ---------------- Verbindungslogik ohne WiFiManager ----------------
static bool tryConnectSaved(uint32_t timeoutMs = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);  // Credentials in NVS
  WiFi.begin();           // letzte gespeicherte Zugangsdaten verwenden

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void startAP() {
  const char* AP_SSID = "SI4732-AP";
  // Offener AP (optional Passwort: WiFi.softAP(AP_SSID, "12345678");)
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP gestartet: %s  IP: %s\n", AP_SSID, ip.toString().c_str());
}

namespace WebUI {

void begin() {
  Serial.println("WebUI: WLAN verbinden (gespeicherte Zugangsdaten)...");
  bool connected = tryConnectSaved();

  setupRoutes();
  server.begin();

  if (connected) {
    Serial.printf("WiFi verbunden: %s  IP: %s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Keine gespeicherten Zugangsdaten oder Verbindung fehlgeschlagen. Starte AP...");
    startAP();
    Serial.println("Konfigurationsseite: http://192.168.4.1/wifi");
  }
}

void loop() {
  // nichts nötig (Async)
}

} // namespace WebUI
