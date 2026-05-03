// Device auto-detection via board macro (set by Arduino FQBN)
#if defined(ARDUINO_M5STACK_CARDPUTER)
  #include <M5Cardputer.h>
  // M5Cardputer's Keyboard_def.h macros conflict with BleKeyboard.h consts
  #undef KEY_LEFT_CTRL
  #undef KEY_LEFT_SHIFT
  #undef KEY_LEFT_ALT
  #undef KEY_LEFT_GUI
  #undef KEY_RIGHT_CTRL
  #undef KEY_RIGHT_SHIFT
  #undef KEY_RIGHT_ALT
  #undef KEY_RIGHT_GUI
  #undef KEY_BACKSPACE
  #undef KEY_TAB
  #undef KEY_RETURN
  #undef KEY_ESC
  #undef KEY_INSERT
  #undef KEY_DELETE
  #undef KEY_PAGE_UP
  #undef KEY_PAGE_DOWN
  #undef KEY_HOME
  #undef KEY_END
  #undef KEY_CAPS_LOCK
  #undef KEY_F1
  #undef KEY_F2
  #undef KEY_F3
  #undef KEY_F4
  #undef KEY_F5
  #undef KEY_F6
  #undef KEY_F7
  #undef KEY_F8
  #undef KEY_F9
  #undef KEY_F10
  #undef KEY_F11
  #undef KEY_F12
  #undef KEY_UP_ARROW
  #undef KEY_DOWN_ARROW
  #undef KEY_LEFT_ARROW
  #undef KEY_RIGHT_ARROW
  #undef KEY_NUM_ENTER
  #undef KEY_PRTSC
  #define DEVICE_NAME   "Cardputer-KB"
  #define SCREEN_W      240
  #define SCREEN_H      135
  #define IS_CARDPUTER  1
#elif defined(ARDUINO_M5STACK_STICKC_PLUS)
  #include <M5StickCPlus.h>
  #define DEVICE_NAME   "M5StickCP-KB"
  #define SCREEN_W      135
  #define SCREEN_H      240
  #define LCD_ROTATION  2
#else
  #include <M5StickC.h>
  #define DEVICE_NAME   "M5StickC-KB"
  #define SCREEN_W      80
  #define SCREEN_H      160
  #define LCD_ROTATION  2
#endif

#include <BleKeyboard.h>
#include <esp_mac.h>
#include <esp_sleep.h>

// 子类化 BleKeyboard 以监听 host 推送的 LED output report（Caps Lock / NumLock）。
// 用作"远程进配置模式"触发：短时间内 Caps Lock LED 切换 ≥6 次 → 进配置模式。
// 6 次对手动操作来说几乎不可能，但 AppleScript 模拟 3 次连按（共 6 次 LED 变化）可触发。
volatile uint8_t  g_caps_state = 0;
volatile uint8_t  g_caps_toggle_count = 0;
volatile unsigned long g_caps_window_start = 0;
const unsigned long CAPS_TRIGGER_WINDOW_MS = 2000;
const uint8_t CAPS_TRIGGER_COUNT = 6;
volatile bool g_remote_trigger_fired = false;

class BleKeyboardWithLed : public BleKeyboard {
public:
  BleKeyboardWithLed(std::string n, std::string m, uint8_t b) : BleKeyboard(n, m, b) {}
  void onWrite(BLECharacteristic* me) override {
    BleKeyboard::onWrite(me);
    std::string v = me->getValue();
    if (v.empty()) return;
    uint8_t led = (uint8_t)v[0];
    uint8_t caps_now = (led >> 1) & 1;
    if (caps_now == g_caps_state) return;
    g_caps_state = caps_now;
    unsigned long now = millis();
    if (now - g_caps_window_start > CAPS_TRIGGER_WINDOW_MS) {
      g_caps_window_start = now;
      g_caps_toggle_count = 1;
    } else {
      g_caps_toggle_count++;
      if (g_caps_toggle_count >= CAPS_TRIGGER_COUNT && !g_remote_trigger_fired) {
        g_remote_trigger_fired = true;
        // 不能在 BLE 回调里直接 esp_restart，标志位让 loop() 处理
      }
    }
  }
};

BleKeyboardWithLed bleKeyboard(DEVICE_NAME, "M5Stack", 100);
char fullDeviceName[32];  // "DeviceName-XXYY" with BLE MAC suffix

bool connected = false;
bool prevConnected = false;
unsigned long lastBatUpdate = 0;
unsigned long lastActivity = 0;
unsigned long lastLedBlink = 0;
unsigned long ledBreathStart = 0;
bool ledBreathing = false;
bool screenOn = true;
unsigned long lastPowerCheck = 0;
int lastPowerCheckBat = -1;
bool warnedAt20 = false;
bool warnedAt10 = false;

const unsigned long BAT_INTERVAL_ACTIVE = 1000;  // 1s: fast refresh to catch state changes
const unsigned long BAT_INTERVAL_IDLE = 300000;
const unsigned long SCREEN_TIMEOUT = 5000;
const unsigned long POWEROFF_TIMEOUT = 1800000;
const unsigned long UNPAIRED_POWEROFF = 300000;  // 5 min: auto off if never connected
const int LOWBAT_THRESHOLD = 5;
const int CHARGING_DROP_MARGIN = 2;
const int BUZZER_FREQ = 2500;
const int BUZZER_PIN = 2;
const bool BUZZER_TEST_ON_BOOT = false;
const unsigned long LED_BLINK_INTERVAL = 10000;
const unsigned long LED_BREATH_DURATION = 1000;
const int LED_BREATH_PEAK = 240;
const int LED_PIN = 10;
const int SCREEN_BRIGHTNESS = 80;

#if IS_CARDPUTER
// ===== WiFi 配置模式 =====
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

enum CtrlAction { CTRL_OPT_TAB=0, CTRL_CMD_SPACE=1, CTRL_CMD_SHIFT_SEMI=2, CTRL_NONE=3 };
enum OptAction  { OPT_CAPS_LOCK=0, OPT_ESC=1, OPT_NONE=2 };
enum FnAction   { FN_ENTER=0, FN_TAB=1, FN_ESC=2, FN_NONE=3 };
enum BacktickAction { BT_ESC=0, BT_BACKTICK=1 };
enum NumAction { NUM_CMD_N=0, NUM_LITERAL=1 };

struct KbConfig {
  uint8_t ctrl_action     = CTRL_OPT_TAB;
  uint8_t opt_action      = OPT_CAPS_LOCK;
  uint8_t fn_action       = FN_ENTER;
  uint8_t backtick_action = BT_ESC;
  uint8_t num_action      = NUM_CMD_N;
};
KbConfig g_config;

bool g_config_mode = false;
WebServer* g_web = nullptr;
char g_apSsid[32] = {0};

// Long-press Fn detector (entry into config mode)
unsigned long fnPressStart = 0;
const unsigned long FN_LONG_PRESS_MS = 5000;

// Modifier tap-hold state (tap = special function, hold = BLE modifier)
bool fnPrevHeld = false;
bool fnUsedAsModifier = false;
bool ctrlPrevHeld = false;
bool ctrlUsedAsModifier = false;
bool optPrevHeld = false;
bool optUsedAsModifier = false;
bool capsLocked = false;

// Key repeat state
uint8_t heldKey = 0;            // which key is being held (0 = none)
unsigned long heldKeyStart = 0; // when the key was first pressed
unsigned long heldKeyLast = 0;  // when last repeat was sent
const unsigned long REPEAT_DELAY = 400;  // ms before repeat starts
const unsigned long REPEAT_RATE  = 50;   // ms between repeats
const uint8_t SCROLL_UP   = 0xFE;  // sentinel for key repeat tracking
const uint8_t SCROLL_DOWN = 0xFF;
const int8_t  SCROLL_STEP = 3;     // scroll wheel delta per tick
#endif

void buzzerTone(int freq, int ms) {
  tone(BUZZER_PIN, freq);
  delay(ms);
  noTone(BUZZER_PIN);
}

void beepLowBattery() {
  buzzerTone(BUZZER_FREQ, 150);
  delay(100);
  buzzerTone(BUZZER_FREQ, 150);
}

void beepPowerOff() {
  buzzerTone(2500, 300);
  delay(50);
  buzzerTone(2000, 300);
  delay(50);
  buzzerTone(1500, 400);
}

