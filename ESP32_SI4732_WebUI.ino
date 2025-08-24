#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "EEPROM.h"
#include <SI4735.h>
#include "DSEG7_Classic_Regular_16.h"
#include "Rotary.h"
#include <patch_ssb_compressed.h>
#include "WebUI.h"

// ========= SSB Patch meta =========
const uint16_t size_content = sizeof ssb_patch_content;
const uint16_t cmd_0x15_size = sizeof cmd_0x15;

#define DEBUG_SSB 1

// ========= Band types =========
#define FM_BAND_TYPE 0
#define MW_BAND_TYPE 1
#define SW_BAND_TYPE 2
#define LW_BAND_TYPE 3

// ========= Pins =========
#define RESET_PIN 12
#define ENCODER_PIN_A 13
#define ENCODER_PIN_B 14
#define ENCODER_PUSH_BUTTON 27

#define ESP32_I2C_SDA 21
#define ESP32_I2C_SCL 22

// ========= Ref clock out =========
#define PIN_RCLK_OUT 25
#define LEDC_CH         0
#define LEDC_TIMERBIT   11
#define REFCLK_HZ       32768.0
#define REFCLK_TRIM_HZ  (0)

// ========= Timings =========
#define MIN_ELAPSED_TIME 300
#define MIN_ELAPSED_RSSI_TIME 200
#define ELAPSED_COMMAND 2000
#define ELAPSED_CLICK 1500

// ========= Defaults =========
#define DEFAULT_VOLUME 35

// ========= Modes =========
#define FM  0
#define LSB 1
#define USB 2
#define AM  3
#define LW  4

// ========= EEPROM =========
#define EEPROM_SIZE 512
#define STORE_TIME 10000

const uint8_t app_id = 56;
const int eeprom_address = 0;
long storeTime = millis();
bool itIsTimeToSave = false;

// ========= State =========
bool bfoOn = false;
bool ssbLoaded = false;

int8_t agcIdx = 0;
uint8_t disableAgc = 0;
int8_t agcNdx = 0;
int8_t softMuteMaxAttIdx = 24;

uint8_t countClick = 0;
uint8_t seekDirection = 1;

bool cmdBand = false;
bool cmdVolume = false;
bool cmdAgc = false;
bool cmdBandwidth = false;
bool cmdStep = false;
bool cmdMode = false;
bool cmdMenu = false;
bool cmdSoftMuteMaxAtt = false;
bool cmdRds = false;
bool cmdRegion = false;
bool cmdAntcap = false;

bool oledEdit = false;
bool fmRDS = true;
bool antcapAuto = true;

int16_t currentBFO = 0;
long elapsedRSSI = millis();
long elapsedButton = millis();
long elapsedClick = millis();
long elapsedCommand = millis();
volatile int encoderCount = 0;
uint16_t currentFrequency;

const uint8_t currentBFOStep = 10;

// ========= AM region/raster =========
enum AMRegion : uint8_t { REGION_9KHZ = 0, REGION_10KHZ = 1 };
uint8_t amRegion = REGION_9KHZ;
inline uint8_t getAmSpacing() { return (amRegion == REGION_9KHZ) ? 9 : 10; }
static uint16_t alignMwToRegion(uint16_t f, uint16_t minF, uint16_t maxF, uint8_t region) {
  const uint8_t sp = (region == REGION_9KHZ) ? 9 : 10;
  const int base = (region == REGION_9KHZ) ? 531 : 530;
  long n = lround((f - base) / (double)sp);
  long aligned = base + n * (long)sp;
  if (aligned < minF) aligned = minF;
  if (aligned > maxF) aligned = maxF;
  return (uint16_t)aligned;
}
static uint16_t alignToGrid(uint16_t f, uint16_t base, uint8_t step, uint16_t minF, uint16_t maxF) {
  long n = lround((f - base) / (double)step);
  long aligned = base + n * (long)step;
  if (aligned < minF) aligned = minF;
  if (aligned > maxF) aligned = maxF;
  return (uint16_t)aligned;
}

// ========= Menu =========
const char *menu[] = { "Volume", "Step", "Mode", "BFO", "BW", "AGC/Att", "SoftMute", "Region", "Seek Up", "Seek Down", "RDS", "ANTCAP" };
int8_t menuIdx = 0;
const int lastMenu = 11;
int8_t currentMenuCmd = -1;

// ========= Bandwidth sets =========
typedef struct { uint8_t idx; const char *desc; } Bandwidth;

int8_t bwIdxSSB = 4;
const int8_t maxSsbBw = 5;
Bandwidth bandwidthSSB[] = {
  {4, "0.5"}, {5, "1.0"}, {0, "1.2"}, {1, "2.2"}, {2, "3.0"}, {3, "4.0"}
};

int8_t bwIdxAM = 4;
const int8_t maxAmBw = 6;
Bandwidth bandwidthAM[] = {
  {4, "1.0"}, {5, "1.8"}, {3, "2.0"}, {6, "2.5"}, {2, "3.0"}, {1, "4.0"}, {0, "6.0"}
};

int8_t bwIdxFM = 0;
const int8_t maxFmBw = 4;
Bandwidth bandwidthFM[] = {
  {0, "AUT"}, {1, "110"}, {2, " 84"}, {3, " 60"}, {4, " 40"}
};

// ========= Steps =========
int tabAmStep[] = {1, 5, 9, 10, 50, 100};
const int lastAmStep = (sizeof tabAmStep / sizeof(int)) - 1;
int idxAmStep = 3;

int tabFmStep[] = {5, 10, 20};
const int lastFmStep = (sizeof tabFmStep / sizeof(int)) - 1;
int idxFmStep = 1;

uint16_t currentStepIdx = 1;

// ========= Mode desc =========
const char *bandModeDesc[] = {"FM ", "LSB", "USB", "AM "};
uint8_t currentMode = FM;

// ========= Bands =========
typedef struct {
  const char *bandName; uint8_t bandType;
  uint16_t minimumFreq, maximumFreq, currentFreq;
  int8_t currentStepIdx, bandwidthIdx;
} Band;

