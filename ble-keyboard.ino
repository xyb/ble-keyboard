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

// AK = ActionKey 枚举：一个 binding 的"主键"。0 = 不发任何键
enum AK : uint8_t {
  AK_NONE = 0,
  // 特殊键
  AK_ENTER, AK_TAB, AK_ESC, AK_BACKSPACE, AK_DELETE, AK_SPACE,
  AK_UP, AK_DOWN, AK_LEFT, AK_RIGHT,
  AK_PAGE_UP, AK_PAGE_DOWN, AK_HOME, AK_END,
  AK_CAPS_LOCK,
  // 功能键
  AK_F1, AK_F2, AK_F3, AK_F4, AK_F5, AK_F6,
  AK_F7, AK_F8, AK_F9, AK_F10, AK_F11, AK_F12,
  // 字母 a-z
  AK_A, AK_B, AK_C, AK_D, AK_E, AK_F, AK_G, AK_H, AK_I, AK_J,
  AK_K, AK_L, AK_M, AK_N, AK_O, AK_P, AK_Q, AK_R, AK_S, AK_T,
  AK_U, AK_V, AK_W, AK_X, AK_Y, AK_Z,
  // 数字 0-9
  AK_0, AK_1, AK_2, AK_3, AK_4, AK_5, AK_6, AK_7, AK_8, AK_9,
  // 常见符号
  AK_BACKTICK, AK_MINUS, AK_EQUAL, AK_LBRACKET, AK_RBRACKET, AK_BACKSLASH,
  AK_SEMICOLON, AK_QUOTE, AK_COMMA, AK_PERIOD, AK_SLASH,
};

// 触发类型
enum TriggerKind : uint8_t {
  TK_NONE = 0,
  TK_CTRL_TAP,    // Ctrl 单按
  TK_OPT_TAP,     // Opt 单按
  TK_FN_TAP,      // Fn 单按
  TK_KEY,         // 一个具体字符键（trigger_key 字段记录字符）
};

// 单条 binding
struct Binding {
  uint8_t trigger;       // TriggerKind
  uint8_t trigger_key;   // TK_KEY 时使用，存 ASCII 字符（如 '`' / 'j'）
  uint8_t cmd;
  uint8_t opt;
  uint8_t ctrl;
  uint8_t shift;
  uint8_t action;        // AK 枚举
};

constexpr int MAX_BINDINGS = 16;

struct KbConfig {
  Binding  bindings[MAX_BINDINGS];
  uint8_t  count;
};

// 预设 profile：LazyTyper + iTerm2（xyb 主力——语音输入 + iTerm2 tab 切换）
// 完整 13 条 binding，初始 NVS 空时直接可用。
const KbConfig PRESET_LAZYTYPER = {
  {
    {TK_CTRL_TAP,  0,  0, 1, 0, 0, AK_TAB},        // Ctrl→Opt+Tab（LazyTyper 听写）
    {TK_OPT_TAP,   0,  0, 0, 0, 0, AK_CAPS_LOCK},  // Opt→CapsLock
    {TK_FN_TAP,    0,  0, 0, 0, 0, AK_ENTER},      // Fn→Enter
    {TK_KEY,      '`', 0, 0, 0, 0, AK_ESC},        // `→Esc
    {TK_KEY,      '1', 1, 0, 0, 0, AK_1},          // 1→Cmd+1（iTerm2 tab 1）
    {TK_KEY,      '2', 1, 0, 0, 0, AK_2},
    {TK_KEY,      '3', 1, 0, 0, 0, AK_3},
    {TK_KEY,      '4', 1, 0, 0, 0, AK_4},
    {TK_KEY,      '5', 1, 0, 0, 0, AK_5},
    {TK_KEY,      '6', 1, 0, 0, 0, AK_6},
    {TK_KEY,      '7', 1, 0, 0, 0, AK_7},
    {TK_KEY,      '8', 1, 0, 0, 0, AK_8},
  },
  12
};

