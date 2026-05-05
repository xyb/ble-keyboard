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

struct ModTapState;

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
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

enum AK : uint8_t {
  AK_NONE = 0,
  AK_ENTER, AK_TAB, AK_ESC, AK_BACKSPACE, AK_DELETE, AK_SPACE,
  AK_UP, AK_DOWN, AK_LEFT, AK_RIGHT,
  AK_PAGE_UP, AK_PAGE_DOWN, AK_HOME, AK_END,
  AK_CAPS_LOCK,
  AK_F1, AK_F2, AK_F3, AK_F4, AK_F5, AK_F6,
  AK_F7, AK_F8, AK_F9, AK_F10, AK_F11, AK_F12,
  AK_A, AK_B, AK_C, AK_D, AK_E, AK_F, AK_G, AK_H, AK_I, AK_J,
  AK_K, AK_L, AK_M, AK_N, AK_O, AK_P, AK_Q, AK_R, AK_S, AK_T,
  AK_U, AK_V, AK_W, AK_X, AK_Y, AK_Z,
  AK_0, AK_1, AK_2, AK_3, AK_4, AK_5, AK_6, AK_7, AK_8, AK_9,
  AK_BACKTICK, AK_MINUS, AK_EQUAL, AK_LBRACKET, AK_RBRACKET, AK_BACKSLASH,
  AK_SEMICOLON, AK_QUOTE, AK_COMMA, AK_PERIOD, AK_SLASH,
};

enum TriggerKind : uint8_t {
  TK_NONE = 0,
  TK_CTRL,
  TK_OPT,
  TK_FN,
  TK_KEY,
  TK_SHIFT,
  TK_ALT,
};

enum TriggerEvent : uint8_t {
  TEV_SINGLE = 0,
  TEV_DOUBLE,
  TEV_TRIPLE,
  TEV_LONG,
};

constexpr int BIND_COMMENT_LEN = 64;
struct Binding {
  uint8_t trigger;       // TriggerKind
  uint8_t trigger_key;
  uint8_t event;         // TriggerEvent
  uint16_t long_ms;
  uint8_t cmd;
  uint8_t opt;
  uint8_t ctrl;
  uint8_t shift;
  uint8_t action;
  char comment[BIND_COMMENT_LEN];
};

constexpr unsigned long TAP_WINDOW_MS = 280;
constexpr unsigned long LONG_PRESS_DEFAULT_MS = 500;

constexpr int MAX_BINDINGS = 16;

struct KbConfig {
  Binding  bindings[MAX_BINDINGS];
  uint8_t  count;
};

const KbConfig PRESET_LAZYTYPER = {
  {
    {TK_CTRL,  0,  TEV_SINGLE, 0,  0, 1, 0, 0, AK_TAB,        "LazyTyper voice (Opt+Tab)"},
    {TK_OPT,   0,  TEV_SINGLE, 0,  0, 0, 0, 0, AK_CAPS_LOCK,  "IME switch (EN/CN, macOS Caps Lock)"},
    {TK_FN,    0,  TEV_SINGLE, 0,  0, 0, 0, 0, AK_ENTER,      "Enter / send"},
    {TK_KEY,  '`', TEV_SINGLE, 0,  0, 0, 0, 0, AK_ESC,        "Esc: cancel"},
    {TK_KEY,  '1', TEV_SINGLE, 0,  1, 0, 0, 0, AK_1,          "iTerm2 tab 1"},
    {TK_KEY,  '2', TEV_SINGLE, 0,  1, 0, 0, 0, AK_2,          "iTerm2 tab 2"},
    {TK_KEY,  '3', TEV_SINGLE, 0,  1, 0, 0, 0, AK_3,          "iTerm2 tab 3"},
    {TK_KEY,  '4', TEV_SINGLE, 0,  1, 0, 0, 0, AK_4,          "iTerm2 tab 4"},
    {TK_KEY,  '5', TEV_SINGLE, 0,  1, 0, 0, 0, AK_5,          "iTerm2 tab 5"},
    {TK_KEY,  '6', TEV_SINGLE, 0,  1, 0, 0, 0, AK_6,          "iTerm2 tab 6"},
    {TK_KEY,  '7', TEV_SINGLE, 0,  1, 0, 0, 0, AK_7,          "iTerm2 tab 7"},
    {TK_KEY,  '8', TEV_SINGLE, 0,  1, 0, 0, 0, AK_8,          "iTerm2 tab 8"},
  },
  12
};

const KbConfig PRESET_PASSTHROUGH = {
  {},
  0
};

const KbConfig& DEFAULT_PRESET = PRESET_LAZYTYPER;

struct PresetEntry { const char* name; const KbConfig* cfg; };
const PresetEntry PRESETS[] = {
  {"LazyTyper + iTerm2",  &PRESET_LAZYTYPER},
  {"Passthrough (no mapping)",      &PRESET_PASSTHROUGH},
};
constexpr int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

KbConfig g_config = PRESET_LAZYTYPER;

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
bool shiftPrevHeld = false;
bool shiftUsedAsModifier = false;
bool altPrevHeld = false;
bool altUsedAsModifier = false;
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

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef FW_BUILD_DATE
#define FW_BUILD_DATE __DATE__
#endif
const char* FIRMWARE_VERSION = FW_VERSION " (" FW_BUILD_DATE ")";

LGFX_Sprite g_canvas(&M5Cardputer.Display);
LGFX_Sprite g_banner(&M5Cardputer.Display);
LGFX_Sprite g_restore(&M5Cardputer.Display);
const int FLASH_BANNER_W = 200;
const int FLASH_BANNER_H = 60;
const int FLASH_BANNER_X = (SCREEN_W - FLASH_BANNER_W) / 2;
const int FLASH_BANNER_Y = (SCREEN_H - FLASH_BANNER_H) / 2;

#define D g_canvas

void drawKeyBlock(int x, int y, int w, int h, uint16_t bg, const char* label, const char* action) {
  D.fillRoundRect(x, y, w, h, KEY_CORNER_R, bg);
  D.setTextColor(BLACK);
  // Label: size 2, auto-shrink if too wide
  int labelLen = strlen(label);
  if (labelLen * FONT2_W <= w - KEY_LABEL_PAD) {
    D.setTextSize(2);
    D.setCursor(x + (w - labelLen * FONT2_W) / 2, y + KEY_LABEL_Y1);
  } else {
    D.setTextSize(1);
    D.setCursor(x + (w - labelLen * FONT1_W) / 2, y + KEY_LABEL_Y1_SM);
  }
  D.print(label);
  // Action: size 1.5 if fits, else size 1
  int actionLen = strlen(action);
  if (actionLen * FONT15_W <= w - KEY_ACTION_PAD) {
    D.setTextSize(1.5);
    D.setCursor(x + (w - actionLen * FONT15_W) / 2, y + h - KEY_ACTION_BOT1);
  } else {
    D.setTextSize(1);
    D.setCursor(x + (w - actionLen * FONT1_W) / 2, y + h - KEY_ACTION_BOT2);
  }
  D.print(action);
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
  D.fillRect(dotX - BLE_DOT_R - BLE_DOT_CLEAR_PAD, BLE_DOT_CLEAR_PAD,
    SCREEN_W - dotX + BLE_DOT_R + BLE_DOT_CLEAR_PAD * 2, BLE_BAR_H, BLACK);
  // BLE status dot + label
  uint16_t bleCol = connected ? BLUE : RED;
  D.fillCircle(dotX, BLE_DOT_Y, BLE_DOT_R, bleCol);
  D.setTextSize(1);
  D.setTextColor(DARKGREY);
  D.setCursor(dotX + BLE_DOT_R + BLE_LABEL_GAP, STATUS_TEXT_Y);
  D.print("BLE");
  // Battery / power status
  D.setTextSize(1);
  D.setTextColor(batCol);
  D.setCursor(dotX + BLE_DOT_R + BLE_TEXT_W + BLE_PCT_GAP, STATUS_TEXT_Y);
  D.print(buf);
  D.pushSprite(0, 0);
}

