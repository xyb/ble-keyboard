#!/bin/bash
# 远程触发 CardPuter BLE 键盘进入 WiFi 配置模式
# 通过快速连按 Caps Lock 让 host 给 BLE 设备发 LED output report，
# 设备端检测到 6 次 LED 状态变化（2 秒内）→ 重启进配置模式
#
# 用法：./trigger-config.sh
# 前置：CardPuter 已通过 BLE 连到本机

set -e

echo "Triggering CardPuter config mode via Caps Lock..."

osascript <<'EOF'
tell application "System Events"
  repeat 7 times
    key code 57  -- Caps Lock
    delay 0.18
  end repeat
end tell
EOF

echo "Done. CardPuter should reboot and enter config mode within ~2s."
echo "Then ping cardputer-kb.local to find it."