// 预设 profile：Wispr Flow（语音输入 app）。只重映射 modifier+backtick，
// 数字键如果用户已配置就保留（merge 加载，不会抹掉）。
const KbConfig PRESET_WISPRFLOW = {
  {
    {TK_CTRL_TAP,  0,  1, 0, 0, 1, AK_SEMICOLON},  // Ctrl→Cmd+Shift+;（macOS 听写默认热键，Wispr 也常用）
    {TK_OPT_TAP,   0,  0, 0, 0, 0, AK_CAPS_LOCK},  // Opt→CapsLock
    {TK_FN_TAP,    0,  0, 0, 0, 0, AK_ENTER},      // Fn→Enter
    {TK_KEY,      '`', 0, 0, 0, 0, AK_ESC},        // `→Esc
  },
  4
};

// 预设 profile：GhostType（AI 输入助手）。只重映射 modifier+backtick。
const KbConfig PRESET_GHOSTTYPE = {
  {
    {TK_CTRL_TAP,  0,  1, 1, 0, 0, AK_G},          // Ctrl→Cmd+Opt+G（GhostType 触发占位，按需调）
    {TK_OPT_TAP,   0,  0, 0, 0, 0, AK_CAPS_LOCK},  // Opt→CapsLock
    {TK_FN_TAP,    0,  0, 0, 0, 0, AK_ENTER},      // Fn→Enter
    {TK_KEY,      '`', 0, 0, 0, 0, AK_ESC},        // `→Esc
  },
  4
};

// 预设 profile：纯透传（所有键直接发字面字符，无映射）
const KbConfig PRESET_PASSTHROUGH = {
  {},
  0
};

const KbConfig& DEFAULT_PRESET = PRESET_LAZYTYPER;

