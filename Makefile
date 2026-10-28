# -----------------------------------------------------------------------------
# Кореневий Makefile — обгортка над Docker-тулчейном.
# Прошивку збирає firmware/firmware.mk всередині контейнера. Згенерований CubeMX
# firmware/Makefile не використовується: CubeMX перезаписує його при кожній генерації.
#
#   make build    — зібрати прошивку: firmware/build/firmware.{elf,bin,hex}
#   make clean    — видалити артефакти збірки
#   make flash    — зібрати і прошити плату через ST-Link
#   make monitor  — відкрити UART-термінал (ST-Link VCP, 115200 8N1)
#   make test     — тести на ПК без плати: periph_check (ЛР1) + unit-тести драйверів
#   make coverage — unit-тести + покриття шару драйверів (gcov), поріг COV_MIN
#   make check-deps — правила залежностей між шарами (scripts/check_deps.sh)
#   make image    — (пере)зібрати Docker-образ з docker/Dockerfile
#
# Налаштування під конкретний комп'ютер (шлях до прошивальника, COM-порт)
# задаються у local.mk, який не комітиться. Шаблон — local.mk.example.
# -----------------------------------------------------------------------------

-include local.mk

IMAGE     ?= stm32-builder
FW_DIR    := firmware
FW_MAKE   := make -C $(FW_DIR) -f firmware.mk
BUILD_DIR := $(FW_DIR)/build
TARGET    := firmware
TESTS_DIR := tests
BAUD      ?= 115200
JOBS      ?= 4

# Git Bash (MSYS) інакше перетворює /workspace на C:/Program Files/Git/workspace
export MSYS_NO_PATHCONV := 1

ifeq ($(OS),Windows_NT)
  DOCKER_USER :=
  PROGRAMMER  ?= STM32_Programmer_CLI
  FLASH_CMD    = "$(PROGRAMMER)" -c port=SWD -w $(BUILD_DIR)/$(TARGET).hex -v -rst
  PORT        ?= COM3
  MONITOR_CMD  = python -m serial.tools.miniterm $(PORT) $(BAUD)
else
  # --user: щоб файли в build/ належали користувачу, а не root
  DOCKER_USER := --user $(shell id -u):$(shell id -g)
  FLASH_CMD    = st-flash --reset write $(BUILD_DIR)/$(TARGET).bin 0x08000000
  PORT        ?= $(firstword $(wildcard /dev/ttyACM* /dev/tty.usbmodem*))
  MONITOR_CMD  = picocom -b $(BAUD) --imap lfcrlf $(PORT)
endif

DOCKER_RUN = docker run --rm $(DOCKER_USER) -v "$(CURDIR):/workspace" -w /workspace $(IMAGE)

.DEFAULT_GOAL := build
.PHONY: build clean test coverage check-deps flash monitor image help

# Образ збирається з кешу за секунду, тому build завжди спирається на актуальний Dockerfile
image:
	docker build -q -t $(IMAGE) docker/

build: image
	$(DOCKER_RUN) $(FW_MAKE) -j$(JOBS) all

clean: image
	$(DOCKER_RUN) $(FW_MAKE) clean
	$(DOCKER_RUN) make -C $(TESTS_DIR) clean

# Тести збираються звичайним gcc у тому ж образі (build-essential)
test: image
	$(DOCKER_RUN) make -C $(TESTS_DIR) run

coverage: image
	$(DOCKER_RUN) make -C $(TESTS_DIR) coverage

# Правила залежностей між шарами Core / Platform / Devices / App
# tr прибирає CR: на Windows скрипт може прийти з CRLF, а sh у контейнері його не розбере
check-deps: image
	$(DOCKER_RUN) sh -c "tr -d '\r' < scripts/check_deps.sh | sh"

flash: build
	$(FLASH_CMD)

monitor:
	$(MONITOR_CMD)

help:
	@echo make build   - build firmware in Docker
	@echo make clean   - remove build artifacts
	@echo make test    - run host tests with mocked HAL, no board needed
	@echo make coverage - host unit tests + driver coverage, COV_MIN=90
	@echo make check-deps - check include rules between firmware layers
	@echo make flash   - build and flash the board via ST-Link
	@echo make monitor - open UART terminal, PORT=$(PORT) BAUD=$(BAUD)
	@echo make image   - rebuild Docker image
