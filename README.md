<sub><b>🌐 English</b> · <a href="README.zh.md">中文</a></sub>

# BLE Keyboard Firmware

BLE HID keyboard firmware for M5Stack devices. A single codebase supports three devices via conditional compilation, designed for triggering voice input shortcuts and terminal tab switching remotely.

## Supported Devices

| Device | Chip | BLE Name | FQBN |
|--------|------|----------|------|
| M5StickC | ESP32-PICO-D4 | M5StickC-KB | `m5stack:esp32:m5stack_stickc` |
| M5StickC Plus | ESP32-PICO-D4 | M5StickCP-KB | `m5stack:esp32:m5stack_stickc_plus` |
| M5Cardputer | ESP32-S3 | Cardputer-KB | `m5stack:esp32:m5stack_cardputer` |

## Key Mapping

### M5StickC / M5StickC Plus

Hold vertically with the USB end up. Button A is on the front face; Power and Button B are on the side.

| Key | Sends | Notes |
|-----|-------|-------|
| Button A (front) | `Opt+Tab` | Voice input shortcut |
| Power button (side) | `Enter` | ~100–200ms delay due to AXP192 hardware |
| Button B (side) | — | Wakes screen only |

### M5Cardputer

**Vertical grip**: hold the device with the left hand, screen facing up in portrait orientation (keyboard columns become rows). The leftmost column of the physical keyboard becomes the top row — thumb naturally reaches Esc → Ctrl → Fn → Tab from top to bottom.

| Key | Sends | Notes |
|-----|-------|-------|
| `1`–`8` | `Cmd+1`–`Cmd+8` | Terminal tab switching |
| `` ` `` (backtick) | `Escape` | Esc key |
| `Ctrl` | `Opt+Tab` | Voice input shortcut (below backtick in vertical grip) |
| `Fn` | `Enter` | Enter (below Ctrl in vertical grip) |
| `Enter` | `Enter` | Enter |

## Display

- Connection status (green Connected / yellow waiting to pair)
- Key map reference blocks (Cardputer: three rows — 1–4, 5–8, special keys)
- Battery percentage (Cardputer: top-right corner; M5StickC: center)
- Auto screen-off (Cardputer: 10s, M5StickC: 5s); any key press wakes it

## Power Management

| Feature | M5StickC / Plus | CardPuter |
|---------|-----------------|-----------|
| CPU frequency | 80MHz | 80MHz |
| Auto screen-off | 5s | 10s |
| Auto power-off | 30 min idle | — |
| Skip power-off while charging | Yes (current trend detection) | — |
| LED heartbeat | GPIO10 breathing effect | — |
| Low battery warning | Buzzer at 20% / 10% | — |

## Build & Flash

Requires Arduino CLI + M5Stack core (`m5stack:esp32`) + ESP32-BLE-Keyboard + NimBLE-Arduino 1.4.3.

```bash
# Cardputer
make flash-card

# M5StickC Plus
make flash-cp

# M5StickC
make flash-c

# Serial monitor
make monitor-card   # or monitor-cp / monitor-c
```

When flashing the Cardputer, switch the ON/OFF slider to **OFF** (USB-only power mode) for a more reliable upload.

## Technical Notes

- Uses T-vK/ESP32-BLE-Keyboard library with NimBLE mode
- NimBLE-Arduino must be version 1.4.3 (2.x API is incompatible)
- `USE_NIMBLE` must be defined inside `BleKeyboard.h` (defining it in the sketch has no effect)
- Cardputer requires `#undef` of macros from M5Cardputer's `Keyboard_def.h` to avoid conflicts with `BleKeyboard.h`
- The three devices are selected via `#if IS_CARDPUTER` / `#elif ARDUINO_M5STACK_STICKC_PLUS` / `#else`