int getBatPercent() {
#if IS_CARDPUTER
  int pct = M5Cardputer.Power.getBatteryLevel();
#else
  float v = M5.Axp.GetBatVoltage();
  int pct = (int)((v - 3.0f) / (4.2f - 3.0f) * 100.0f);
#endif
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return pct;
}

void screenWake() {
  if (!screenOn) {
#if IS_CARDPUTER
    M5Cardputer.Display.wakeup();
    M5Cardputer.Display.setBrightness(SCREEN_BRIGHTNESS);
#else
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(SCREEN_BRIGHTNESS);
#endif
    screenOn = true;
    drawStatus();
  }
  lastActivity = millis();
#if !IS_CARDPUTER
  lastPowerCheck = millis();
  lastPowerCheckBat = getBatPercent();
#endif
}

void screenSleep() {
  if (screenOn) {
#if IS_CARDPUTER
    M5Cardputer.Display.setBrightness(0);
    M5Cardputer.Display.sleep();
#else
    M5.Axp.SetLDO2(false);
    lastLedBlink = millis();
#endif
    screenOn = false;
  }
}

// ============================================================
//  Display functions — completely separate per device
// ============================================================

#if IS_CARDPUTER
// ----- M5Cardputer: landscape 240x135 -----

// Font metrics
const int FONT1_W = 6;
const int FONT1_H = 8;
const int FONT2_W = 12;
const int FONT2_H = 16;
const int FONT15_W = 9;   // font size 1.5
const int FONT3_W = 18;
const int FONT3_H = 24;

// Key block colors
#define COL_KEY_NUM  0x07FF  // cyan
#define COL_KEY_FN   0xF81F  // magenta
#define COL_KEY_ENT  0xFFE0  // yellow
#define COL_KEY_ESC  0xFD20  // orange

// Key block layout
const int KEY_CORNER_R    = 3;
const int KEY_LABEL_PAD   = 4;   // horizontal padding for label fit check
const int KEY_LABEL_Y1    = 2;   // label y offset when size 2
const int KEY_LABEL_Y1_SM = 6;   // label y offset when shrunk to size 1
const int KEY_ACTION_PAD  = 2;   // horizontal padding for action fit check
const int KEY_ACTION_BOT1 = 14;  // action bottom offset when size 1.5
const int KEY_ACTION_BOT2 = 10;  // action bottom offset when size 1

// Status bar
const int STATUS_MARGIN   = 4;
const int STATUS_TEXT_Y   = 4;

// BLE indicator
const int BLE_DOT_R       = 4;
const int BLE_TEXT_W      = 18;  // "BLE" = 3 chars * FONT1_W
const int BLE_DOT_CLEAR_PAD = 2;
const int BLE_DOT_Y       = 8;
const int BLE_BAR_H       = 12;
const int BLE_LABEL_GAP   = 2;  // gap between dot and "BLE" text
const int BLE_PCT_GAP     = 4;  // gap between "BLE" text and percentage

// Connected screen key grid
const int GRID_TOP_Y      = 22;
const int GRID_BLOCK_H    = 35;
const int GRID_GAP        = 3;
const int GRID_BLOCK_W    = 57;

// Waiting screen layout (4 lines, vertically centered in usable area)
const int WAIT_AREA_TOP   = 18;   // below top status bar
const int WAIT_LINE_H     = 16;   // text size 2 height
const int WAIT_LINE_GAP   = 12;   // gap between lines
// Total block: 4*16 + 3*12 = 100px, centered in 117px usable area
const int WAIT_Y1         = WAIT_AREA_TOP + (SCREEN_H - WAIT_AREA_TOP - (4 * WAIT_LINE_H + 3 * WAIT_LINE_GAP)) / 2;
const int WAIT_Y2         = WAIT_Y1 + WAIT_LINE_H + WAIT_LINE_GAP;
const int WAIT_Y3         = WAIT_Y2 + WAIT_LINE_H + WAIT_LINE_GAP;
const int WAIT_Y4         = WAIT_Y3 + WAIT_LINE_H + WAIT_LINE_GAP;

// Flash overlay
const int FLASH_DELAY      = 200;

void drawKeyBlock(int x, int y, int w, int h, uint16_t bg, const char* label, const char* action) {
  M5Cardputer.Display.fillRoundRect(x, y, w, h, KEY_CORNER_R, bg);
  M5Cardputer.Display.setTextColor(BLACK);
  // Label: size 2, auto-shrink if too wide
  int labelLen = strlen(label);
  if (labelLen * FONT2_W <= w - KEY_LABEL_PAD) {
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setCursor(x + (w - labelLen * FONT2_W) / 2, y + KEY_LABEL_Y1);
  } else {
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(x + (w - labelLen * FONT1_W) / 2, y + KEY_LABEL_Y1_SM);
  }
  M5Cardputer.Display.print(label);
  // Action: size 1.5 if fits, else size 1
  int actionLen = strlen(action);
  if (actionLen * FONT15_W <= w - KEY_ACTION_PAD) {
    M5Cardputer.Display.setTextSize(1.5);
    M5Cardputer.Display.setCursor(x + (w - actionLen * FONT15_W) / 2, y + h - KEY_ACTION_BOT1);
  } else {
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(x + (w - actionLen * FONT1_W) / 2, y + h - KEY_ACTION_BOT2);
  }
  M5Cardputer.Display.print(action);
}

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);
  if (!screenOn) return;

  // Detect power state:
  //   Method 1: hardware isCharging()
  //   Method 2: getBatteryCurrent() > 0 (positive = charging)
  //   Method 3: pct == 100 without confirmed charging = USB powered
  auto chgState = M5Cardputer.Power.isCharging();
  bool charging = (chgState == m5::Power_Class::is_charging);
  if (!charging) {
    int32_t current = M5Cardputer.Power.getBatteryCurrent();
    charging = (current > 50);  // >50mA threshold to avoid noise
  }
  bool usbPowered = (pct >= 100 && !charging);

  // Display: color by level, text suffix for state
  uint16_t batCol = pct > 50 ? GREEN : (pct > 20 ? YELLOW : RED);
  char buf[16];
  if (usbPowered) {
    snprintf(buf, sizeof(buf), "USB");
    batCol = GREEN;
  } else if (charging) {
    snprintf(buf, sizeof(buf), "%d%% CHG", pct);
  } else {
    snprintf(buf, sizeof(buf), "%d%%", pct);
  }

  int tw = strlen(buf) * FONT1_W;
  int totalW = BLE_DOT_R + BLE_LABEL_GAP + BLE_TEXT_W + BLE_PCT_GAP + tw;
  int dotX = SCREEN_W - totalW - BLE_DOT_R - BLE_DOT_CLEAR_PAD * 2;
  // Clear area for dot + BLE + battery text
  M5Cardputer.Display.fillRect(dotX - BLE_DOT_R - BLE_DOT_CLEAR_PAD, BLE_DOT_CLEAR_PAD,
    SCREEN_W - dotX + BLE_DOT_R + BLE_DOT_CLEAR_PAD * 2, BLE_BAR_H, BLACK);
  // BLE status dot + label
  uint16_t bleCol = connected ? BLUE : RED;
  M5Cardputer.Display.fillCircle(dotX, BLE_DOT_Y, BLE_DOT_R, bleCol);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(DARKGREY);
  M5Cardputer.Display.setCursor(dotX + BLE_DOT_R + BLE_LABEL_GAP, STATUS_TEXT_Y);
  M5Cardputer.Display.print("BLE");
  // Battery / power status
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(batCol);
  M5Cardputer.Display.setCursor(dotX + BLE_DOT_R + BLE_TEXT_W + BLE_PCT_GAP, STATUS_TEXT_Y);
  M5Cardputer.Display.print(buf);
}

