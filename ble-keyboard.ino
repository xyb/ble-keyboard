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

BleKeyboard bleKeyboard(DEVICE_NAME, "M5Stack", 100);
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

const unsigned long BAT_INTERVAL_ACTIVE = 30000;
const unsigned long BAT_INTERVAL_IDLE = 300000;
const unsigned long SCREEN_TIMEOUT = 5000;
const unsigned long POWEROFF_TIMEOUT = 1800000;
const int LOWBAT_THRESHOLD = 5;
const int CHARGING_DROP_MARGIN = 2;
const int BUZZER_FREQ = 2500;
const int BUZZER_PIN = 2;
const bool BUZZER_TEST_ON_BOOT = false;
const unsigned long LED_BLINK_INTERVAL = 10000;
const unsigned long LED_BREATH_DURATION = 1000;
const int LED_BREATH_PEAK = 240;
const int LED_PIN = 10;

#if IS_CARDPUTER
bool fnPrevHeld = false;
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
    M5Cardputer.Display.setBrightness(80);
#else
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(80);
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

#define COL_KEY_NUM  0x07FF  // cyan
#define COL_KEY_FN   0xF81F  // magenta
#define COL_KEY_ENT  0xFFE0  // yellow
#define COL_KEY_ESC  0xFD20  // orange

void drawKeyBlock(int x, int y, int w, int h, uint16_t bg, const char* label, const char* action) {
  M5Cardputer.Display.fillRoundRect(x, y, w, h, 3, bg);
  M5Cardputer.Display.setTextColor(BLACK);
  // Label: size 2, auto-shrink if too wide
  int labelLen = strlen(label);
  if (labelLen * 12 <= w - 4) {
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setCursor(x + (w - labelLen * 12) / 2, y + 2);
  } else {
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(x + (w - labelLen * 6) / 2, y + 6);
  }
  M5Cardputer.Display.print(label);
  // Action: size 1.5 if fits, else size 1
  int actionLen = strlen(action);
  if (actionLen * 9 <= w - 2) {
    M5Cardputer.Display.setTextSize(1.5);
    M5Cardputer.Display.setCursor(x + (w - actionLen * 9) / 2, y + h - 14);
  } else {
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(x + (w - actionLen * 6) / 2, y + h - 10);
  }
  M5Cardputer.Display.print(action);
}

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);
  if (!screenOn) return;

  // Top-right: BLE dot + battery percentage
  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  int tw = strlen(buf) * 6;
  int dotR = 4;
  int dotX = SCREEN_W - tw - dotR - 10;
  // Clear area for dot + percentage
  M5Cardputer.Display.fillRect(dotX - dotR - 2, 2, SCREEN_W - dotX + dotR + 2, 12, BLACK);
  // BLE status dot
  uint16_t bleCol = connected ? GREEN : RED;
  M5Cardputer.Display.fillCircle(dotX, 8, dotR, bleCol);
  // Battery percentage
  M5Cardputer.Display.setTextSize(1);
  if (pct > 50)      M5Cardputer.Display.setTextColor(GREEN);
  else if (pct > 20) M5Cardputer.Display.setTextColor(YELLOW);
  else               M5Cardputer.Display.setTextColor(RED);
  M5Cardputer.Display.setCursor(dotX + dotR + 4, 4);
  M5Cardputer.Display.print(buf);
}

