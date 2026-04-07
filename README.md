# BLE 蓝牙键盘固件

M5Stack 设备的 BLE HID 键盘固件。一份代码通过条件编译支持三个设备，用于远程触发语音输入快捷键、Ghostty 终端 tab 切换等操作。

## 支持设备

| 设备 | 芯片 | BLE 名称 | FQBN |
|------|------|----------|------|
| M5StickC | ESP32-PICO-D4 | M5StickC-KB | `m5stack:esp32:m5stack_stickc` |
| M5StickC Plus | ESP32-PICO-D4 | M5StickCP-KB | `m5stack:esp32:m5stack_stickc_plus` |
| M5Cardputer | ESP32-S3 | Cardputer-KB | `m5stack:esp32:m5stack_cardputer` |

## 键位映射

### M5StickC / M5StickC Plus

竖屏握持（USB 端朝上），Button A 在正面，Power 和 Button B 在侧面。

| 按键 | 发送 | 说明 |
|------|------|------|
| Button A（正面大按键） | `Opt+Tab` | 语音输入快捷键 |
| Power 键（侧面） | `Enter` | 回车（响应延迟约 100-200ms，AXP192 硬件限制） |
| Button B（侧面） | 无 | 仅唤醒屏幕 |

### M5Cardputer

**竖握**：左手握住设备，屏幕竖着看（键盘横排变成竖列）。原始键盘的最左列变成了顶行，大拇指从上往下自然够到 Esc → Ctrl → Fn → Tab 这一列。

| 按键 | 发送 | 说明 |
|------|------|------|
| `1`-`8` | `Cmd+1` - `Cmd+8` | Ghostty 终端 tab 切换 |
| `` ` ``（反引号） | `Escape` | ESC 键 |
| `Ctrl` | `Opt+Tab` | 语音输入快捷键（竖握时在反引号下方，大拇指方便按到） |
| `Fn` | `Enter` | 回车（竖握时在 Ctrl 下方） |
| `Enter` | `Enter` | 回车 |

## 屏幕显示

- 连接状态（绿色 Connected / 黄色等待配对）
- 键位映射速查色块（CardPuter 显示三行：1-4、5-8、特殊键）
- 电量百分比（CardPuter 右上角，M5StickC 系列中间）
- 自动息屏（CardPuter 10 秒，M5StickC 5 秒），按键唤醒

## 电源管理

| 功能 | M5StickC / Plus | CardPuter |
|------|-----------------|-----------|
| CPU 降频 | 80MHz | 80MHz |
| 自动息屏 | 5 秒 | 10 秒 |
| 自动关机 | 30 分钟无操作 | 无 |
| 充电跳过关机 | 有（电量趋势检测） | 无 |
| LED 心跳 | GPIO10 呼吸灯 | 无 |
| 低电量警告 | 蜂鸣器 20%/10% | 无 |

## 编译上传

需要 Arduino CLI + M5Stack 核心 (`m5stack:esp32`) + ESP32-BLE-Keyboard + NimBLE-Arduino 1.4.3。

```bash
# CardPuter
make flash-card

# M5StickC Plus
make flash-cp

# M5StickC
make flash-c

# 串口监视
make monitor-card   # 或 monitor-cp / monitor-c
```

CardPuter 上传时需将 ON/OFF 开关打到 **OFF**（纯 USB 供电模式），上传更稳定。

## 技术说明

- 使用 T-vK/ESP32-BLE-Keyboard 库 + NimBLE 模式
- NimBLE-Arduino 必须用 1.4.3（2.x API 不兼容）
- USE_NIMBLE 必须在 `BleKeyboard.h` 库源文件中定义
- CardPuter 需 `#undef` M5Cardputer Keyboard_def.h 的宏以解决与 BleKeyboard.h 冲突
- 三个设备通过 `#if IS_CARDPUTER` / `#elif ARDUINO_M5STACK_STICKC_PLUS` / `#else` 条件编译

## 仓库

`YOUR_GITEA_HOST:xyb/ble-keyboard`