void drawStatus() {
  if (!screenOn) return;
  M5Cardputer.Display.fillScreen(BLACK);

  // Top bar: device name (left) + BLE dot + battery (right via updateBattery)
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(DARKGREY);
  M5Cardputer.Display.setCursor(STATUS_MARGIN, STATUS_TEXT_Y);
  M5Cardputer.Display.print(fullDeviceName);
  updateBattery();

  if (!connected) {
    // 4-line waiting screen, all size 2, vertically centered
    auto centerPrint = [](const char* msg, int y, uint16_t color) {
      M5Cardputer.Display.setTextSize(2);
      M5Cardputer.Display.setTextColor(color);
      int x = (SCREEN_W - strlen(msg) * FONT2_W) / 2;
      M5Cardputer.Display.setCursor(x > 0 ? x : 0, y);
      M5Cardputer.Display.print(msg);
    };
    centerPrint("BLE Keyboard",  WAIT_Y1, CYAN);      // type first
    centerPrint(fullDeviceName,  WAIT_Y2, WHITE);    // then device name
    centerPrint("Waiting...",    WAIT_Y3, YELLOW);   // status + action
    centerPrint("Pair Bluetooth", WAIT_Y4, YELLOW);  // same color
    updateBattery();
    return;
  }

  // Row 1: 1-4   Row 2: 5-8   Row 3: Fn, Tab, Enter, Esc
  // Full height available: SCREEN_H - GRID_TOP_Y, 3 rows + 2 gaps
  const char* nums[] = {"1","2","3","4","5","6","7","8"};
  for (int i = 0; i < 4; i++) {
    char action[8];
    snprintf(action, sizeof(action), "CMD+%s", nums[i]);
    drawKeyBlock(STATUS_MARGIN + i * (GRID_BLOCK_W + GRID_GAP), GRID_TOP_Y, GRID_BLOCK_W, GRID_BLOCK_H, COL_KEY_NUM, nums[i], action);
  }
  int y2 = GRID_TOP_Y + GRID_BLOCK_H + GRID_GAP;
  for (int i = 4; i < 8; i++) {
    char action[8];
    snprintf(action, sizeof(action), "CMD+%s", nums[i]);
    drawKeyBlock(STATUS_MARGIN + (i - 4) * (GRID_BLOCK_W + GRID_GAP), y2, GRID_BLOCK_W, GRID_BLOCK_H, COL_KEY_NUM, nums[i], action);
  }
  int y3 = y2 + GRID_BLOCK_H + GRID_GAP;
  drawKeyBlock(STATUS_MARGIN, y3, GRID_BLOCK_W, GRID_BLOCK_H, COL_KEY_FN, "Ctrl", "OPT+TAB");
  drawKeyBlock(STATUS_MARGIN + GRID_BLOCK_W + GRID_GAP, y3, GRID_BLOCK_W, GRID_BLOCK_H, COL_KEY_ENT, "Fn", "ENTER");
  drawKeyBlock(STATUS_MARGIN + (GRID_BLOCK_W + GRID_GAP) * 2, y3, GRID_BLOCK_W, GRID_BLOCK_H, COL_KEY_ENT, "Enter", "ENTER");
  drawKeyBlock(STATUS_MARGIN + (GRID_BLOCK_W + GRID_GAP) * 3, y3, GRID_BLOCK_W, GRID_BLOCK_H, COL_KEY_ESC, "`", "ESC");

  updateBattery();
}

void showFlash(const char* text, uint16_t color) {
  if (!screenOn) {
    M5Cardputer.Display.wakeup();
    M5Cardputer.Display.setBrightness(SCREEN_BRIGHTNESS);
    screenOn = true;
  }
  M5Cardputer.Display.fillScreen(color);
  M5Cardputer.Display.setTextSize(3);
  M5Cardputer.Display.setTextColor(BLACK);
  int tw = strlen(text) * FONT3_W;
  M5Cardputer.Display.setCursor((SCREEN_W - tw) / 2, (SCREEN_H - FONT3_H) / 2);
  M5Cardputer.Display.print(text);
  delay(FLASH_DELAY);
  drawStatus();
  lastActivity = millis();
}

#elif defined(ARDUINO_M5STACK_STICKC_PLUS)
// ----- M5StickC Plus: portrait 135x240, USB end UP -----
// Physical buttons:
//   Button A: front face, top       → full circle, top center
//   Power:    right side edge, top  → left half-circle, right edge (flush)
//   Button B: left side edge, mid   → right half-circle, left edge (flush)

// Font metrics
const int FONT1_W = 6;
const int FONT1_H = 8;
const int FONT2_W = 12;
const int FONT2_H = 16;
const int FONT3_W = 18;
const int FONT3_H = 24;
const int FONT4_W = 24;

// Indicator/button dots
const int INDICATOR_R      = 8;
const int INDICATOR_PAD    = 2;  // gap from edge for dot center

// Block layout
const int BLOCK_X          = 16;
const int BLOCK_W          = 100;
const int BLOCK_H          = 48;
const int BLOCK_GAP        = 6;
const int BLOCK_CORNER_R   = 4;
const int BLOCK_TEXT1_Y    = 4;   // first line y offset within block
const int BLOCK_TEXT2_Y    = 26;  // second line y offset within block
const int BLOCK_LINE_GAP   = 6;  // gap from dot to first block
#define COL_BLOCK_DARK     0x4208  // dark grey for inactive block

// Status bar / bottom area
const int NAME_Y           = 210;
const int NAME_H           = 10;
const int BAT_Y            = 222;
const int BAT_AREA_H       = 18;
const int STATUS_MARGIN     = 4;

// BLE indicator (bottom bar)
const int BLE_DOT_X        = 10;
const int BLE_DOT_R        = 4;
const int BLE_DOT_CY_OFF   = 8;  // dot center y offset from batY
const int BLE_TEXT_X        = 18;
const int BLE_TEXT_Y_OFF    = 5;  // text y offset from batY

// Battery bar
const int BAT_BAR_X        = 44;
const int BAT_BAR_GAP      = 8;  // gap between bar end and percentage text
const int BAT_BAR_H        = 8;
const int BAT_BAR_Y_OFF    = 4;  // bar y offset from batY

// Disconnect text layout
const int DISC_TITLE_X     = 12;
const int DISC_TITLE_Y     = 60;
const int DISC_SUB_X       = 12;
const int DISC_SUB_Y       = 100;

// Flash overlay
const int FLASH_TEXT_Y      = 100;
const int FLASH_POWER_DELAY = 300;
const int FLASH_BTNA_DELAY  = 500;

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);
  if (!screenOn) return;

  // Device name
  M5.Lcd.fillRect(0, NAME_Y, SCREEN_W, NAME_H, BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(DARKGREY);
  int nameW = strlen(fullDeviceName) * FONT1_W;
  M5.Lcd.setCursor((SCREEN_W - nameW) / 2, NAME_Y + 1);
  M5.Lcd.print(fullDeviceName);

  M5.Lcd.fillRect(0, BAT_Y, SCREEN_W, BAT_AREA_H, BLACK);

  // BLE status dot
  uint16_t bleCol = connected ? BLUE : RED;
  M5.Lcd.fillCircle(BLE_DOT_X, BAT_Y + BLE_DOT_CY_OFF, BLE_DOT_R, bleCol);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(DARKGREY);
  M5.Lcd.setCursor(BLE_TEXT_X, BAT_Y + BLE_TEXT_Y_OFF);
  M5.Lcd.print("BLE");

  // Battery percentage
  uint16_t batCol = pct > 50 ? GREEN : (pct > 20 ? YELLOW : RED);
  M5.Lcd.setTextColor(batCol);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  int tw = strlen(buf) * FONT1_W;
  M5.Lcd.setCursor(SCREEN_W - tw - STATUS_MARGIN, BAT_Y + BLE_TEXT_Y_OFF);
  M5.Lcd.print(buf);

  // Battery bar
  int barW = SCREEN_W - tw - BAT_BAR_X - BAT_BAR_GAP;
  M5.Lcd.drawRect(BAT_BAR_X, BAT_Y + BAT_BAR_Y_OFF, barW, BAT_BAR_H, DARKGREY);
  M5.Lcd.fillRect(BAT_BAR_X + 1, BAT_Y + BAT_BAR_Y_OFF + 1, barW - 2, BAT_BAR_H - 2, BLACK);
  int fillW = (barW - 2) * pct / 100;
  if (fillW > 0) M5.Lcd.fillRect(BAT_BAR_X + 1, BAT_Y + BAT_BAR_Y_OFF + 1, fillW, BAT_BAR_H - 2, batCol);
}