Band band[] = {
  {"FM ",  FM_BAND_TYPE,   8750, 10800, 10130, 1, 0},

  {"LW ",  LW_BAND_TYPE,    153,   279,   198,  3, 4},
  {"MW-EU",MW_BAND_TYPE,    531,  1602,   999,  2, 4},
  {"MW-NA",MW_BAND_TYPE,    530,  1700,  1000,  3, 4},

  // Amateur
  {"630M", MW_BAND_TYPE,    472,   479,   475,  0, 4},
  {"160M", MW_BAND_TYPE,   1800,  2000,  1840,  0, 4},
  {"80M",  MW_BAND_TYPE,   3500,  4000,  3700,  0, 4},
  {"60M",  SW_BAND_TYPE,   5351,  5367,  5363,  0, 4},
  {"40M",  SW_BAND_TYPE,   7000,  7300,  7100,  0, 4},
  {"30M",  SW_BAND_TYPE,  10100, 10150, 10120,  0, 4},
  {"20M",  SW_BAND_TYPE,  14000, 14350, 14200,  0, 4},
  {"17M",  SW_BAND_TYPE,  18068, 18168, 18100,  0, 4},
  {"15M",  SW_BAND_TYPE,  21000, 21450, 21100,  0,  4},
  {"12M",  SW_BAND_TYPE,  24890, 24990, 24940,  0,  4},
  {"10M",  SW_BAND_TYPE,  28000, 29700, 28400,  0,  4},

  // CB
  {"CB ",    SW_BAND_TYPE, 26965, 27405, 27285, 3, 4},
  {"CB-DE",  SW_BAND_TYPE, 26565, 27405, 27285, 3, 4},

  // SW Broadcast
  {"120m", SW_BAND_TYPE,   2300,  2495,  2400,  1, 4},
  {"90m",  SW_BAND_TYPE,   3200,  3400,  3300,  1, 4},
  {"75m",  SW_BAND_TYPE,   3900,  4000,  3950,  1,  4},
  {"60m",  SW_BAND_TYPE,   4750,  5060,  4900,  1,  4},
  {"49m",  SW_BAND_TYPE,   5800,  6200,  6000,  1,  4},
  {"41m",  SW_BAND_TYPE,   7200,  7600,  7400,  1,  4},
  {"31m",  SW_BAND_TYPE,   9400,  9900,  9700,  1,  4},
  {"25m",  SW_BAND_TYPE,  11600, 12100, 11900,  1,  4},
  {"22m",  SW_BAND_TYPE,  13570, 13870, 13700,  1,  4},
  {"19m",  SW_BAND_TYPE,  15100, 15800, 15300,  1,  4},
  {"16m",  SW_BAND_TYPE,  17480, 17900, 17600,  1,  4},
  {"15m",  SW_BAND_TYPE,  18900, 19020, 18950,  1,  4},
  {"13m",  SW_BAND_TYPE,  21450, 21850, 21600,  1,  4},
  {"11m",  SW_BAND_TYPE,  25670, 26100, 25800,  1,  4},

  {"ALL", SW_BAND_TYPE,     150, 30000, 15000,  1,  4}
};

const int lastBand = (sizeof band / sizeof(Band)) - 1;
int bandIdx = 0;

static bool isBroadcastMWBandIdx(int i) {
  const char* n = band[i].bandName;
  return (strcmp(n, "MW-EU") == 0 || strcmp(n, "MW-NA") == 0);
}
static bool isCBBandIdx(int i) {
  const char* n = band[i].bandName;
  return (strncmp(n, "CB", 2) == 0);
}
static uint8_t getSeekSpacingForCurrentBand() {
  if (isBroadcastMWBandIdx(bandIdx)) return getAmSpacing();
  if (isCBBandIdx(bandIdx)) return 10;
  return 5;
}

// Alternative steps
int tabStep[] = {1, 5, 10, 50, 100, 500, 1000};
const int lastStep = (sizeof tabStep / sizeof(int)) - 1;

// ========= STATUS =========
uint8_t rssi = 0;
uint8_t snr = 0;
uint8_t volume = DEFAULT_VOLUME;

// ========= HW =========
Rotary encoder = Rotary(ENCODER_PIN_A, ENCODER_PIN_B);
Adafruit_SSD1306 oled = Adafruit_SSD1306(128, 32, &Wire);
SI4735 rx;

// Ref clock
double g_realRefHz = 0.0;

// Helper: 0=LSB, 1=USB
static inline uint8_t sbSelFromMode(uint8_t mode) { return (mode == USB) ? 1 : 0; }

// ========== Forward decls ==========
void saveAllReceiverInformation();
void readAllReceiverInformation();
void resetEepromDelay();
void disableCommands();

void oledShowFrequency();
void oledShowBandMode();
void oledShowRSSI();
void oledShowFrequencyScreen();

void showCommandStatus(char * currentCmd);
void showMenu();
void doMenu(int8_t v);
void doCurrentMenuCmd();
bool isMenuMode();
void setBand(int8_t up_down);
void useBand();
void loadSSB();
void doBandwidth(int8_t v);
void doAgc(int8_t v);
void doStep(int8_t v);
void doMode(int8_t v);
void doVolume(int8_t v);
void showFrequencySeek(uint16_t freq);
void doSeek();
void doSoftMute(int8_t v);
void doRegion(int8_t v);

// ========== API für WebUI ==========
int bandCount() { return lastBand + 1; }
void setBandIndex(uint8_t newIdx) {
  if (newIdx > lastBand) newIdx = lastBand;
  band[bandIdx].currentFreq = currentFrequency;
  band[bandIdx].currentStepIdx = currentStepIdx;
  bandIdx = newIdx;
  useBand();
  delay(MIN_ELAPSED_TIME);
  elapsedCommand = millis();
}

// ========= RDS =========
static void sanitizeAscii(char* s) { if (!s) return; for (uint8_t i = 0; s[i]; i++) if ((uint8_t)s[i] < 32) s[i] = ' '; }

// Top (PS)
char rdsPSShown[9] = "";
char rdsPSCand[9] = "";
unsigned long rdsPSCandSince = 0;

void rdsResetTop() { rdsPSShown[0] = 0; rdsPSCand[0] = 0; rdsPSCandSince = 0; }

