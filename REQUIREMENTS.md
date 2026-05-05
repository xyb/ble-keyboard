# BLE 蓝牙键盘固件 — 需求文档

M5StickC / M5StickC Plus BLE HID 键盘固件的完整需求清单。

最后更新：2026-04-02

## 一、按键功能

### 已实现

- **Button A**（GPIO37，正面大按键）：发送 `Opt+Tab`（macOS 语音输入快捷键）
  - 按下时同时唤醒屏幕
  - 全屏青色 (Cyan) 闪烁反馈，持续 500ms
- **Power 键**（AXP192 电源键，侧面）：发送 `Enter`
  - 通过 `M5.Axp.GetBtnPress()` 检测短按（返回值 2）
  - 按下时同时唤醒屏幕
  - 全屏品红色 (Magenta) 闪烁反馈，持续 300ms
  - 响应延迟约 100-200ms（AXP192 硬件限制，需等松手后判断短按/长按）
- **Button B**（GPIO39，侧面）：仅唤醒屏幕，不发送按键
  - 用于查看电量和连接状态

### 按键选择理由

Button A（正面）和 Power（侧面靠近 A）是最顺手的组合。Button B 离 Button A 太远，来回切换需要翻转设备，不实用。Power 键虽有延迟，但对回车键等确认类操作影响不大。

## 二、屏幕显示

### 布局

- **竖屏模式**（Portrait，USB 端朝上）：`setRotation(2)`
- 顶部：Button A 色块（青色圆角矩形）+ "Opt+Tab" + "A" 标签
  - 左右留边距（Plus 12px，C 8px），给 Power 键指示线腾空间
- 中部：连接状态 "Connected"（绿色）
- 中下：电量百分比 + 电压（如 "85% 3.95V"）
  - 颜色分级：绿 >50%、黄 20%-50%、红 <20%
- 底部：Power 键色块（品红色圆角矩形）+ "Enter"
- Power 键指示线：从底部 Enter 块右侧**垂直向上**，到顶部附近 **45° 斜向右上角**
  - 附 "PWR" 文字标签
  - 表示 Power 物理按键在屏幕右上方侧面

### 按键反馈

- Button A 按下：**整个屏幕**变青色 + 大字 "Opt+Tab"，500ms 后恢复
- Power 按下：**整个屏幕**变品红色 + 大字 "Enter"，300ms 后恢复

### 未连接状态

- 居中显示 "BLE KB"（黄色大字）+ "Waiting for pair..."

### 两设备显示代码完全分离

M5StickC（80×160，ST7735S）和 M5StickC Plus（135×240，ST7789v2）的所有显示函数通过 `#if defined()` 完全分离，不共用代码。原因：
- 分辨率差异大，文字大小和坐标不能复用
- ST7735 驱动的 rotation 行为与 ST7789 不同

## 三、电源管理

### 已实现

- **CPU 降频**：`setCpuFrequencyMhz(80)`（BLE 所需最低频率）
- **自动息屏**：5 秒无操作后关闭屏幕背光
  - 使用 `M5.Axp.SetLDO2(false)` 彻底断电（`ScreenBreath(0)` 仍有微弱残光）
- **按键自动亮屏**：任何按键（A / B / Power）都会先唤醒屏幕
  - 亮屏亮度：`ScreenBreath(80)`（百分比，范围 0-100）
- **BLE 连接始终保持**：不使用 deep sleep / light sleep（两者都会断开 BLE 连接）
- **30 分钟无操作自动关机**（`POWEROFF_TIMEOUT = 1800000`）
  - 调用 `M5.Axp.PowerOff()` 彻底关机，需手动长按电源键开机
  - **充电检测**：每 30 分钟比较当前电量与上次记录的电量
    - 电量几乎没掉（差距 < 2%）→ 判定在充电 → 跳过关机，更新电量基准，30 分钟后再检查
    - 电量明显下降（≥ 2%）→ 在用电池且无人操作 → 自动关机
  - **低电量保护**：电量 < 5% 且 30 分钟无操作 → 无论是否充电，直接关机

### 功耗数据

| 配置 | 功耗 | 续航（95mAh / 120mAh） |
|------|------|------------------------|
| 默认 240MHz + 屏幕常亮 | ~77mA | ~70-90 分钟 |
| 80MHz + 自动关屏 | ~25-30mA | ~3-3.5 小时 |

## 四、LED 心跳指示

### 已实现

- **息屏时**：GPIO10 红色 LED 做呼吸灯效果
  - PWM 控制，GPIO10 为 active LOW（255 = 灭，0 = 最亮）
  - 呼吸周期：1 秒（500ms 渐亮 + 500ms 渐灭）
  - 最高亮度：约 6%（`LED_BREATH_PEAK = 240`，即 255-240=15 的 PWM 幅度）
  - 呼吸间隔：10 秒
  - 息屏后等完整间隔才开始第一次呼吸（不立即闪）
- **亮屏时**：LED 关闭

## 五、BLE 连接

### 设备名称

- M5StickC：`M5StickC-KB`
- M5StickC Plus：`M5StickCP-KB`

### 电量上报