void drawStatus() {
  if (!screenOn) return;
  M5.Lcd.fillScreen(BLACK);

  if (!connected) {
    M5.Lcd.setTextSize(3);
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(DISC_TITLE_X, DISC_TITLE_Y);
    M5.Lcd.print("BLE KB");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(DARKGREY);
    M5.Lcd.setCursor(DISC_SUB_X, DISC_SUB_Y);
    M5.Lcd.print("Waiting for pair...");
    updateBattery();
    return;
  }

  // Derived layout values
  int blockRight = BLOCK_X + BLOCK_W;
  int lineX = blockRight + (SCREEN_W - blockRight) / 2;

  // === Button A: full circle, top center ===
  int dotA_y = INDICATOR_R + INDICATOR_PAD;
  M5.Lcd.fillCircle(SCREEN_W / 2, dotA_y, INDICATOR_R, CYAN);

  // === Power: left half-circle, flush with right edge ===
  M5.Lcd.fillCircle(SCREEN_W - 1, dotA_y, INDICATOR_R, MAGENTA);

  // Line from A dot down to block A
  int blockA_y = dotA_y + INDICATOR_R + BLOCK_LINE_GAP;
  M5.Lcd.drawLine(SCREEN_W / 2, dotA_y + INDICATOR_R, SCREEN_W / 2, blockA_y, CYAN);

  // Block A
  M5.Lcd.fillRoundRect(BLOCK_X, blockA_y, BLOCK_W, BLOCK_H, BLOCK_CORNER_R, CYAN);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - FONT2_W) / 2, blockA_y + BLOCK_TEXT1_Y);
  M5.Lcd.print("A");
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - 7 * FONT2_W) / 2, blockA_y + BLOCK_TEXT2_Y);
  M5.Lcd.print("OPT+TAB");

  // === Button B: right half-circle, flush with left edge ===
  int blockB_y = blockA_y + BLOCK_H + BLOCK_GAP;
  int blockB_mid = blockB_y + BLOCK_H / 2;
  M5.Lcd.fillCircle(0, blockB_mid, INDICATOR_R, DARKGREY);
  M5.Lcd.drawLine(INDICATOR_R, blockB_mid, BLOCK_X, blockB_mid, DARKGREY);

  // Block B
  M5.Lcd.fillRoundRect(BLOCK_X, blockB_y, BLOCK_W, BLOCK_H, BLOCK_CORNER_R, COL_BLOCK_DARK);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - FONT2_W) / 2, blockB_y + BLOCK_TEXT1_Y);
  M5.Lcd.print("B");
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - 4 * FONT2_W) / 2, blockB_y + BLOCK_TEXT2_Y);
  M5.Lcd.print("WAKE");

  // === Power line: half-circle → diagonal → vertical → diagonal → block ===
  int blockP_y = blockB_y + BLOCK_H + BLOCK_GAP;

  // Start from bottom-left edge of half-circle (on the circle perimeter)
  int diagStartX = SCREEN_W - 1 - INDICATOR_R / 2;
  int diagStartY = dotA_y + INDICATOR_R - 1;
  int vertStartY = diagStartY + (diagStartX - lineX);
  M5.Lcd.drawLine(diagStartX, diagStartY, lineX, vertStartY, MAGENTA);
  int vertEndY = blockP_y + BLOCK_H / 4;
  M5.Lcd.drawLine(lineX, vertStartY, lineX, vertEndY, MAGENTA);
  M5.Lcd.drawLine(lineX, vertEndY, blockRight, blockP_y + BLOCK_H / 2, MAGENTA);

  // Block PWR
  M5.Lcd.fillRoundRect(BLOCK_X, blockP_y, BLOCK_W, BLOCK_H, BLOCK_CORNER_R, MAGENTA);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - 3 * FONT2_W) / 2, blockP_y + BLOCK_TEXT1_Y);
  M5.Lcd.print("PWR");
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - 5 * FONT2_W) / 2, blockP_y + BLOCK_TEXT2_Y);
  M5.Lcd.print("ENTER");

  updateBattery();
}

void flashPower() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(SCREEN_BRIGHTNESS);
    screenOn = true;
  }
  M5.Lcd.fillScreen(MAGENTA);
  M5.Lcd.setTextSize(4);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor((SCREEN_W - 5 * FONT4_W) / 2, FLASH_TEXT_Y);
  M5.Lcd.print("ENTER");
  delay(FLASH_POWER_DELAY);
  drawStatus();
  lastActivity = millis();
}

void flashBtnA() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(SCREEN_BRIGHTNESS);
    screenOn = true;
  }
  M5.Lcd.fillScreen(CYAN);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setCursor((SCREEN_W - 7 * FONT3_W) / 2, FLASH_TEXT_Y);
  M5.Lcd.print("OPT+TAB");
  delay(FLASH_BTNA_DELAY);
  drawStatus();
  lastActivity = millis();
}

#else
// ----- M5StickC: portrait 80x160, rotation 2, USB end UP -----
// Same physical layout as Plus, smaller screen

// Font metrics
const int FONT1_W = 6;
const int FONT1_H = 8;
const int FONT2_W = 12;
const int FONT2_H = 16;

// Indicator/button dots
const int INDICATOR_R      = 6;
const int INDICATOR_PAD    = 2;

// Block layout
const int BLOCK_X          = 10;
const int BLOCK_W          = 60;
const int BLOCK_H          = 32;
const int BLOCK_GAP        = 4;
const int BLOCK_CORNER_R   = 3;
const int BLOCK_TEXT1_Y    = 2;   // first line y offset within block
const int BLOCK_TEXT2_Y    = 21;  // second line y offset within block (font size 1)
const int BLOCK_LINE_GAP   = 4;  // gap from dot to first block
#define COL_BLOCK_DARK     0x4208  // dark grey for inactive block

// Derived: compute blocks_end_y (bottom of third block) from constants
// dotA_y = INDICATOR_R + INDICATOR_PAD
// blockA_y = dotA_y + INDICATOR_R + BLOCK_LINE_GAP
// blockB_y = blockA_y + BLOCK_H + BLOCK_GAP
// blockP_y = blockB_y + BLOCK_H + BLOCK_GAP
// blocks_end_y = blockP_y + BLOCK_H
const int BLOCKS_END_Y = (INDICATOR_R + INDICATOR_PAD) + INDICATOR_R + BLOCK_LINE_GAP
                        + BLOCK_H + BLOCK_GAP
                        + BLOCK_H + BLOCK_GAP
                        + BLOCK_H;

// Content area below blocks
const int CONTENT_GAP      = 4;
const int STATUS_MARGIN    = 2;
const int NAME_MARGIN      = 4;  // total horizontal margin for name wrapping

// BLE indicator (bottom bar)
const int BLE_DOT_X        = 6;
const int BLE_DOT_R        = 3;
const int BLE_DOT_CY_OFF   = 5;
const int BAT_AREA_H       = 12;

// Battery bar
const int BAT_BAR_X        = 14;
const int BAT_BAR_GAP      = 6;
const int BAT_BAR_H        = 6;
const int BAT_BAR_Y_OFF    = 3;
const int BAT_TEXT_Y_OFF   = 2;

// Disconnect text layout
const int DISC_TITLE_Y     = 40;
const int DISC_SUB_Y       = 70;

// Flash overlay
const int FLASH_TEXT_Y     = 68;
const int FLASH_POWER_DELAY = 300;
const int FLASH_BTNA_DELAY  = 500;

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);
  if (!screenOn) return;

  // Device name — may need 2 lines on narrow screen
  int nameY = BLOCKS_END_Y + CONTENT_GAP;
  int nameLen = strlen(fullDeviceName);
  int maxChars = (SCREEN_W - NAME_MARGIN) / FONT1_W;
  int nameLines = (nameLen <= maxChars) ? 1 : 2;
  M5.Lcd.fillRect(0, nameY, SCREEN_W, nameLines * FONT1_H, BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(DARKGREY);
  if (nameLines == 1) {
    M5.Lcd.setCursor((SCREEN_W - nameLen * FONT1_W) / 2, nameY);
    M5.Lcd.print(fullDeviceName);
  } else {
    // Line 1: first maxChars characters
    int line1W = maxChars * FONT1_W;
    M5.Lcd.setCursor((SCREEN_W - line1W) / 2, nameY);
    for (int i = 0; i < maxChars; i++) M5.Lcd.write(fullDeviceName[i]);
    // Line 2: remaining characters, centered
    int remain = nameLen - maxChars;
    int line2W = remain * FONT1_W;
    M5.Lcd.setCursor((SCREEN_W - line2W) / 2, nameY + FONT1_H);
    M5.Lcd.print(fullDeviceName + maxChars);
  }

  int batY = nameY + nameLines * FONT1_H + CONTENT_GAP;
  M5.Lcd.fillRect(0, batY, SCREEN_W, BAT_AREA_H, BLACK);

  uint16_t bleCol = connected ? BLUE : RED;
  M5.Lcd.fillCircle(BLE_DOT_X, batY + BLE_DOT_CY_OFF, BLE_DOT_R, bleCol);

  uint16_t batCol = pct > 50 ? GREEN : (pct > 20 ? YELLOW : RED);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(batCol);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  int tw = strlen(buf) * FONT1_W;
  M5.Lcd.setCursor(SCREEN_W - tw - STATUS_MARGIN, batY + BAT_TEXT_Y_OFF);
  M5.Lcd.print(buf);

  int barW = SCREEN_W - tw - BAT_BAR_X - BAT_BAR_GAP;
  M5.Lcd.drawRect(BAT_BAR_X, batY + BAT_BAR_Y_OFF, barW, BAT_BAR_H, DARKGREY);
  M5.Lcd.fillRect(BAT_BAR_X + 1, batY + BAT_BAR_Y_OFF + 1, barW - 2, BAT_BAR_H - 2, BLACK);
  int fillW = (barW - 2) * pct / 100;
  if (fillW > 0) M5.Lcd.fillRect(BAT_BAR_X + 1, batY + BAT_BAR_Y_OFF + 1, fillW, BAT_BAR_H - 2, batCol);
}