// Kurz-PS mit Limit (z. B. 8 Zeichen) für die erste OLED-Zeile
static void rdsGetPsShortN(char* out, size_t outsz, uint8_t maxChars) {
  if (!out || outsz == 0) return;
  char tmp[9];
  strncpy(tmp, rdsPSShown, sizeof(tmp));
  tmp[sizeof(tmp)-1] = '\0';
  if (maxChars > 8) maxChars = 8;
  char buf[9];
  uint8_t i=0; for (; i<maxChars && tmp[i]; i++) buf[i] = tmp[i];
  buf[i] = '\0';
  strncpy(out, buf, outsz);
  out[outsz-1] = '\0';
}

void rdsTopRender(const char* txt8) {
  if (!txt8) return;
  char eight[9]; strncpy(eight, txt8, 8); eight[8] = '\0'; sanitizeAscii(eight);
  if (strncmp(eight, rdsPSShown, 8) != 0) {
    strncpy(rdsPSShown, eight, 9);
    rdsPSShown[8] = '\0';
    // Kopfzeile sofort aktualisieren, sofern keine Menüs aktiv sind
    if (!isMenuMode() && !oledEdit) {
      oledShowBandMode();
    }
  }
}

void rdsPollTop() {
  if (!rx.isCurrentTuneFM()) return;
  if (!rx.getRdsReceived()) return;
  if (!rx.getRdsSync() || rx.getNumRdsFifoUsed() == 0) return;
  char* ps = rx.getRdsStationName();
  if (!ps || !ps[0]) return;

  char tmp[9]; uint8_t i=0; for (; i<8 && ps[i]; i++) tmp[i]=ps[i]; tmp[i]='\0'; sanitizeAscii(tmp);
  unsigned long now = millis();
  if (strcmp(tmp, rdsPSCand) != 0) {
    strncpy(rdsPSCand, tmp, 9); rdsPSCand[8]='\0'; rdsPSCandSince = now; return;
  }
  if (rdsPSCandSince && (now - rdsPSCandSince) >= 800UL) {
    rdsTopRender(rdsPSCand);
    rdsPSCandSince = 0;
  }
}

// Bottom (RT)
char rdsRT[65] = "";
char rdsRTCand[65] = "";
unsigned long rdsRTCandSince = 0;
int rdsRTIndex = 0;
unsigned long rdsRTNextScrollMs = 0;
const int RT_WINDOW = 20;

void rdsResetBottom() {
  rdsRT[0]=0; rdsRTCand[0]=0; rdsRTCandSince=0; rdsRTIndex=0; rdsRTNextScrollMs=0;
}
void rdsBottomRenderWindow() {
  char win[RT_WINDOW+1];
  int len = strnlen(rdsRT, 64);
  if (len <= RT_WINDOW) {
    int i=0; for (; i<len; i++) win[i]=rdsRT[i]; for (; i<RT_WINDOW; i++) win[i]=' '; win[RT_WINDOW]='\0';
  } else {
    if (rdsRTIndex > (len - RT_WINDOW)) rdsRTIndex = 0;
    memcpy(win, rdsRT + rdsRTIndex, RT_WINDOW);
    win[RT_WINDOW] = '\0';
  }
}
void rdsPollBottom() {
  if (!rx.isCurrentTuneFM()) return;
  if (!rx.getRdsReceived()) return;
  if (!rx.getRdsSync() || rx.getNumRdsFifoUsed() == 0) return;
  char* rt = rx.getRdsProgramInformation();
  if (rt && rt[0]) {
    char tmp[65]; uint8_t i=0; for (; i<64 && rt[i]; i++) tmp[i] = (rt[i] >= 32 ? rt[i] : ' '); tmp[i]='\0';
    unsigned long now = millis();
    if (strcmp(tmp, rdsRTCand) != 0) {
      strncpy(rdsRTCand, tmp, 65); rdsRTCand[64]='\0'; rdsRTCandSince = now; return;
    }
    if (rdsRTCandSince && (now - rdsRTCandSince) >= 800UL) {
      strncpy(rdsRT, rdsRTCand, 65); rdsRT[64]='\0';
      rdsRTIndex = 0; rdsBottomRenderWindow(); rdsRTCandSince = 0; rdsRTNextScrollMs = now + 700UL;
      return;
    }
  }
  if (rdsRT[0]) {
    unsigned long now = millis();
    if (now >= rdsRTNextScrollMs) {
      int len = strnlen(rdsRT, 64);
      if (len > RT_WINDOW) { rdsRTIndex += 1; rdsBottomRenderWindow(); }
      rdsRTNextScrollMs = now + 700UL;
    }
  }
}

// ========= ISR =========
void IRAM_ATTR rotaryEncoder() {
  uint8_t encoderStatus = encoder.process();
  if (encoderStatus) encoderCount = (encoderStatus == DIR_CW) ? 1 : -1;
}

// ========= Ref clock =========
void startRefClock() {
  g_realRefHz = ledcSetup(LEDC_CH, REFCLK_HZ, LEDC_TIMERBIT);
  ledcAttachPin(PIN_RCLK_OUT, LEDC_CH);
  uint32_t duty = 1U << (LEDC_TIMERBIT - 1);
  ledcWrite(LEDC_CH, duty);
}

// ========= OLED (normal) =========
void oledShowFrequency() {
  oled.fillRect(0, 8, 128, 17, SSD1306_BLACK);

  char tmp[8], out[8];
  sprintf(tmp, "%5.5u", currentFrequency);
  out[0] = (tmp[0] == '0') ? ' ' : tmp[0];
  out[1] = tmp[1];
  const char* unit;
  bool isFM = rx.isCurrentTuneFM();
  if (isFM) {
    out[2] = tmp[2]; out[3] = '.'; out[4] = tmp[3];
    unit = "MHz";
  } else {
    if (currentFrequency < 1000) { out[1] = ' '; out[2] = tmp[2]; out[3] = tmp[3]; out[4] = tmp[4]; }
    else { out[2] = tmp[2]; out[3] = tmp[3]; out[4] = tmp[4]; }
    unit = "kHz";
  }
  out[5] = '\0';

  // Frequenz groß
  oled.setFont(&DSEG7_Classic_Regular_16);
  oled.setCursor(20, 24);
  oled.print(out);

  // Einheit
  oled.setFont(NULL);
  oled.setTextSize(1);
  if (isFM) {
    // weiter nach rechts gerückt (vorher 64)
    oled.setCursor(76, 15);
    oled.print(unit);
  } else {
    oled.setCursor(90, 15);
    oled.print(unit);
  }

  oled.display();
}

