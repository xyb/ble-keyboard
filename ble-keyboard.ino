// Device auto-detection via board macro (set by Arduino FQBN)
#if defined(ARDUINO_M5STACK_STICKC_PLUS)
  #include <M5StickCPlus.h>
  #define SCREEN_W      240
  #define SCREEN_H      135
  #define DEVICE_NAME   "M5SCP-KB"
#else
  #include <M5StickC.h>
  #define SCREEN_W      160
  #define SCREEN_H      80
  #define DEVICE_NAME   "M5StickC-KB"
#endif

#include <BleKeyboard.h>

BleKeyboard bleKeyboard(DEVICE_NAME, "M5Stack", 100);

bool connected = false;
bool prevConnected = false;
unsigned long lastBatUpdate = 0;
unsigned long lastActivity = 0;
bool screenOn = true;

const unsigned long BAT_INTERVAL_ACTIVE = 30000;
const unsigned long BAT_INTERVAL_IDLE = 300000;
const unsigned long SCREEN_TIMEOUT = 5000;

// --- Layout constants (landscape, setRotation 1) ---
// Both devices: USB+Grove on RIGHT, HAT exp pins on LEFT
//   Button A (GPIO37) : front face, RIGHT of screen
//   Power   (AXP192)  : bottom edge, RIGHT portion
//   Button B (GPIO39) : top edge,    CENTER
//
// Button A right strip
#define BTNA_W        (SCREEN_W * 23 / 100)
#define BTNA_X        (SCREEN_W - BTNA_W)
#define BTNA_H        (SCREEN_H * 70 / 100)

// Button B top-center indicator
#define BTNB_W        (SCREEN_W / 4)
#define BTNB_H        (SCREEN_H * 18 / 100)
#define BTNB_X        ((SCREEN_W - BTNB_W) / 2)

// Power button bottom-right block
#define PWR_H         (SCREEN_H * 28 / 100)
#define PWR_W         (SCREEN_W * 45 / 100)
#define PWR_X         (SCREEN_W - PWR_W)
#define PWR_Y         (SCREEN_H - PWR_H)

// Battery display bottom-left
#define BAT_Y         (SCREEN_H - 18)

// Status text
#if SCREEN_W >= 240
  #define STATUS_SIZE   3
  #define STATUS_Y      (SCREEN_H / 3)
  #define FLASH_SIZE    4
  #define TITLE_SIZE    3
  #define BAT_SIZE      2
#else
  #define STATUS_SIZE   2
  #define STATUS_Y      (SCREEN_H / 3)
  #define FLASH_SIZE    3
  #define TITLE_SIZE    2
  #define BAT_SIZE      1
#endif

int getBatPercent() {
  float v = M5.Axp.GetBatVoltage();
  int pct = (int)((v - 3.0f) / (4.2f - 3.0f) * 100.0f);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return pct;
}

void screenWake() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(80);
    screenOn = true;
    drawStatus();
  }
  lastActivity = millis();
}

void screenSleep() {
  if (screenOn) {
    M5.Axp.SetLDO2(false);
    screenOn = false;
  }
}

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);

  if (!screenOn) return;
  float v = M5.Axp.GetBatVoltage();

  M5.Lcd.fillRect(2, BAT_Y - 2, BTNA_X - 4, 20, BLACK);
  M5.Lcd.setTextSize(BAT_SIZE);

  if (pct > 50)      M5.Lcd.setTextColor(GREEN);
  else if (pct > 20) M5.Lcd.setTextColor(YELLOW);
  else               M5.Lcd.setTextColor(RED);

  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%  %.2fV", pct, v);
  M5.Lcd.setCursor(4, BAT_Y);
  M5.Lcd.print(buf);
}

void drawStatus() {
  if (!screenOn) return;
  M5.Lcd.fillScreen(BLACK);

  if (!connected) {
    M5.Lcd.setTextSize(TITLE_SIZE);
    M5.Lcd.setCursor(SCREEN_W / 8, SCREEN_H / 6);
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.print("BLE KB");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(SCREEN_W / 8, SCREEN_H / 2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.print("Waiting for pair...");
    updateBattery();
    return;
  }

  // Top-center: Button B indicator
  M5.Lcd.fillRoundRect(BTNB_X, 0, BTNB_W, BTNB_H, 3, 0x4208);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(BTNB_X + 4, BTNB_H / 4);
  M5.Lcd.print("[B]wk");

  // Right strip: Button A
  M5.Lcd.fillRoundRect(BTNA_X, 0, BTNA_W, BTNA_H, 4, CYAN);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(BTNA_X + 4, BTNA_H / 6);
  M5.Lcd.print("Opt");
  M5.Lcd.setCursor(BTNA_X + 4, BTNA_H / 6 + 14);
  M5.Lcd.print("+Tab");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(BTNA_X + (BTNA_W - 12) / 2, BTNA_H / 2);
  M5.Lcd.print("A");

  // Bottom-right: Power button
  M5.Lcd.fillRoundRect(PWR_X, PWR_Y, PWR_W, PWR_H, 4, MAGENTA);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(PWR_X + 4, PWR_Y + PWR_H / 4);
  M5.Lcd.print("Enter[PWR]");

  // Center-left: connection status
  M5.Lcd.setTextColor(GREEN);
  M5.Lcd.setTextSize(STATUS_SIZE);
  M5.Lcd.setCursor(4, STATUS_Y);
  M5.Lcd.print("Connect");

  updateBattery();
}

void flashPower() {
  if (!screenOn) {
    M5.Axp.SetLDO2(true);
    M5.Axp.ScreenBreath(80);
    screenOn = true;
  }
  M5.Lcd.fillScreen(MAGENTA);
  M5.Lcd.setTextSize(FLASH_SIZE);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor(SCREEN_W / 8, SCREEN_H / 3);
  M5.Lcd.print("Enter");
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
  M5.Lcd.setTextSize(FLASH_SIZE);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setCursor(SCREEN_W / 16, SCREEN_H / 3);
  M5.Lcd.print("Opt+Tab");
  delay(500);
  drawStatus();
  lastActivity = millis();
}

void setup() {
  M5.begin();
  setCpuFrequencyMhz(80);
  M5.Axp.ScreenBreath(80);
  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(BLACK);

  bleKeyboard.begin();
  drawStatus();
  lastActivity = millis();
  lastBatUpdate = millis();
}

void loop() {
  M5.update();

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

  // Battery refresh: 30s when screen on, 5min when idle
  unsigned long batInterval = screenOn ? BAT_INTERVAL_ACTIVE : BAT_INTERVAL_IDLE;
  if (millis() - lastBatUpdate >= batInterval) {
    lastBatUpdate = millis();
    updateBattery();
  }

  // Button B (top edge, GPIO39): wake screen only
  if (M5.BtnB.wasPressed()) {
    screenWake();
  }

  if (!connected) {
    delay(100);
    return;
  }

  // Button A (right, front face): Opt+Tab
  if (M5.BtnA.wasPressed()) {
    bleKeyboard.press(KEY_LEFT_ALT);
    bleKeyboard.press(KEY_TAB);
    bleKeyboard.releaseAll();
    flashBtnA();
  }

  // Power button (bottom edge): Enter
  if (M5.Axp.GetBtnPress() == 2) {
    bleKeyboard.write(KEY_RETURN);
    flashPower();
  }

  delay(10);
}
