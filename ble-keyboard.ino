// Device auto-detection via board macro (set by Arduino FQBN)
#if defined(ARDUINO_M5STACK_STICKC_PLUS)
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

BleKeyboard bleKeyboard(DEVICE_NAME, "M5Stack", 100);

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
const unsigned long LED_BLINK_INTERVAL = 10000;
const unsigned long LED_BREATH_DURATION = 1000;
const int LED_BREATH_PEAK = 240;
const int LED_PIN = 10;

void beepLowBattery() {
  M5.Beep.tone(BUZZER_FREQ, 150);
  delay(200);
  M5.Beep.tone(BUZZER_FREQ, 150);
  delay(200);
  M5.Beep.mute();
}

void beepPowerOff() {
  M5.Beep.tone(BUZZER_FREQ, 800);
  delay(900);
  M5.Beep.mute();
}

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
  lastPowerCheck = millis();
  lastPowerCheckBat = getBatPercent();
}

void screenSleep() {
  if (screenOn) {
    M5.Axp.SetLDO2(false);
    screenOn = false;
    lastLedBlink = millis();
  }
}

// ============================================================
//  Display functions — completely separate per device
// ============================================================

#if defined(ARDUINO_M5STACK_STICKC_PLUS)
// ----- M5StickC Plus: portrait 135x240, USB end UP -----

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);
  if (!screenOn) return;

  float v = M5.Axp.GetBatVoltage();
  int batY = 188;
  M5.Lcd.fillRect(0, batY - 2, 135, 20, BLACK);
  M5.Lcd.setTextSize(2);
  if (pct > 50)      M5.Lcd.setTextColor(GREEN);
  else if (pct > 20) M5.Lcd.setTextColor(YELLOW);
  else               M5.Lcd.setTextColor(RED);

  char buf[16];
  snprintf(buf, sizeof(buf), "%d%% %.2fV", pct, v);
  int tw = strlen(buf) * 12;
  M5.Lcd.setCursor((135 - tw) / 2, batY);
  M5.Lcd.print(buf);
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
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(12, 120);
    M5.Lcd.print("Waiting for pair...");
    updateBattery();
    return;
  }

  // Top block: Button A — left/right margins so indicator line has room
  int btnMargin = 12;
  M5.Lcd.fillRoundRect(btnMargin, 0, 135 - btnMargin * 2, 43, 4, CYAN);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor((135 - 84) / 2, 4);
  M5.Lcd.print("Opt+Tab");
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor((135 - 18) / 2, 22);
  M5.Lcd.print("A");

  // Center: connection status
  M5.Lcd.setTextColor(GREEN);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor((135 - 108) / 2, 95);
  M5.Lcd.print("Connected");

  // Bottom block: Power button (Enter)
  int pwrY = 204;
  int pwrH = 36;
  M5.Lcd.fillRoundRect(0, pwrY, 135, pwrH, 4, MAGENTA);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor((135 - 60) / 2, pwrY + 10);
  M5.Lcd.print("Enter");

  // Indicator line: Power button is on RIGHT side edge near top
  // Vertical line up along right margin, then 45° to upper-right corner
  int lineX = 135 - btnMargin / 2;
  int lineBottom = pwrY - 2;
  int turnY = 20;
  M5.Lcd.drawLine(lineX, lineBottom, lineX, turnY, MAGENTA);
  M5.Lcd.drawLine(lineX, turnY, 134, 0, MAGENTA);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(MAGENTA);
  M5.Lcd.setCursor(lineX - 22, turnY + 4);
  M5.Lcd.print("PWR");

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
  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setCursor((135 - 126) / 2, 100);
  M5.Lcd.print("Opt+Tab");
  delay(500);
  drawStatus();
  lastActivity = millis();
}

#else
// ----- M5StickC: portrait 80x160, rotation 2, USB end UP -----
//   Button A (GPIO37) : front face, near TOP
//   Power   (AXP192)  : side edge, near TOP
//   Button B (GPIO39) : side edge, CENTER

void updateBattery() {
  int pct = getBatPercent();
  bleKeyboard.setBatteryLevel(pct);
  if (!screenOn) return;

  float v = M5.Axp.GetBatVoltage();
  int batY = 115;
  M5.Lcd.fillRect(0, batY - 2, 80, 12, BLACK);
  M5.Lcd.setTextSize(1);
  if (pct > 50)      M5.Lcd.setTextColor(GREEN);
  else if (pct > 20) M5.Lcd.setTextColor(YELLOW);
  else               M5.Lcd.setTextColor(RED);

  char buf[16];
  snprintf(buf, sizeof(buf), "%d%% %.2fV", pct, v);
  int tw = strlen(buf) * 6;
  M5.Lcd.setCursor((80 - tw) / 2, batY);
  M5.Lcd.print(buf);
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
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor((80 - 60) / 2, 80);
    M5.Lcd.print("Waiting...");
    updateBattery();
    return;
  }

  // Top block: Button A — left/right margins for indicator line
  int btnMargin = 8;
  M5.Lcd.fillRoundRect(btnMargin, 0, 80 - btnMargin * 2, 28, 3, CYAN);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor((80 - 42) / 2, 3);
  M5.Lcd.print("Opt+Tab");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor((80 - 12) / 2, 13);
  M5.Lcd.print("A");

  // Center: connection status
  M5.Lcd.setTextColor(GREEN);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor((80 - 54) / 2, 55);
  M5.Lcd.print("Connected");

  // Bottom block: Power button (Enter)
  int pwrY = 136;
  int pwrH = 24;
  M5.Lcd.fillRoundRect(0, pwrY, 80, pwrH, 3, MAGENTA);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor((80 - 30) / 2, pwrY + 8);
  M5.Lcd.print("Enter");

  // Indicator line: vertical up along right margin, then 45° to corner
  int lineX = 80 - btnMargin / 2;
  int lineBottom = pwrY - 2;
  int turnY = 14;
  M5.Lcd.drawLine(lineX, lineBottom, lineX, turnY, MAGENTA);
  M5.Lcd.drawLine(lineX, turnY, 79, 0, MAGENTA);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(MAGENTA);
  M5.Lcd.setCursor(lineX - 22, turnY + 4);
  M5.Lcd.print("PWR");

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
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(BLACK);
  M5.Lcd.setCursor((80 - 84) / 2, 68);
  M5.Lcd.print("Opt+Tab");
  delay(500);
  drawStatus();
  lastActivity = millis();
}

#endif

// ============================================================
//  Setup & Loop — shared
// ============================================================

void setup() {
  M5.begin();
  setCpuFrequencyMhz(80);
  M5.Axp.ScreenBreath(80);
  M5.Lcd.setRotation(LCD_ROTATION);
  M5.Lcd.fillScreen(BLACK);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  bleKeyboard.begin();
  drawStatus();
  lastActivity = millis();
  lastBatUpdate = millis();
  lastPowerCheck = millis();
  lastPowerCheckBat = getBatPercent();

  beepLowBattery();
  delay(800);
  beepPowerOff();
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

  // Battery refresh: 30s when screen on, 5min when idle
  unsigned long batInterval = screenOn ? BAT_INTERVAL_ACTIVE : BAT_INTERVAL_IDLE;
  if (millis() - lastBatUpdate >= batInterval) {
    lastBatUpdate = millis();
    updateBattery();

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
  }

  // Button B: wake screen only
  if (M5.BtnB.wasPressed()) {
    screenWake();
  }

  if (!connected) {
    delay(100);
    return;
  }

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

  delay(10);
}
