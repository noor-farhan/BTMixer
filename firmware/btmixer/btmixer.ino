// BTMixer - wireless per-app volume mixer (MaxMix-style)
// Board:     ESP32C3 Dev Module (ESP32-C3 Super Mini)
// Libraries: U8g2, NimBLE-Arduino 2.x
//
// Controls:  rotate            = volume of selected app
//            click             = mute / unmute
//            double-click      = next app (entry 1 is Master)
//            long press        = previous app
//            hold + rotate     = switch app (still works)
//
// Protocol (text, over a Nordic-UART-style BLE service):
//   PC -> device (write):  S|idx|count|name|vol|mute
//   device -> PC (notify): V|idx|vol    M|idx

#include <Arduino.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>

// ---------- Pins ----------
constexpr int PIN_SDA    = 8;
constexpr int PIN_SCL    = 9;
constexpr int PIN_ENC_A  = 0;
constexpr int PIN_ENC_B  = 1;
constexpr int PIN_ENC_SW = 3;

// ---------- Tuning ----------
constexpr int  STEPS_PER_DETENT = 4;   // quadrature edges per click; try 2 if your knob skips
constexpr bool ENC_INVERT       = false;
constexpr int  VOL_STEP         = 2;   // % per detent
constexpr int  MAX_SESSIONS     = 12;
constexpr uint32_t SEND_INTERVAL_MS = 40;
constexpr uint32_t DOUBLE_CLICK_MS  = 300;  // window for a second click
constexpr uint32_t LONG_PRESS_MS    = 600;

#define NUS_SERVICE "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // PC -> device
#define NUS_TX      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // device -> PC

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

struct Session { char name[16]; uint8_t vol; bool mute; };
Session sessions[MAX_SESSIONS];
int  count = 0;
int  sel   = 0;
bool dirty = true;

volatile bool connected = false;
NimBLECharacteristic* txChar = nullptr;
QueueHandle_t rxQueue;

// ---------- Encoder (interrupt, state table) ----------
volatile int32_t encPos = 0;
volatile uint8_t encState = 0;
const int8_t ENC_TABLE[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};

void IRAM_ATTR encISR() {
  encState = ((encState << 2) | (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B)) & 0x0F;
  encPos += ENC_TABLE[encState];
}

// ---------- Button ----------
bool btnRaw = false, btnDown = false, rotatedWhileHeld = false;
uint32_t btnTime = 0, pressTime = 0, clickTime = 0;
bool longFired = false, waitSecond = false;

// ---------- Volume send throttle ----------
bool pendingVol = false;
int  pendingIdx = 0;
uint32_t lastSend = 0;

void sendLine(const char* s) {
  if (connected && txChar) txChar->notify((const uint8_t*)s, strlen(s));
}

void flushVolume() {
  if (!pendingVol) return;
  char buf[24];
  snprintf(buf, sizeof buf, "V|%d|%d", pendingIdx, sessions[pendingIdx].vol);
  sendLine(buf);
  pendingVol = false;
  lastSend = millis();
}

// ---------- BLE ----------
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override { connected = true; dirty = true; }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    connected = false;
    NimBLEDevice::startAdvertising();
  }
};

class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue val = c->getValue();
    char buf[48];
    size_t n = min((size_t)val.length(), sizeof(buf) - 1);
    memcpy(buf, val.data(), n);
    buf[n] = 0;
    xQueueSend(rxQueue, buf, 0);
  }
};

void handleMessage(char* msg) {
  char* save;
  char* t = strtok_r(msg, "|", &save);
  if (!t || t[0] != 'S') return;
  char* f[5];
  for (int i = 0; i < 5; i++) {
    f[i] = strtok_r(nullptr, "|", &save);
    if (!f[i]) return;
  }
  int idx = atoi(f[0]), n = atoi(f[1]);
  if (idx < 0 || idx >= MAX_SESSIONS || n < 0 || n > MAX_SESSIONS) return;
  strlcpy(sessions[idx].name, f[2], sizeof sessions[idx].name);
  sessions[idx].vol  = constrain(atoi(f[3]), 0, 100);
  sessions[idx].mute = atoi(f[4]) != 0;
  count = n;
  if (sel >= count) sel = max(0, count - 1);
  dirty = true;
}

// ---------- Display ----------
// Cyberpunk HUD skin. Purely visual: nothing in here changes behaviour.