void drawStatus() {
  if (!screenOn) return;
  M5.Lcd.fillScreen(BLACK);

  if (!connected) {
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor((SCREEN_W - 6 * FONT2_W) / 2, DISC_TITLE_Y);
    M5.Lcd.print("BLE KB");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(DARKGREY);
    M5.Lcd.setCursor((SCREEN_W - 10 * FONT1_W) / 2, DISC_SUB_Y);
    M5.Lcd.print("Waiting...");
    updateBattery();
    return;
  }

  int blockRight = BLOCK_X + BLOCK_W;
  int lineX = blockRight + (SCREEN_W - blockRight) / 2;

  // Button A: full circle, top center
  int dotA_y = INDICATOR_R + INDICATOR_PAD;
  M5.Lcd.fillCircle(SCREEN_W / 2, dotA_y, INDICATOR_R, CYAN);

  // Power: left half-circle, flush with right edge
  M5.Lcd.fillCircle(SCREEN_W - 1, dotA_y, INDICATOR_R, MAGENTA);

  // Line from A dot to block A
  int blockA_y = dotA_y + INDICATOR_R + BLOCK_LINE_GAP;
  M5.Lcd.drawLine(SCREEN_W / 2, dotA_y + INDICATOR_R, SCREEN_W / 2, blockA_y, CYAN);

  // Block A
  M5.Lcd.fillRoundRect(BLOCK_X, blockA_y, BLOCK_W, BLOCK_H, BLOCK_CORNER_R, CYAN);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - FONT2_W) / 2, blockA_y + BLOCK_TEXT1_Y);
  M5.Lcd.print("A");
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - 7 * FONT1_W) / 2, blockA_y + BLOCK_TEXT2_Y);
  M5.Lcd.print("OPT+TAB");

  // Button B: right half-circle, flush with left edge
  int blockB_y = blockA_y + BLOCK_H + BLOCK_GAP;
  int blockB_mid = blockB_y + BLOCK_H / 2;
  M5.Lcd.fillCircle(0, blockB_mid, INDICATOR_R, DARKGREY);
  M5.Lcd.drawLine(INDICATOR_R, blockB_mid, BLOCK_X, blockB_mid, DARKGREY);

  // Block B
  M5.Lcd.fillRoundRect(BLOCK_X, blockB_y, BLOCK_W, BLOCK_H, BLOCK_CORNER_R, COL_BLOCK_DARK);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - FONT2_W) / 2, blockB_y + BLOCK_TEXT1_Y);
  M5.Lcd.print("B");
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - 4 * FONT1_W) / 2, blockB_y + BLOCK_TEXT2_Y);
  M5.Lcd.print("WAKE");

  // Power line: half-circle → diagonal → vertical → diagonal → block
  int blockP_y = blockB_y + BLOCK_H + BLOCK_GAP;
  int diagStartX = SCREEN_W - 1 - INDICATOR_R / 2;
  int diagStartY = dotA_y + INDICATOR_R - 1;
  int vertStartY = diagStartY + (diagStartX - lineX);
  M5.Lcd.drawLine(diagStartX, diagStartY, lineX, vertStartY, MAGENTA);
  int vertEndY = blockP_y + BLOCK_H / 4;
  M5.Lcd.drawLine(lineX, vertStartY, lineX, vertEndY, MAGENTA);
  M5.Lcd.drawLine(lineX, vertEndY, blockRight, blockP_y + BLOCK_H / 2, MAGENTA);

  // Block POWER
  M5.Lcd.fillRoundRect(BLOCK_X, blockP_y, BLOCK_W, BLOCK_H, BLOCK_CORNER_R, MAGENTA);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - 5 * FONT2_W) / 2, blockP_y + BLOCK_TEXT1_Y);
  M5.Lcd.print("POWER");
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(BLOCK_X + (BLOCK_W - 5 * FONT1_W) / 2, blockP_y + BLOCK_TEXT2_Y);
  M5.Lcd.print("ENTER");

  updateBattery();
}

void flashPower() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(SCREEN_BRIGHTNESS);
    screenOn = true;
  }
  M5.Lcd.fillScreen(MAGENTA);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor((SCREEN_W - 5 * FONT2_W) / 2, FLASH_TEXT_Y);
  M5.Lcd.print("ENTER");
  delay(FLASH_POWER_DELAY);
  drawStatus();
  lastActivity = millis();
}

void flashBtnA() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(SCREEN_BRIGHTNESS);
    screenOn = true;
  }
  M5.Lcd.fillScreen(CYAN);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setCursor((SCREEN_W - 7 * FONT2_W) / 2, FLASH_TEXT_Y);
  M5.Lcd.print("OPT+TAB");
  delay(FLASH_BTNA_DELAY);
  drawStatus();
  lastActivity = millis();
}

#endif

// ============================================================
//  WiFi 配置模式：NVS + action helpers + AP/web server
// ============================================================
#if IS_CARDPUTER

void loadConfig() {
  Preferences prefs;
  prefs.begin("kb", true);
  g_config.ctrl_action     = prefs.getUChar("ctrl", CTRL_OPT_TAB);
  g_config.opt_action      = prefs.getUChar("opt",  OPT_CAPS_LOCK);
  g_config.fn_action       = prefs.getUChar("fn",   FN_ENTER);
  g_config.backtick_action = prefs.getUChar("bt",   BT_ESC);
  g_config.num_action      = prefs.getUChar("num",  NUM_CMD_N);
  prefs.end();
}
void saveConfig() {
  Preferences prefs;
  prefs.begin("kb", false);
  prefs.putUChar("ctrl", g_config.ctrl_action);
  prefs.putUChar("opt",  g_config.opt_action);
  prefs.putUChar("fn",   g_config.fn_action);
  prefs.putUChar("bt",   g_config.backtick_action);
  prefs.putUChar("num",  g_config.num_action);
  prefs.end();
}
bool consumeConfigBootFlag() {
  Preferences prefs;
  prefs.begin("kb", false);
  bool flag = prefs.getBool("cfg_boot", false);
  if (flag) prefs.putBool("cfg_boot", false);
  prefs.end();
  return flag;
}
void requestConfigBoot() {
  Preferences prefs;
  prefs.begin("kb", false);
  prefs.putBool("cfg_boot", true);
  prefs.end();
}

// 单按动作（press-and-release 时调用）
void doCtrlTap() {
  switch (g_config.ctrl_action) {
    case CTRL_OPT_TAB:
      bleKeyboard.press(KEY_LEFT_ALT); bleKeyboard.press(KEY_TAB); bleKeyboard.releaseAll();
      showFlash("Opt+Tab", COL_KEY_FN); break;
    case CTRL_CMD_SPACE:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(' '); bleKeyboard.releaseAll();
      showFlash("Cmd+Spc", COL_KEY_FN); break;
    case CTRL_CMD_SHIFT_SEMI:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press(';');
      bleKeyboard.releaseAll();
      showFlash("Cmd+Sh+;", COL_KEY_FN); break;
    case CTRL_NONE: break;
  }
}
void doOptTap() {
  switch (g_config.opt_action) {
    case OPT_CAPS_LOCK:
      capsLocked = !capsLocked;
      bleKeyboard.write(KEY_CAPS_LOCK);
      showFlash(capsLocked ? "CAPS ON" : "caps off", TFT_YELLOW); break;
    case OPT_ESC:
      bleKeyboard.write(KEY_ESC);
      showFlash("Esc", COL_KEY_ESC); break;
    case OPT_NONE: break;
  }
}
void doFnTap() {
  switch (g_config.fn_action) {
    case FN_ENTER:
      bleKeyboard.write(KEY_RETURN); showFlash("Enter", COL_KEY_ENT); break;
    case FN_TAB:
      bleKeyboard.write(KEY_TAB); showFlash("Tab", COL_KEY_ENT); break;
    case FN_ESC:
      bleKeyboard.write(KEY_ESC); showFlash("Esc", COL_KEY_ESC); break;
    case FN_NONE: break;
  }
}
void doBacktick() {
  switch (g_config.backtick_action) {
    case BT_ESC:
      bleKeyboard.write(KEY_ESC); showFlash("Esc", COL_KEY_ESC); break;
    case BT_BACKTICK:
      bleKeyboard.write('`'); break;
  }
}
void doNumberKey(char c) {  // c in '1'..'8'
  char msg[12];
  switch (g_config.num_action) {
    case NUM_CMD_N:
      bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(c); bleKeyboard.releaseAll();
      snprintf(msg, sizeof(msg), "Cmd+%c", c);
      showFlash(msg, COL_KEY_NUM); break;
    case NUM_LITERAL:
      bleKeyboard.write(c); break;
  }
}