void oledShowBandMode() {
  oled.fillRect(0, 0, 128, 8, SSD1306_BLACK);
  oled.setTextSize(1);

  // Mode links
  const char* modeStr =
    (band[bandIdx].bandType == LW_BAND_TYPE) ? "LW  " : bandModeDesc[currentMode];
  oled.setCursor(0, 0);
  oled.print(modeStr);

  // Bei FM: Sendername (PS) direkt neben "FM " (max. 8 Zeichen)
  if (currentMode == FM) {
    char ps[9] = {0};
    rdsGetPsShortN(ps, sizeof(ps), 8);
    if (ps[0]) {
      // „FM “ ~18 px breit im 6x8 Font; 28 px als Start ist passend
      oled.setCursor(28, 0);
      oled.print(ps);
    }
  }

  // Bandname rechts wie gehabt
  oled.setCursor(90, 0);
  oled.print(band[bandIdx].bandName);

  oled.display();
}

void oledShowRSSI() {
  char sMeter[10];
  sprintf(sMeter, "S:%d ", rssi);
  oled.fillRect(0, 25, 128, 10, SSD1306_BLACK);
  oled.setTextSize(1);
  oled.setCursor(80, 25); oled.print(sMeter);
  if (currentMode == FM) { oled.setCursor(0, 25); oled.print(rx.getCurrentPilot() ? "ST" : "MO"); }
  oled.display();
}

void oledShowFrequencyScreen() {
  oledShowBandMode();
  oledShowFrequency();
  oledShowRSSI();
}

// ========= UI helpers =========
void showCommandStatus(char * currentCmd) {
  oled.fillRect(40, 0, 50, 8, SSD1306_BLACK);
  oled.setCursor(40, 0);
  oled.setTextSize(1);
  oled.print(currentCmd);
  oled.display();
}

void showMenu() {
  oled.clearDisplay();
  oled.setCursor(0, 10);
  oled.setTextSize(1);
  oled.print(menu[menuIdx]);
  oled.display();
  showCommandStatus((char *) "Menu");
}

// ========= SSB patch =========
void loadSSB() {
  // Patch immer neu laden, wenn SSB aktiviert wird
  rx.setI2CFastModeCustom(200000);
  rx.queryLibraryId();
  rx.patchPowerUp();
  delay(50);
  rx.downloadCompressedPatch(ssb_patch_content, size_content, cmd_0x15, cmd_0x15_size);
  rx.setSSBConfig(bandwidthSSB[bwIdxSSB].idx, 1, 0, 1, 0, 1);
  rx.setI2CStandardMode();
  ssbLoaded = true;
#if DEBUG_SSB
  Serial.println("[SSB] Patch (re)loaded.");
#endif
}

// ========= Band select =========
void setBand(int8_t up_down) {
  band[bandIdx].currentFreq = currentFrequency;
  band[bandIdx].currentStepIdx = currentStepIdx;
  if (up_down == 1) bandIdx = (bandIdx < lastBand) ? (bandIdx + 1) : 0;
  else bandIdx = (bandIdx > 0) ? (bandIdx - 1) : lastBand;
  useBand();
  delay(MIN_ELAPSED_TIME);
  elapsedCommand = millis();
}

void useBand() {
  if (band[bandIdx].bandType == FM_BAND_TYPE) {
    currentMode = FM;
    rx.setTuneFrequencyAntennaCapacitor(0);
    rx.setFM(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq, band[bandIdx].currentFreq, tabFmStep[band[bandIdx].currentStepIdx]);
    rx.setSeekFmLimits(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq);
    bfoOn = false;
    ssbLoaded = false;
    bwIdxFM = band[bandIdx].bandwidthIdx;
    rx.setFmBandwidth(bandwidthFM[bwIdxFM].idx);
    rx.setFmSoftMuteMaxAttenuation(softMuteMaxAttIdx);
    rx.setRdsConfig(3, 3, 3, 3, 3);
    rx.setFifoCount(1);
    rdsResetTop();
    rdsResetBottom();
  } else {
    rx.setTuneFrequencyAntennaCapacitor(antcapAuto ? 0 : 1);

    if (currentMode == LSB || currentMode == USB) {
      if (!ssbLoaded) loadSSB();
      rx.setSSB(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq,
                band[bandIdx].currentFreq, tabAmStep[band[bandIdx].currentStepIdx],
                sbSelFromMode(currentMode)); // 0=LSB, 1=USB
      rx.setSSBAutomaticVolumeControl(1);
      rx.setSsbSoftMuteMaxAttenuation(softMuteMaxAttIdx);
      bwIdxSSB = band[bandIdx].bandwidthIdx;
      rx.setSSBAudioBandwidth(bandwidthSSB[bwIdxSSB].idx);
      rx.setSSBBfo(currentBFO);
#if DEBUG_SSB
      const char* m = (currentMode==LSB)?"LSB":"USB";
      Serial.printf("[SSB] useBand->setSSB: mode=%s, f=%u kHz, step=%d, BW=%s, BFO=%d\n",
                    m, band[bandIdx].currentFreq, tabAmStep[band[bandIdx].currentStepIdx],
                    bandwidthSSB[bwIdxSSB].desc, (int)currentBFO);
#endif
    } else {
      currentMode = AM;
      rx.setAM(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq,
               band[bandIdx].currentFreq, tabAmStep[band[bandIdx].currentStepIdx]);
      bfoOn = false;
      ssbLoaded = false;
      bwIdxAM = band[bandIdx].bandwidthIdx;
      rx.setBandwidth(bandwidthAM[bwIdxAM].idx, 1);
      rx.setAmSoftMuteMaxAttenuation(softMuteMaxAttIdx);

      if (isBroadcastMWBandIdx(bandIdx)) {
        uint16_t fNew = alignMwToRegion(band[bandIdx].currentFreq, band[bandIdx].minimumFreq, band[bandIdx].maximumFreq, amRegion);
        if (fNew != band[bandIdx].currentFreq) {
          band[bandIdx].currentFreq = fNew;
          rx.setFrequency(fNew);
        }
      } else if (isCBBandIdx(bandIdx)) {
        uint16_t base = (strcmp(band[bandIdx].bandName, "CB-DE") == 0) ? 26565 : 26965;
        uint16_t fNew = alignToGrid(band[bandIdx].currentFreq, base, 10, band[bandIdx].minimumFreq, band[bandIdx].maximumFreq);
        if (fNew != band[bandIdx].currentFreq) {
          band[bandIdx].currentFreq = fNew;
          rx.setFrequency(fNew);
        }
      }
    }
    rx.setAutomaticGainControl(disableAgc, agcNdx);
    rx.setSeekAmLimits(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq);
    rx.setSeekAmSpacing(getSeekSpacingForCurrentBand());
    rdsResetTop();
    rdsResetBottom();
  }

  delay(50);
  uint16_t fDev = rx.getFrequency();
  band[bandIdx].currentFreq = fDev;
  currentFrequency = fDev;

  currentStepIdx = band[bandIdx].currentStepIdx;

  rssi = 0;
  oledShowFrequencyScreen();
  showCommandStatus((char *) "Band");
}

