ARDUINO_CLI := arduino-cli
SKETCH      := ble-keyboard.ino
CACHE_DIR   := $(HOME)/Library/Caches/arduino/sketches

# Per-device build directories to avoid cache conflicts
BUILD_DIR_C    := build/stickc
BUILD_DIR_CP   := build/stickc-plus
BUILD_DIR_CARD := build/cardputer

FQBN_C      := m5stack:esp32:m5stack_stickc
FQBN_CP     := m5stack:esp32:m5stack_stickc_plus
FQBN_CARD   := m5stack:esp32:m5stack_cardputer:PartitionScheme=huge_app

PORT_C      ?= /dev/cu.usbserial-XXXXXXXX
PORT_CP     ?= /dev/cu.usbserial-XXXXXXXX
PORT_CARD   ?= /dev/cu.usbmodem1101

BAUD        ?= 115200
UPLOAD_BAUD ?= 1500000

# esptool's bundled pyserial crashes on macOS when a USB device has
# non-UTF8 characters in its IOKit name.  This wrapper monkey-patches
# IORegistryEntryGetName to decode with errors='replace'.
ESPTOOL_PYTHON := /opt/homebrew/Cellar/esptool/5.1.0/libexec/bin/python
define ESPTOOL_CMD
$(ESPTOOL_PYTHON) -c "\
import serial.tools.list_ports_osx as _lpo; \
import ctypes as _ct; \
_orig = _lpo.IORegistryEntryGetName; \
def _safe(dev): \
    try: return _orig(dev) \
    except UnicodeDecodeError: \
        b = _ct.create_string_buffer(512); \
        _lpo.iokit.IORegistryEntryGetName(dev, _ct.byref(b)); \
        return b.value.decode('utf-8', errors='replace'); \
_lpo.IORegistryEntryGetName = _safe; \
import sys, esptool; \
sys.argv = ['esptool'] + '$(1)'.split(); \
esptool._main()"
endef

.PHONY: help build-c build-cp build-card upload-c upload-cp upload-card flash-c flash-cp flash-card monitor-c monitor-cp monitor-card clean

help:
	@echo "BLE Keyboard firmware — M5StickC / M5StickC Plus / CardPuter"
	@echo ""
	@echo "  make build-c                      编译 M5StickC"
	@echo "  make build-cp                     编译 M5StickC Plus"
	@echo "  make build-card                   编译 CardPuter"
	@echo "  make upload-c    [PORT_C=...]     上传 M5StickC"
	@echo "  make upload-cp   [PORT_CP=...]    上传 M5StickC Plus"
	@echo "  make upload-card [PORT_CARD=...]  上传 CardPuter"
	@echo "  make flash-c     [PORT_C=...]     编译 + 上传 M5StickC"
	@echo "  make flash-cp    [PORT_CP=...]    编译 + 上传 M5StickC Plus"
	@echo "  make flash-card  [PORT_CARD=...]  编译 + 上传 CardPuter"
	@echo "  make monitor-c   [PORT_C=...]     串口监视 M5StickC"
	@echo "  make monitor-cp  [PORT_CP=...]    串口监视 M5StickC Plus"
	@echo "  make monitor-card [PORT_CARD=...] 串口监视 CardPuter"
	@echo "  make clean                        清理编译缓存"
	@echo ""
	@echo "默认串口:"
	@echo "  M5StickC:      $(PORT_C)"
	@echo "  M5StickC Plus: $(PORT_CP)"
	@echo "  CardPuter:     $(PORT_CARD)"

build-c:
	$(ARDUINO_CLI) compile --fqbn $(FQBN_C) --build-path $(BUILD_DIR_C) $(SKETCH)

build-cp:
	$(ARDUINO_CLI) compile --fqbn $(FQBN_CP) --build-path $(BUILD_DIR_CP) $(SKETCH)

build-card:
	$(ARDUINO_CLI) compile --fqbn $(FQBN_CARD) --build-path $(BUILD_DIR_CARD) $(SKETCH)

# Upload using arduino-cli first; if it hits the pyserial UTF-8 bug,
# fall back to esptool with monkey-patched pyserial.
# Uses per-partition writes (not merged.bin) to preserve NVS/bonding data.
upload-c:
	@$(ARDUINO_CLI) upload --fqbn $(FQBN_C) -p $(PORT_C) --input-dir $(BUILD_DIR_C) 2>&1 \
	|| { echo "arduino-cli upload failed, falling back to patched esptool..."; \
	     $(call ESPTOOL_CMD,--chip esp32 --port $(PORT_C) --baud $(UPLOAD_BAUD) \
	       write-flash \
	       0x1000  $(BUILD_DIR_C)/$(SKETCH).bootloader.bin \
	       0x8000  $(BUILD_DIR_C)/$(SKETCH).partitions.bin \
	       0xe000  $(HOME)/.arduino15/packages/m5stack/hardware/esp32/*/tools/partitions/boot_app0.bin \
	       0x10000 $(BUILD_DIR_C)/$(SKETCH).bin); }

upload-cp:
	@$(ARDUINO_CLI) upload --fqbn $(FQBN_CP) -p $(PORT_CP) --input-dir $(BUILD_DIR_CP) 2>&1 \
	|| { echo "arduino-cli upload failed, falling back to patched esptool..."; \
	     $(call ESPTOOL_CMD,--chip esp32 --port $(PORT_CP) --baud $(UPLOAD_BAUD) \
	       write-flash \
	       0x1000  $(BUILD_DIR_CP)/$(SKETCH).bootloader.bin \
	       0x8000  $(BUILD_DIR_CP)/$(SKETCH).partitions.bin \
	       0xe000  $(HOME)/.arduino15/packages/m5stack/hardware/esp32/*/tools/partitions/boot_app0.bin \
	       0x10000 $(BUILD_DIR_CP)/$(SKETCH).bin); }

upload-card:
	$(ARDUINO_CLI) upload --fqbn $(FQBN_CARD) -p $(PORT_CARD) --input-dir $(BUILD_DIR_CARD)

flash-c: build-c upload-c

flash-cp: build-cp upload-cp

flash-card: build-card upload-card

monitor-card:
	$(ARDUINO_CLI) monitor -p $(PORT_CARD) -c baudrate=$(BAUD)

monitor-c:
	stty -f "$(PORT_C)" $(BAUD) && cat "$(PORT_C)"

monitor-cp:
	stty -f "$(PORT_CP)" $(BAUD) && cat "$(PORT_CP)"

clean:
	@rm -rf $(BUILD_DIR_C) $(BUILD_DIR_CP) $(BUILD_DIR_CARD)
	@echo "编译缓存已清理"
