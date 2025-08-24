#include "WebUI.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <SI4735.h>

// ===== Externe Symbole aus der .ino =====
extern SI4735   rx;
extern uint16_t currentFrequency;
extern uint8_t  currentMode;
extern void     oledShowFrequencyScreen();
extern void     setBand(int8_t up_down);
extern void     doMode(int8_t v);
extern int      bandCount();
extern void     setBandIndex(uint8_t newIdx);

typedef struct {
  const char *bandName; uint8_t bandType;
  uint16_t minimumFreq, maximumFreq, currentFreq;
  int8_t currentStepIdx, bandwidthIdx;
} Band;
extern Band band[];
extern int  bandIdx;

extern int tabAmStep[];
extern int tabFmStep[];
extern uint16_t currentStepIdx;

// RDS-PS aus .ino (8 Zeichen + 0)
extern char rdsPSShown[9];

// ===== Server =====
static AsyncWebServer server(80);

// ===== UI-Helfer =====
static inline const char* modeToStr(uint8_t m) {
  switch (m) {
    case 0: return "FM";
    case 1: return "LSB";
    case 2: return "USB";
    case 3: return "AM";
    default: return "UNK";
  }
}

static String makeFreqString() {
  char tmp[8], out[16];
  sprintf(tmp, "%5.5u", currentFrequency);
  if (rx.isCurrentTuneFM()) {
    out[0] = tmp[0]; out[1] = tmp[1]; out[2] = tmp[2]; out[3] = '.'; out[4] = tmp[3]; out[5] = '\0';
    String s(out); s += " MHz"; return s;
  } else {
    out[0] = (tmp[0] == '0') ? ' ' : tmp[0];
    out[1] = tmp[1];
    if (currentFrequency < 1000) { out[1] = ' '; out[2] = tmp[2]; out[3] = tmp[3]; out[4] = tmp[4]; out[5]='\0'; }
    else                         { out[2] = tmp[2]; out[3] = tmp[3]; out[4] = tmp[4]; out[5]='\0'; }
    String s(out); s.trim(); s += " kHz"; return s;
  }
}

static int getStepKHz() {
  return rx.isCurrentTuneFM() ? (tabFmStep[currentStepIdx] * 10) : tabAmStep[currentStepIdx];
}

static void setFrequencySafe(uint16_t v) {
  currentFrequency = v;
  rx.setFrequency(currentFrequency);
  oledShowFrequencyScreen();
}

static void tuneDelta(int delta) {
  int32_t f = (int32_t)currentFrequency + delta;
  if (f < 1) f = 1;
  if (f > 300000) f = 300000;
  setFrequencySafe((uint16_t)f);
}