// ========= Bandwidth/AGC/Step =========
void doBandwidth(int8_t v) {
  if (currentMode == LSB || currentMode == USB) {
    bwIdxSSB = (v == 1) ? bwIdxSSB + 1 : bwIdxSSB - 1;
    if (bwIdxSSB > maxSsbBw) bwIdxSSB = 0;
    else if (bwIdxSSB < 0)   bwIdxSSB = maxSsbBw;
    rx.setSSBAudioBandwidth(bandwidthSSB[bwIdxSSB].idx);
    if (bandwidthSSB[bwIdxSSB].idx == 0 || bandwidthSSB[bwIdxSSB].idx == 4 || bandwidthSSB[bwIdxSSB].idx == 5)
      rx.setSSBSidebandCutoffFilter(0);
    else
      rx.setSSBSidebandCutoffFilter(1);
    band[bandIdx].bandwidthIdx = bwIdxSSB;
  } else if (currentMode == AM) {
    bwIdxAM = (v == 1) ? bwIdxAM + 1 : bwIdxAM - 1;
    if (bwIdxAM > maxAmBw) bwIdxAM = 0;
    else if (bwIdxAM < 0)  bwIdxAM = maxAmBw;
    rx.setBandwidth(bandwidthAM[bwIdxAM].idx, 1);
    band[bandIdx].bandwidthIdx = bwIdxAM;
  } else {
    bwIdxFM = (v == 1) ? bwIdxFM + 1 : bwIdxFM - 1;
    if (bwIdxFM > maxFmBw) bwIdxFM = 0;
    else if (bwIdxFM < 0)  bwIdxFM = maxFmBw;
    rx.setFmBandwidth(bandwidthFM[bwIdxFM].idx);
    band[bandIdx].bandwidthIdx = bwIdxFM;
  }
  elapsedCommand = millis();
}

void doAgc(int8_t v) {
  agcIdx = (v == 1) ? agcIdx + 1 : agcIdx - 1;
  if (agcIdx < 0 ) agcIdx = 35;
  else if ( agcIdx > 35) agcIdx = 0;
  disableAgc = (agcIdx > 0);
  agcNdx = (agcIdx > 1) ? (agcIdx - 1) : 0;
  rx.setAutomaticGainControl(disableAgc, agcNdx);
  elapsedCommand = millis();
}

void doStep(int8_t v) {
  if ( currentMode == FM ) {
    idxFmStep = (v == 1) ? idxFmStep + 1 : idxFmStep - 1;
    if (idxFmStep > lastFmStep) idxFmStep = 0;
    else if (idxFmStep < 0)     idxFmStep = lastFmStep;
    currentStepIdx = idxFmStep;
    rx.setFrequencyStep(tabFmStep[currentStepIdx]);
  } else {
    idxAmStep = (v == 1) ? idxAmStep + 1 : idxAmStep - 1;
    if (idxAmStep > lastAmStep) idxAmStep = 0;
    else if (idxAmStep < 0)     idxAmStep = lastAmStep;
    currentStepIdx = idxAmStep;
    rx.setFrequencyStep(tabAmStep[currentStepIdx]);
    rx.setSeekAmSpacing(getSeekSpacingForCurrentBand());
  }
  band[bandIdx].currentStepIdx = currentStepIdx;
  elapsedCommand = millis();
}

