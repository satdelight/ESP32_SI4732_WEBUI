# ESP32 + SI4732/4735 WebUI Receiver

Documentation for an ESP32-based broadcast/ham-band receiver using the Silicon Labs SI4732/4735 and a small OLED display, with a built-in web interface for remote control.

- Hardware: ESP32, SI4732/4735, SSD1306 OLED 128×32 (I2C), rotary encoder with push button
- Modes: FM, AM, LSB, USB (SSB patch auto-load)
- Bands: FM, LW, MW (EU/NA), many SW broadcast bands, ham bands, CB
- RDS: FM Program Service name (PS) shown on OLED and in Web UI; auto-clears on RDS loss
- Web UI: Band selection, tuning step buttons, mode switching (AM/SSB), frequency entry, Wi‑Fi setup
- REST API: JSON endpoints for status, bands, tuning, mode, etc.
- Settings persisted to EEPROM

---

## Features

- SI473x control for FM/AM/SSB with configurable bandwidths and step sizes
- OLED UI (128×32):
  - Top row: Mode and FM RDS station name (PS), plus band name on the right
  - Middle row: Large 7‑segment style frequency with unit (MHz/kHz)
  - Bottom row: S‑meter (RSSI), SNR, and FM stereo/mono flag (ST/MO)
- Rotary encoder:
  - Rotate to tune
  - Single press: Toggle band selection
  - Double press: Toggle menu (Volume, Step, Mode, BFO, Bandwidth, AGC/Att, SoftMute, Region 9/10 kHz, Seek Up/Down, RDS on/off, ANTCAP)
- SSB patch is automatically (re)loaded when switching to SSB
- Web UI served by ESPAsyncWebServer with zero-cache responses to keep status fresh
- FM RDS handling with stabilization and “loss timeout” so PS disappears if RDS signal goes away
- Wi‑Fi AP fallback for first-time configuration

---

## Hardware

Pins (as used in this project; adjust if you need):

- I2C (ESP32): SDA 21, SCL 22
- OLED: SSD1306 on I2C address 0x3C
- SI473x:
  - I2C bus (same as OLED)
  - RESET: ESP32 pin 12
  - Reference clock (optional but used here): ESP32 pin 25 outputs ~32.768 kHz to SI473x RCLK pin
- Rotary encoder:
  - A: GPIO 13
  - B: GPIO 14
  - Push button: GPIO 27 (pull-up enabled)

Power as appropriate for your modules. Keep I2C lines short and use proper pull-ups if needed.

---

## Software Requirements

Arduino (or PlatformIO) with these libraries:

- SI4735 Arduino Library (PU2CLR / Ricardo Lima Caratti)
- Adafruit GFX
- Adafruit SSD1306
- ESPAsyncWebServer (ESP32)
- AsyncTCP (ESP32)
- ArduinoJson
- Rotary encoder library (a library providing “Rotary.h”)

Project files:
- ESP32_SI4732_WEBUI.ino
- WebUI.cpp / WebUI.h
- DSEG7_Classic_Regular_16.h (font for large frequency display)
- patch_ssb_compressed.h (SSB patch data)

Note: The code uses XOSCEN_RCLK and outputs the reference clock on GPIO 25 via LEDC.

---

## Building and Flashing

1. Install the required libraries (see above).
2. Open the project in the Arduino IDE (Board: ESP32 Dev Module) or PlatformIO.
3. Ensure the included files are present (particularly patch_ssb_compressed.h).
4. Compile and upload.

On first boot:
- The device tries STA mode using stored credentials (if any).
- If not connected within a few seconds, it enables AP mode with SSID like SI4732-XXXX.
- Connect to the AP and open http://192.168.4.1/ to access the Web UI.

---

## Wi‑Fi Setup

- Web UI path: /wifi
- Enter SSID and password and submit.
- The device attempts to connect and shows the assigned IP.
- Manually restart the device after saving (as noted on the page).
- On next boot, it should join your Wi‑Fi; open the shown IP to use the Web UI.

AP fallback remains available if STA cannot connect.

---

## Using the Device

On the OLED:
- Top row: Mode (FM/AM/LSB/USB) and, on FM, the RDS PS name to the right of “FM”. Band name is at the top-right.
- Middle row: Large frequency (DSEG7 font) with unit:
  - FM shows MHz with one decimal (e.g., 101.3)
  - AM/LW/SW show kHz
- Bottom row: RSSI as S:xx, and ST/MO in FM. SNR is also tracked.

Rotary encoder:
- Rotate to tune up/down by current step.
- Press once to toggle band selection mode, then rotate to change band.
- Double-press to open the menu; rotate to select an item, press to enter/confirm.

SSB:
- When switching into LSB/USB, the SSB patch is (re)loaded if not already present.
- BFO can be adjusted when enabled in the menu.

RDS:
- FM PS (station name) is shown on the OLED (top row) and in the Web UI next to the frequency.
- If RDS is not received or sync is lost for ~2.5 s, the PS is cleared automatically.

---

## Web UI