void drawStatus() {
  if (!screenOn) return;
  M5Cardputer.Display.fillScreen(BLACK);

  // Top bar: device name (left) + BLE dot (right)
  // Top bar: device name (left) + BLE dot + battery (right via updateBattery)
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(DARKGREY);
  M5Cardputer.Display.setCursor(4, 4);
  M5Cardputer.Display.print(fullDeviceName);
  updateBattery();

  if (!connected) {
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(YELLOW);
    M5Cardputer.Display.setCursor(4, 50);
    M5Cardputer.Display.print("Waiting...");
    updateBattery();
    return;
  }

  // Row 1: 1-4   Row 2: 5-8   Row 3: Fn, Tab, Enter, Esc
  // Full height available: 135 - 22(title) = 113, 3 rows + 2 gaps
  int y1 = 22, bh = 35, gap = 3, numW = 57;
  const char* nums[] = {"1","2","3","4","5","6","7","8"};
  for (int i = 0; i < 4; i++) {
    char action[8];
    snprintf(action, sizeof(action), "CMD+%s", nums[i]);
    drawKeyBlock(4 + i * (numW + gap), y1, numW, bh, COL_KEY_NUM, nums[i], action);
  }
  int y2 = y1 + bh + gap;
  for (int i = 4; i < 8; i++) {
    char action[8];
    snprintf(action, sizeof(action), "CMD+%s", nums[i]);
    drawKeyBlock(4 + (i - 4) * (numW + gap), y2, numW, bh, COL_KEY_NUM, nums[i], action);
  }
  int y3 = y2 + bh + gap;
  int w3 = 57;
  drawKeyBlock(4, y3, w3, bh, COL_KEY_FN, "Ctrl", "OPT+TAB");
  drawKeyBlock(4 + w3 + gap, y3, w3, bh, COL_KEY_ENT, "Fn", "ENTER");
  drawKeyBlock(4 + (w3 + gap) * 2, y3, w3, bh, COL_KEY_ENT, "Enter", "ENTER");
  drawKeyBlock(4 + (w3 + gap) * 3, y3, w3, bh, COL_KEY_ESC, "`", "ESC");

  updateBattery();
}

void showFlash(const char* text, uint16_t color) {
  if (!screenOn) {
    M5Cardputer.Display.wakeup();
    M5Cardputer.Display.setBrightness(80);
    screenOn = true;
  }
  M5Cardputer.Display.fillScreen(color);
  M5Cardputer.Display.setTextSize(3);
  M5Cardputer.Display.setTextColor(BLACK);
  int tw = strlen(text) * 18;
  M5Cardputer.Display.setCursor((SCREEN_W - tw) / 2, (SCREEN_H - 24) / 2);
  M5Cardputer.Display.print(text);
  delay(200);
  drawStatus();
  lastActivity = millis();
}

#elif defined(ARDUINO_M5STACK_STICKC_PLUS)
// ----- M5StickC Plus: portrait 135x240, USB end UP -----
// Physical buttons:
//   Button A: front face, top       → full circle, top center
//   Power:    right side edge, top  → left half-circle, right edge (flush)
//   Button B: left side edge, mid   → right half-circle, left edge (flush)

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);
  if (!screenOn) return;

  // Device name
  int nameY = 210;
  M5.Lcd.fillRect(0, nameY, 135, 10, BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(DARKGREY);
  int nameW = strlen(fullDeviceName) * 6;
  M5.Lcd.setCursor((135 - nameW) / 2, nameY + 1);
  M5.Lcd.print(fullDeviceName);

  int batY = 222;
  M5.Lcd.fillRect(0, batY, 135, 18, BLACK);

  // BLE status dot
  uint16_t bleCol = connected ? GREEN : RED;
  M5.Lcd.fillCircle(10, batY + 8, 4, bleCol);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(DARKGREY);
  M5.Lcd.setCursor(18, batY + 5);
  M5.Lcd.print("BLE");

  // Battery percentage
  uint16_t batCol = pct > 50 ? GREEN : (pct > 20 ? YELLOW : RED);
  M5.Lcd.setTextColor(batCol);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  int tw = strlen(buf) * 6;
  M5.Lcd.setCursor(135 - tw - 4, batY + 5);
  M5.Lcd.print(buf);

  // Battery bar
  int barX = 44, barW = 135 - tw - barX - 8, barH = 8;
  M5.Lcd.drawRect(barX, batY + 4, barW, barH, DARKGREY);
  M5.Lcd.fillRect(barX + 1, batY + 5, barW - 2, barH - 2, BLACK);
  int fillW = (barW - 2) * pct / 100;
  if (fillW > 0) M5.Lcd.fillRect(barX + 1, batY + 5, fillW, barH - 2, batCol);
}