// AP + 配置网页
static const char* ctrlNames[] = {"Opt+Tab (语音输入)", "Cmd+Space (Spotlight)", "Cmd+Shift+; (听写)", "(无)"};
static const char* optNames[]  = {"Caps Lock", "Esc", "(无)"};
static const char* fnNames[]   = {"Enter", "Tab", "Esc", "(无)"};
static const char* btNames[]   = {"Esc", "` 反引号"};
static const char* numNames[]  = {"Cmd+N (Ghostty tab)", "原样数字 1-8"};

String htmlOption(int idx, int curr, const char* label) {
  String s = "<option value='" + String(idx) + "'";
  if (idx == curr) s += " selected";
  s += ">"; s += label; s += "</option>";
  return s;
}
String htmlSelect(const char* name, int curr, const char* opts[], int n) {
  String s = "<select name='"; s += name; s += "'>";
  for (int i = 0; i < n; i++) s += htmlOption(i, curr, opts[i]);
  s += "</select>";
  return s;
}

void handleRoot() {
  String body = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  body += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  body += "<title>CardPuter Keyboard Config</title>";
  body += "<style>body{font-family:system-ui;max-width:500px;margin:1em auto;padding:0 1em}";
  body += "label{display:block;margin:1em 0 .3em;font-weight:600}";
  body += "select{width:100%;padding:.5em;font-size:1em}";
  body += "button{margin-top:1.5em;padding:.7em 1.5em;font-size:1em;width:100%}";
  body += ".save{background:#2563eb;color:#fff;border:0;border-radius:4px}";
  body += ".reboot{background:#dc2626;color:#fff;border:0;border-radius:4px;margin-top:.5em}";
  body += "</style></head><body>";
  body += "<h2>CardPuter Keyboard 配置</h2>";
  body += "<form method='POST' action='/save'>";
  body += "<label>Ctrl 单按</label>" + htmlSelect("ctrl", g_config.ctrl_action, ctrlNames, 4);
  body += "<label>Opt 单按</label>" + htmlSelect("opt", g_config.opt_action, optNames, 3);
  body += "<label>Fn 单按</label>" + htmlSelect("fn", g_config.fn_action, fnNames, 4);
  body += "<label>反引号 (`)</label>" + htmlSelect("bt", g_config.backtick_action, btNames, 2);
  body += "<label>数字键 1-8</label>" + htmlSelect("num", g_config.num_action, numNames, 2);
  body += "<button class='save' type='submit'>保存并重启</button>";
  body += "</form>";
  body += "<form method='POST' action='/cancel'>";
  body += "<button class='reboot' type='submit'>放弃 / 直接重启</button>";
  body += "</form>";
  body += "</body></html>";
  g_web->send(200, "text/html; charset=utf-8", body);
}

void handleSave() {
  if (g_web->hasArg("ctrl")) g_config.ctrl_action     = g_web->arg("ctrl").toInt();
  if (g_web->hasArg("opt"))  g_config.opt_action      = g_web->arg("opt").toInt();
  if (g_web->hasArg("fn"))   g_config.fn_action       = g_web->arg("fn").toInt();
  if (g_web->hasArg("bt"))   g_config.backtick_action = g_web->arg("bt").toInt();
  if (g_web->hasArg("num"))  g_config.num_action      = g_web->arg("num").toInt();
  saveConfig();
  g_web->send(200, "text/html; charset=utf-8",
              "<html><body style='font-family:system-ui;text-align:center;margin-top:3em'>"
              "<h2>已保存，正在重启…</h2></body></html>");
  delay(500);
  esp_restart();
}

void handleCancel() {
  g_web->send(200, "text/html; charset=utf-8",
              "<html><body style='font-family:system-ui;text-align:center;margin-top:3em'>"
              "<h2>正在重启…</h2></body></html>");
  delay(500);
  esp_restart();
}

// ===== 配置模式：STA 连家庭 WiFi（写死，仅试验用）=====
#include <ESPmDNS.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* MDNS_NAME = "cardputer-kb";  // 访问 http://cardputer-kb.local
const unsigned long WIFI_TIMEOUT_MS = 30000;  // 30 秒连不上就重启回正常模式

unsigned long g_configEnterTime = 0;
String g_staIp = "";
bool g_staConnected = false;

void drawConfigScreen(const char* status, const String& ip) {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(8, 4);
  M5Cardputer.Display.print("WiFi Config");

  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(8, 32);
  M5Cardputer.Display.print("SSID:   ");
  M5Cardputer.Display.println(WIFI_SSID);
  M5Cardputer.Display.setCursor(8, 48);
  M5Cardputer.Display.print("Status: ");
  M5Cardputer.Display.println(status);
  M5Cardputer.Display.setCursor(8, 64);
  M5Cardputer.Display.print("IP:     ");
  M5Cardputer.Display.println(ip);
  M5Cardputer.Display.setCursor(8, 80);
  M5Cardputer.Display.print("mDNS:   http://");
  M5Cardputer.Display.print(MDNS_NAME);
  M5Cardputer.Display.println(".local");

  M5Cardputer.Display.setCursor(8, 110);
  M5Cardputer.Display.setTextColor(0xFD20, BLACK);
  M5Cardputer.Display.println("Reboot to exit");
}

// /api/status — JSON 健康状态
void handleStatus() {
  char json[384];
  int rssi = WiFi.RSSI();
  unsigned long uptime = millis() / 1000;
  int bat = M5Cardputer.Power.getBatteryLevel();
  snprintf(json, sizeof(json),
    "{\"mode\":\"config\",\"wifi_ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
    "\"mdns\":\"%s.local\",\"uptime_s\":%lu,\"battery\":%d,"
    "\"cfg\":{\"ctrl\":%u,\"opt\":%u,\"fn\":%u,\"bt\":%u,\"num\":%u},"
    "\"heap_free\":%u}",
    WIFI_SSID, g_staIp.c_str(), rssi, MDNS_NAME, uptime, bat,
    g_config.ctrl_action, g_config.opt_action, g_config.fn_action,
    g_config.backtick_action, g_config.num_action,
    (unsigned)ESP.getFreeHeap());
  g_web->send(200, "application/json", json);
}

// /api/screenshot — 当前屏幕 BMP（240x135 16bpp → 24bpp BMP）
void handleScreenshot() {
  const uint32_t W = 240, H = 135;
  uint32_t rowSize = (W * 3 + 3) & ~3;  // 720
  uint32_t imageSize = rowSize * H;
  uint32_t fileSize = 54 + imageSize;
  uint8_t header[54] = {
    'B','M',
    (uint8_t)fileSize,(uint8_t)(fileSize>>8),(uint8_t)(fileSize>>16),(uint8_t)(fileSize>>24),
    0,0,0,0, 54,0,0,0, 40,0,0,0,
    (uint8_t)W,(uint8_t)(W>>8),(uint8_t)(W>>16),(uint8_t)(W>>24),
    (uint8_t)H,(uint8_t)(H>>8),(uint8_t)(H>>16),(uint8_t)(H>>24),
    1,0, 24,0, 0,0,0,0,
    (uint8_t)imageSize,(uint8_t)(imageSize>>8),(uint8_t)(imageSize>>16),(uint8_t)(imageSize>>24),
    0x13,0x0B,0,0, 0x13,0x0B,0,0, 0,0,0,0, 0,0,0,0
  };
  g_web->setContentLength(fileSize);
  g_web->send(200, "image/bmp", "");
  WiFiClient client = g_web->client();
  client.write(header, 54);
  uint8_t row[720];
  for (int y = (int)H - 1; y >= 0; y--) {
    int idx = 0;
    for (int x = 0; x < (int)W; x++) {
      uint16_t color = M5Cardputer.Display.readPixel(x, y);
      uint8_t r = (color >> 11) & 0x1F;
      uint8_t g = (color >> 5) & 0x3F;
      uint8_t b = color & 0x1F;
      r = (r << 3) | (r >> 2);
      g = (g << 2) | (g >> 4);
      b = (b << 3) | (b >> 2);
      row[idx++] = b;
      row[idx++] = g;
      row[idx++] = r;
    }
    client.write(row, rowSize);
  }
}