struct PresetEntry { const char* name; const KbConfig* cfg; };
const PresetEntry PRESETS[] = {
  {"LazyTyper + iTerm2",  &PRESET_LAZYTYPER},
  {"Wispr Flow",          &PRESET_WISPRFLOW},
  {"GhostType",           &PRESET_GHOSTTYPE},
  {"纯透传（无映射）",      &PRESET_PASSTHROUGH},
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

// WiFi config 模式相关全局（提前到 loadConfig 之前，避免编译顺序问题）
String g_wifiSsid = "";
String g_wifiPass = "";
const char* MDNS_NAME = "cardputer-kb";  // 访问 http://cardputer-kb.local
const unsigned long WIFI_TIMEOUT_MS = 30000;  // 30 秒连不上就 fallback 到 AP
unsigned long g_configEnterTime = 0;
String g_staIp = "";
String g_apIp = "";
bool g_staConnected = false;
bool g_apMode = false;
char g_apSsidStr[32] = {0};

// 单条 binding 序列化为 7 字节（用字面值，避免 Arduino 自动 forward decl 看不到 constexpr）
#define BIND_BYTES 7
static void bindingPack(uint8_t* out, const Binding& b) {
  out[0] = b.trigger; out[1] = b.trigger_key;
  out[2] = b.cmd; out[3] = b.opt; out[4] = b.ctrl; out[5] = b.shift;
  out[6] = b.action;
}
static void bindingUnpack(const uint8_t* in, Binding& b) {
  b.trigger = in[0]; b.trigger_key = in[1];
  b.cmd = in[2] ? 1 : 0; b.opt = in[3] ? 1 : 0; b.ctrl = in[4] ? 1 : 0; b.shift = in[5] ? 1 : 0;
  b.action = in[6];
}

void loadConfig() {
  Preferences prefs;
  prefs.begin("kb", true);
  // 新 schema："binds" key 存 binary blob：1 字节 count + count*BIND_BYTES
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
  prefs.end();
  // ⚠️ 开发期默认值：NVS 空时 fallback 到家里的 WiFi。push 前要改成空字符串。
  if (g_wifiSsid.length() == 0) {
    g_wifiSsid = "YOUR_SSID";
    g_wifiPass = "YOUR_PASSWORD";
  }
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

// AK 枚举 → BleKeyboard library 的 HID 键值（非 char 的，return 0 = 字面字符走另外路径）
// 返回值：高位 0x80 = 字面字符（低 7 位是 ASCII），其它 = HID 键值
static uint16_t akToKey(uint8_t a) {
  switch (a) {
    case AK_ENTER:     return KEY_RETURN;
    case AK_TAB:       return KEY_TAB;
    case AK_ESC:       return KEY_ESC;
    case AK_BACKSPACE: return KEY_BACKSPACE;
    case AK_DELETE:    return KEY_DELETE;
    case AK_SPACE:     return 0x80 | ' ';
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
  // 字母 a-z
  if (a >= AK_A && a <= AK_Z) return 0x80 | ('a' + (a - AK_A));
  // 数字 0-9
  if (a >= AK_0 && a <= AK_9) return 0x80 | ('0' + (a - AK_0));
  // 符号
  switch (a) {
    case AK_BACKTICK:  return 0x80 | '`';
    case AK_MINUS:     return 0x80 | '-';
    case AK_EQUAL:     return 0x80 | '=';
    case AK_LBRACKET:  return 0x80 | '[';
    case AK_RBRACKET:  return 0x80 | ']';
    case AK_BACKSLASH: return 0x80 | '\\';
    case AK_SEMICOLON: return 0x80 | ';';
    case AK_QUOTE:     return 0x80 | '\'';
    case AK_COMMA:     return 0x80 | ',';
    case AK_PERIOD:    return 0x80 | '.';
    case AK_SLASH:     return 0x80 | '/';
  }
  return 0;
}

// 显示用的 binding 标签（"Cmd+Tab"、"Esc" 等），写入 buf
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
    case AK_NONE:      return "(无)";
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

// 通用执行器：按 binding 发出按键序列
static void executeBinding(const Binding& b, uint16_t flashColor) {
  // CapsLock 单按特殊处理
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

  uint16_t k = akToKey(b.action);
  if (k != 0) {
    uint8_t kk = (k & 0x80) ? (k & 0x7F) : (uint8_t)k;
    bleKeyboard.press(kk);
  }
  bleKeyboard.releaseAll();

  char label[32];
  formatBindingLabel(label, sizeof(label), b.cmd, b.opt, b.ctrl, b.shift,
                     b.action == AK_NONE ? "" : akName(b.action));
  showFlash(label, flashColor);
}

// 在 g_config.bindings 里查找指定 trigger 的 binding（找到第一个）
static const Binding* findBinding(uint8_t trigger, uint8_t trigger_key = 0) {
  for (int i = 0; i < g_config.count; i++) {
    const Binding& b = g_config.bindings[i];
    if (b.trigger != trigger) continue;
    if (trigger == TK_KEY && b.trigger_key != trigger_key) continue;
    return &b;
  }
  return nullptr;
}

void doCtrlTap() {
  const Binding* b = findBinding(TK_CTRL_TAP);
  if (b) executeBinding(*b, COL_KEY_FN);
}
void doOptTap() {
  const Binding* b = findBinding(TK_OPT_TAP);
  if (b) executeBinding(*b, TFT_YELLOW);
}
void doFnTap() {
  const Binding* b = findBinding(TK_FN_TAP);
  if (b) executeBinding(*b, COL_KEY_ENT);
}
// 任意字符键的处理（normal mode 没有别的 layer 干预时）
// 返回 true = 已被某 binding 消费；false = 走默认字面透传
bool dispatchKeyTap(char c) {
  const Binding* b = findBinding(TK_KEY, (uint8_t)c);
  if (b) {
    uint16_t color = (c == '`') ? COL_KEY_ESC :
                     (c >= '0' && c <= '9') ? COL_KEY_NUM : COL_KEY_NUM;
    executeBinding(*b, color);
    return true;
  }
  return false;
}

// AK 动作键的 dropdown 选项分组
struct AkOption { uint8_t ak; const char* label; };
static const AkOption AK_OPTIONS_SPECIAL[] = {
  {AK_NONE, "(无)"}, {AK_ENTER, "Enter"}, {AK_TAB, "Tab"}, {AK_ESC, "Esc"},
  {AK_BACKSPACE, "Backspace"}, {AK_DELETE, "Delete"}, {AK_SPACE, "Space"},
  {AK_UP, "↑ 上"}, {AK_DOWN, "↓ 下"}, {AK_LEFT, "← 左"}, {AK_RIGHT, "→ 右"},
  {AK_PAGE_UP, "PgUp"}, {AK_PAGE_DOWN, "PgDn"}, {AK_HOME, "Home"}, {AK_END, "End"},
  {AK_CAPS_LOCK, "Caps Lock"},
};
static const AkOption AK_OPTIONS_FN[] = {
  {AK_F1, "F1"}, {AK_F2, "F2"}, {AK_F3, "F3"}, {AK_F4, "F4"},
  {AK_F5, "F5"}, {AK_F6, "F6"}, {AK_F7, "F7"}, {AK_F8, "F8"},
  {AK_F9, "F9"}, {AK_F10, "F10"}, {AK_F11, "F11"}, {AK_F12, "F12"},
};

// 渲染 AK dropdown，optgroup 分组
static String htmlAkSelect(const char* name, uint8_t curr) {
  String s = "<select name='"; s += name; s += "'>";
  s += "<optgroup label='特殊键'>";
  for (auto& o : AK_OPTIONS_SPECIAL) {
    s += "<option value='"; s += o.ak; s += "'";
    if (o.ak == curr) s += " selected";
    s += ">"; s += o.label; s += "</option>";
  }
  s += "</optgroup><optgroup label='功能键'>";
  for (auto& o : AK_OPTIONS_FN) {
    s += "<option value='"; s += o.ak; s += "'";
    if (o.ak == curr) s += " selected";
    s += ">"; s += o.label; s += "</option>";
  }
  s += "</optgroup><optgroup label='字母 a-z'>";
  for (uint8_t a = AK_A; a <= AK_Z; a++) {
    s += "<option value='"; s += a; s += "'";
    if (a == curr) s += " selected";
    s += ">"; s += (char)('a' + (a - AK_A)); s += "</option>";
  }
  s += "</optgroup><optgroup label='数字 0-9'>";
  for (uint8_t a = AK_0; a <= AK_9; a++) {
    s += "<option value='"; s += a; s += "'";
    if (a == curr) s += " selected";
    s += ">"; s += (char)('0' + (a - AK_0)); s += "</option>";
  }
  s += "</optgroup><optgroup label='符号'>";
  for (uint8_t a = AK_BACKTICK; a <= AK_SLASH; a++) {
    s += "<option value='"; s += a; s += "'";
    if (a == curr) s += " selected";
    s += ">"; s += akName(a); s += "</option>";
  }
  s += "</optgroup></select>";
  return s;
}

// 把 binding 序列化成 JSON 对象（用于 GET /api/config 和 GET /api/presets）
static int bindingJsonInto(char* buf, int n, const Binding& b) {
  return snprintf(buf, n,
    "{\"trigger\":%u,\"key\":%u,\"cmd\":%u,\"opt\":%u,\"ctrl\":%u,\"shift\":%u,\"action\":%u}",
    b.trigger, b.trigger_key, b.cmd, b.opt, b.ctrl, b.shift, b.action);
}
static String configJson(const KbConfig& c) {
  String s = "[";
  char buf[128];
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
"function trigSelectHtml(curr,key){"
"let h=\"<select class='b-trig' onchange='trigChanged(this)'>\";"
"for(const v in TRIGGERS) h+=`<option value='${v}' ${v==curr?'selected':''}>${TRIGGERS[v]}</option>`;"
"h+=\"</select>\";"
"const ch=key?String.fromCharCode(key):\"\";"
"h+=`<input type='text' class='b-key' maxlength='1' value='${ch}' style='display:${curr==4?'inline':'none'}' placeholder='键'>`;"
"return h;"
"}"
"function trigChanged(sel){"
"const inp=sel.parentElement.querySelector('.b-key');"
"inp.style.display=sel.value=='4'?'inline':'none';"
"}"
"function rowHtml(b){"
"b=b||{trigger:1,key:0,cmd:0,opt:0,ctrl:0,shift:0,action:0};"
"return `<tr class='bind-row'>"
"<td><div class='trig-cell'>${trigSelectHtml(b.trigger,b.key)}</div></td>"
"<td class='col-mod'><input type='checkbox' class='b-cmd' ${b.cmd?'checked':''}></td>"
"<td class='col-mod'><input type='checkbox' class='b-opt' ${b.opt?'checked':''}></td>"
"<td class='col-mod'><input type='checkbox' class='b-ctrl' ${b.ctrl?'checked':''}></td>"
"<td class='col-mod'><input type='checkbox' class='b-shift' ${b.shift?'checked':''}></td>"
"<td class='col-act'>${akSelectHtml(b.action)}</td>"
"<td class='col-del'><button type='button' class='del' title='删除这条映射' onclick='this.closest(\"tr\").remove()'>🗑</button></td>"
"</tr>`;"
"}"
"function addRow(b){"
"const tbody=document.getElementById('binds');"
"const tmp=document.createElement('tbody');"
"tmp.innerHTML=rowHtml(b);"
"tbody.appendChild(tmp.firstElementChild);"
"}"
"function clearRows(){document.getElementById('binds').innerHTML=\"\"}"
"function loadBindings(arr){clearRows();arr.forEach(addRow)}"
"function collectBindings(){"
"const rows=document.querySelectorAll('.bind-row');"
"const out=[];"
"for(const r of rows){"
"const trig=parseInt(r.querySelector('.b-trig').value);"
"const keyInp=r.querySelector('.b-key');"
"const ka=r.querySelector('.b-act');"
"out.push({"
"trigger:trig,"
"key:trig==4?(keyInp.value.charCodeAt(0)||0):0,"
"cmd:r.querySelector('.b-cmd').checked?1:0,"
"opt:r.querySelector('.b-opt').checked?1:0,"
"ctrl:r.querySelector('.b-ctrl').checked?1:0,"
"shift:r.querySelector('.b-shift').checked?1:0,"
"action:ka?parseInt(ka.value):0"
"});"
"}"
"return out;"
"}"
"function bindingKey(b){return b.trigger==4?(b.trigger+':'+b.key):String(b.trigger);}"
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
"if(p.bindings.length===0){clearRows();return;}"
// 合并：preset 定义的 trigger 覆盖现有，未定义的保留
"const merged=mergeBindings(collectBindings(),p.bindings);"
"loadBindings(merged);"
"}"
"async function init(){"
"const r=await fetch('/api/config');const cfg=await r.json();"
"loadBindings(cfg.bindings);"
"const pr=await fetch('/api/presets');const presets=await pr.json();"
"const sel=document.getElementById('preset');"
"for(const p of presets){"
"const o=document.createElement('option');o.value=p.name;o.textContent=p.name;sel.appendChild(o);"
"}"
"}"
"async function saveAll(){"
"const data={"
"ssid:document.getElementById('ssid').value,"
"pass:document.getElementById('pass').value,"
"bindings:collectBindings()"
"};"
"const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});"
"document.body.innerHTML=\"<h2 style='text-align:center;margin-top:3em'>已保存，正在重启…</h2>\";"
"}"
"async function cancelAll(){"
"await fetch('/cancel',{method:'POST'});"
"document.body.innerHTML=\"<h2 style='text-align:center;margin-top:3em'>正在重启…</h2>\";"
"}"
"async function scanWifi(){"
"const el=document.getElementById('wifi-list');el.textContent='扫描中…';"
"try{const r=await fetch('/api/wifi/scan');const nets=await r.json();"
"if(!nets.length){el.textContent='没找到 WiFi';return}"
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
"}catch(e){el.textContent='扫描失败：'+e}"
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
  body += ".banner{padding:.7em 1em;border-radius:6px;margin-bottom:1em;font-size:.95em}";
  body += ".banner-sta{background:#dcfce7;color:#166534;border:1px solid #86efac}";
  body += ".banner-ap{background:#fef3c7;color:#854d0e;border:1px solid #fcd34d}";
  // 用 <table> 让 header 和 row 列宽自动对齐
  body += "table.bind-table{width:100%;border-collapse:collapse;margin-top:.5em}";
  body += "table.bind-table th{font-size:.8em;color:#666;font-weight:600;padding:.4em .2em;border-bottom:1px solid #ccc;text-align:center}";
  body += "table.bind-table th:first-child{text-align:left}";
  body += "table.bind-table td{padding:.4em .2em;border-bottom:1px solid #eee;text-align:center;vertical-align:middle}";
  body += "table.bind-table td:first-child{text-align:left}";
  body += "table.bind-table th.col-mod,table.bind-table td.col-mod{width:2.6em}";
  body += "table.bind-table th.col-act,table.bind-table td.col-act{width:9em}";
  body += "table.bind-table th.col-del,table.bind-table td.col-del{width:2.4em}";
  body += "table.bind-table input[type=checkbox]{transform:scale(1.2);margin:0}";
  body += "table.bind-table select,table.bind-table input[type=text]{font-size:.9em;padding:.3em;width:100%;box-sizing:border-box}";
  body += "table.bind-table .trig-cell{display:flex;gap:.3em;align-items:center}";
  body += "table.bind-table .trig-cell select{flex:1;min-width:0}";
  body += "table.bind-table .trig-cell input.b-key{width:2.5em;flex-shrink:0}";
  body += "table.bind-table .del{background:none;border:0;cursor:pointer;padding:.2em;font-size:1.1em;line-height:1}";
  body += "table.bind-table .del:hover{background:#fee;border-radius:4px}";
  body += ".add-btn{background:#10b981;color:#fff;border:0;border-radius:4px;padding:.5em 1em;margin-top:.5em;cursor:pointer;font-size:.95em;width:auto}";
  body += "</style></head><body>";
  body += "<h2>CardPuter Keyboard 配置</h2>";

  // 顶部状态横幅占位（下方 body.replace 填入）
  body += "[BANNER]";

  body += "<fieldset><legend>预设方案（一键载入到下方表单）</legend>";
  body += "<select id='preset' onchange='loadPreset(this.value)'>";
  body += "<option value=''>-- 选择预设 --</option>";
  body += "</select>";
  body += "</fieldset>";

  body += "<fieldset><legend>WiFi（连家庭网络）</legend>";
  body += "<label>SSID</label><input id='ssid' value='";
  body += g_wifiSsid;
  body += "'>";
  body += "<label>密码</label><input id='pass' type='password' placeholder='";
  body += g_wifiPass.length() > 0 ? "(已存，留空保留)" : "";
  body += "'>";
  body += "<button type='button' class='scan' onclick='scanWifi()'>扫描附近 WiFi</button>";
  body += "<div id='wifi-list'></div>";
  body += "</fieldset>";

  body += "<fieldset><legend>键映射（每行 = 一条触发→动作）</legend>";
  body += "<table class='bind-table'><thead><tr>";
  body += "<th>触发</th>";
  body += "<th class='col-mod'>Cmd</th><th class='col-mod'>Opt</th><th class='col-mod'>Ctrl</th><th class='col-mod'>Shift</th>";
  body += "<th class='col-act'>主键</th>";
  body += "<th class='col-del'></th>";
  body += "</tr></thead><tbody id='binds'></tbody></table>";
  body += "<button type='button' class='add-btn' onclick='addRow()'>+ 增加映射</button>";
  body += "</fieldset>";

  body += "<button class='save' type='button' onclick='saveAll()'>保存并重启</button>";
  body += "<button class='reboot' type='button' onclick='cancelAll()'>放弃 / 直接重启</button>";

  // banner
  String banner;
  if (g_apMode) {
    banner = "<div class='banner banner-ap'>当前 <b>AP 模式</b>。在下面填家庭 WiFi 保存后会切到 STA 模式，AP 会消失。</div>";
  } else {
    banner = "<div class='banner banner-sta'>当前 <b>STA 模式</b>，已连 <b>" + g_wifiSsid +
             "</b>（IP <code>" + g_staIp + "</code>，RSSI " + String(WiFi.RSSI()) + " dBm）。</div>";
  }
  body.replace("[BANNER]", banner);

  // ===== JS =====
  body += "<script>";

  // AK 主键 dropdown 的选项数据（C++ 序列化成 JS 数组）
  body += "const AK_OPTS=[";
  body += "{g:'特殊键',o:[";
  for (size_t i = 0; i < sizeof(AK_OPTIONS_SPECIAL)/sizeof(AK_OPTIONS_SPECIAL[0]); i++) {
    if (i) body += ",";
    body += "[" + String(AK_OPTIONS_SPECIAL[i].ak) + ",'" + AK_OPTIONS_SPECIAL[i].label + "']";
  }
  body += "]},";
  body += "{g:'功能键',o:[";
  for (size_t i = 0; i < sizeof(AK_OPTIONS_FN)/sizeof(AK_OPTIONS_FN[0]); i++) {
    if (i) body += ",";
    body += "[" + String(AK_OPTIONS_FN[i].ak) + ",'" + AK_OPTIONS_FN[i].label + "']";
  }
  body += "]},";
  body += "{g:'字母 a-z',o:[";
  for (uint8_t a = AK_A; a <= AK_Z; a++) {
    if (a > AK_A) body += ",";
    body += "[" + String(a) + ",'" + (char)('a' + (a - AK_A)) + "']";
  }
  body += "]},";
  body += "{g:'数字 0-9',o:[";
  for (uint8_t a = AK_0; a <= AK_9; a++) {
    if (a > AK_0) body += ",";
    body += "[" + String(a) + ",'" + (char)('0' + (a - AK_0)) + "']";
  }
  body += "]},";
  body += "{g:'符号',o:[";
  for (uint8_t a = AK_BACKTICK; a <= AK_SLASH; a++) {
    if (a > AK_BACKTICK) body += ",";
    // 用 JSON 风格双引号字符串 + 转义，反斜杠/单引号才不会破 JS
    const char* nm = akName(a);
    String esc;
    for (const char* p = nm; *p; p++) {
      if (*p == '\\' || *p == '"') esc += '\\';
      esc += *p;
    }
    body += "[" + String(a) + ",\"" + esc + "\"]";
  }
  body += "]}];";

  body += "const TRIGGERS={1:'Ctrl 单按',2:'Opt 单按',3:'Fn 单按',4:'特定键'};";

  // 渲染一行 binding（addRow 调用，row 是 binding 对象）
  body += JS_CODE;
  body += "</script></body></html>";
  g_web->send(200, "text/html; charset=utf-8", body);
}

// /api/config — 当前配置 JSON
void handleApiConfig() {
  String s = "{\"bindings\":";
  s += configJson(g_config);
  s += "}";
  g_web->send(200, "application/json", s);
}

// /api/presets — 内置预设列表
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

// 极简 JSON 解析 helper：找 "key":数字 / "key":"值"
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
  // POST body 必为 JSON {ssid, pass, bindings:[...]}
  String body = g_web->arg("plain");
  if (body.length() == 0) {
    g_web->send(400, "text/plain", "expected JSON body"); return;
  }

  // 解析 bindings 数组
  int bArrStart = body.indexOf("\"bindings\":[");
  if (bArrStart < 0) {
    g_web->send(400, "text/plain", "missing bindings"); return;
  }
  int p = bArrStart + 12;  // 跳过 "bindings":[
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
    b.cmd         = jsonFindInt(obj, "cmd", 0)   ? 1 : 0;
    b.opt         = jsonFindInt(obj, "opt", 0)   ? 1 : 0;
    b.ctrl        = jsonFindInt(obj, "ctrl", 0)  ? 1 : 0;
    b.shift       = jsonFindInt(obj, "shift", 0) ? 1 : 0;
    b.action      = jsonFindInt(obj, "action", 0);
    if (b.trigger != TK_NONE) count++;
    p = objEnd + 1;
    if (body[p] == ']') break;
  }
  g_config.count = count;
  saveConfig();

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
              "<h2>正在重启…</h2></body></html>");
  delay(500);
  esp_restart();
}

