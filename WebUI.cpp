#include "WebUI.h"
#include <WiFi.h>
#include <WiFiManager.h>     // tzapu/WiFiManager
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>

// Aus deinem bestehenden Sketch:
#include <Wire.h>
#include <SI4735.h>

// Symbole aus ESP32_SI4732.ino
extern SI4735   rx;
extern uint16_t currentFrequency; // kHz
extern uint8_t  currentMode;      // 0=FM, 1=LSB, 2=USB, 3=AM (laut Sketch)
extern void     oledShowFrequencyScreen();
extern void     setBand(int8_t up_down); // vorhanden in deinem Sketch
extern void     doMode(int8_t v);        // vorhanden in deinem Sketch

static AsyncWebServer server(80);

static inline const char* modeToStr(uint8_t m) {
  switch (m) {
    case 0: return "FM";
    case 1: return "LSB";
    case 2: return "USB";
    case 3: return "AM";
    default: return "UNK";
  }
}

static void setFrequencySafe(uint16_t khz) {
  currentFrequency = khz;
  rx.setFrequency(currentFrequency);
  oledShowFrequencyScreen();
}

static void tuneDelta(int deltaKhz) {
  int32_t f = (int32_t)currentFrequency + deltaKhz;
  if (f < 1) f = 1;
  if (f > 300000) f = 300000; // Schutz
  setFrequencySafe((uint16_t)f);
}

// Kleines eingebettetes HTML (kein SPIFFS nötig)
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
  </style>
</head>
<body>
  <h1>ESP32 SI4732</h1>
  <div class="stat">Frequenz: <span id="freq">-</span> kHz</div>
  <div class="stat">Mode: <span id="mode">-</span></div>
  <div class="stat">RSSI: <span id="rssi">-</span> dBμ, SNR: <span id="snr">-</span> dB</div>
  <div class="row">
    <button onclick="tune(-1000)">-1 kHz</button>
    <button onclick="tune(-10)">-10 kHz</button>
    <button onclick="tune(10)">+10 kHz</button>
    <button onclick="tune(1000)">+1 kHz</button>
  </div>
  <div class="row">
    <button onclick="modeNext()">Mode wechseln</button>
    <button onclick="band(-1)">Band &lt;</button>
    <button onclick="band(1)">Band &gt;</button>
  </div>
  <div class="stat">Direkt setzen: <input id="in" type="number" min="1" step="1" style="width:120px"> <button onclick="setfreq()">Set</button> <small>(kHz)</small></div>
  <p>API: <code>/api/status</code>, <code>/api/tune?delta=10</code>, <code>/api/setfreq?khz=7100</code>, <code>/api/mode?next=1</code>, <code>/api/band?dir=1</code></p>
<script>
async function getStatus(){
  try{
    const r = await fetch('/api/status'); const j = await r.json();
    document.getElementById('freq').textContent = j.freq_khz;
    document.getElementById('mode').textContent = j.mode;
    document.getElementById('rssi').textContent = j.rssi_dbuv;
    document.getElementById('snr').textContent = j.snr_db;
  }catch(e){}
}
async function tune(d){ await fetch('/api/tune?delta='+d); getStatus(); }
async function setfreq(){
  const v = document.getElementById('in').value;
  if(!v) return;
  await fetch('/api/setfreq?khz='+encodeURIComponent(v));
  getStatus();
}
async function modeNext(){ await fetch('/api/mode?next=1'); getStatus(); }
async function band(d){ await fetch('/api/band?dir='+d); getStatus(); }
getStatus(); setInterval(getStatus, 1000);
</script>
</body>
</html>
)HTML";

namespace WebUI {

  void begin() {
    // WLAN mit WiFiManager (Captive Portal beim ersten Start)
    WiFi.mode(WIFI_STA);
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // 3 Minuten
    bool ok = wm.autoConnect("SI4732-Setup");
    if (!ok) {
      ESP.restart();
      return;
    }

    // Routen
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
      req->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
      int8_t rssi = 0, snr = 0, mult = 0, freqOfs = 0;
      rx.getCurrentReceivedSignalQuality(&rssi, &snr, &mult, &freqOfs);

      StaticJsonDocument<256> doc;
      doc["freq_khz"] = currentFrequency;
      doc["mode"] = modeToStr(currentMode);
      doc["rssi_dbuv"] = rssi;
      doc["snr_db"] = snr;
      doc["freq_ofs"] = freqOfs;

      String out;
      serializeJson(doc, out);
      req->send(200, "application/json", out);
    });

    server.on("/api/tune", HTTP_GET, [](AsyncWebServerRequest* req) {
      if (!req->hasParam("delta")) { req->send(400, "text/plain", "missing delta"); return; }
      int delta = req->getParam("delta")->value().toInt();
      tuneDelta(delta);
      req->send(200, "text/plain", "OK");
    });

    server.on("/api/setfreq", HTTP_GET, [](AsyncWebServerRequest* req) {
      if (!req->hasParam("khz")) { req->send(400, "text/plain", "missing khz"); return; }
      uint16_t khz = (uint16_t) req->getParam("khz")->value().toInt();
      setFrequencySafe(khz);
      req->send(200, "text/plain", "OK");
    });

    // Mode zyklisch weiter
    server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest* req) {
      doMode(1);
      req->send(200, "text/plain", "OK");
    });

    // Band wechseln mit deiner vorhandenen setBand(+/-1)
    server.on("/api/band", HTTP_GET, [](AsyncWebServerRequest* req) {
      if (!req->hasParam("dir")) { req->send(400, "text/plain", "missing dir"); return; }
      int dir = req->getParam("dir")->value().toInt();
      setBand((dir >= 0) ? +1 : -1);
      oledShowFrequencyScreen();
      req->send(200, "text/plain", "OK");
    });

    server.begin();

    Serial.printf("WiFi verbunden: %s  IP: %s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
  }

  void loop() {
    // Aktuell nichts nötig (Async-Webserver)
  }

} // namespace WebUI