void drawStatus() {
  if (!screenOn) return;
  M5.Lcd.fillScreen(BLACK);

  if (!connected) {
    M5.Lcd.setTextSize(3);
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(12, 60);
    M5.Lcd.print("BLE KB");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(DARKGREY);
    M5.Lcd.setCursor(12, 100);
    M5.Lcd.print("Waiting for pair...");
    updateBattery();
    return;
  }

  // Layout constants
  int W = 135;
  int blockX = 16, blockW = 100;
  int blockH = 48, gap = 6;
  int r = 8;
  int blockRight = blockX + blockW;         // 116
  int lineX = blockRight + (W - blockRight) / 2;  // center of right margin

  // === Button A: full circle, top center ===
  int dotA_y = r + 2;
  M5.Lcd.fillCircle(W / 2, dotA_y, r, CYAN);

  // === Power: left half-circle, flush with right edge ===
  M5.Lcd.fillCircle(W - 1, dotA_y, r, MAGENTA);

  // Line from A dot down to block A
  int blockA_y = dotA_y + r + 6;
  M5.Lcd.drawLine(W / 2, dotA_y + r, W / 2, blockA_y, CYAN);

  // Block A
  M5.Lcd.fillRoundRect(blockX, blockA_y, blockW, blockH, 4, CYAN);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(blockX + (blockW - 12) / 2, blockA_y + 4);
  M5.Lcd.print("A");
  M5.Lcd.setCursor(blockX + (blockW - 84) / 2, blockA_y + 26);
  M5.Lcd.print("OPT+TAB");

  // === Button B: right half-circle, flush with left edge ===
  int blockB_y = blockA_y + blockH + gap;
  int blockB_mid = blockB_y + blockH / 2;
  M5.Lcd.fillCircle(0, blockB_mid, r, DARKGREY);
  M5.Lcd.drawLine(r, blockB_mid, blockX, blockB_mid, DARKGREY);

  // Block B
  M5.Lcd.fillRoundRect(blockX, blockB_y, blockW, blockH, 4, 0x4208);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(blockX + (blockW - 12) / 2, blockB_y + 4);
  M5.Lcd.print("B");
  M5.Lcd.setCursor(blockX + (blockW - 48) / 2, blockB_y + 26);
  M5.Lcd.print("WAKE");

  // === Power line: half-circle → diagonal → vertical → diagonal → block ===
  int blockP_y = blockB_y + blockH + gap;

  // Start from bottom-left edge of half-circle (on the circle perimeter)
  // Circle center (W-1, dotA_y), r. Pick point at ~240° on circle.
  int diagStartX = W - 1 - r / 2;        // midpoint of visible half-circle
  int diagStartY = dotA_y + r - 1;        // near bottom edge of circle
  // Diagonal down to vertical line x
  int vertStartY = diagStartY + (diagStartX - lineX);
  M5.Lcd.drawLine(diagStartX, diagStartY, lineX, vertStartY, MAGENTA);
  // Vertical down
  int vertEndY = blockP_y + blockH / 4;
  M5.Lcd.drawLine(lineX, vertStartY, lineX, vertEndY, MAGENTA);
  // Diagonal into block right edge
  M5.Lcd.drawLine(lineX, vertEndY, blockRight, blockP_y + blockH / 2, MAGENTA);

  // Block PWR
  M5.Lcd.fillRoundRect(blockX, blockP_y, blockW, blockH, 4, MAGENTA);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(blockX + (blockW - 36) / 2, blockP_y + 4);
  M5.Lcd.print("PWR");
  M5.Lcd.setCursor(blockX + (blockW - 60) / 2, blockP_y + 26);
  M5.Lcd.print("ENTER");

  updateBattery();
}