void drawStatus() {
  if (!screenOn) return;
  D.fillScreen(BLACK);

  // Top bar: device name (left) + BLE dot + battery (right via updateBattery)
  D.setTextSize(1);
  D.setTextColor(DARKGREY);
  D.setCursor(STATUS_MARGIN, STATUS_TEXT_Y);
  D.print(fullDeviceName);
  updateBattery();

  if (!connected) {
    // 4-line waiting screen, all size 2, vertically centered
    auto centerPrint = [](const char* msg, int y, uint16_t color) {
      D.setTextSize(2);
      D.setTextColor(color);
      int x = (SCREEN_W - strlen(msg) * FONT2_W) / 2;
      D.setCursor(x > 0 ? x : 0, y);
      D.print(msg);
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
  g_canvas.pushSprite(&g_restore, -FLASH_BANNER_X, -FLASH_BANNER_Y);
  g_banner.fillSprite(color);
  g_banner.setTextSize(3);
  g_banner.setTextColor(BLACK, color);
  int tw = strlen(text) * FONT3_W;
  g_banner.setCursor((FLASH_BANNER_W - tw) / 2, (FLASH_BANNER_H - FONT3_H) / 2);
  g_banner.print(text);
  g_banner.pushSprite(FLASH_BANNER_X, FLASH_BANNER_Y);
  delay(FLASH_DELAY);
  g_restore.pushSprite(FLASH_BANNER_X, FLASH_BANNER_Y);
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
// ============================================================
#if IS_CARDPUTER

String g_wifiSsid = "";
String g_wifiPass = "";
String g_lastPreset = "";
char g_mdnsName[24] = {0};
static const char* mdnsName() {
  if (g_mdnsName[0] == 0) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(g_mdnsName, sizeof(g_mdnsName), "cardputer-kb-%02x%02x", mac[4], mac[5]);
  }
  return g_mdnsName;
}
#define MDNS_NAME mdnsName()
const unsigned long WIFI_TIMEOUT_MS = 30000;
unsigned long g_configEnterTime = 0;
String g_staIp = "";
String g_apIp = "";
bool g_staConnected = false;
bool g_apMode = false;
char g_apSsidStr[32] = {0};

#define BIND_BYTES (10 + BIND_COMMENT_LEN)
static void bindingPack(uint8_t* out, const Binding& b) {
  out[0] = b.trigger;
  out[1] = b.trigger_key;
  out[2] = b.event;
  out[3] = (uint8_t)(b.long_ms & 0xFF);
  out[4] = (uint8_t)((b.long_ms >> 8) & 0xFF);
  out[5] = b.cmd; out[6] = b.opt; out[7] = b.ctrl; out[8] = b.shift;
  out[9] = b.action;
  memcpy(out + 10, b.comment, BIND_COMMENT_LEN);
  out[10 + BIND_COMMENT_LEN - 1] = 0;
}
static void bindingUnpack(const uint8_t* in, Binding& b) {
  b.trigger     = in[0];
  b.trigger_key = in[1];
  b.event       = in[2];
  b.long_ms     = (uint16_t)in[3] | ((uint16_t)in[4] << 8);
  b.cmd = in[5] ? 1 : 0; b.opt = in[6] ? 1 : 0; b.ctrl = in[7] ? 1 : 0; b.shift = in[8] ? 1 : 0;
  b.action = in[9];
  memcpy(b.comment, in + 10, BIND_COMMENT_LEN);
  b.comment[BIND_COMMENT_LEN - 1] = 0;
}

void loadConfig() {
  Preferences prefs;
  prefs.begin("kb", true);
  uint8_t buf[1 + MAX_BINDINGS * BIND_BYTES];
  size_t n = prefs.getBytes("binds", buf, sizeof(buf));
  if (n >= 1 && buf[0] <= MAX_BINDINGS && n == 1 + buf[0] * BIND_BYTES) {
    g_config.count = buf[0];
    for (int i = 0; i < g_config.count; i++) {
      bindingUnpack(buf + 1 + i * BIND_BYTES, g_config.bindings[i]);
    }
  } else {
    g_config = PRESET_LAZYTYPER;
  }
  g_wifiSsid = prefs.getString("ssid", "");
  g_wifiPass = prefs.getString("pass", "");
  g_lastPreset = prefs.getString("preset", "");
  prefs.end();
  // Empty SSID is fine: setupConfigMode falls through to AP, and a USB serial
  // `wifi <ssid> <pass>` line can pre-fill creds without entering AP mode.
}
void saveConfig() {
  Preferences prefs;
  prefs.begin("kb", false);
  uint8_t buf[1 + MAX_BINDINGS * BIND_BYTES];
  buf[0] = g_config.count;
  for (int i = 0; i < g_config.count; i++) {
    bindingPack(buf + 1 + i * BIND_BYTES, g_config.bindings[i]);
  }
  prefs.putBytes("binds", buf, 1 + g_config.count * BIND_BYTES);
  prefs.end();
}
void saveWifi(const String& ssid, const String& pass) {
  Preferences prefs;
  prefs.begin("kb", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  g_wifiSsid = ssid;
  g_wifiPass = pass;
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

static uint8_t akToKey(uint8_t a) {
  switch (a) {
    case AK_ENTER:     return KEY_RETURN;
    case AK_TAB:       return KEY_TAB;
    case AK_ESC:       return KEY_ESC;
    case AK_BACKSPACE: return KEY_BACKSPACE;
    case AK_DELETE:    return KEY_DELETE;
    case AK_SPACE:     return ' ';
    case AK_UP:        return KEY_UP_ARROW;
    case AK_DOWN:      return KEY_DOWN_ARROW;
    case AK_LEFT:      return KEY_LEFT_ARROW;
    case AK_RIGHT:     return KEY_RIGHT_ARROW;
    case AK_PAGE_UP:   return KEY_PAGE_UP;
    case AK_PAGE_DOWN: return KEY_PAGE_DOWN;
    case AK_HOME:      return KEY_HOME;
    case AK_END:       return KEY_END;
    case AK_CAPS_LOCK: return KEY_CAPS_LOCK;
    case AK_F1:  return KEY_F1;  case AK_F2:  return KEY_F2;
    case AK_F3:  return KEY_F3;  case AK_F4:  return KEY_F4;
    case AK_F5:  return KEY_F5;  case AK_F6:  return KEY_F6;
    case AK_F7:  return KEY_F7;  case AK_F8:  return KEY_F8;
    case AK_F9:  return KEY_F9;  case AK_F10: return KEY_F10;
    case AK_F11: return KEY_F11; case AK_F12: return KEY_F12;
    default: break;
  }
  if (a >= AK_A && a <= AK_Z) return 'a' + (a - AK_A);
  if (a >= AK_0 && a <= AK_9) return '0' + (a - AK_0);
  switch (a) {
    case AK_BACKTICK:  return '`';
    case AK_MINUS:     return '-';
    case AK_EQUAL:     return '=';
    case AK_LBRACKET:  return '[';
    case AK_RBRACKET:  return ']';
    case AK_BACKSLASH: return '\\';
    case AK_SEMICOLON: return ';';
    case AK_QUOTE:     return '\'';
    case AK_COMMA:     return ',';
    case AK_PERIOD:    return '.';
    case AK_SLASH:     return '/';
  }
  return 0;
}

static void formatBindingLabel(char* buf, size_t n,
                               uint8_t cmd, uint8_t opt, uint8_t ctrl, uint8_t shift,
                               const char* keyName) {
  buf[0] = 0;
  size_t off = 0;
  if (cmd && off < n - 5)   { strcpy(buf + off, "Cmd+");   off += 4; }
  if (opt && off < n - 5)   { strcpy(buf + off, "Opt+");   off += 4; }
  if (ctrl && off < n - 6)  { strcpy(buf + off, "Ctrl+");  off += 5; }
  if (shift && off < n - 7) { strcpy(buf + off, "Shift+"); off += 6; }
  strncpy(buf + off, keyName, n - off - 1);
  buf[n - 1] = 0;
}

static const char* akName(uint8_t a) {
  switch (a) {
    case AK_NONE:      return "(none)";
    case AK_ENTER:     return "Enter";
    case AK_TAB:       return "Tab";
    case AK_ESC:       return "Esc";
    case AK_BACKSPACE: return "Backspace";
    case AK_DELETE:    return "Delete";
    case AK_SPACE:     return "Space";
    case AK_UP:        return "↑";
    case AK_DOWN:      return "↓";
    case AK_LEFT:      return "←";
    case AK_RIGHT:     return "→";
    case AK_PAGE_UP:   return "PgUp";
    case AK_PAGE_DOWN: return "PgDn";
    case AK_HOME:      return "Home";
    case AK_END:       return "End";
    case AK_CAPS_LOCK: return "CapsLock";
  }
  if (a >= AK_F1 && a <= AK_F12) {
    static char f[4]; snprintf(f, sizeof(f), "F%d", a - AK_F1 + 1); return f;
  }
  if (a >= AK_A && a <= AK_Z) {
    static char ch[2]; ch[0] = 'a' + (a - AK_A); ch[1] = 0; return ch;
  }
  if (a >= AK_0 && a <= AK_9) {
    static char ch[2]; ch[0] = '0' + (a - AK_0); ch[1] = 0; return ch;
  }
  switch (a) {
    case AK_BACKTICK:  return "`";
    case AK_MINUS:     return "-";
    case AK_EQUAL:     return "=";
    case AK_LBRACKET:  return "[";
    case AK_RBRACKET:  return "]";
    case AK_BACKSLASH: return "\\";
    case AK_SEMICOLON: return ";";
    case AK_QUOTE:     return "'";
    case AK_COMMA:     return ",";
    case AK_PERIOD:    return ".";
    case AK_SLASH:     return "/";
  }
  return "?";
}

static void executeBinding(const Binding& b, uint16_t flashColor) {
  if (b.action == AK_CAPS_LOCK && !b.cmd && !b.opt && !b.ctrl && !b.shift) {
    capsLocked = !capsLocked;
    bleKeyboard.write(KEY_CAPS_LOCK);
    showFlash(capsLocked ? "CAPS ON" : "caps off", TFT_YELLOW);
    return;
  }
  if (b.action == AK_NONE && !b.cmd && !b.opt && !b.ctrl && !b.shift) return;

  if (b.cmd)   bleKeyboard.press(KEY_LEFT_GUI);
  if (b.opt)   bleKeyboard.press(KEY_LEFT_ALT);
  if (b.ctrl)  bleKeyboard.press(KEY_LEFT_CTRL);
  if (b.shift) bleKeyboard.press(KEY_LEFT_SHIFT);

  uint8_t k = akToKey(b.action);
  if (k != 0) bleKeyboard.press(k);
  bleKeyboard.releaseAll();

  char label[32];
  formatBindingLabel(label, sizeof(label), b.cmd, b.opt, b.ctrl, b.shift,
                     b.action == AK_NONE ? "" : akName(b.action));
  showFlash(label, flashColor);
}

static const Binding* findBinding(uint8_t trigger, uint8_t event, uint8_t trigger_key = 0) {
  for (int i = 0; i < g_config.count; i++) {
    const Binding& b = g_config.bindings[i];
    if (b.trigger != trigger) continue;
    if (b.event != event) continue;
    if (trigger == TK_KEY && b.trigger_key != trigger_key) continue;
    return &b;
  }
  return nullptr;
}

static bool hasMultiTap(uint8_t trigger, uint8_t trigger_key = 0) {
  for (int i = 0; i < g_config.count; i++) {
    const Binding& b = g_config.bindings[i];
    if (b.trigger != trigger) continue;
    if (trigger == TK_KEY && b.trigger_key != trigger_key) continue;
    if (b.event == TEV_DOUBLE || b.event == TEV_TRIPLE) return true;
  }
  return false;
}
static bool hasLongPress(uint8_t trigger, uint8_t trigger_key = 0) {
  for (int i = 0; i < g_config.count; i++) {
    const Binding& b = g_config.bindings[i];
    if (b.trigger != trigger) continue;
    if (trigger == TK_KEY && b.trigger_key != trigger_key) continue;
    if (b.event == TEV_LONG) return true;
  }
  return false;
}
static uint16_t getLongMs(uint8_t trigger, uint8_t trigger_key = 0) {
  for (int i = 0; i < g_config.count; i++) {
    const Binding& b = g_config.bindings[i];
    if (b.trigger != trigger) continue;
    if (trigger == TK_KEY && b.trigger_key != trigger_key) continue;
    if (b.event == TEV_LONG) return b.long_ms ? b.long_ms : LONG_PRESS_DEFAULT_MS;
  }
  return LONG_PRESS_DEFAULT_MS;
}

struct ModTapState {
  unsigned long press_start = 0;
  unsigned long last_release = 0;
  uint8_t tap_count = 0;
  bool long_fired = false;
};
ModTapState ctrlState, optState, fnState, shiftState, altState;

static uint8_t tapCountToEvent(uint8_t n) {
  if (n >= 3) return TEV_TRIPLE;
  if (n == 2) return TEV_DOUBLE;
  return TEV_SINGLE;
}

static void onModPress(ModTapState* sp, unsigned long now) {
  sp->press_start = now;
  sp->long_fired = false;
  if (sp->last_release > 0 && now - sp->last_release < TAP_WINDOW_MS) {
    sp->tap_count++;
  } else {
    sp->tap_count = 1;
  }
}

static void onModRelease(ModTapState* sp, unsigned long now,
                         bool usedAsModifier, uint8_t trigger, uint16_t color) {
  if (usedAsModifier || sp->long_fired) {
    sp->tap_count = 0;
    sp->last_release = 0;
    return;
  }
  sp->last_release = now;
  if (!hasMultiTap(trigger)) {
    const Binding* b = findBinding(trigger, TEV_SINGLE);
    if (b) executeBinding(*b, color);
    sp->tap_count = 0;
    sp->last_release = 0;
  }
}

static void pollMod(ModTapState* sp, uint8_t trigger, uint16_t color, bool held) {
  unsigned long now = millis();
  if (held && !sp->long_fired && sp->press_start > 0 && hasLongPress(trigger)) {
    uint16_t lms = getLongMs(trigger);
    if (now - sp->press_start >= lms) {
      const Binding* b = findBinding(trigger, TEV_LONG);
      if (b) {
        executeBinding(*b, color);
        sp->long_fired = true;
        sp->tap_count = 0;
      }
    }
  }
  if (!held && sp->tap_count > 0 && sp->last_release > 0 &&
      (now - sp->last_release >= TAP_WINDOW_MS)) {
    uint8_t ev = tapCountToEvent(sp->tap_count);
    const Binding* b = findBinding(trigger, ev);
    if (b) executeBinding(*b, color);
    sp->tap_count = 0;
    sp->last_release = 0;
  }
}
bool dispatchKeyTap(char c) {
  const Binding* b = findBinding(TK_KEY, TEV_SINGLE, (uint8_t)c);
  if (b) {
    uint16_t color = (c == '`') ? COL_KEY_ESC :
                     (c >= '0' && c <= '9') ? COL_KEY_NUM : COL_KEY_NUM;
    executeBinding(*b, color);
    return true;
  }
  return false;
}

struct AkOption { uint8_t ak; const char* label; };
static const AkOption AK_OPTIONS_SPECIAL[] = {
  {AK_NONE, "(none)"}, {AK_ENTER, "Enter"}, {AK_TAB, "Tab"}, {AK_ESC, "Esc"},
  {AK_BACKSPACE, "Backspace"}, {AK_DELETE, "Delete"}, {AK_SPACE, "Space"},
  {AK_UP, "↑ Up"}, {AK_DOWN, "↓ Down"}, {AK_LEFT, "← Left"}, {AK_RIGHT, "→ Right"},
  {AK_PAGE_UP, "PgUp"}, {AK_PAGE_DOWN, "PgDn"}, {AK_HOME, "Home"}, {AK_END, "End"},
  {AK_CAPS_LOCK, "Caps Lock"},
};
static const AkOption AK_OPTIONS_FN[] = {
  {AK_F1, "F1"}, {AK_F2, "F2"}, {AK_F3, "F3"}, {AK_F4, "F4"},
  {AK_F5, "F5"}, {AK_F6, "F6"}, {AK_F7, "F7"}, {AK_F8, "F8"},
  {AK_F9, "F9"}, {AK_F10, "F10"}, {AK_F11, "F11"}, {AK_F12, "F12"},
};

static String htmlAkSelect(const char* name, uint8_t curr) {
  String s = "<select name='"; s += name; s += "'>";
  s += "<optgroup label='Special'>";
  for (auto& o : AK_OPTIONS_SPECIAL) {
    s += "<option value='"; s += o.ak; s += "'";
    if (o.ak == curr) s += " selected";
    s += ">"; s += o.label; s += "</option>";
  }
  s += "</optgroup><optgroup label='Function'>";
  for (auto& o : AK_OPTIONS_FN) {
    s += "<option value='"; s += o.ak; s += "'";
    if (o.ak == curr) s += " selected";
    s += ">"; s += o.label; s += "</option>";
  }
  s += "</optgroup><optgroup label='Letters a-z'>";
  for (uint8_t a = AK_A; a <= AK_Z; a++) {
    s += "<option value='"; s += a; s += "'";
    if (a == curr) s += " selected";
    s += ">"; s += (char)('a' + (a - AK_A)); s += "</option>";
  }
  s += "</optgroup><optgroup label='Digits 0-9'>";
  for (uint8_t a = AK_0; a <= AK_9; a++) {
    s += "<option value='"; s += a; s += "'";
    if (a == curr) s += " selected";
    s += ">"; s += (char)('0' + (a - AK_0)); s += "</option>";
  }
  s += "</optgroup><optgroup label='Symbols'>";
  for (uint8_t a = AK_BACKTICK; a <= AK_SLASH; a++) {
    s += "<option value='"; s += a; s += "'";
    if (a == curr) s += " selected";
    s += ">"; s += akName(a); s += "</option>";
  }
  s += "</optgroup></select>";
  return s;
}

static String escapeJsonString(const char* s) {
  String out;
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
    else if ((uint8_t)*p < 0x20) continue;
    else out += *p;
  }
  return out;
}
static int bindingJsonInto(char* buf, int n, const Binding& b) {
  String esc = escapeJsonString(b.comment);
  return snprintf(buf, n,
    "{\"trigger\":%u,\"key\":%u,\"event\":%u,\"long_ms\":%u,"
    "\"cmd\":%u,\"opt\":%u,\"ctrl\":%u,\"shift\":%u,\"action\":%u,\"comment\":\"%s\"}",
    b.trigger, b.trigger_key, b.event, b.long_ms,
    b.cmd, b.opt, b.ctrl, b.shift, b.action, esc.c_str());
}
static String configJson(const KbConfig& c) {
  String s = "[";
  char buf[320];
  for (int i = 0; i < c.count; i++) {
    if (i > 0) s += ",";
    bindingJsonInto(buf, sizeof(buf), c.bindings[i]);
    s += buf;
  }
  s += "]";
  return s;
}

const char JS_CODE[] =
"function akSelectHtml(curr){"
"let h=\"<select class='b-act'>\";"
"for(const grp of AK_OPTS){"
"h+=`<optgroup label='${grp.g}'>`;"
"for(const [v,l] of grp.o) h+=`<option value='${v}' ${v==curr?'selected':''}>${l}</option>`;"
"h+=\"</optgroup>\";"
"}"
"return h+\"</select>\";"
"}"
"const SYM_NAMES={32:'Space',9:'Tab',10:'Enter',8:'Backspace',96:'backtick `',45:'minus -',61:'equals =',91:'left bracket [',93:'right bracket ]',92:'backslash \\\\',59:'semicolon ;',39:'apostrophe \\'',44:'comma ,',46:'period .',47:'slash /'};"
"function trigSelectHtml(curr,key){"
"let cur='';"
"if((curr>=1&&curr<=3)||curr==5||curr==6)cur='m'+curr;"
"else if(curr==4&&key)cur='k'+key;"
"let h=\"<select class='b-trig'>\";"
"h+=\"<optgroup label='Modifiers'>\";"
"for(const [v,l] of [['m1','Ctrl'],['m2','Opt'],['m3','Fn'],['m5','Aa (Shift)'],['m6','Alt']])"
"h+=`<option value='${v}' ${cur==v?'selected':''} title='${l}'>${l}</option>`;"
"h+=\"</optgroup>\";"
"for(const grp of TRIG_KEY_OPTS){"
"h+=`<optgroup label='${grp.g}'>`;"
"for(const [code,l] of grp.o){"
"const v='k'+code;"
"const t=SYM_NAMES[code]||l;"
"h+=`<option value='${v}' ${cur==v?'selected':''} title='${t}'>${l}</option>`;"
"}"
"h+=\"</optgroup>\";"
"}"
"return h+\"</select>\";"
"}"
"function eventSelectHtml(curr,longMs){"
"const showLong=curr==3?'flex':'none';"
"let h=\"<select class='b-event' onchange='eventChanged(this)'>\";"
"for(const v in EVENTS) h+=`<option value='${v}' ${v==curr?'selected':''}>${EVENTS[v]}</option>`;"
"h+=\"</select>\";"
"h+=`<span class='long-row' style='display:${showLong}'>`;"
"h+=`<input type='number' class='b-longms' min='100' max='5000' step='50' value='${longMs||500}' title='Long press ms'>`;"
"h+=\"<span class='unit'>ms</span>\";"
"h+=\"</span>\";"
"return h;"
"}"
"function eventChanged(sel){"
"const row=sel.parentElement.querySelector('.long-row');"
"row.style.display=sel.value=='3'?'flex':'none';"
"}"
"function escAttr(s){return (s||'').replace(/&/g,'&amp;').replace(/\"/g,'&quot;').replace(/</g,'&lt;')}"
"function delRowFn(t){const r=t.closest('tr'),n=r.nextElementSibling;r.remove();if(n&&n.classList.contains('bind-cmt-row'))n.remove()}"
"function rowHtml(b){"
"b=b||{trigger:1,key:0,event:0,long_ms:500,cmd:0,opt:0,ctrl:0,shift:0,action:0,comment:''};"
"return `<tr class='bind-row'>"
"<td class='col-trig'>${trigSelectHtml(b.trigger,b.key)}</td>"
"<td class='col-event'><div class='event-cell'>${eventSelectHtml(b.event,b.long_ms)}</div></td>"
"<td class='col-mod'><input type='checkbox' class='b-cmd' ${b.cmd?'checked':''}></td>"
"<td class='col-mod'><input type='checkbox' class='b-opt' ${b.opt?'checked':''}></td>"
"<td class='col-mod'><input type='checkbox' class='b-ctrl' ${b.ctrl?'checked':''}></td>"
"<td class='col-mod'><input type='checkbox' class='b-shift' ${b.shift?'checked':''}></td>"
"<td class='col-act'>${akSelectHtml(b.action)}</td>"
"<td class='col-del'><button type='button' class='del' title='Delete this binding' onclick='delRowFn(this)'>×</button></td>"
"</tr>"
"<tr class='bind-cmt-row'><td colspan='8'>"
"<input type='text' class='b-comment' maxlength='63' placeholder='Note (optional, ~60 chars)' value='${escAttr(b.comment)}'>"
"</td></tr>`;"
"}"
"function addRow(b){"
"const tbody=document.getElementById('binds');"
"const tmp=document.createElement('tbody');"
"tmp.innerHTML=rowHtml(b);"
"while(tmp.firstElementChild) tbody.appendChild(tmp.firstElementChild);"
"}"
"function clearRows(){document.getElementById('binds').innerHTML=\"\"}"
"function loadBindings(arr){clearRows();arr.forEach(addRow)}"
"function collectBindings(){"
"const rows=document.querySelectorAll('.bind-row');"
"const out=[];"
"for(const r of rows){"
"const tv=r.querySelector('.b-trig').value;"
"let trig=0,key=0;"
"if(tv.startsWith('m')){trig=parseInt(tv.slice(1));}"
"else if(tv.startsWith('k')){trig=4;key=parseInt(tv.slice(1));}"
"const ev=parseInt(r.querySelector('.b-event').value);"
"const longInp=r.querySelector('.b-longms');"
"const ka=r.querySelector('.b-act');"
"const cmtRow=r.nextElementSibling;"
"const cmtInp=cmtRow&&cmtRow.classList.contains('bind-cmt-row')?cmtRow.querySelector('.b-comment'):null;"
"out.push({"
"trigger:trig,"
"key:key,"
"event:ev,"
"long_ms:ev==3?(parseInt(longInp.value)||500):0,"
"cmd:r.querySelector('.b-cmd').checked?1:0,"
"opt:r.querySelector('.b-opt').checked?1:0,"
"ctrl:r.querySelector('.b-ctrl').checked?1:0,"
"shift:r.querySelector('.b-shift').checked?1:0,"
"action:ka?parseInt(ka.value):0,"
"comment:cmtInp?cmtInp.value:''"
"});"
"}"
"return out;"
"}"
"function bindingKey(b){const t=b.trigger==4?(b.trigger+':'+b.key):String(b.trigger);return t+'/'+(b.event||0);}"
"function mergeBindings(existing,incoming){"
"const map=new Map(existing.map(b=>[bindingKey(b),b]));"
"for(const b of incoming) map.set(bindingKey(b),b);"
"return [...map.values()];"
"}"
"async function loadPreset(name){"
"if(!name)return;"
"const r=await fetch('/api/presets');const presets=await r.json();"
"const p=presets.find(x=>x.name===name);"
"if(!p)return;"
"if(p.bindings.length===0){clearRows();highlightConflicts();return;}"
"const merged=mergeBindings(collectBindings(),p.bindings);"
"loadBindings(merged);"
"highlightConflicts();"
"}"
"async function init(){"
"const r=await fetch('/api/config');const cfg=await r.json();"
"loadBindings(cfg.bindings);"
"const pr=await fetch('/api/presets');const presets=await pr.json();"
"const sel=document.getElementById('preset');"
"for(const p of presets){"
"const o=document.createElement('option');o.value=p.name;o.textContent=p.name;sel.appendChild(o);"
"}"
"if(cfg.last_preset)sel.value=cfg.last_preset;"
"document.getElementById('binds').addEventListener('change',highlightConflicts);"
"highlightConflicts();"
"}"
"function highlightConflicts(){"
"const rows=document.querySelectorAll('.bind-row');"
"rows.forEach(r=>{r.classList.remove('conflict');const c=r.nextElementSibling;if(c&&c.classList.contains('bind-cmt-row'))c.classList.remove('conflict');});"
"const seen={};const conflicts=new Set();"
"rows.forEach((r,i)=>{"
"const tv=r.querySelector('.b-trig').value;"
"const ev=r.querySelector('.b-event').value;"
"const k=tv+'|'+ev;"
"if(seen[k]!==undefined){conflicts.add(seen[k]);conflicts.add(i);}"
"else seen[k]=i;"
"});"
"conflicts.forEach(i=>{rows[i].classList.add('conflict');const c=rows[i].nextElementSibling;if(c&&c.classList.contains('bind-cmt-row'))c.classList.add('conflict');});"
"const banner=document.getElementById('conflict-banner');"
"if(banner)banner.style.display=conflicts.size>0?'block':'none';"
"return conflicts.size;"
"}"
"async function saveAll(){"
"if(highlightConflicts()>0){if(!confirm('Conflicts exist (red rows). Save anyway?'))return;}"
"const data={"
"ssid:document.getElementById('ssid').value,"
"pass:document.getElementById('pass').value,"
"last_preset:document.getElementById('preset').value||'',"
"bindings:collectBindings()"
"};"
"const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});"
"document.body.innerHTML=\"<h2 style='text-align:center;margin-top:3em'>Saved. Rebooting...</h2>\";"
"}"
"function exportConfig(){"
"const cfg={last_preset:document.getElementById('preset').value||'',bindings:collectBindings()};"
"const blob=new Blob([JSON.stringify(cfg,null,2)],{type:'application/json'});"
"const a=document.createElement('a');"
"a.href=URL.createObjectURL(blob);"
"a.download='cardputer-keyboard-config.json';"
"a.click();"
"URL.revokeObjectURL(a.href);"
"}"
"function importConfig(){"
"const txt=prompt('Paste JSON config (exported format):');"
"if(!txt)return;"
"try{const cfg=JSON.parse(txt);"
"if(!cfg.bindings||!Array.isArray(cfg.bindings))throw new Error('missing bindings array');"
"loadBindings(cfg.bindings);"
"if(cfg.last_preset)document.getElementById('preset').value=cfg.last_preset;"
"highlightConflicts();"
"alert('Loaded. Click Save & Reboot to apply.');"
"}catch(e){alert('Parse failed: '+e.message);}"
"}"
"async function cancelAll(){"
"await fetch('/cancel',{method:'POST'});"
"document.body.innerHTML=\"<h2 style='text-align:center;margin-top:3em'>Rebooting...</h2>\";"
"}"
"async function switchToAp(){"
"if(!confirm('Device will reboot into AP mode.\\nNext config-mode entry still tries STA first. Continue?'))return;"
"await fetch('/api/switch-to-ap',{method:'POST'});"
"document.body.innerHTML=\"<h2 style='text-align:center;margin-top:3em'>Switching to AP...<br><small>Connect to CardPuter-KB-XXXX WiFi then visit http://192.168.4.1/</small></h2>\";"
"}"
"async function scanWifi(){"
"const el=document.getElementById('wifi-list');el.textContent='Scanning...';"
"try{const r=await fetch('/api/wifi/scan');const nets=await r.json();"
"if(!nets.length){el.textContent='No WiFi found';return}"
"el.innerHTML='';"
"const bars=(r)=>{const lvl=r>=-50?4:r>=-60?3:r>=-70?2:r>=-80?1:0;return '▰'.repeat(lvl)+'▱'.repeat(4-lvl);};"
"const barColor=(r)=>r>=-60?'#10b981':r>=-75?'#f59e0b':'#dc2626';"
"for(const n of nets){"
"const a=document.createElement('a');a.href='#';"
"a.style.display='flex';a.style.alignItems='center';a.style.gap='.5em';"
"const ssidSpan=document.createElement('span');ssidSpan.textContent=n.ssid+(n.enc?' 🔒':'');ssidSpan.style.flex='1';"
"const barSpan=document.createElement('span');barSpan.textContent=bars(n.rssi);barSpan.style.color=barColor(n.rssi);barSpan.style.fontFamily='monospace';barSpan.style.letterSpacing='-1px';"
"const dbmSpan=document.createElement('span');dbmSpan.textContent=n.rssi+'dBm';dbmSpan.style.color='#888';dbmSpan.style.fontSize='.85em';dbmSpan.style.minWidth='4em';dbmSpan.style.textAlign='right';"
"a.appendChild(ssidSpan);a.appendChild(barSpan);a.appendChild(dbmSpan);"
"a.onclick=(e)=>{e.preventDefault();document.getElementById('ssid').value=n.ssid;document.getElementById('pass').focus();};"
"el.appendChild(a);"
"}"
"}catch(e){el.textContent='Scan failed: '+e}"
"}"
"init();";

void handleRoot() {
  String body = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  body += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  body += "<title>CardPuter Keyboard Config</title>";
  body += "<style>body{font-family:system-ui;max-width:500px;margin:1em auto;padding:0 1em}";
  body += "label{display:block;margin:1em 0 .3em;font-weight:600}";
  body += "select,input{width:100%;padding:.5em;font-size:1em;box-sizing:border-box}";
  body += "button{margin-top:1.5em;padding:.7em 1.5em;font-size:1em;width:100%;cursor:pointer}";
  body += ".save{background:#2563eb;color:#fff;border:0;border-radius:4px}";
  body += ".reboot{background:#dc2626;color:#fff;border:0;border-radius:4px;margin-top:.5em}";
  body += ".scan{background:#10b981;color:#fff;border:0;border-radius:4px;margin-top:.3em}";
  body += "fieldset{margin-top:1.5em;border:1px solid #ddd;border-radius:6px;padding:.5em 1em 1em}";
  body += "legend{font-weight:600;padding:0 .5em}";
  body += "#wifi-list{margin-top:.5em;font-size:.9em}";
  body += "#wifi-list a{display:block;padding:.4em;color:#2563eb;text-decoration:none;border-bottom:1px solid #eee}";
  body += "table.bindings{width:100%;border-collapse:collapse;font-size:.9em}";
  body += "table.bindings th,table.bindings td{padding:.4em .3em;text-align:center;border-bottom:1px solid #eee}";
  body += "table.bindings th:first-child,table.bindings td:first-child{text-align:left;font-weight:600}";
  body += "table.bindings select{width:auto;min-width:8em}";
  body += "table.bindings input[type=checkbox]{width:auto;transform:scale(1.3)}";
  body += ".banner{padding:.9em 1.2em;border-radius:6px;margin-bottom:1em;font-size:1.05em;line-height:1.6}";
  body += ".banner b,.banner code{font-size:1.1em}";
  body += ".banner code{font-family:ui-monospace,monospace;background:rgba(0,0,0,.05);padding:.05em .3em;border-radius:3px}";
  body += ".banner-sta{background:#dcfce7;color:#166534;border:1px solid #86efac}";
  body += ".banner-ap{background:#fef3c7;color:#854d0e;border:1px solid #fcd34d}";
  body += ".banner small{opacity:.85;font-size:.85em}";
  body += ".banner button.switch-mode{margin-top:.4em;background:#fff;color:#374151;border:1px solid #9ca3af;border-radius:4px;padding:.3em .8em;cursor:pointer;font-size:.85em}";
  body += ".banner button.switch-mode:hover{background:#f3f4f6}";
  body += "table.bind-table{width:100%;border-collapse:collapse;margin-top:.5em}";
  body += "table.bind-table th{font-size:.8em;color:#666;font-weight:600;padding:.4em .2em;border-bottom:1px solid #ccc;text-align:center;white-space:nowrap}";
  body += "table.bind-table td{padding:.4em .2em;border-bottom:1px solid #eee;text-align:center;vertical-align:middle}";
  body += "table.bind-table th.col-trig,table.bind-table td.col-trig{width:7em}";
  body += "table.bind-table th.col-event,table.bind-table td.col-event{width:7.5em}";
  body += "table.bind-table .event-cell .long-row{display:flex;gap:.25em;align-items:center;margin-top:.25em;width:100%}";
  body += "table.bind-table .event-cell .long-row input.b-longms{flex:1;min-width:0;text-align:right}";
  body += "table.bind-table .event-cell .long-row .unit{font-size:.85em;color:#666;flex-shrink:0}";
  body += "table.bind-table th.col-mod,table.bind-table td.col-mod{width:2.4em}";
  body += "table.bind-table th.col-act,table.bind-table td.col-act{width:10em}";
  body += "table.bind-table th.col-del,table.bind-table td.col-del{width:2em}";
  body += "table.bind-table .event-cell{display:flex;flex-direction:column;align-items:stretch;gap:0}";
  body += "table.bind-table .event-cell select{width:100%;min-width:0}";
  body += "table.bind-table input[type=checkbox]{transform:scale(1.2);margin:0}";
  body += "table.bind-table select,table.bind-table input[type=text]{font-size:.9em;padding:.3em;width:100%;box-sizing:border-box}";
  body += "table.bind-table td.col-del{line-height:0}";
  body += "table.bind-table .del{background:none;border:0;cursor:pointer;padding:0;margin:0;font-size:1.1em;color:#999;width:1.8em;height:1.8em;text-align:center;border-radius:50%;display:inline-flex;align-items:center;justify-content:center;line-height:1}";
  body += "table.bind-table .del:hover{background:#fee;color:#c00}";
  body += "table.bind-table tr.bind-row td{border-bottom:0}";
  body += "table.bind-table tr.bind-cmt-row td{padding:0 .4em .6em;border-bottom:1px solid #eee}";
  body += "table.bind-table tr.bind-cmt-row input.b-comment{width:100%;font-size:.85em;padding:.25em .5em;color:#555;border:1px dashed #ccc;border-radius:3px;background:#fafafa;box-sizing:border-box}";
  body += "table.bind-table tr.bind-cmt-row input.b-comment:focus{border-style:solid;border-color:#888;background:#fff;color:#222;outline:none}";
  body += ".add-btn{background:#10b981;color:#fff;border:0;border-radius:4px;padding:.5em 1em;cursor:pointer;font-size:.95em;width:auto}";
  body += ".io-btn{background:#fff;color:#374151;border:1px solid #d1d5db;border-radius:4px;padding:.5em 1em;cursor:pointer;font-size:.9em;width:auto}";
  body += ".io-btn:hover{background:#f3f4f6}";
  body += "table.bind-table tr.bind-row.conflict td,table.bind-table tr.bind-cmt-row.conflict td{background:#fef2f2}";
  body += "table.bind-table tr.bind-row.conflict td:first-child{box-shadow:inset 3px 0 0 #dc2626}";
  body += "</style></head><body>";
  body += "<h2>CardPuter Keyboard Config</h2>";

  body += "[BANNER]";

  body += "<fieldset><legend>Preset (one-click load below)</legend>";
  body += "<select id='preset' onchange='loadPreset(this.value)'>";
  body += "<option value=''>-- Select preset --</option>";
  body += "</select>";
  body += "</fieldset>";

  body += "<fieldset><legend>WiFi (home network)</legend>";
  body += "<label>SSID</label><input id='ssid' value='";
  body += g_wifiSsid;
  body += "'>";
  body += "<label>Password</label><input id='pass' type='password' placeholder='";
  body += g_wifiPass.length() > 0 ? "(saved, leave blank to keep)" : "";
  body += "'>";
  body += "<button type='button' class='scan' onclick='scanWifi()'>Scan WiFi</button>";
  body += "<div id='wifi-list'></div>";
  body += "</fieldset>";

  body += "<fieldset><legend>Bindings (one row = trigger → action)</legend>";
  body += "<table class='bind-table'><thead><tr>";
  body += "<th class='col-trig'>Trigger</th>";
  body += "<th class='col-event'>Event</th>";
  body += "<th class='col-mod'>Cmd</th><th class='col-mod'>Opt</th><th class='col-mod'>Ctrl</th><th class='col-mod'>Shift</th>";
  body += "<th class='col-act'>Action</th>";
  body += "<th class='col-del'></th>";
  body += "</tr></thead><tbody id='binds'></tbody></table>";
  body += "<div id='conflict-banner' style='display:none;background:#fee2e2;color:#991b1b;border:1px solid #fca5a5;padding:.5em .8em;border-radius:4px;margin-top:.5em;font-size:.9em'>";
  body += "Red rows have duplicate triggers — please review before save";
  body += "</div>";
  body += "<div style='margin-top:.5em;display:flex;gap:.5em;flex-wrap:wrap;align-items:center'>";
  body += "<button type='button' class='add-btn' onclick='addRow()'>+ Add binding</button>";
  body += "<button type='button' class='io-btn' onclick='exportConfig()'>Export JSON</button>";
  body += "<button type='button' class='io-btn' onclick='importConfig()'>Import JSON</button>";
  body += "</div>";
  body += "</fieldset>";

  body += "<button class='save' type='button' onclick='saveAll()'>Save & Reboot</button>";
  body += "<button class='reboot' type='button' onclick='cancelAll()'>Cancel & Reboot</button>";

  // banner
  String banner;
  if (g_apMode) {
    banner = "<div class='banner banner-ap'><b>📡 AP MODE</b><br>"
             "SSID <code>" + String(g_apSsidStr) + "</code>, then connect AP and visit <code>http://" + g_apIp + "/</code><br>"
             "<small>Fill WiFi below and save to switch to STA (AP closes).</small></div>";
  } else {
    banner = "<div class='banner banner-sta'><b>📶 STA MODE</b>, connected to <b>" + g_wifiSsid + "</b>"
             "(RSSI " + String(WiFi.RSSI()) + " dBm)<br>"
             "🔗 <code>http://" + g_staIp + "/</code>"
             " or <code>http://" + String(MDNS_NAME) + ".local/</code><br>"
             "<button class='switch-mode' type='button' onclick='switchToAp()'>Switch to AP (reboot)</button></div>";
  }
  body.replace("[BANNER]", banner);

  // ===== JS =====
  body += "<script>";

  body += "const AK_OPTS=[";
  body += "{g:'Special',o:[";
  for (size_t i = 0; i < sizeof(AK_OPTIONS_SPECIAL)/sizeof(AK_OPTIONS_SPECIAL[0]); i++) {
    if (i) body += ",";
    body += "[" + String(AK_OPTIONS_SPECIAL[i].ak) + ",'" + AK_OPTIONS_SPECIAL[i].label + "']";
  }
  body += "]},";
  body += "{g:'Function',o:[";
  for (size_t i = 0; i < sizeof(AK_OPTIONS_FN)/sizeof(AK_OPTIONS_FN[0]); i++) {
    if (i) body += ",";
    body += "[" + String(AK_OPTIONS_FN[i].ak) + ",'" + AK_OPTIONS_FN[i].label + "']";
  }
  body += "]},";
  body += "{g:'Letters a-z',o:[";
  for (uint8_t a = AK_A; a <= AK_Z; a++) {
    if (a > AK_A) body += ",";
    body += "[" + String(a) + ",'" + (char)('a' + (a - AK_A)) + "']";
  }
  body += "]},";
  body += "{g:'Digits 0-9',o:[";
  for (uint8_t a = AK_0; a <= AK_9; a++) {
    if (a > AK_0) body += ",";
    body += "[" + String(a) + ",'" + (char)('0' + (a - AK_0)) + "']";
  }
  body += "]},";
  body += "{g:'Symbols',o:[";
  for (uint8_t a = AK_BACKTICK; a <= AK_SLASH; a++) {
    if (a > AK_BACKTICK) body += ",";
    const char* nm = akName(a);
    String esc;
    for (const char* p = nm; *p; p++) {
      if (*p == '\\' || *p == '"') esc += '\\';
      esc += *p;
    }
    body += "[" + String(a) + ",\"" + esc + "\"]";
  }
  body += "]}];";

  body += "const TRIG_KEY_OPTS=[";
  body += "{g:'Special',o:[";
  body += "[32,\"Space\"],[9,\"Tab\"],[10,\"Enter\"],[8,\"Backspace\"]";
  body += "]},{g:'Digits',o:[";
  {
    const char* row = "1234567890";
    for (const char* p = row; *p; p++) {
      if (p != row) body += ",";
      body += "[" + String((int)(uint8_t)*p) + ",\"" + *p + "\"]";
    }
  }
  body += "]},{g:'Letters',o:[";
  for (char c = 'a'; c <= 'z'; c++) {
    if (c != 'a') body += ",";
    body += "[" + String((int)c) + ",\"" + c + "\"]";
  }
  body += "]},{g:'Symbols',o:[";
  {
    const char* row = "`-=[]\\;',./";
    for (const char* p = row; *p; p++) {
      if (p != row) body += ",";
      String esc;
      if (*p == '\\' || *p == '"') esc += '\\';
      esc += *p;
      body += "[" + String((int)(uint8_t)*p) + ",\"" + esc + "\"]";
    }
  }
  body += "]}];";
  body += "const EVENTS={0:'Single',1:'Double',2:'Triple',3:'Long'};";

  body += JS_CODE;
  body += "</script></body></html>";
  g_web->send(200, "text/html; charset=utf-8", body);
}

void handleApiConfig() {
  String s = "{\"last_preset\":\"";
  s += escapeJsonString(g_lastPreset.c_str());
  s += "\",\"bindings\":";
  s += configJson(g_config);
  s += "}";
  g_web->send(200, "application/json", s);
}

void handleApiPresets() {
  String s = "[";
  for (int i = 0; i < PRESET_COUNT; i++) {
    if (i) s += ",";
    s += "{\"name\":\""; s += PRESETS[i].name; s += "\",\"bindings\":";
    s += configJson(*PRESETS[i].cfg);
    s += "}";
  }
  s += "]";
  g_web->send(200, "application/json", s);
}

static int jsonFindInt(const String& body, const String& key, int def = 0) {
  String pat = "\"" + key + "\":";
  int p = body.indexOf(pat);
  if (p < 0) return def;
  return body.substring(p + pat.length()).toInt();
}
static String jsonFindStr(const String& body, const String& key) {
  String pat = "\"" + key + "\":\"";
  int p = body.indexOf(pat);
  if (p < 0) return "";
  int start = p + pat.length();
  int end = body.indexOf('"', start);
  if (end < 0) return "";
  return body.substring(start, end);
}

void handleSave() {
  String body = g_web->arg("plain");
  if (body.length() == 0) {
    g_web->send(400, "text/plain", "expected JSON body"); return;
  }

  int bArrStart = body.indexOf("\"bindings\":[");
  if (bArrStart < 0) {
    g_web->send(400, "text/plain", "missing bindings"); return;
  }
  int p = bArrStart + 12;
  int count = 0;
  while (p < (int)body.length() && count < MAX_BINDINGS) {
    int objStart = body.indexOf('{', p);
    if (objStart < 0) break;
    int objEnd = body.indexOf('}', objStart);
    if (objEnd < 0) break;
    String obj = body.substring(objStart, objEnd + 1);
    Binding& b = g_config.bindings[count];
    b.trigger     = jsonFindInt(obj, "trigger", 0);
    b.trigger_key = jsonFindInt(obj, "key", 0);
    b.event       = jsonFindInt(obj, "event", TEV_SINGLE);
    b.long_ms     = jsonFindInt(obj, "long_ms", LONG_PRESS_DEFAULT_MS);
    b.cmd         = jsonFindInt(obj, "cmd", 0)   ? 1 : 0;
    b.opt         = jsonFindInt(obj, "opt", 0)   ? 1 : 0;
    b.ctrl        = jsonFindInt(obj, "ctrl", 0)  ? 1 : 0;
    b.shift       = jsonFindInt(obj, "shift", 0) ? 1 : 0;
    b.action      = jsonFindInt(obj, "action", 0);
    String cmt    = jsonFindStr(obj, "comment");
    strncpy(b.comment, cmt.c_str(), BIND_COMMENT_LEN - 1);
    b.comment[BIND_COMMENT_LEN - 1] = 0;
    if (b.trigger != TK_NONE) count++;
    p = objEnd + 1;
    if (body[p] == ']') break;
  }
  g_config.count = count;
  saveConfig();

  // last preset name (purely UI memory, NVS string)
  String lp = jsonFindStr(body, "last_preset");
  g_lastPreset = lp;
  {
    Preferences prefs;
    prefs.begin("kb", false);
    prefs.putString("preset", lp);
    prefs.end();
  }

  // WiFi creds
  String newSsid = jsonFindStr(body, "ssid");
  String newPass = jsonFindStr(body, "pass");
  if (newPass.length() == 0) newPass = g_wifiPass;
  bool wifiChanged = false;
  if (newSsid.length() > 0 && (newSsid != g_wifiSsid || newPass != g_wifiPass)) {
    saveWifi(newSsid, newPass);
    wifiChanged = true;
  }

  if (wifiChanged) requestConfigBoot();
  g_web->send(200, "application/json", "{\"ok\":true}");
  delay(500);
  esp_restart();
}

void handleCancel() {
  g_web->send(200, "text/html; charset=utf-8",
              "<html><body style='font-family:system-ui;text-align:center;margin-top:3em'>"
              "<h2>Rebooting...</h2></body></html>");
  delay(500);
  esp_restart();
}

#include <ESPmDNS.h>
#include <esp_wifi.h>


static void drawApClientLine() {
  M5Cardputer.Display.fillRect(22, 70, SCREEN_W - 22, 18, BLACK);
  int n = WiFi.softAPgetStationNum();
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(28, 70);
  if (n == 0) {
    M5Cardputer.Display.setTextColor(0xFFE0 /* yellow */, BLACK);
    M5Cardputer.Display.print("Waiting client");
  } else {
    M5Cardputer.Display.setTextColor(0x07E0 /* green */, BLACK);
    M5Cardputer.Display.printf("%d client%s", n, n > 1 ? "s" : "");
  }
}

void drawConfigScreen(const char* status, const String& ip) {
  M5Cardputer.Display.fillScreen(BLACK);

  const int barW = 22;
  const uint16_t barBg = 0x4810; /* dark purple RGB565 (72,0,128) #480080 */
  M5Cardputer.Display.fillRect(0, 0, barW, SCREEN_H, barBg);
  {
    LGFX_Sprite sprite(&M5Cardputer.Display);
    sprite.createSprite(80, 18);
    sprite.fillSprite(barBg);
    sprite.setTextSize(2);
    sprite.setTextColor(0xFFFF, barBg);
    sprite.setCursor(4, 1);
    sprite.print("CONFIG");
    sprite.setPivot(40, 9);
    M5Cardputer.Display.setPivot(barW / 2, SCREEN_H / 2);
    sprite.pushRotated(-90.0f);
    sprite.deleteSprite();
  }

  M5Cardputer.Display.setTextSize(2);
  if (g_apMode) {
    M5Cardputer.Display.setTextColor(0xFD20 /* orange */, BLACK);
    M5Cardputer.Display.setCursor(28, 4);
    M5Cardputer.Display.print("AP MODE");
  } else {
    bool connecting = (strstr(status, "Connect") != nullptr) && !g_staConnected;
    uint16_t color = g_staConnected ? 0x07E0 /* green */
                    : connecting   ? 0xFFE0 /* yellow */
                                   : 0xF800 /* red */;
    M5Cardputer.Display.setTextColor(color, BLACK);
    M5Cardputer.Display.setCursor(28, 4);
    char title[28];
    snprintf(title, sizeof(title), "STA %.13s%s",
             g_wifiSsid.c_str(),
             g_staConnected ? "" : (connecting ? "..." : " ?"));
    M5Cardputer.Display.print(title);
  }

  M5Cardputer.Display.setTextSize(2);
  if (g_apMode) {
    M5Cardputer.Display.setTextColor(0xFFE0 /* yellow */, BLACK);
    M5Cardputer.Display.setCursor(28, 24);
    M5Cardputer.Display.print(g_apSsidStr);  // CardPuter-KB-XXXX
    M5Cardputer.Display.setTextColor(0x07FF /* cyan */, BLACK);
    M5Cardputer.Display.setCursor(28, 46);
    M5Cardputer.Display.print(ip);            // 192.168.4.1
    drawApClientLine();
  } else {
    // STA: IP + mDNS hostname + .local
    M5Cardputer.Display.setTextColor(0x07FF /* cyan */, BLACK);
    M5Cardputer.Display.setCursor(28, 24);
    M5Cardputer.Display.print(ip);
    M5Cardputer.Display.setTextColor(0xFFE0 /* yellow */, BLACK);
    M5Cardputer.Display.setCursor(28, 46);
    M5Cardputer.Display.print(MDNS_NAME);
    M5Cardputer.Display.setCursor(28, 68);
    M5Cardputer.Display.print(".local");
  }

  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(0xFD20 /* orange */, BLACK);
  M5Cardputer.Display.setCursor(28, 95);
  M5Cardputer.Display.print(g_apMode ? "[A] Swap STA Mode" : "[A] Swap AP Mode");
  M5Cardputer.Display.setCursor(28, 115);
  M5Cardputer.Display.print("[Q] Exit to BLE");
}

void handleStatus() {
  int rssi = WiFi.RSSI();
  unsigned long uptime = millis() / 1000;
  int bat = M5Cardputer.Power.getBatteryLevel();
  const char* mode = g_apMode ? "config-ap" : "config-sta";
  const char* ssid = g_apMode ? g_apSsidStr : g_wifiSsid.c_str();
  const char* ip   = g_apMode ? g_apIp.c_str() : g_staIp.c_str();

  static char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"firmware\":\"%s\","
    "\"mode\":\"%s\",\"wifi_ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
    "\"mdns\":\"%s.local\",\"uptime_s\":%lu,\"battery\":%d,"
    "\"binding_count\":%u,\"stored_ssid\":\"%s\",\"heap_free\":%u}",
    FIRMWARE_VERSION,
    mode, ssid, ip, rssi, MDNS_NAME, uptime, bat,
    g_config.count, g_wifiSsid.c_str(), (unsigned)ESP.getFreeHeap());
  g_web->send(200, "application/json", String(buf));
}

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

void handleWifiScan() {
  int n = WiFi.scanNetworks(false, false, false, 300);
  if (n < 0) n = 0;
  String json = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (i > 0) json += ",";
    String s = WiFi.SSID(i);
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    json += "{\"ssid\":\"";
    json += s;
    json += "\",\"rssi\":";
    json += WiFi.RSSI(i);
    json += ",\"enc\":";
    json += (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false";
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();
  g_web->send(200, "application/json", json);
}

static void registerWebRoutes() {
  g_web->on("/",                HTTP_GET,  handleRoot);
  g_web->on("/save",            HTTP_POST, handleSave);
  g_web->on("/cancel",          HTTP_POST, handleCancel);
  g_web->on("/api/status",      HTTP_GET,  handleStatus);
  g_web->on("/api/config",      HTTP_GET,  handleApiConfig);
  g_web->on("/api/presets",     HTTP_GET,  handleApiPresets);
  g_web->on("/api/screenshot",  HTTP_GET,  handleScreenshot);
  g_web->on("/api/wifi/scan",   HTTP_GET,  handleWifiScan);
  g_web->on("/api/reboot",      HTTP_POST, handleCancel);
  g_web->on("/api/switch-to-ap",HTTP_POST, handleSwitchAp);
}

static void startApMode() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(g_apSsidStr, sizeof(g_apSsidStr), "CardPuter-KB-%02x%02x", mac[4], mac[5]);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_apSsidStr);
  delay(200);
  g_apIp = WiFi.softAPIP().toString();
  g_apMode = true;
  Serial.printf("[config] AP started: %s, ip=%s\n", g_apSsidStr, g_apIp.c_str());

  g_web = new WebServer(80);
  registerWebRoutes();
  g_web->begin();
  drawConfigScreen("AP active", g_apIp);
}

static bool tryStaConnect() {
  if (g_wifiSsid.length() == 0) return false;
  drawConfigScreen("Connecting...", g_wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_NAME);
  WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[config] STA connect failed");
    WiFi.disconnect(true);
    return false;
  }
  g_staConnected = true;
  g_staIp = WiFi.localIP().toString();
  Serial.printf("[config] STA connected, IP=%s\n", g_staIp.c_str());
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[config] mDNS: %s.local\n", MDNS_NAME);
  }
  g_web = new WebServer(80);
  registerWebRoutes();
  g_web->begin();
  drawConfigScreen("Connected", g_staIp);
  return true;
}