// ===== HTML (PROGMEM) =====
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 SI4732 Web UI</title>
<style>
  :root { --gap: 8px; --btn-bg:#f6f6f8; --btn-act:#06c; --btn-act-t:#fff; }
  body{font-family:system-ui,Arial,sans-serif;margin:16px}
  .row{display:flex;gap:var(--gap);flex-wrap:wrap;margin:8px 0}
  .grid{display:flex;flex-wrap:wrap;gap:var(--gap)}
  .group{margin:10px 0}
  .group h3{margin:8px 0 6px 0;font-size:16px}
  button{font-size:15px;padding:8px 12px;border:1px solid #ddd;background:var(--btn-bg);border-radius:6px;cursor:pointer}
  button.active{background:var(--btn-act);color:var(--btn-act-t);border-color:var(--btn-act)}
  .stat{font-size:18px;margin:4px 0}
  small{color:#666}
  a{color:#06c;text-decoration:none}
</style>
</head>
<body>
  <h1>ESP32 SI4732</h1>
  <div class="stat">Frequenz: <span id="freq">-</span> <span id="ps"></span></div>
  <div class="stat">Mode: <span id="mode">-</span> | Band: <span id="bandtext">-</span></div>
  <div class="stat">RSSI: <span id="rssi">-</span> dBμ, SNR: <span id="snr">-</span> dB</div>

  <div class="group"><h3>Hauptbänder</h3><div class="grid" id="grp-main"></div></div>
  <div class="group"><h3>Rundfunkbänder</h3><div class="grid" id="grp-bc"></div></div>
  <div class="group"><h3>Amateurfunkbänder</h3><div class="grid" id="grp-ham"></div></div>

  <div class="row" style="margin-top:6px">
    <button id="b1">-</button>
    <button id="b2">-</button>
    <button id="b3">-</button>
    <button id="b4">-</button>
  </div>
  <small id="hint">Hinweis: Schritte werden aus der aktuell eingestellten Schrittweite abgeleitet.</small>

  <div class="row" style="margin-top:10px">
    <input id="in" type="number" min="1" step="1" style="width:220px" placeholder="kHz (AM/SW/LW) bzw. 10-kHz-Einheiten (FM)">
    <button id="setbtn">Set</button>
    <button id="modebtn">Mode wechseln</button>
  </div>

  <p style="margin-top:8px"><a href="/wifi">WLAN konfigurieren</a></p>

  <p>API: <code>/api/status</code>, <code>/api/bands</code>, <code>/api/band/set?idx=...</code>, <code>/api/tune?delta=...</code>, <code>/api/setfreq?val=...</code>, <code>/api/mode?next=1</code>, <code>/api/band?dir=1</code></p>

<script>
let bandList = [];
let currentBandIdx = -1;

function isMain(name){
  const n = String(name||'').trim();
  return (n === 'FM' || n === 'FM ' || n === 'MW-EU' || n === 'MW-NA' || n === 'LW' || n === 'LW ' || n === 'ALL');
}
function isHam(name){
  const n = String(name||'').trim();
  if (n.startsWith('CB')) return true;
  return /^[0-9]{1,3}M$/.test(n);
}
function isBroadcast(name){
  const n = String(name||'').trim();
  return /^[0-9]{2,3}m$/.test(n);
}

function makeBtn(item){
  const b = document.createElement('button');
  b.textContent = item.name.trim();
  b.dataset.idx = item.idx;
  b.addEventListener('click', async ()=>{
    await fetch('/api/band/set?idx='+encodeURIComponent(item.idx));
    getStatus();
  });
  return b;
}

function renderGroups(){
  const main = document.getElementById('grp-main');
  const bc   = document.getElementById('grp-bc');
  const ham  = document.getElementById('grp-ham');
  main.innerHTML=''; bc.innerHTML=''; ham.innerHTML='';

  const prefer = ['FM ','FM','MW-EU','MW-NA','LW ','LW','ALL'];
  const mainPref = [];
  const mainRest = [];
  bandList.forEach(it=>{
    if (isMain(it.name)){
      const pos = prefer.indexOf(it.name);
      if (pos>=0) mainPref[pos]=it; else mainRest.push(it);
    }
  });
  [...mainPref.filter(Boolean), ...mainRest].forEach(it=> main.appendChild(makeBtn(it)));

  const bcItems = bandList.filter(it => isBroadcast(it.name));
  bcItems.sort((a, b) => {
    const an = String(a.name).trim();
    const bn = String(b.name).trim();
    if (an === '120m' && bn !== '120m') return 1;
    if (bn === '120m' && an !== '120m') return -1;
    return an.localeCompare(bn, 'de');
  });
  bcItems.forEach(it => bc.appendChild(makeBtn(it)));

  const toNum = (n)=>{ if (String(n).startsWith('CB')) return 100000; const m=String(n).match(/^(\d+)/); return m?parseInt(m[1],10):99999; };
  bandList.filter(it=>isHam(it.name))
          .sort((a,b)=>toNum(a.name)-toNum(b.name))
          .forEach(it=> ham.appendChild(makeBtn(it)));

  highlightActive();
}

function highlightActive(){
  document.querySelectorAll('button[data-idx]').forEach(btn=>{
    btn.classList.toggle('active', Number(btn.dataset.idx)===Number(currentBandIdx));
  });
}

function renderButtons(mode, step_khz){
  const defs = [{mult:-5},{mult:-1},{mult:+1},{mult:+5}];
  const btns = [b1,b2,b3,b4];
  defs.forEach((o,i) => {
    const khz = o.mult * step_khz;
    const label = (Math.abs(khz) >= 1000)
      ? ((khz/1000).toFixed(3).replace(/\.?0+$/,'') + ' MHz')
      : (khz + ' kHz');
    btns[i].textContent = (khz<0?'-':'+') + label.replace(/^[-+]/,'');
    const delta_internal = (mode === 'FM') ? Math.round(khz/10) : khz;
    btns[i].dataset.delta = delta_internal;
  });
  document.getElementById('hint').textContent =
    (mode === 'FM')
    ? ('FM Step: ' + step_khz + ' kHz (Buttons: ±1× bzw. ±5× Step)')
    : ('AM/SW/LW Step: ' + step_khz + ' kHz (Buttons: ±1× bzw. ±5× Step)');
}

async function loadBands(){
  try{
    const r = await fetch('/api/bands', {cache:'no-store'});
    const j = await r.json();
    bandList = (j.items||[]).map(x=>({idx:x.idx, name:String(x.name||'')}));
    renderGroups();
    if (typeof j.current_idx === 'number') currentBandIdx = j.current_idx, highlightActive();
  }catch(e){}
}

async function getStatus(){
  try{
    const r = await fetch('/api/status', {cache:'no-store'});
    const j = await r.json();
    document.getElementById('freq').textContent = j.freq_str;
    document.getElementById('mode').textContent = j.mode;
    document.getElementById('bandtext').textContent = j.band;
    document.getElementById('rssi').textContent = j.rssi_dbuv;
    document.getElementById('snr').textContent = j.snr_db;

    // PS neben der Frequenz anzeigen (nur wenn vorhanden)
    const psEl = document.getElementById('ps');
    const ps = (j.ps || '').trim();
    psEl.textContent = ps ? '(' + ps + ')' : '';

    renderButtons(j.mode, j.step_khz);
    if (typeof j.band_idx === 'number') { currentBandIdx = j.band_idx; highlightActive(); }
  }catch(e){}
}

async function tuneFromBtn(ev){
  const d = ev.target.dataset.delta;
  if(!d) return;
  await fetch('/api/tune?delta='+encodeURIComponent(d));
  getStatus();
}
const b1 = document.getElementById('b1');
const b2 = document.getElementById('b2');
const b3 = document.getElementById('b3');
const b4 = document.getElementById('b4');
[b1,b2,b3,b4].forEach(b=>b.addEventListener('click', tuneFromBtn));

document.getElementById('setbtn').addEventListener('click', async ()=>{
  const v = document.getElementById('in').value;
  if(!v) return;
  await fetch('/api/setfreq?val='+encodeURIComponent(v));
  getStatus();
});
document.getElementById('modebtn').addEventListener('click', async ()=>{
  await fetch('/api/mode?next=1'); getStatus();
});

loadBands();
getStatus(); setInterval(getStatus, 1000);
</script>
</body>
</html>)HTML";

static const char WIFI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="de">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>WLAN konfigurieren</title>
<style>body{font-family:system-ui,Arial,sans-serif;margin:16px}input,button{font-size:16px;padding:8px}form{display:flex;flex-direction:column;gap:8px;max-width:360px}</style>
</head>
<body>
  <h1>WLAN konfigurieren</h1>
  <form method="post" action="/wifi">
    <label>SSID<br><input type="text" name="ssid" required></label>
    <label>Passwort<br><input type="password" name="pass"></label>
    <button type="submit">Verbinden & Speichern</button>
  </form>
  <p>Tipp: Nach erfolgreicher Verbindung bitte das Gerät einmal manuell neu starten.</p>
</body>
</html>)HTML";

// ===== Routen =====
static void setupRoutes() {
  // HTML-Seiten
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html; charset=utf-8", WIFI_HTML);
  });

  // WLAN-POST
  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest* req) {
    String ssid = req->hasParam("ssid", true) ? req->getParam("ssid", true)->value() : "";
    String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    if (ssid.length() == 0) { req->send(400, "text/plain", "Missing SSID"); return; }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      req->send(200, "text/html; charset=utf-8",
                "Verbunden mit " + WiFi.SSID() + " - IP: " + WiFi.localIP().toString() + "<br>Bitte Gerät manuell neu starten.");
    } else {
      req->send(200, "text/html; charset=utf-8",
                "Verbindung fehlgeschlagen. Bitte SSID/Passwort prüfen und erneut versuchen.");
    }
  });

  // API: Bands (no-cache)
  server.on("/api/bands", HTTP_GET, [](AsyncWebServerRequest* req) {
    const int total = bandCount();
    StaticJsonDocument<2048> doc;
    doc["total"] = total;
    doc["current_idx"] = bandIdx;
    JsonArray arr = doc.createNestedArray("items");
    for (int i = 0; i < total; i++) {
      JsonObject o = arr.createNestedObject();
      o["idx"] = i;
      o["name"] = band[i].bandName;
    }
    String out; serializeJson(doc, out);
    AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    res->addHeader("Pragma", "no-cache");
    res->addHeader("Expires", "0");
    req->send(res);
  });

  // API: Band setzen
  server.on("/api/band/set", HTTP_GET, [](AsyncWebServerRequest* req) {
    int total = bandCount();
    int target = -1;
    if (req->hasParam("idx")) {
      target = req->getParam("idx")->value().toInt();
    } else if (req->hasParam("name")) {
      String name = req->getParam("name")->value();
      for (int i = 0; i < total; i++) {
        if (name.equalsIgnoreCase(band[i].bandName)) { target = i; break; }
      }
    } else {
      req->send(400, "text/plain", "missing idx/name"); return;
    }
    if (target < 0 || target >= total) { req->send(400, "text/plain", "invalid idx"); return; }

    if (target != bandIdx) setBandIndex((uint8_t) target);
    req->send(200, "text/plain", "OK");
  });

  // API: Status (+ PS) – no-cache
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    rx.getCurrentReceivedSignalQuality();
    int8_t rssi = rx.getCurrentRSSI();
    int8_t snr  = rx.getCurrentSNR();

    uint16_t freqNow = rx.getFrequency();
    if (freqNow != currentFrequency) currentFrequency = freqNow;

    const int step_khz = getStepKHz();

    StaticJsonDocument<512> doc;
    doc["mode"]      = modeToStr(currentMode);
    doc["band"]      = band[bandIdx].bandName;
    doc["band_idx"]  = bandIdx;
    doc["freq_raw"]  = currentFrequency;
    doc["freq_str"]  = makeFreqString();
    doc["step_khz"]  = step_khz;
    doc["rssi_dbuv"] = rssi;
    doc["snr_db"]    = snr;
    doc["net_mode"]  = (WiFi.getMode() & WIFI_AP) ? "AP" : "STA";
    doc["ip"]        = ((WiFi.getMode() & WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP()).toString();
    doc["ps"]        = rx.isCurrentTuneFM() ? rdsPSShown : "";

    String out;
    serializeJson(doc, out);
    AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    res->addHeader("Pragma", "no-cache");
    res->addHeader("Expires", "0");
    req->send(res);
  });

  // API: Tuning
  server.on("/api/tune", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("delta")) { req->send(400, "text/plain", "missing delta"); return; }
    int delta = req->getParam("delta")->value().toInt();
    tuneDelta(delta);
    req->send(200, "text/plain", "OK");
  });

  // API: Frequenz setzen
  server.on("/api/setfreq", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("val")) { req->send(400, "text/plain", "missing val"); return; }
    uint16_t v = (uint16_t) req->getParam("val")->value().toInt();
    setFrequencySafe(v);
    req->send(200, "text/plain", "OK");
  });

  // API: Mode
  server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest* req) {
    doMode(1);
    req->send(200, "text/plain", "OK");
  });

  // API: Band vor/zurück
  server.on("/api/band", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("dir")) { req->send(400, "text/plain", "missing dir"); return; }
    int dir = req->getParam("dir")->value().toInt();
    setBand((dir >= 0) ? +1 : -1);
    oledShowFrequencyScreen();
    req->send(200, "text/plain", "OK");
  });
}

// ===== Serverstart =====
namespace {
  bool serverStarted = false;
}

namespace WebUI {

void begin() {
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_AP_STA);
  }

  if (WiFi.getMode() & WIFI_STA) {
    WiFi.begin();
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 3000) {
      delay(10);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (!(WiFi.getMode() & WIFI_AP)) {
      WiFi.mode(WIFI_AP_STA);
    }
    String ssid = "SI4732-" + String((uint32_t)ESP.getEfuseMac(), HEX).substring(2);
    if (!WiFi.softAP(ssid.c_str())) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP("SI4732");
    }
  }

  setupRoutes();

  if (!serverStarted) {
    server.begin();
    serverStarted = true;
  }
}

void loop() {
  // AsyncWebServer benötigt hier nichts
}

} // namespace WebUI