// ========= Mode switch (AM/LSB/USB) =========
void doMode(int8_t v) {
  if (rx.isCurrentTuneFM()) return;

  uint8_t nextMode;
  if (v == 1) {
    if (currentMode == AM) nextMode = LSB;
    else if (currentMode == LSB) nextMode = USB;
    else nextMode = AM; // USB -> AM
  } else {
    if (currentMode == AM) nextMode = USB;
    else if (currentMode == USB) nextMode = LSB;
    else nextMode = AM; // LSB -> AM
  }

  Band &b = band[bandIdx];
  const uint16_t minF = b.minimumFreq;
  const uint16_t maxF = b.maximumFreq;
  const uint16_t step = tabAmStep[b.currentStepIdx];

  if (nextMode == AM) {
    rx.setAM(minF, maxF, currentFrequency, step);
    bfoOn = false;
    // Wichtig: Patch beim nächsten Wechsel nach SSB neu laden
    ssbLoaded = false;
    bwIdxAM = b.bandwidthIdx;
    rx.setBandwidth(bandwidthAM[bwIdxAM].idx, 1);
    rx.setAmSoftMuteMaxAttenuation(softMuteMaxAttIdx);

    if (strcmp(b.bandName, "MW-EU") == 0 || strcmp(b.bandName, "MW-NA") == 0) {
      uint16_t fNew = alignMwToRegion(currentFrequency, minF, maxF, amRegion);
      if (fNew != currentFrequency) rx.setFrequency(fNew);
    } else if (strncmp(b.bandName, "CB", 2) == 0) {
      uint16_t base = (strcmp(b.bandName, "CB-DE") == 0) ? 26565 : 26965;
      uint16_t fNew = alignToGrid(currentFrequency, base, 10, minF, maxF);
      if (fNew != currentFrequency) rx.setFrequency(fNew);
    }
  } else {
    // Beim Wechsel nach SSB Patch neu laden, falls nötig
    if (!ssbLoaded) loadSSB();
    rx.setSSB(minF, maxF, currentFrequency, step, sbSelFromMode(nextMode)); // 0=LSB, 1=USB
    rx.setSSBAutomaticVolumeControl(1);
    rx.setSsbSoftMuteMaxAttenuation(softMuteMaxAttIdx);
    bwIdxSSB = b.bandwidthIdx;
    rx.setSSBAudioBandwidth(bandwidthSSB[bwIdxSSB].idx);
    rx.setSSBBfo(currentBFO);
  }

  rx.setAutomaticGainControl(disableAgc, agcNdx);

  delay(50);
  currentFrequency = rx.getFrequency();
  b.currentFreq = currentFrequency;
  currentMode = nextMode;

#if DEBUG_SSB
  Serial.printf("[SSB] doMode: %s, usblsb=%u, f=%u kHz, ssbLoaded=%s\n",
                (currentMode==AM)?"AM":(currentMode==USB)?"USB":"LSB",
                (currentMode==USB)?1:0, currentFrequency, ssbLoaded?"true":"false");
#endif

  oledShowFrequencyScreen();
  elapsedCommand = millis();
}

// ========= Volume/Seek/SoftMute/Region/Menu =========
void doVolume( int8_t v ) {
  if ( v == 1) rx.volumeUp(); else rx.volumeDown();
  elapsedCommand = millis();
}

void showFrequencySeek(uint16_t freq) {
  currentFrequency = freq;
  if (!oledEdit && !isMenuMode()) oledShowFrequencyScreen();
}

void doSeek() {
  if ((currentMode == LSB || currentMode == USB)) return;
  rx.seekStationProgress(showFrequencySeek, seekDirection);
  currentFrequency = rx.getFrequency();

  if (rx.isCurrentTuneFM()) {
    rdsResetTop();
    rdsResetBottom();
  }
}

void doSoftMute(int8_t v) {
  softMuteMaxAttIdx = (v == 1) ? softMuteMaxAttIdx + 1 : softMuteMaxAttIdx - 1;
  if (softMuteMaxAttIdx > 32) softMuteMaxAttIdx = 0;
  else if (softMuteMaxAttIdx < 0) softMuteMaxAttIdx = 32;

  if (currentMode == FM) {
    rx.setFmSoftMuteMaxAttenuation(softMuteMaxAttIdx);
  } else if (currentMode == LSB || currentMode == USB) {
    rx.setSsbSoftMuteMaxAttenuation(softMuteMaxAttIdx);
  } else {
    rx.setAmSoftMuteMaxAttenuation(softMuteMaxAttIdx);
  }

  elapsedCommand = millis();
}

void doRegion(int8_t v) {
  if (v == 1 || v == -1) {
    amRegion = (amRegion == REGION_9KHZ) ? REGION_10KHZ : REGION_9KHZ;
  }
  rx.setSeekAmSpacing(getSeekSpacingForCurrentBand());

  if (isBroadcastMWBandIdx(bandIdx)) {
    uint16_t fNew = alignMwToRegion(currentFrequency, band[bandIdx].minimumFreq, band[bandIdx].maximumFreq, amRegion);
    if (fNew != currentFrequency) {
      currentFrequency = fNew;
      rx.setFrequency(currentFrequency);
      oledShowFrequencyScreen();
    }
  }
  if (oledEdit) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.setTextSize(1);
    oled.print("Region");
    oled.setCursor(0, 16);
    oled.print((amRegion == REGION_9KHZ) ? "9 kHz" : "10 kHz");
    oled.display();
  }
  elapsedCommand = millis();
  resetEepromDelay();
}

// ========= Menu/Edit helpers =========
void doMenu( int8_t v) {
  menuIdx = (v == 1) ? menuIdx + 1 : menuIdx - 1;
  if (menuIdx > lastMenu) menuIdx = 0;
  else if (menuIdx < 0)   menuIdx = lastMenu;
  showMenu();
  delay(MIN_ELAPSED_TIME);
  elapsedCommand = millis();
}

void enterEditScreen() { oledEdit = true; }
void leaveEditScreen() {
  oledEdit = false;
  disableCommands();
  oledShowFrequencyScreen();
}

void doCurrentMenuCmd() {
  disableCommands();
  switch (currentMenuCmd) {
    case 0: cmdVolume = true; enterEditScreen(); break;
    case 1: cmdStep   = true; enterEditScreen(); break;
    case 2: cmdMode   = true; enterEditScreen(); break;
    case 3: bfoOn     = true; enterEditScreen(); break;
    case 4: cmdBandwidth = true; enterEditScreen(); break;
    case 5: cmdAgc    = true; enterEditScreen(); break;
    case 6: cmdSoftMuteMaxAtt = true; enterEditScreen(); break;
    case 7: cmdRegion = true; enterEditScreen(); break;
    case 8: seekDirection = 1; doSeek(); oledShowFrequencyScreen(); break;
    case 9: seekDirection = 0; doSeek(); oledShowFrequencyScreen(); break;
    case 10: cmdRds = true; enterEditScreen(); break;
    case 11: cmdAntcap = true; enterEditScreen(); break;
    default: break;
  }
  currentMenuCmd = -1;
  elapsedCommand = millis();
}

bool isMenuMode() {
  return (cmdMenu || cmdStep || cmdBandwidth || cmdAgc || cmdVolume || cmdSoftMuteMaxAtt || cmdMode || cmdRds || cmdRegion || cmdAntcap || bfoOn || cmdBand);
}

