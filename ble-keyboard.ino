#include <M5StickC.h>
#include <BleKeyboard.h>

BleKeyboard bleKeyboard("M5StickC-KB", "M5Stack", 100);

bool connected = false;
bool prevConnected = false;
unsigned long lastBatUpdate = 0;
unsigned long lastActivity = 0;
bool screenOn = true;

const unsigned long BAT_UPDATE_INTERVAL = 30000;
const unsigned long SCREEN_TIMEOUT = 5000;

// rotation(1): 160x80, USB+Grove on RIGHT, HAT exp pins on LEFT
//
// Physical button positions (landscape, CW 90 from vertical):
//   Button A (GPIO37) : front face, RIGHT of screen   (x->159)
//   Power   (AXP192)  : bottom edge, RIGHT portion    (x->159, y->79)
//   Button B (GPIO39) : top edge,    CENTER             (x~80,   y->0)
//   USB-C + Grove     : right edge
//   HAT exp pins      : left edge
//
// Screen layout matches physical positions:
//   BtnB
//   vv
//   ┌──────────────────────────────┬────────┐
//   │                              │ Opt    │
//   │    Connected                 │ +Tab   │ <- Btn A (right)
//   │    85% 4.05V                 │  [A]   │
//   │                   ┌──────────┤        │
//   │                   │Ent [PWR] │        │ <- Power (bottom-right)
//   └───────────────────┴──────────┴────────┘

int getBatPercent() {
  float v = M5.Axp.GetBatVoltage();
  int pct = (int)((v - 3.0f) / (4.2f - 3.0f) * 100.0f);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return pct;
}

void screenWake() {
  if (!screenOn) {
    M5.Axp.ScreenBreath(80);
    screenOn = true;
    drawStatus();
  }
  lastActivity = millis();
}

void screenSleep() {
  if (screenOn) {
    M5.Axp.ScreenBreath(0);
    screenOn = false;
  }
}

void drawBattery() {
  if (!screenOn) return;
  float v = M5.Axp.GetBatVoltage();
  int pct = getBatPercent();

  M5.Lcd.fillRect(2, 60, 84, 18, BLACK);
  M5.Lcd.setTextSize(1);

  if (pct > 50)      M5.Lcd.setTextColor(GREEN);
  else if (pct > 20) M5.Lcd.setTextColor(YELLOW);
  else               M5.Lcd.setTextColor(RED);

  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%  %.2fV", pct, v);
  M5.Lcd.setCursor(4, 64);
  M5.Lcd.print(buf);
}

void drawStatus() {
  if (!screenOn) return;
  M5.Lcd.fillScreen(BLACK);

  if (!connected) {
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(20, 10);
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.print("BLE KB");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(22, 34);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.print("Waiting for pair...");
    drawBattery();
    return;
  }

  // Top-center: Button B indicator (physically top edge, center)
  M5.Lcd.fillRoundRect(42, 0, 40, 14, 3, 0x4208);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(46, 3);
  M5.Lcd.print("[B]wk");

  // Right strip: Button A (physically right of screen, front face)
  M5.Lcd.fillRoundRect(124, 0, 36, 56, 4, CYAN);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(128, 8);
  M5.Lcd.print("Opt");
  M5.Lcd.setCursor(128, 20);
  M5.Lcd.print("+Tab");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(133, 36);
  M5.Lcd.print("[A]");

  // Bottom-right: Power button (physically bottom edge, right portion)
  M5.Lcd.fillRoundRect(88, 58, 72, 22, 4, MAGENTA);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(92, 65);
  M5.Lcd.print("Enter[PWR]");

  // Center: status
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(GREEN);
  M5.Lcd.setCursor(4, 24);
  M5.Lcd.setTextSize(2);
  M5.Lcd.print("Connect");

  // Battery
  drawBattery();
}

void flashPower() {
  if (!screenOn) {
    M5.Axp.ScreenBreath(80);
    screenOn = true;
  }
  M5.Lcd.fillScreen(MAGENTA);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor(20, 25);
  M5.Lcd.print("Enter");
  delay(300);
  drawStatus();
  lastActivity = millis();
}

void flashBtnA() {
  if (!screenOn) {
    M5.Axp.ScreenBreath(80);
    screenOn = true;
  }
  M5.Lcd.fillScreen(CYAN);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setCursor(4, 25);
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

  // Battery refresh (only when screen is on)
  if (screenOn && (millis() - lastBatUpdate >= BAT_UPDATE_INTERVAL)) {
    lastBatUpdate = millis();
    drawBattery();
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