Open the device IP (or http://192.168.4.1/ in AP mode). You’ll see:
- Current frequency, mode, band, RSSI/SNR
- Grouped band buttons (Main, Broadcast, Amateur)
- Four quick-tune buttons (±1× and ±5× of the current step)
- Frequency input and a button to set it directly
- Mode button (cycles AM/SSB when not in FM)
- Link to Wi‑Fi config

FM shows the RDS PS name next to the frequency if available. The page auto-refreshes status every second and disables caching at both server and client.

---

## REST API

All endpoints return JSON or plain text as noted. Responses include no-cache headers to avoid stale data.

- GET /api/status
  - Returns current receiver status and network info:
  ```json
  {
    "mode": "FM|AM|LSB|USB",
    "band": "31m",
    "band_idx": 12,
    "freq_raw": 10130,
    "freq_str": "101.3 MHz",
    "step_khz": 10,
    "rssi_dbuv": 23,
    "snr_db": 18,
    "net_mode": "AP|STA",
    "ip": "192.168.4.1",
    "ps": "STATION"
  }
  ```

- GET /api/bands
  - Lists all bands:
  ```json
  {
    "total": 28,
    "current_idx": 0,
    "items": [
      {"idx":0,"name":"FM "},
      {"idx":1,"name":"LW "},
      {"idx":2,"name":"MW-EU"}
    ]
  }
  ```

- GET /api/band/set?idx=N  
  Switch to band N. Returns plain “OK”.

- GET /api/band?dir=1  
  dir=1 for next band, dir=-1 for previous. Returns “OK”.

- GET /api/tune?delta=X  
  Tune by delta:
  - AM/LW/SW: X in kHz
  - FM: X in 10 kHz units (e.g., +1 = +0.01 MHz)

- GET /api/setfreq?val=V  
  Set absolute frequency:
  - AM/LW/SW: V in kHz (e.g., 999)
  - FM: V in 10 kHz units (e.g., 10130 for 101.3 MHz)

- GET /api/mode?next=1  
  Cycles mode when not in FM: AM -> LSB -> USB -> AM.

- GET /wifi (HTML)  
  Wi‑Fi configuration form (GET/POST).

Notes:
- Server sets Cache-Control: no-cache, no-store for status and bands.
- The frontend uses fetch(..., {cache: 'no-store'}) for status polling.

---

## Configuration and Defaults

- Default volume: 35
- FM steps: 5/10/20 (internally 10 kHz units, UI shows kHz)
- AM steps: 1/5/9/10/50/100 kHz
- Region: MW spacing 9 kHz (EU) or 10 kHz (NA); configurable via menu. MW tuning is aligned to region spacing.
- CB: Grid aligned to 10 kHz; separate “CB-DE” band included
- Bandwidth tables per mode (FM/AM/SSB) mapped to SI473x bandwidth indices
- EEPROM stores: volume, band index, RDS on/off, mode, BFO, soft mute, AGC, region, ANTCAP, each band’s last frequency/step/BW

---

## Troubleshooting

Web page shows “plain text” only:
- Ensure HTML handlers use send_P with Content-Type “text/html; charset=utf-8”.
- Hard-refresh the browser (Ctrl+F5) or clear cache.

Web UI doesn’t update PS/RDS reliably:
- The server adds no-cache headers to /api/status, and the client uses no-store fetch.
- Ensure both sides are using the updated code paths.

RDS PS doesn’t clear when signal is gone:
- Implemented an RDS loss timeout (~2.5 s) that clears PS and refreshes the OLED and Web UI.

SSB stops working after switching modes multiple times:
- The code resets ssbLoaded=false when leaving SSB and reloads the patch on next SSB entry.

No audio or poor reception:
- Check antenna, SI473x power pins, reference clock wiring.
- For non-FM bands, you can toggle ANTCAP auto/hold in the menu; retune to reapply.

---

## Credits

This project builds on the excellent work of the following people and projects:

- Ricardo Lima Caratti (PU2CLR) — SI4735 Arduino Library  
  https://github.com/pu2clr/SI4735

- Adafruit Industries — Adafruit GFX and SSD1306 libraries  
  https://github.com/adafruit/Adafruit-GFX-Library  
  https://github.com/adafruit/Adafruit_SSD1306

- me-no-dev — ESPAsyncWebServer and AsyncTCP (ESP32)  
  https://github.com/me-no-dev/ESPAsyncWebServer  
  https://github.com/me-no-dev/AsyncTCP

- Benoît Blanchon — ArduinoJson  
  https://github.com/bblanchon/ArduinoJson

- Keshikan — DSEG font family (used for the 7‑segment style display)  
  https://github.com/keshikan/DSEG

- Rotary encoder library (Rotary.h) — credit the author of the specific library you install for your environment.

Please refer to the linked repositories for original authorship and licenses.

---

## License

MIT License

Copyright (c) 2025

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the “Software”), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

---

## Third‑party Licenses and Notices

This project depends on external libraries and assets that carry their own licenses. Please review their repositories for full terms. Notable examples:

- DSEG fonts by Keshikan — SIL Open Font License 1.1
- Adafruit libraries — BSD/MIT-style licenses
- ESPAsyncWebServer / AsyncTCP — as per their repositories
- ArduinoJson — MIT
- SI4735 Arduino Library (PU2CLR) — as per the repository

If you redistribute binaries or sources, include appropriate license files as required by those projects.