void flashPower() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(80);
    screenOn = true;
  }
  M5.Lcd.fillScreen(MAGENTA);
  M5.Lcd.setTextSize(4);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor((135 - 120) / 2, 100);
  M5.Lcd.print("ENTER");
  delay(300);
  drawStatus();
  lastActivity = millis();
}

void flashBtnA() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(80);
    screenOn = true;
  }
  M5.Lcd.fillScreen(CYAN);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setCursor((135 - 126) / 2, 100);
  M5.Lcd.print("OPT+TAB");
  delay(500);
  drawStatus();
  lastActivity = millis();
}

#else
// ----- M5StickC: portrait 80x160, rotation 2, USB end UP -----
// Same physical layout as Plus, smaller screen

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);
  if (!screenOn) return;

  // Device name
  int nameY = 138;
  M5.Lcd.fillRect(0, nameY, 80, 10, BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(DARKGREY);
  int nameW = strlen(fullDeviceName) * 6;
  M5.Lcd.setCursor((80 - nameW) / 2, nameY + 1);
  M5.Lcd.print(fullDeviceName);

  int batY = 150;
  M5.Lcd.fillRect(0, batY, 80, 12, BLACK);

  uint16_t bleCol = connected ? GREEN : RED;
  M5.Lcd.fillCircle(6, batY + 5, 3, bleCol);

  uint16_t batCol = pct > 50 ? GREEN : (pct > 20 ? YELLOW : RED);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(batCol);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  int tw = strlen(buf) * 6;
  M5.Lcd.setCursor(80 - tw - 2, batY + 2);
  M5.Lcd.print(buf);

  int barX = 14, barW = 80 - tw - barX - 6, barH = 6;
  M5.Lcd.drawRect(barX, batY + 3, barW, barH, DARKGREY);
  M5.Lcd.fillRect(barX + 1, batY + 4, barW - 2, barH - 2, BLACK);
  int fillW = (barW - 2) * pct / 100;
  if (fillW > 0) M5.Lcd.fillRect(barX + 1, batY + 4, fillW, barH - 2, batCol);
}

void drawStatus() {
  if (!screenOn) return;
  M5.Lcd.fillScreen(BLACK);

  if (!connected) {
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor((80 - 72) / 2, 40);
    M5.Lcd.print("BLE KB");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(DARKGREY);
    M5.Lcd.setCursor((80 - 60) / 2, 70);
    M5.Lcd.print("Waiting...");
    updateBattery();
    return;
  }

  int W = 80;
  int blockX = 10, blockW = 60;
  int blockH = 32, gap = 4;
  int r = 6;
  int blockRight = blockX + blockW;
  int lineX = blockRight + (W - blockRight) / 2;

  // Button A: full circle, top center
  int dotA_y = r + 2;
  M5.Lcd.fillCircle(W / 2, dotA_y, r, CYAN);

  // Power: left half-circle, flush with right edge
  M5.Lcd.fillCircle(W - 1, dotA_y, r, MAGENTA);

  // Line from A dot to block A
  int blockA_y = dotA_y + r + 4;
  M5.Lcd.drawLine(W / 2, dotA_y + r, W / 2, blockA_y, CYAN);

  // Block A
  M5.Lcd.fillRoundRect(blockX, blockA_y, blockW, blockH, 3, CYAN);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(blockX + (blockW - 12) / 2, blockA_y + 2);
  M5.Lcd.print("A");
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(blockX + (blockW - 42) / 2, blockA_y + 21);
  M5.Lcd.print("OPT+TAB");

  // Button B: right half-circle, flush with left edge
  int blockB_y = blockA_y + blockH + gap;
  int blockB_mid = blockB_y + blockH / 2;
  M5.Lcd.fillCircle(0, blockB_mid, r, DARKGREY);
  M5.Lcd.drawLine(r, blockB_mid, blockX, blockB_mid, DARKGREY);

  // Block B
  M5.Lcd.fillRoundRect(blockX, blockB_y, blockW, blockH, 3, 0x4208);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(blockX + (blockW - 12) / 2, blockB_y + 2);
  M5.Lcd.print("B");
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(blockX + (blockW - 24) / 2, blockB_y + 21);
  M5.Lcd.print("WAKE");

  // Power line: half-circle → diagonal → vertical → diagonal → block
  int blockP_y = blockB_y + blockH + gap;
  int diagStartX = W - 1 - r / 2;
  int diagStartY = dotA_y + r - 1;
  int vertStartY = diagStartY + (diagStartX - lineX);
  M5.Lcd.drawLine(diagStartX, diagStartY, lineX, vertStartY, MAGENTA);
  int vertEndY = blockP_y + blockH / 4;
  M5.Lcd.drawLine(lineX, vertStartY, lineX, vertEndY, MAGENTA);
  M5.Lcd.drawLine(lineX, vertEndY, blockRight, blockP_y + blockH / 2, MAGENTA);

  // Block PWR
  M5.Lcd.fillRoundRect(blockX, blockP_y, blockW, blockH, 3, MAGENTA);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(blockX + (blockW - 18) / 2, blockP_y + 2);
  M5.Lcd.print("PWR");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(blockX + (blockW - 12) / 2, blockP_y + 14);
  M5.Lcd.print("OK");

  updateBattery();
}