void setupConfigMode() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("[config] enter, stored ssid='%s'\n", g_wifiSsid.c_str());

  bool forceAp = false;
  {
    Preferences prefs;
    prefs.begin("kb", false);
    forceAp = prefs.getBool("force_ap", false);
    if (forceAp) prefs.remove("force_ap");
    prefs.end();
  }

  if (forceAp) {
    Serial.println("[config] force_ap flag set, going AP");
    startApMode();
  } else if (!tryStaConnect()) {
    startApMode();
  }
  g_configEnterTime = millis();
}

void handleSwitchAp() {
  Preferences prefs;
  prefs.begin("kb", false);
  prefs.putBool("force_ap", true);
  prefs.end();
  requestConfigBoot();
  g_web->send(200, "text/plain", "switching to AP, rebooting");
  delay(200);
  esp_restart();
}

void configModeLoop() {
  if (g_web) g_web->handleClient();
  if (g_staConnected && WiFi.status() != WL_CONNECTED) {
    delay(3000);
    if (WiFi.status() != WL_CONNECTED) esp_restart();
  }

  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto& keys = M5Cardputer.Keyboard.keysState();
    auto drawTransition = [](const char* msg, uint16_t color) {
      M5Cardputer.Display.fillScreen(BLACK);
      M5Cardputer.Display.setTextSize(3);
      M5Cardputer.Display.setTextColor(color, BLACK);
      int tw = strlen(msg) * FONT3_W;
      M5Cardputer.Display.setCursor((SCREEN_W - tw) / 2, (SCREEN_H - FONT3_H) / 2);
      M5Cardputer.Display.print(msg);
    };
    for (auto c : keys.word) {
      if (c == 'a' || c == 'A') {
        Preferences prefs;
        prefs.begin("kb", false);
        if (g_apMode) {
          prefs.remove("force_ap");
        } else {
          prefs.putBool("force_ap", true);
        }
        prefs.end();
        drawTransition(g_apMode ? "-> STA" : "-> AP", 0xFFE0);
        requestConfigBoot();
        delay(400);
        esp_restart();
      } else if (c == 'q' || c == 'Q') {
        drawTransition("-> BLE", 0x07FF);
        delay(400);
        esp_restart();
      }
    }
  }

  static unsigned long lastApRefresh = 0;
  static int lastStationNum = -1;
  if (g_apMode && millis() - lastApRefresh > 1000) {
    int n = WiFi.softAPgetStationNum();
    if (n != lastStationNum) {
      drawApClientLine();
      lastStationNum = n;
    }
    lastApRefresh = millis();
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
  {
    M5Cardputer.Display.setTextSize(3);
    M5Cardputer.Display.setTextColor(0xFFE0, BLACK);
    const char* msg = "Booting...";
    int tw = strlen(msg) * FONT3_W;
    M5Cardputer.Display.setCursor((SCREEN_W - tw) / 2, (SCREEN_H - FONT3_H) / 2);
    M5Cardputer.Display.print(msg);
  }
  g_canvas.setPsram(true);
  g_banner.setPsram(true);
  g_restore.setPsram(true);
  g_canvas.createSprite(SCREEN_W, SCREEN_H);
  g_banner.createSprite(FLASH_BANNER_W, FLASH_BANNER_H);
  g_restore.createSprite(FLASH_BANNER_W, FLASH_BANNER_H);
  Serial.printf("[boot] firmware: %s\n", FIRMWARE_VERSION);
  Serial.println("[boot] M5Cardputer ok");

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
  static char serialBuf[96];
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
      } else if (strncmp(serialBuf, "wifi ", 5) == 0) {
        char* sp = strchr(serialBuf + 5, ' ');
        if (sp) {
          *sp = 0;
          String ssid(serialBuf + 5);
          String pass(sp + 1);
          saveWifi(ssid, pass);
          Serial.printf("[trigger] saved wifi ssid='%s' (pass len=%d)\n", ssid.c_str(), pass.length());
        } else {
          Serial.println("[trigger] usage: wifi <ssid> <pass>");
        }
      } else if (strcmp(serialBuf, "wifi-clear") == 0) {
        saveWifi("", "");
        Serial.println("[trigger] cleared wifi creds");
      } else if (serialIdx > 0) {
        Serial.printf("[trigger] unknown cmd: %s\n", serialBuf);
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

  {
    auto& _ks = M5Cardputer.Keyboard.keysState();
    pollMod(&ctrlState,  TK_CTRL,  COL_KEY_FN,  _ks.ctrl);
    pollMod(&optState,   TK_OPT,   TFT_YELLOW,  _ks.opt);
    pollMod(&fnState,    TK_FN,    COL_KEY_ENT, _ks.fn);
    pollMod(&shiftState, TK_SHIFT, TFT_CYAN,    _ks.shift);
    pollMod(&altState,   TK_ALT,   TFT_MAGENTA, _ks.alt);

    unsigned long _now = millis();
    if (_ks.fn && !fnUsedAsModifier && fnPressStart > 0
        && (_now - fnPressStart >= FN_LONG_PRESS_MS)) {
      M5Cardputer.Display.fillScreen(TFT_YELLOW);
      M5Cardputer.Display.setTextSize(3);
      M5Cardputer.Display.setTextColor(BLACK);
      {
        const char* msg = "Config Mode";
        int tw = strlen(msg) * FONT3_W;
        M5Cardputer.Display.setCursor((SCREEN_W - tw) / 2, (SCREEN_H - FONT3_H) / 2);
        M5Cardputer.Display.print(msg);
      }
      delay(600);
      requestConfigBoot();
      esp_restart();
    }
  }

  if (M5Cardputer.Keyboard.isChange()) {
    // Any key activity wakes the screen
    if (M5Cardputer.Keyboard.isPressed()) screenWake();

    auto& keys = M5Cardputer.Keyboard.keysState();
    bool fnNow    = keys.fn;
    bool ctrlNow  = keys.ctrl;
    bool optNow   = keys.opt;
    bool shiftOn  = keys.shift;
    bool altNow   = keys.alt;

    unsigned long _now = millis();
    if (fnNow && !fnPrevHeld)         { fnUsedAsModifier = false; fnPressStart = _now; onModPress(&fnState, _now); }
    if (ctrlNow && !ctrlPrevHeld)     { ctrlUsedAsModifier = false; onModPress(&ctrlState, _now); }
    if (optNow && !optPrevHeld)       { optUsedAsModifier = false; onModPress(&optState, _now); }
    if (shiftOn && !shiftPrevHeld)    { shiftUsedAsModifier = false; onModPress(&shiftState, _now); }
    if (altNow && !altPrevHeld)       { altUsedAsModifier = false; onModPress(&altState, _now); }

    if (!fnNow && fnPrevHeld) {
      onModRelease(&fnState, _now, fnUsedAsModifier, TK_FN, COL_KEY_ENT);
      fnPressStart = 0;
      screenWake();
    }
    if (!ctrlNow && ctrlPrevHeld) {
      onModRelease(&ctrlState, _now, ctrlUsedAsModifier, TK_CTRL, COL_KEY_FN);
      screenWake();
    }
    if (!optNow && optPrevHeld) {
      onModRelease(&optState, _now, optUsedAsModifier, TK_OPT, TFT_YELLOW);
      screenWake();
    }
    if (!shiftOn && shiftPrevHeld) {
      onModRelease(&shiftState, _now, shiftUsedAsModifier, TK_SHIFT, TFT_CYAN);
      screenWake();
    }
    if (!altNow && altPrevHeld) {
      onModRelease(&altState, _now, altUsedAsModifier, TK_ALT, TFT_MAGENTA);
      screenWake();
    }
    fnPrevHeld    = fnNow;
    ctrlPrevHeld  = ctrlNow;
    optPrevHeld   = optNow;
    shiftPrevHeld = shiftOn;
    altPrevHeld   = altNow;

    // Which modifiers are active as BLE modifiers (held + other key)?
    bool modCtrl  = ctrlNow;  // Ctrl held → BLE Ctrl modifier
    bool modOpt   = optNow;   // Opt held  → BLE Alt/Option modifier
    bool modShift = shiftOn;  // Shift held → BLE Shift modifier
    bool modAlt   = altNow;

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
        if (ctrlNow)  ctrlUsedAsModifier  = true;
        if (optNow)   optUsedAsModifier   = true;
        if (shiftOn)  shiftUsedAsModifier = true;
        if (altNow)   altUsedAsModifier   = true;
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
          if (!dispatchKeyTap(c)) {
            bleKeyboard.write(c);
          }
        }
      }

      // Special keys via boolean flags.
      // Enter/del/tab are only in flags (not in keys.word).
      // Space is in BOTH flag and word per M5Cardputer lib; word loop skips it so this is the single source of truth.
      bool noLayer = !modCtrl && !modOpt && !fnNow;
      if (keys.enter) {
        if (!(noLayer && dispatchKeyTap('\n'))) SEND_WITH_MODS(KEY_RETURN);
        screenWake();
      }
      if (keys.del) {
        if (!(noLayer && dispatchKeyTap('\b'))) SEND_WITH_MODS(KEY_BACKSPACE);
        screenWake();
      }
      if (keys.space) {
        if (!(noLayer && dispatchKeyTap(' '))) SEND_WITH_MODS(' ');
        screenWake();
      }
      if (keys.tab) {
        if (!(noLayer && dispatchKeyTap('\t'))) SEND_WITH_MODS(KEY_TAB);
        screenWake();
      }
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