// corner brackets  [ ]  around a rectangle
static void uiBrackets(int x, int y, int w, int h, int l) {
  u8g2.drawHLine(x, y, l);              u8g2.drawVLine(x, y, l);
  u8g2.drawHLine(x + w - l, y, l);      u8g2.drawVLine(x + w - 1, y, l);
  u8g2.drawHLine(x, y + h - 1, l);      u8g2.drawVLine(x, y + h - l, l);
  u8g2.drawHLine(x + w - l, y + h - 1, l); u8g2.drawVLine(x + w - 1, y + h - l, l);
}

// 20-cell segmented level bar with a ruler on top.
// full cell = solid block, empty cell = thin line
static void uiSegBar(int y, int filled) {
  for (int i = 0; i <= 20; i++) {
    int x = 4 + i * 6;
    if (i % 5 == 0) u8g2.drawVLine(x, y - 5, 3);   // major tick
    else            u8g2.drawVLine(x, y - 3, 1);   // minor tick
    if (i == 20) break;
    if (i < filled) u8g2.drawBox(x, y, 5, 8);
    else            u8g2.drawHLine(x, y + 7, 5);
  }
}

void draw() {
  u8g2.clearBuffer();
  uiBrackets(0, 0, 128, 64, 4);

  if (!connected || count == 0) {
    // ---- idle / link screen ----
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(5, 9, "BTMIXER//CTRL");
    const char* st = connected ? "SYNC" : "OFFLINE";
    u8g2.drawStr(124 - u8g2.getStrWidth(st), 9, st);
    u8g2.drawHLine(0, 12, 128);

    u8g2.setFont(u8g2_font_helvB10_tr);
    int tw = u8g2.getStrWidth("BT MIXER");
    int tx = (128 - tw) / 2;
    u8g2.drawStr(tx, 30, "BT MIXER");
    u8g2.drawHLine(6, 25, tx - 12);
    u8g2.drawHLine(tx + tw + 6, 25, 122 - (tx + tw + 6));
    u8g2.drawBox(6, 24, 3, 3);
    u8g2.drawBox(119, 24, 3, 3);

    u8g2.setFont(u8g2_font_5x7_tr);
    const char* msg = connected ? "> SYNCING..." : "> AWAITING PC LINK_";
    int mw = u8g2.getStrWidth(msg);
    u8g2.drawFrame(10, 36, 108, 12);
    if (connected) {
      u8g2.drawBox(10, 36, 108, 12);
      u8g2.setDrawColor(0);
      u8g2.drawStr((128 - mw) / 2, 45, msg);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr((128 - mw) / 2, 45, msg);
    }

    uiSegBar(53, 0);
  } else {
    // ---- mixer screen ----
    Session& s = sessions[sel];

    char nm[16];
    size_t k = 0;
    for (; k < sizeof nm - 1 && s.name[k]; k++) nm[k] = toupper((unsigned char)s.name[k]);
    nm[k] = 0;

    // header: app name (inverted = "hold to switch app") + channel index
    u8g2.setFont(u8g2_font_6x10_tr);
    int w = u8g2.getStrWidth(nm);
    if (btnDown) {
      u8g2.drawBox(0, 0, w + 6, 12);
      u8g2.setDrawColor(0);
      u8g2.drawStr(3, 9, nm);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(3, 9, nm);
    }

    char idx[8];
    snprintf(idx, sizeof idx, "%02d/%02d", sel + 1, count);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(124 - u8g2.getStrWidth(idx), 9, idx);

    u8g2.drawHLine(0, 12, 128);
    for (int x = 4; x < 128; x += 8) u8g2.drawPixel(x, 14);   // ruler ticks

    // left: big readout
    if (s.mute) {
      u8g2.setFont(u8g2_font_helvB14_tr);
      int mw = u8g2.getStrWidth("MUTED");
      u8g2.drawBox(3, 21, mw + 6, 21);
      u8g2.setDrawColor(0);
      u8g2.drawStr(6, 37, "MUTED");
      u8g2.setDrawColor(1);
    } else {
      char v[8];
      snprintf(v, sizeof v, "%03d", s.vol);
      u8g2.setFont(u8g2_font_logisoso24_tn);
      int vw = u8g2.getStrWidth(v);
      u8g2.drawStr(4, 44, v);
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(4 + vw + 2, 44, "%");
    }

    // right: data table
    u8g2.drawVLine(65, 16, 30);
    u8g2.drawHLine(66, 25, 61);
    u8g2.drawHLine(66, 35, 61);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(69, 23, "SRC");
    u8g2.drawStr(69, 33, "STAT");
    u8g2.drawStr(69, 43, "LINK");

    const char* src = (sel == 0) ? "MASTER" : "APP";
    u8g2.drawStr(124 - u8g2.getStrWidth(src), 23, src);

    const char* stat = s.mute ? "MUTE" : "LIVE";
    int sw = u8g2.getStrWidth(stat);
    if (s.mute) {
      u8g2.drawBox(124 - sw - 2, 26, sw + 4, 9);
      u8g2.setDrawColor(0);
      u8g2.drawStr(124 - sw, 33, stat);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(124 - sw, 33, stat);
    }

    const char* lnk = "BLE//OK";
    u8g2.drawStr(124 - u8g2.getStrWidth(lnk), 43, lnk);

    // bottom: segmented level bar
    u8g2.drawHLine(0, 46, 128);
    uiSegBar(53, s.mute ? 0 : (s.vol * 20 + 99) / 100);
  }
  u8g2.sendBuffer();
}

