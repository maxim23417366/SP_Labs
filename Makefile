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
#   make mem-report — ЛР3: звіти пам'яті firmware/build/mem/ (size, nm, .su)
#   make fw-check — ЛР3 (I10): єдина прошивка — без RTOS, усі модулі, git-хеш у .elf
#   make lab3     — ЛР3 (I10): усе разом, як CI (docs/lab_3/lab3-build.md)
#   make host-checks — ЛР3 (I11): усі перевірки без плати + лог tests/build/I11_host_checks.log
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

# ЛР3: git-хеш збірки обчислюється на хості і передається в Docker; «-dirty» —
# у робочій копії є незакомічені зміни відстежуваних файлів
GIT_REV   := $(shell git rev-parse --short HEAD 2>/dev/null)
GIT_DIRTY := $(if $(GIT_REV),$(shell git status --porcelain --untracked-files=no 2>/dev/null))
GIT_HASH  ?= $(if $(GIT_REV),$(GIT_REV)$(if $(GIT_DIRTY),-dirty),nogit)

DOCKER_RUN = docker run --rm $(DOCKER_USER) -v "$(CURDIR):/workspace" -w /workspace $(IMAGE)

.DEFAULT_GOAL := build
.PHONY: build clean test coverage check-deps flash monitor image help mem-report tools-test \
        fw-check lab3 version host-checks
REQUIRE_GIT ?= 0

# Образ збирається з кешу за секунду, тому build завжди спирається на актуальний Dockerfile
image:
	docker build -q -t $(IMAGE) docker/

build: image
	$(DOCKER_RUN) $(FW_MAKE) -j$(JOBS) all GIT_HASH=$(GIT_HASH)

mem-report: build
	$(DOCKER_RUN) $(FW_MAKE) mem-report GIT_HASH=$(GIT_HASH)

clean: image
	$(DOCKER_RUN) $(FW_MAKE) clean
	$(DOCKER_RUN) make -C $(TESTS_DIR) clean

# Тести збираються звичайним gcc у тому ж образі (build-essential)
test: image
	$(DOCKER_RUN) make -C $(TESTS_DIR) run

coverage: image
	$(DOCKER_RUN) make -C $(TESTS_DIR) coverage

# ЛР3 (I08): експорт кадру C → UART-лог → tools/frame_decode.py → PNG, без плати
tools-test: image
	$(DOCKER_RUN) make -C $(TESTS_DIR) tools

# Правила залежностей між шарами Core / Platform / Devices / App
# tr прибирає CR: на Windows скрипт може прийти з CRLF, а sh у контейнері його не розбере
check-deps: image
	$(DOCKER_RUN) sh -c "tr -d '\r' < scripts/check_deps.sh | sh"

# ЛР3 (I10): зібрана прошивка — одна, без RTOS, з усіма модулями і цим git-хешем.
# REQUIRE_GIT=1 (CI) — збірка без git (nogit) є помилкою
fw-check: build
	$(DOCKER_RUN) sh -c "tr -d '\r' < scripts/check_firmware.sh | GIT_HASH=$(GIT_HASH) REQUIRE_GIT=$(REQUIRE_GIT) sh"

# Який хеш отримає прошивка, якщо зібрати зараз
version:
	@echo GIT_HASH=$(GIT_HASH)

# ЛР3 (I10): повна перевірка єдиної прошивки — ті самі кроки, що в CI
lab3: check-deps fw-check mem-report host-checks
	@echo "LAB3: PASS git=$(GIT_HASH) — check-deps, build, fw-check, mem-report, host-checks (test, coverage, tools, python)"

# ЛР3 (I11): усі перевірки без плати одним запуском — check-deps, periph_check, усі
# Unity-набори з покриттям, self_test, tools (кадр → PNG), Python-тести scripts/ і tools/.
# Повний вивід і підсумок — tests/build/I11_host_checks.log (docs/lab_3/host-tests.md)
host-checks: image
	$(DOCKER_RUN) python3 scripts/host_checks.py --git $(GIT_HASH) --out tests/build/I11_host_checks.log

flash: build
	$(FLASH_CMD)

monitor:
	$(MONITOR_CMD)

help:
	@echo make build   - build firmware in Docker
	@echo make clean   - remove build artifacts
	@echo make test    - run host tests with mocked HAL, no board needed
	@echo make coverage - host unit tests + driver coverage, COV_MIN=90
	@echo make tools-test - frame export: C -> UART log -> tools/frame_decode.py -> PNG
	@echo make check-deps - check include rules between firmware layers
	@echo make mem-report - LR3 memory reports in firmware/build/mem
	@echo make fw-check - LR3 single firmware: no RTOS, all modules strong, git hash in .elf
	@echo make lab3    - LR3 full check: check-deps build fw-check mem-report host-checks
	@echo make host-checks - LR3 all checks without board + tests/build/I11_host_checks.log
	@echo make version - print GIT_HASH that the next build will embed
	@echo make flash   - build and flash the board via ST-Link
	@echo make monitor - open UART terminal, PORT=$(PORT) BAUD=$(BAUD)
	@echo make image   - rebuild Docker image