void setupConfigMode() {
  Serial.begin(115200);
  Serial.println("\n[config] entering STA mode");
  drawConfigScreen("Connecting...", "");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[config] WiFi failed, reverting to BLE mode");
    drawConfigScreen("FAILED, reboot", "");
    delay(2000);
    esp_restart();
  }

  g_staConnected = true;
  g_staIp = WiFi.localIP().toString();
  Serial.printf("[config] connected, IP=%s\n", g_staIp.c_str());

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[config] mDNS: %s.local\n", MDNS_NAME);
  }

  g_web = new WebServer(80);
  g_web->on("/",                HTTP_GET,  handleRoot);
  g_web->on("/save",            HTTP_POST, handleSave);
  g_web->on("/cancel",          HTTP_POST, handleCancel);
  g_web->on("/api/status",      HTTP_GET,  handleStatus);
  g_web->on("/api/screenshot",  HTTP_GET,  handleScreenshot);
  g_web->on("/api/reboot",      HTTP_POST, handleCancel);
  g_web->begin();

  drawConfigScreen("Connected", g_staIp);
  g_configEnterTime = millis();
}

void configModeLoop() {
  if (g_web) g_web->handleClient();
  // WiFi 掉线后自动重启回正常模式（避免卡死）
  if (g_staConnected && WiFi.status() != WL_CONNECTED) {
    delay(3000);
    if (WiFi.status() != WL_CONNECTED) esp_restart();
  }
  delay(2);
}

#endif  // IS_CARDPUTER

// ============================================================
//  Setup & Loop — shared
// ============================================================

void setup() {
#if IS_CARDPUTER
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[boot] start");

  auto cfg = M5.config();
  M5Cardputer.begin(cfg);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(SCREEN_BRIGHTNESS);
  M5Cardputer.Display.fillScreen(BLACK);
  Serial.println("[boot] M5Cardputer ok");

  // 进配置模式标志（防御：失败默认 false，把它放在 BLE 之前是为了节省 BLE 不必要的初始化）
  bool wantConfig = false;
  {
    Preferences p;
    if (p.begin("kb", false)) {
      wantConfig = p.getBool("cfg_boot", false);
      if (wantConfig) p.putBool("cfg_boot", false);
      p.end();
    } else {
      Serial.println("[boot] NVS kb begin failed, normal mode");
    }
  }
  Serial.printf("[boot] cfg_boot=%d\n", (int)wantConfig);

  loadConfig();
  Serial.println("[boot] config loaded");

  if (wantConfig) {
    g_config_mode = true;
    setupConfigMode();
    return;
  }
#else
  M5.begin();
  M5.Axp.ScreenBreath(SCREEN_BRIGHTNESS);
  M5.Lcd.setRotation(LCD_ROTATION);
  M5.Lcd.fillScreen(BLACK);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
#endif

  setCpuFrequencyMhz(80);

  // Build device name with BLE MAC suffix: "DeviceName-XXYY"
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(fullDeviceName, sizeof(fullDeviceName), "%s-%02X%02X",
           DEVICE_NAME, mac[4], mac[5]);
  bleKeyboard.setName(fullDeviceName);
  bleKeyboard.begin();
  drawStatus();
  lastActivity = millis();
  lastBatUpdate = millis();

#if !IS_CARDPUTER
  lastPowerCheck = millis();
  lastPowerCheckBat = getBatPercent();

  if (BUZZER_TEST_ON_BOOT) {
    beepLowBattery();
    delay(800);
    beepPowerOff();
  }
#endif
}

void loop() {
#if IS_CARDPUTER
  // 远程触发：BLE 回调里检测到 host 反复切 Caps Lock 后置标志，main loop 这里执行重启
  // （注意：xyb 的 Mac 把 Caps Lock 改成了 IME 切换，BLE 路径暂时无效，靠下面 USB 串口）
  if (g_remote_trigger_fired) {
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(TFT_YELLOW, BLACK);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setCursor(8, 50);
    M5Cardputer.Display.println("Remote Config");
    delay(800);
    requestConfigBoot();
    esp_restart();
  }
  // 开发期备用触发：USB 串口收到 "config\n" 进配置模式
  static char serialBuf[16];
  static uint8_t serialIdx = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialBuf[serialIdx] = 0;
      if (strcmp(serialBuf, "config") == 0) {
        Serial.println("[trigger] serial 'config' received");
        requestConfigBoot();
        delay(50);
        esp_restart();
      }
      serialIdx = 0;
    } else if (serialIdx < sizeof(serialBuf) - 1) {
      serialBuf[serialIdx++] = c;
    }
  }
  if (g_config_mode) {
    configModeLoop();
    return;
  }
  M5Cardputer.update();
#else
  M5.update();
#endif

  connected = bleKeyboard.isConnected();
  if (connected != prevConnected) {
    prevConnected = connected;
    screenWake();
    drawStatus();
    lastBatUpdate = millis();
  }

  // Auto screen off (skip when not connected — keep screen on for pairing)
  if (screenOn && connected && (millis() - lastActivity >= SCREEN_TIMEOUT)) {
    screenSleep();
  }

#if IS_CARDPUTER
  // CardPuter: auto power off (deep sleep) after 5 min without connection
  if (!connected && millis() >= UNPAIRED_POWEROFF) {
    beepPowerOff();
    esp_deep_sleep_start();  // no wakeup source = effectively off, reset button to restart
  }
#endif

#if !IS_CARDPUTER
  // Auto power off after 30 min idle (skip if charging)
  if (millis() - lastActivity >= POWEROFF_TIMEOUT
      && millis() - lastPowerCheck >= POWEROFF_TIMEOUT) {
    int curBat = getBatPercent();
    if (curBat < LOWBAT_THRESHOLD) {
      beepPowerOff();
      M5.Axp.PowerOff();
    }
    if (curBat <= lastPowerCheckBat - CHARGING_DROP_MARGIN) {
      beepPowerOff();
      M5.Axp.PowerOff();
    }
    lastPowerCheck = millis();
    lastPowerCheckBat = curBat;
  }

  // Heartbeat LED: breathing effect when screen is off
  if (!screenOn) {
    if (!ledBreathing && (millis() - lastLedBlink >= LED_BLINK_INTERVAL)) {
      ledBreathing = true;
      ledBreathStart = millis();
    }
    if (ledBreathing) {
      unsigned long elapsed = millis() - ledBreathStart;
      if (elapsed >= LED_BREATH_DURATION) {
        analogWrite(LED_PIN, 255);
        ledBreathing = false;
        lastLedBlink = millis();
      } else {
        unsigned long half = LED_BREATH_DURATION / 2;
        float progress = (elapsed < half)
          ? (float)elapsed / half
          : 1.0f - (float)(elapsed - half) / half;
        int pwm = 255 - (int)((255 - LED_BREATH_PEAK) * progress);
        analogWrite(LED_PIN, pwm);
      }
    }
  } else if (ledBreathing) {
    analogWrite(LED_PIN, 255);
    ledBreathing = false;
  }
#endif

  // Battery refresh: 30s when screen on, 5min when idle
  unsigned long batInterval = screenOn ? BAT_INTERVAL_ACTIVE : BAT_INTERVAL_IDLE;
  if (millis() - lastBatUpdate >= batInterval) {
    lastBatUpdate = millis();
    updateBattery();

#if !IS_CARDPUTER
    int pct = getBatPercent();
    if (pct <= 20 && pct > 10 && !warnedAt20) {
      warnedAt20 = true;
      beepLowBattery();
    } else if (pct <= 10 && !warnedAt10) {
      warnedAt10 = true;
      beepLowBattery();
    }
    if (pct > 20) { warnedAt20 = false; warnedAt10 = false; }
    else if (pct > 10) { warnedAt10 = false; }
#endif
  }

