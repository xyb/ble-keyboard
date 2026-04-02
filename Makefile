ARDUINO_CLI := arduino-cli
SKETCH      := ble-keyboard.ino
CACHE_DIR   := $(HOME)/Library/Caches/arduino/sketches

FQBN_C      := m5stack:esp32:m5stack_stickc
FQBN_CP     := m5stack:esp32:m5stack_stickc_plus

PORT_C      ?= /dev/cu.usbserial-XXXXXXXX
PORT_CP     ?= /dev/cu.usbserial-XXXXXXXX

BAUD        ?= 115200

.PHONY: help build-c build-cp upload-c upload-cp flash-c flash-cp monitor-c monitor-cp clean

help:
	@echo "BLE Keyboard firmware — M5StickC / M5StickC Plus"
	@echo ""
	@echo "  make build-c                   编译 M5StickC"
	@echo "  make build-cp                  编译 M5StickC Plus"
	@echo "  make upload-c  [PORT_C=...]    上传 M5StickC"
	@echo "  make upload-cp [PORT_CP=...]   上传 M5StickC Plus"
	@echo "  make flash-c   [PORT_C=...]    编译 + 上传 M5StickC"
	@echo "  make flash-cp  [PORT_CP=...]   编译 + 上传 M5StickC Plus"
	@echo "  make monitor-c [PORT_C=...]    串口监视 M5StickC"
	@echo "  make monitor-cp [PORT_CP=...]  串口监视 M5StickC Plus"
	@echo "  make clean                     清理编译缓存"
	@echo ""
	@echo "默认串口:"
	@echo "  M5StickC:      $(PORT_C)"
	@echo "  M5StickC Plus: $(PORT_CP)"

build-c: clean
	$(ARDUINO_CLI) compile --fqbn $(FQBN_C) $(SKETCH)

build-cp: clean
	$(ARDUINO_CLI) compile --fqbn $(FQBN_CP) $(SKETCH)

upload-c:
	$(ARDUINO_CLI) upload --fqbn $(FQBN_C) -p $(PORT_C) $(SKETCH)

upload-cp:
	$(ARDUINO_CLI) upload --fqbn $(FQBN_CP) -p $(PORT_CP) $(SKETCH)

flash-c: build-c upload-c

flash-cp: build-cp upload-cp

monitor-c:
	stty -f "$(PORT_C)" $(BAUD) && cat "$(PORT_C)"

monitor-cp:
	stty -f "$(PORT_CP)" $(BAUD) && cat "$(PORT_CP)"

clean:
	@rm -rf $(CACHE_DIR)/*
	@echo "编译缓存已清理"