// ===== 配置模式：STA 连家庭 WiFi（写死，仅试验用）=====
#include <ESPmDNS.h>

// 这些全局在 loadConfig 之前已声明（forward 顺序需要）

void drawConfigScreen(const char* status, const String& ip) {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(8, 4);
  M5Cardputer.Display.print(g_apMode ? "AP Setup" : "WiFi Config");

  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(8, 32);
  M5Cardputer.Display.print("SSID:   ");
  M5Cardputer.Display.println(g_apMode ? g_apSsidStr : g_wifiSsid.c_str());
  M5Cardputer.Display.setCursor(8, 48);
  M5Cardputer.Display.print("Status: ");
  M5Cardputer.Display.println(status);
  M5Cardputer.Display.setCursor(8, 64);
  M5Cardputer.Display.print("IP:     ");
  M5Cardputer.Display.println(ip);
  if (!g_apMode) {
    M5Cardputer.Display.setCursor(8, 80);
    M5Cardputer.Display.print("mDNS:   http://");
    M5Cardputer.Display.print(MDNS_NAME);
    M5Cardputer.Display.println(".local");
  } else {
    M5Cardputer.Display.setCursor(8, 80);
    M5Cardputer.Display.setTextColor(0xC618, BLACK);
    M5Cardputer.Display.println("Connect to AP, then");
    M5Cardputer.Display.setCursor(8, 92);
    M5Cardputer.Display.print("open http://");
    M5Cardputer.Display.println(ip);
  }

  M5Cardputer.Display.setCursor(8, 115);
  M5Cardputer.Display.setTextColor(0xFD20, BLACK);
  M5Cardputer.Display.println("Reboot to exit");
}