void flashPower() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(80);
    screenOn = true;
  }
  M5.Lcd.fillScreen(MAGENTA);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor((80 - 60) / 2, 68);
  M5.Lcd.print("ENTER");
  delay(300);
  drawStatus();
  lastActivity = millis();
}

void flashBtnA() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(80);
    screenOn = true;
  }
  M5.Lcd.fillScreen(CYAN);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setCursor((80 - 84) / 2, 68);
  M5.Lcd.print("OPT+TAB");
  delay(500);
  drawStatus();
  lastActivity = millis();
}

#endif

// ============================================================
//  Setup & Loop — shared
// ============================================================

void setup() {
#if IS_CARDPUTER
  auto cfg = M5.config();
  M5Cardputer.begin(cfg);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(80);
  M5Cardputer.Display.fillScreen(BLACK);
#else
  M5.begin();
  M5.Axp.ScreenBreath(80);
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

  // Auto screen off
  if (screenOn && (millis() - lastActivity >= SCREEN_TIMEOUT)) {
    screenSleep();
  }

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

    // Fn → Enter (on press)
    bool fnNow = keys.fn;
    if (fnNow && !fnPrevHeld) {
      bleKeyboard.write(KEY_RETURN);
      showFlash("Enter", COL_KEY_ENT);
    }
    fnPrevHeld = fnNow;

    // Ctrl → Opt+Tab (voice input, on press)
    static bool ctrlPrev = false;
    if (keys.ctrl && !ctrlPrev) {
      bleKeyboard.press(KEY_LEFT_ALT);
      bleKeyboard.press(KEY_TAB);
      bleKeyboard.releaseAll();
      showFlash("Opt+Tab", COL_KEY_FN);
    }
    ctrlPrev = keys.ctrl;

    if (M5Cardputer.Keyboard.isPressed()) {
      for (auto c : keys.word) {
        screenWake();
        switch (c) {
          case '1': case '2': case '3': case '4': case '5':
          case '6': case '7': case '8': {
            bleKeyboard.press(KEY_LEFT_GUI);
            bleKeyboard.press(c);
            bleKeyboard.releaseAll();
            char msg[8];
            snprintf(msg, sizeof(msg), "Cmd+%c", c);
            showFlash(msg, COL_KEY_NUM);
            break;
          }
          case '`':
            bleKeyboard.write(KEY_ESC);
            showFlash("Esc", COL_KEY_ESC);
            break;
          default:
            break;
        }
      }
      if (keys.enter) {
        bleKeyboard.write(KEY_RETURN);
        screenWake();
      }
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