#if IS_CARDPUTER
  // CardPuter keyboard input
  if (!connected) { delay(100); return; }

  if (M5Cardputer.Keyboard.isChange()) {
    // Any key activity wakes the screen
    if (M5Cardputer.Keyboard.isPressed()) screenWake();

    auto& keys = M5Cardputer.Keyboard.keysState();
    bool fnNow   = keys.fn;
    bool ctrlNow = keys.ctrl;
    bool optNow  = keys.opt;
    bool shiftOn = keys.shift;

    // --- Modifier tap-hold: on press, reset "used as modifier" flag ---
    if (fnNow && !fnPrevHeld)     { fnUsedAsModifier = false; fnPressStart = millis(); }
    if (ctrlNow && !ctrlPrevHeld) ctrlUsedAsModifier = false;
    if (optNow && !optPrevHeld)   optUsedAsModifier = false;

    // --- Fn 长按 5 秒进入 WiFi 配置模式 ---
    if (fnNow && !fnUsedAsModifier && fnPressStart > 0
        && (millis() - fnPressStart >= FN_LONG_PRESS_MS)) {
      showFlash("Config Mode", TFT_YELLOW);
      delay(800);
      requestConfigBoot();
      esp_restart();
    }

    // --- Modifier tap-hold: on release, fire tap action if not used as modifier ---
    if (!fnNow && fnPrevHeld && !fnUsedAsModifier) {
      doFnTap();
      screenWake();
    }
    if (!ctrlNow && ctrlPrevHeld && !ctrlUsedAsModifier) {
      doCtrlTap();
      screenWake();
    }
    if (!optNow && optPrevHeld && !optUsedAsModifier) {
      doOptTap();
      screenWake();
    }
    fnPrevHeld   = fnNow;
    ctrlPrevHeld = ctrlNow;
    optPrevHeld  = optNow;

    // Which modifiers are active as BLE modifiers (held + other key)?
    bool modCtrl  = ctrlNow;  // Ctrl held → BLE Ctrl modifier
    bool modOpt   = optNow;   // Opt held  → BLE Alt/Option modifier
    bool modShift = shiftOn;  // Shift held → BLE Shift modifier

    // Helper: press active BLE modifiers
    #define PRESS_MODS() do { \
      if (modCtrl)  bleKeyboard.press(KEY_LEFT_CTRL);  \
      if (modOpt)   bleKeyboard.press(KEY_LEFT_ALT);   \
      if (modShift) bleKeyboard.press(KEY_LEFT_SHIFT);  \
    } while(0)

    // Helper: send a key with active modifiers
    #define SEND_WITH_MODS(k) do { PRESS_MODS(); bleKeyboard.press(k); bleKeyboard.releaseAll(); } while(0)

    if (M5Cardputer.Keyboard.isPressed()) {
      // Mark modifiers as "used" only when non-modifier keys are also pressed
      bool hasContentKeys = !keys.word.empty() || keys.enter || keys.del || keys.space || keys.tab;
      if (hasContentKeys) {
        if (ctrlNow) ctrlUsedAsModifier = true;
        if (optNow)  optUsedAsModifier = true;
      }

      for (auto c : keys.word) {
        // Space is duplicated in both keys.word and keys.space by M5Cardputer lib.
        // Handle it only via keys.space flag below (consistent with enter/del/tab).
        if (c == ' ') continue;
        screenWake();
        if (ctrlNow && optNow && !fnNow) {
          // Ctrl+Opt layer: mouse scroll
          // Note: Ctrl triggers value_second in library, so ; becomes : and . becomes >
          ctrlUsedAsModifier = true;
          optUsedAsModifier = true;
          switch (c) {
            case ';': case ':':  bleKeyboard.sendMouseScroll(SCROLL_STEP); break;   // scroll up
            case '.': case '>':  bleKeyboard.sendMouseScroll(-SCROLL_STEP); break;  // scroll down
            default: break;
          }
        } else if (fnNow) {
          // Fn layer: arrow keys
          fnUsedAsModifier = true;
          switch (c) {
            case ';': case ':':  SEND_WITH_MODS(KEY_UP_ARROW); break;
            case '.': case '>':  SEND_WITH_MODS(KEY_DOWN_ARROW); break;
            case ',': case '<':  SEND_WITH_MODS(KEY_LEFT_ARROW); break;
            case '/': case '?':  SEND_WITH_MODS(KEY_RIGHT_ARROW); break;
            default: break;
          }
        } else if (modCtrl || modOpt) {
          // Ctrl/Opt held: send key with BLE modifier
          // Undo library's shift effect (ctrl/shift triggers value_second)
          char key = c;
          if (key >= 'A' && key <= 'Z') key = key + 32;
          PRESS_MODS();
          bleKeyboard.press(key);
          bleKeyboard.releaseAll();
        } else {
          // Normal mode (shift already applied by library for characters)
          switch (c) {
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8':
              doNumberKey(c);
              break;
            case '`':
              doBacktick();
              break;
            default:
              bleKeyboard.write(c);
              break;
          }
        }
      }

      // Special keys via boolean flags.
      // Enter/del/tab are only in flags (not in keys.word).
      // Space is in BOTH flag and word per M5Cardputer lib; word loop skips it so this is the single source of truth.
      if (keys.enter) { SEND_WITH_MODS(KEY_RETURN);    screenWake(); }
      if (keys.del)   { SEND_WITH_MODS(KEY_BACKSPACE);  screenWake(); }
      if (keys.space) { SEND_WITH_MODS(' ');             screenWake(); }
      if (keys.tab)   { SEND_WITH_MODS(KEY_TAB);        screenWake(); }
    }

    #undef PRESS_MODS
    #undef SEND_WITH_MODS

    // Track held key for repeat (all keys)
    uint8_t newHeld = 0;
    if (keys.del) {
      newHeld = KEY_BACKSPACE;
    } else if (keys.space) {
      newHeld = ' ';
    } else if (keys.enter) {
      newHeld = KEY_RETURN;
    } else if (keys.tab) {
      newHeld = KEY_TAB;
    } else if (ctrlNow && optNow && !fnNow) {
      for (auto c : keys.word) {
        if (c == ';' || c == ':') { newHeld = SCROLL_UP; break; }
        if (c == '.' || c == '>') { newHeld = SCROLL_DOWN; break; }
      }
    } else if (fnNow) {
      for (auto c : keys.word) {
        if (c == ';' || c == ':') { newHeld = KEY_UP_ARROW; break; }
        if (c == '.' || c == '>') { newHeld = KEY_DOWN_ARROW; break; }
        if (c == ',' || c == '<') { newHeld = KEY_LEFT_ARROW; break; }
        if (c == '/' || c == '?') { newHeld = KEY_RIGHT_ARROW; break; }
      }
    } else if (!keys.word.empty() && !ctrlNow && !optNow) {
      // Regular character: track for repeat
      newHeld = keys.word[0];
    }
    if (newHeld != heldKey) {
      heldKey = newHeld;
      heldKeyStart = millis();
      heldKeyLast = 0;
    }
  } else if (heldKey && !M5Cardputer.Keyboard.isPressed()) {
    heldKey = 0;
  }

  // Key repeat: runs every loop iteration, outside isChange()
  if (heldKey && connected) {
    unsigned long now = millis();
    unsigned long elapsed = now - heldKeyStart;
    if (elapsed >= REPEAT_DELAY && (now - heldKeyLast) >= REPEAT_RATE) {
      if (heldKey == SCROLL_UP) {
        bleKeyboard.sendMouseScroll(SCROLL_STEP);
      } else if (heldKey == SCROLL_DOWN) {
        bleKeyboard.sendMouseScroll(-SCROLL_STEP);
      } else {
        bleKeyboard.write(heldKey);
      }
      heldKeyLast = now;
      screenWake();
    }
  }

#else
  // M5StickC / M5StickC Plus button input
  if (M5.BtnB.wasPressed()) {
    screenWake();
  }

  if (!connected) { delay(100); return; }

  // Button A: Opt+Tab
  if (M5.BtnA.wasPressed()) {
    bleKeyboard.press(KEY_LEFT_ALT);
    bleKeyboard.press(KEY_TAB);
    bleKeyboard.releaseAll();
    flashBtnA();
  }

  // Power button: Enter
  if (M5.Axp.GetBtnPress() == 2) {
    bleKeyboard.write(KEY_RETURN);
    flashPower();
  }
#endif

  delay(10);
}