// ========= Setup =========
void setup() {
  pinMode(ENCODER_PUSH_BUTTON, INPUT_PULLUP);
  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);

  Wire.begin(ESP32_I2C_SDA, ESP32_I2C_SCL);

  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  EEPROM.begin(EEPROM_SIZE);

  if (digitalRead(ENCODER_PUSH_BUTTON) == LOW) {
    EEPROM.write(eeprom_address, 0);
    EEPROM.commit();
    oled.setTextSize(2);
    oled.setCursor(0,0); oled.print("EEPROM");
    oled.setCursor(0,16); oled.print("RESET");
    oled.display();
    delay(1500);
    oled.clearDisplay();
  }

  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_A), rotaryEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_B), rotaryEncoder, CHANGE);

  rx.setI2CFastModeCustom(100000);

  startRefClock();

  Serial.begin(115200);

  rx.getDeviceI2CAddress(RESET_PIN);
  rx.setRefClock((uint32_t)round(g_realRefHz + REFCLK_TRIM_HZ));
  rx.setRefClockPrescaler(1);
  rx.setup(RESET_PIN, 0, MW_BAND_TYPE, SI473X_ANALOG_AUDIO, XOSCEN_RCLK);

  delay(250);

  if (EEPROM.read(eeprom_address) == app_id) {
    readAllReceiverInformation();
  } else {
    rx.setVolume(volume);
  }

  useBand();
  oledShowFrequencyScreen();

  WebUI::begin();
}

// ========= EEPROM save/load =========
void saveAllReceiverInformation() {
  int addr_offset;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(eeprom_address, app_id);
  EEPROM.write(eeprom_address + 1, rx.getVolume());
  EEPROM.write(eeprom_address + 2, bandIdx);
  EEPROM.write(eeprom_address + 3, fmRDS);
  EEPROM.write(eeprom_address + 4, currentMode);
  EEPROM.write(eeprom_address + 5, currentBFO >> 8);
  EEPROM.write(eeprom_address + 6, currentBFO & 0XFF);
  uint8_t sm = (softMuteMaxAttIdx < 0) ? 0 : (softMuteMaxAttIdx > 32 ? 32 : (uint8_t)softMuteMaxAttIdx);
  EEPROM.write(eeprom_address + 7, sm);
  uint8_t agcStore = (agcIdx < 0) ? 0 : (agcIdx > 35) ? 35 : (uint8_t)agcIdx;
  EEPROM.write(eeprom_address + 8, agcStore);
  EEPROM.write(eeprom_address + 9, amRegion);
  EEPROM.write(eeprom_address + 10, antcapAuto ? 1 : 0);

  int addr_offset2 = 11;
  band[bandIdx].currentFreq = currentFrequency;
  for (int i = 0; i <= lastBand; i++) {
    EEPROM.write(addr_offset2++, (band[i].currentFreq >> 8));
    EEPROM.write(addr_offset2++, (band[i].currentFreq & 0xFF));
    EEPROM.write(addr_offset2++, band[i].currentStepIdx);
    EEPROM.write(addr_offset2++, band[i].bandwidthIdx);
  }
  EEPROM.commit();
  EEPROM.end();
}

void readAllReceiverInformation() {
  uint8_t vol; int addr_offset; int bwIdx;
  EEPROM.begin(EEPROM_SIZE);

  vol = EEPROM.read(eeprom_address + 1);
  bandIdx = EEPROM.read(eeprom_address + 2);
  fmRDS = EEPROM.read(eeprom_address + 3);
  currentMode = EEPROM.read(eeprom_address + 4);
  currentBFO = EEPROM.read(eeprom_address + 5) << 8;
  currentBFO |= EEPROM.read(eeprom_address + 6);
  uint8_t sm = EEPROM.read(eeprom_address + 7);
  if (sm > 32) sm = 24;
  softMuteMaxAttIdx = (int8_t)sm;
  uint8_t agcLoad = EEPROM.read(eeprom_address + 8);
  if (agcLoad > 35) agcLoad = 0;
  agcIdx = (int8_t)agcLoad;
  disableAgc = (agcIdx > 0);
  agcNdx = (agcIdx > 1) ? (agcIdx - 1) : 0;
  amRegion = EEPROM.read(eeprom_address + 9);
  if (amRegion > REGION_10KHZ) amRegion = REGION_9KHZ;
  uint8_t ac = EEPROM.read(eeprom_address + 10);
  antcapAuto = (ac == 0xFF) ? true : (ac ? true : false);

  addr_offset = 11;
  for (int i = 0; i <= lastBand; i++) {
    band[i].currentFreq = EEPROM.read(addr_offset++) << 8;
    band[i].currentFreq |= EEPROM.read(addr_offset++);
    band[i].currentStepIdx = EEPROM.read(addr_offset++);
    band[i].bandwidthIdx = EEPROM.read(addr_offset++);
  }

  EEPROM.end();

  currentFrequency = band[bandIdx].currentFreq;

  if (band[bandIdx].bandType == FM_BAND_TYPE) {
    currentStepIdx = idxFmStep = band[bandIdx].currentStepIdx;
    rx.setFrequencyStep(tabFmStep[currentStepIdx]);
  } else {
    currentStepIdx = idxAmStep = band[bandIdx].currentStepIdx;
    rx.setFrequencyStep(tabAmStep[currentStepIdx]);
  }

  bwIdx = band[bandIdx].bandwidthIdx;

  if (currentMode == LSB || currentMode == USB) {
    loadSSB();
    bwIdxSSB = (bwIdx > 5) ? 5 : bwIdx;
    rx.setSSBAudioBandwidth(bandwidthSSB[bwIdxSSB].idx);
    if (bandwidthSSB[bwIdxSSB].idx == 0 || bandwidthSSB[bwIdxSSB].idx == 4 || bandwidthSSB[bwIdxSSB].idx == 5)
      rx.setSSBSidebandCutoffFilter(0);
    else
      rx.setSSBSidebandCutoffFilter(1);
  } else if (currentMode == AM) {
    bwIdxAM = bwIdx;
    rx.setBandwidth(bandwidthAM[bwIdxAM].idx, 1);
  } else {
    bwIdxFM = bwIdx;
    rx.setFmBandwidth(bandwidthFM[bwIdxFM].idx);
  }

  delay(50);
  rx.setVolume(vol);
  rx.setSeekAmSpacing(getSeekSpacingForCurrentBand());
}