- 通过 BLE Battery Service (0x180F) 上报电量百分比到主机端
- 已修改 `BleKeyboard.cpp`，在 `setBatteryLevel()` 中加 `batteryLevel()->notify()` 主动推送
- 上报频率：屏幕亮时 30 秒，息屏时 5 分钟

### 已知限制

- BLE Battery Service 无法上报充电状态（协议只有 Battery Level 字段，无充电状态）
- macOS 充电图标只对 Apple 自家设备显示
- macOS 缓存蓝牙设备名，改名后需"忘记设备"重新配对

## 六、蜂鸣器提示音

两款设备都内置压电蜂鸣器（GPIO2），通过 `M5.Beep` API 控制。蜂鸣器音量很小，最佳可听频率约 2500Hz，低于 250Hz 或高于 3000Hz 几乎听不到。

### 待实现

- **低电量警告音**：电量降到一定阈值时（如 20%、10%），发出短促的提示音
  - 频率约 2500Hz（蜂鸣器共振频率附近，最响）
  - 短促蜂鸣（如 100-200ms），不要太突兀
  - 触发时机：随电量上报周期检查，满足条件时响一次
- **自动关机提示音**：执行 `PowerOff()` 前发出较长的提示音
  - 比低电量警告更长（如 500ms-1s），让用户能注意到设备正在关机
  - 与低电量警告音在音调或节奏上有区别，方便辨认

### 硬件说明

- 蜂鸣器 GPIO：GPIO2（M5StickC 和 M5StickC Plus 相同）
- API：`M5.Beep.tone(frequency, duration)`、`M5.Beep.beep()`
- 需要在 `loop()` 中持续调用 `M5.update()` 才能正确播放带时长的音调（已满足）

## 七、多设备支持

### 已实现

- 同一套代码同时支持 M5StickC 和 M5StickC Plus 1.1
- 通过条件编译 `#if defined(ARDUINO_M5STACK_STICKC_PLUS)` 自动检测设备
- 编译时通过 FQBN 选择设备：
  - M5StickC：`--fqbn m5stack:esp32:m5stack_stickc`
  - M5StickC Plus：`--fqbn m5stack:esp32:m5stack_stickc_plus`
- 共享逻辑：BLE 连接、按键处理、电源管理、LED 心跳
- 分离逻辑：所有屏幕显示函数（`drawStatus`、`updateBattery`、`flashPower`、`flashBtnA`）

## 七、开发环境

| 项目 | 值 |
|------|------|
| arduino-cli 核心 | `m5stack:esp32` 3.2.5 |
| BLE 库 | ESP32-BLE-Keyboard 0.3.2（从 GitHub 手动安装） |
| NimBLE 版本 | **必须用 1.4.3**（2.x API 不兼容） |
| USE_NIMBLE | 必须在 `BleKeyboard.h` 头文件中定义（sketch 中定义无效） |
| M5StickC 串口 | `/dev/cu.usbserial-XXXXXXXX` |
| M5StickC Plus 串口 | `/dev/cu.usbserial-XXXXXXXX` |

### 烧录步骤

M5StickC 需手动进入下载模式：G0 接 GND → 运行上传命令 → "Connecting..." 时长按 Power 1-2 秒。

M5StickC Plus 通常无需手动操作，直接上传。

## 八、未来计划

| 功能 | 调研状态 | 参考 |
|------|----------|------|
| **Web 配置界面（开源准备）** | 需求已明确，未实现 | 见下方详细描述 |
| BLE 多设备配对切换（同时记住多台电脑/手机） | 已调研，可行但需大改 BLE 库底层 | 详见 Obsidian `ESP32 BLE 多设备配对.md` |
| 屏幕上显示充电状态（充电图标） | 提过，未实现 | AXP192 可读充电电流 |
| AXP192 IRQ 即时唤醒 | 调研过，复杂度高 | GPIO35 PEK_DBF 中断 |

### Web 配置界面

目标：让用户通过浏览器配置键位映射，不需要修改代码重新编译。为开源做准备。

**进入配置模式**：
- 设备上通过组合键（如长按 Fn + 某个键）进入配置模式
- 屏幕显示 WiFi 配置选项：AP 模式 / 连接已有 WiFi

**WiFi 连接方式**（二选一）：
- **AP 模式**：设备自己开热点（如 `BLE-KB-Config`），手机/电脑连接后访问配置页面
- **连接已有 WiFi**：连接家里的 WiFi，屏幕上显示分配到的 IP 地址

**Web 配置页面**：
- 设备启动内置 HTTP 服务，提供配置页面
- 可配置项：每个物理按键对应的快捷键（修饰键 + 按键的组合）
- CardPuter：每个键盘按键的映射都可自定义
- M5StickC/Plus：Button A、Power 键的映射
- BLE 设备名称前缀
- 屏幕超时时间等参数

**配置存储**：
- 使用 ESP32 NVS (Non-Volatile Storage) 或 SPIFFS/LittleFS 存储配置
- 开机时读取配置，覆盖默认键位
- 配置页面支持恢复默认值

**参考实现**：
- audio-recorder 项目的 Web 配置（`src/net/web_config.cpp`）已有完整的 AP 模式 + Web 界面实现