// /api/status — JSON 健康状态
void handleStatus() {
  int rssi = WiFi.RSSI();
  unsigned long uptime = millis() / 1000;
  int bat = M5Cardputer.Power.getBatteryLevel();
  const char* mode = g_apMode ? "config-ap" : "config-sta";
  const char* ssid = g_apMode ? g_apSsidStr : g_wifiSsid.c_str();
  const char* ip   = g_apMode ? g_apIp.c_str() : g_staIp.c_str();

  static char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"mode\":\"%s\",\"wifi_ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
    "\"mdns\":\"%s.local\",\"uptime_s\":%lu,\"battery\":%d,"
    "\"binding_count\":%u,\"stored_ssid\":\"%s\",\"heap_free\":%u}",
    mode, ssid, ip, rssi, MDNS_NAME, uptime, bat,
    g_config.count, g_wifiSsid.c_str(), (unsigned)ESP.getFreeHeap());
  g_web->send(200, "application/json", String(buf));
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

// /api/wifi/scan — 列附近 WiFi（参考 audio-recorder 实现）
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
}

static void startApMode() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(g_apSsidStr, sizeof(g_apSsidStr), "CardPuter-KB-CFG-%02X%02X", mac[4], mac[5]);

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

  // 有 stored creds → 试 STA；空或失败 → AP fallback
  if (!tryStaConnect()) {
    startApMode();
  }
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
  // 开发期备用串口命令：
  //   "config\n"           → 进配置模式
  //   "wifi <ssid> <pass>" → 写 WiFi creds 到 NVS（不重启）
  //   "wifi-clear\n"       → 清掉 NVS 里的 ssid/pass
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
          // Normal mode：按 binding 列表查映射，没找到就字面透传
          if (!dispatchKeyTap(c)) {
            bleKeyboard.write(c);
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