// ========= State helpers =========
void resetEepromDelay() {
  elapsedCommand = storeTime = millis();
  itIsTimeToSave = true;
}

void disableCommands() {
  cmdBand = false; bfoOn = false; cmdVolume = false; cmdAgc = false;
  cmdBandwidth = false; cmdStep = false; cmdMode = false;
  cmdMenu = false; cmdSoftMuteMaxAtt = false; cmdRds = false; cmdRegion = false; cmdAntcap = false; countClick = 0;
  oledEdit = false;
}

// ========= Loop =========
void loop() {
  if (encoderCount != 0) {
    if (bfoOn && (currentMode == LSB || currentMode == USB)) {
      currentBFO = (encoderCount == 1) ? (currentBFO + currentBFOStep) : (currentBFO - currentBFOStep);
      rx.setSSBBfo(currentBFO);
#if DEBUG_SSB
      Serial.printf("[SSB] BFO=%d (step=%u Hz) @ f=%u kHz\n", (int)currentBFO, (unsigned)currentBFOStep, currentFrequency);
#endif
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("BFO"); oled.setCursor(0,16); oled.print(currentBFO); oled.display(); }
    } else if (cmdMenu) {
      doMenu(encoderCount);
    } else if (cmdMode) {
      doMode(encoderCount);
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("Mode"); oled.setCursor(0,16); oled.print((currentMode==FM)?"FM":(currentMode==AM)?"AM":(currentMode==LSB)?"LSB":"USB"); oled.display(); }
    } else if (cmdStep) {
      doStep(encoderCount);
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("Step"); oled.setCursor(0,16); oled.print((currentMode==FM)? (tabFmStep[currentStepIdx]*10) : tabAmStep[currentStepIdx]); oled.display(); }
    } else if (cmdAgc) {
      doAgc(encoderCount);
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("AGC/Att"); oled.setCursor(0,16); oled.print((disableAgc)?agcNdx:0); oled.display(); }
    } else if (cmdBandwidth) {
      doBandwidth(encoderCount);
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("BW"); oled.setCursor(0,16); oled.print((currentMode==AM)? bandwidthAM[bwIdxAM].desc : (currentMode==FM)? bandwidthFM[bwIdxFM].desc : bandwidthSSB[bwIdxSSB].desc); oled.display(); }
    } else if (cmdVolume) {
      doVolume(encoderCount);
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("Volume"); oled.setCursor(0,16); oled.print((int)rx.getVolume()); oled.display(); }
    } else if (cmdSoftMuteMaxAtt) {
      doSoftMute(encoderCount);
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("SoftMute"); oled.setCursor(0,16); oled.print(softMuteMaxAttIdx); oled.display(); }
    } else if (cmdRegion) {
      doRegion(encoderCount);
    } else if (cmdRds) {
      fmRDS = !fmRDS;
      rdsResetTop(); rdsResetBottom();
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("RDS"); oled.setCursor(0,16); oled.print(fmRDS?"ON":"OFF"); oled.display(); }
      resetEepromDelay();
      elapsedCommand = millis();
    } else if (cmdAntcap) {
      antcapAuto = !antcapAuto;
      if (!rx.isCurrentTuneFM()) {
        rx.setTuneFrequencyAntennaCapacitor(antcapAuto ? 0 : 1);
        rx.setFrequency(currentFrequency);
      }
      if (oledEdit) { oled.clearDisplay(); oled.setCursor(0,0); oled.print("ANTCAP"); oled.setCursor(0,16); oled.print(antcapAuto?"Auto":"Hold"); oled.display(); }
      resetEepromDelay();
      elapsedCommand = millis();
    } else if (cmdBand) {
      setBand(encoderCount);
    } else {
      if (encoderCount == 1) rx.frequencyUp();
      else                   rx.frequencyDown();
      uint16_t prev = currentFrequency;
      currentFrequency = rx.getFrequency();
      if (rx.isCurrentTuneFM() && currentFrequency != prev) {
        rdsResetTop();
        rdsResetBottom();
      }
      if (!isMenuMode() && !oledEdit) {
        oledShowFrequencyScreen();
      }
    }
    encoderCount = 0;
    resetEepromDelay();
  } else {
    if (digitalRead(ENCODER_PUSH_BUTTON) == LOW) {
      countClick++;
      if (cmdMenu) {
        currentMenuCmd = menuIdx;
        doCurrentMenuCmd();
      } else if (countClick == 1) {
        if (oledEdit || isMenuMode()) {
          leaveEditScreen();
        } else if (bfoOn) {
          bfoOn = false;
          oledShowFrequencyScreen();
        } else {
          cmdBand = !cmdBand;
          showCommandStatus((char *)"Band");
        }
      } else {
        cmdMenu = !cmdMenu;
        if (cmdMenu) showMenu();
        else         oledShowFrequencyScreen();
      }
      delay(MIN_ELAPSED_TIME);
      elapsedCommand = millis();
    }
  }

  if ((millis() - elapsedRSSI) > MIN_ELAPSED_RSSI_TIME * 6) {
    rx.getCurrentReceivedSignalQuality();
    uint8_t newRssi = rx.getCurrentRSSI();
    uint8_t newSnr  = rx.getCurrentSNR();
    if ((rssi != newRssi || snr != newSnr) && !isMenuMode() && !oledEdit) {
      rssi = newRssi;
      snr  = newSnr;
      oledShowRSSI();
    }
    elapsedRSSI = millis();
  }

  if (rx.isCurrentTuneFM() && fmRDS) {
    rx.getRdsStatus();
    rdsPollTop();
    rdsPollBottom();
  }

  if ((millis() - elapsedCommand) > ELAPSED_COMMAND) {
    if (oledEdit || isMenuMode()) {
      leaveEditScreen();
    }
    elapsedCommand = millis();
  }

  if ((millis() - elapsedClick) > ELAPSED_CLICK) {
    countClick = 0;
    elapsedClick = millis();
  }

  if (itIsTimeToSave) {
    if ((millis() - storeTime) > STORE_TIME) {
      saveAllReceiverInformation();
      storeTime = millis();
      itIsTimeToSave = false;
    }
  }

  WebUI::loop();
  delay(5);
}