// ---------- Actions ----------
void changeSel(int dir) {
  if (count == 0) return;
  flushVolume();
  sel = ((sel + dir) % count + count) % count;
  dirty = true;
}

void toggleMute() {
  if (count == 0) return;
  flushVolume();
  sessions[sel].mute = !sessions[sel].mute;
  char buf[16];
  snprintf(buf, sizeof buf, "M|%d", sel);
  sendLine(buf);
  dirty = true;
}

// ---------- Setup / loop ----------
void setup() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  attachInterrupt(PIN_ENC_A, encISR, CHANGE);
  attachInterrupt(PIN_ENC_B, encISR, CHANGE);

  u8g2.begin();
  u8g2.setBusClock(400000);

  rxQueue = xQueueCreate(16, 48);

  NimBLEDevice::init("BTMixer");
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCb());
  NimBLEService* svc = server->createService(NUS_SERVICE);
  txChar = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* rx =
      svc->createCharacteristic(NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCb());
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->enableScanResponse(true);
  adv->start();
}

void loop() {
  // messages from the PC
  char msg[48];
  while (xQueueReceive(rxQueue, msg, 0) == pdTRUE) handleMessage(msg);
  if (!connected && count != 0) { count = 0; sel = 0; dirty = true; }

  // encoder
  noInterrupts();
  int32_t d = encPos / STEPS_PER_DETENT;
  encPos -= d * STEPS_PER_DETENT;
  interrupts();
  if (ENC_INVERT) d = -d;

  if (d != 0 && count > 0) {
    if (btnDown) {                              // hold + rotate = switch app
      flushVolume();
      rotatedWhileHeld = true;
      sel = ((sel + d) % count + count) % count;
    } else {                                    // rotate = volume
      Session& s = sessions[sel];
      s.vol = constrain((int)s.vol + (int)d * VOL_STEP, 0, 100);
      pendingVol = true;
      pendingIdx = sel;
    }
    dirty = true;
  }

  // button (debounced)
  bool raw = digitalRead(PIN_ENC_SW) == LOW;
  if (raw != btnRaw) { btnRaw = raw; btnTime = millis(); }
  if (millis() - btnTime > 25 && raw != btnDown) {
    btnDown = raw;
    if (btnDown) {
      rotatedWhileHeld = false;
      longFired = false;
      pressTime = millis();
    } else if (!rotatedWhileHeld && !longFired && count > 0) {
      if (waitSecond) {                 // second click in time = next app
        waitSecond = false;
        changeSel(+1);
      } else {                          // first click: wait to see if a second follows
        waitSecond = true;
        clickTime = millis();
      }
    }
    dirty = true;
  }

  // long press = previous app
  if (btnDown && !rotatedWhileHeld && !longFired && count > 0 &&
      millis() - pressTime > LONG_PRESS_MS) {
    longFired = true;
    waitSecond = false;
    changeSel(-1);
  }

  // no second click arrived = it was a single click = mute
  if (waitSecond && millis() - clickTime > DOUBLE_CLICK_MS) {
    waitSecond = false;
    toggleMute();
  }

  if (pendingVol && millis() - lastSend >= SEND_INTERVAL_MS) flushVolume();

  static uint32_t lastDraw = 0;
  if (dirty && millis() - lastDraw >= 30) {
    dirty = false;
    lastDraw = millis();
    draw();
  }
}
