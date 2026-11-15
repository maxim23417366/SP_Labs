# -----------------------------------------------------------------------------
# Збірка прошивки SmartLock, яку НЕ перезаписує STM32CubeMX.
#
# CubeMX при кожній генерації з firmware.ioc переписує firmware/Makefile цілком,
# тож будь-які наші файли, додані туди, зникли б. Тому прошивка збирається
# цим файлом (кореневий Makefile: make -C firmware -f firmware.mk), а
# згенерований Makefile лишається лише довідкою: після перегенерації його
# C_SOURCES порівнюють зі списком HAL_SOURCES нижче.
#
# Шари (docs/architecture.md):
#   Core/      — код CubeMX (init периферії, IRQ, main.c лише в USER CODE)
#   Drivers/   — CMSIS + HAL від ST, не редагуються
#   Platform/  — знання про плату: хендли, піни, обгортки шин над HAL
#   Devices/   — драйвери пристроїв, бачать лише Devices/dev_bus.h
#   App/       — прикладна логіка, без HAL
#
# Правила залежностей (хто кого має право включати):
#   App/      -> platform.h, Devices/      (без HAL, CubeMX, board.h)
#   Platform/ -> Core/, HAL, Devices/      (не залежить від App/)
#   Devices/  -> лише dev_bus.h і libc     (без HAL, CubeMX, Platform, App)
# Автоматична перевірка правил: make check-deps (scripts/check_deps.sh)
# -----------------------------------------------------------------------------

TARGET    := firmware
BUILD_DIR := build
OPT       ?= -Og

# ЛР3: ідентифікатор збірки в першому рядку UART-логу (BUILD,...,git=<hash>).
# Кореневий Makefile обчислює його на хості (git є там, а не обов'язково в Docker).
GIT_HASH  ?= nogit

PREFIX := arm-none-eabi-
CC     := $(PREFIX)gcc
AS     := $(PREFIX)gcc -x assembler-with-cpp
CP     := $(PREFIX)objcopy
SZ     := $(PREFIX)size

MCU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# ---- Джерела по шарах --------------------------------------------------------

CORE_SOURCES := \
  Core/Src/main.c \
  Core/Src/gpio.c \
  Core/Src/i2c.c \
  Core/Src/spi.c \
  Core/Src/tim.c \
  Core/Src/usart.c \
  Core/Src/stm32f4xx_it.c \
  Core/Src/stm32f4xx_hal_msp.c \
  Core/Src/system_stm32f4xx.c \
  Core/Src/sysmem.c \
  Core/Src/syscalls.c

# Лише модулі, увімкнені в Core/Inc/stm32f4xx_hal_conf.h (STM32Cube FW_F4 V1.28.3).
# Навіщо кожен — docs/architecture.md, розділ «Модулі HAL».
HAL_DIR := Drivers/STM32F4xx_HAL_Driver/Src
HAL_SOURCES := $(addprefix $(HAL_DIR)/stm32f4xx_hal, \
  .c _cortex.c _rcc.c _rcc_ex.c _pwr.c _pwr_ex.c _flash.c _flash_ex.c _flash_ramfunc.c \
  _gpio.c _exti.c _dma.c _dma_ex.c _i2c.c _i2c_ex.c _spi.c _tim.c _tim_ex.c _uart.c)

OWN_SOURCES := $(wildcard Platform/*.c Devices/*.c App/*.c)

ASM_SOURCES := startup_stm32f411xe.s

# ---- Прапорці ------------------------------------------------------------------

C_DEFS := -DUSE_HAL_DRIVER -DSTM32F411xE -DGIT_HASH=\"$(GIT_HASH)\"
C_INCLUDES := \
  -ICore/Inc -IPlatform -IDevices -IApp \
  -IDrivers/STM32F4xx_HAL_Driver/Inc \
  -IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy \
  -IDrivers/CMSIS/Device/ST/STM32F4xx/Include \
  -IDrivers/CMSIS/Include

# ЛР3 (I06): -fstack-usage — файл .su поруч з кожним .o: розмір кадру стека
# кожної функції (static / dynamic), методичка §5.4
CFLAGS_BASE := $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -std=gnu11 -g -gdwarf-2 \
               -fdata-sections -ffunction-sections -MMD -MP -fstack-usage

# Наш код і код CubeMX: без жодного попередження (вимога 4.5 ЛР2)
WARN_OWN    := -Wall -Wextra -Werror
# Вендорський HAL: придушено рівно одне попередження і тільки для нього —
# у HAL є колбеки з параметрами, які вони не використовують
WARN_VENDOR := -Wall -Wextra -Wno-unused-parameter

LDSCRIPT := STM32F411xx_FLASH.ld
# ЛР3 (I06): --print-memory-usage — заповнення FLASH / RAM після кожної збірки
LDFLAGS  := $(MCU) -specs=nano.specs -T$(LDSCRIPT) -lc -lm -lnosys \
            -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections \
            -Wl,--print-memory-usage

# ---- Правила -------------------------------------------------------------------

# Об'єкти зберігають шлях джерела, тож однакові імена в різних шарах не конфліктують
obj = $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(basename $(1))))

OWN_OBJS    := $(call obj,$(CORE_SOURCES) $(OWN_SOURCES))
VENDOR_OBJS := $(call obj,$(HAL_SOURCES))
ASM_OBJS    := $(call obj,$(ASM_SOURCES))
OBJECTS     := $(OWN_OBJS) $(VENDOR_OBJS) $(ASM_OBJS)

.PHONY: all clean mem-report
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

# Зміна git-хеша має перезібрати файли, що його друкують: окремий файл-мітка на хеш
GIT_STAMP := $(BUILD_DIR)/.git_hash_$(GIT_HASH)
$(GIT_STAMP):
	@mkdir -p $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.git_hash_*
	@touch $@
$(BUILD_DIR)/App/app.o: $(GIT_STAMP)

$(OWN_OBJS): $(BUILD_DIR)/%.o: %.c firmware.mk
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_BASE) $(WARN_OWN) $< -o $@

$(VENDOR_OBJS): $(BUILD_DIR)/%.o: %.c firmware.mk
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_BASE) $(WARN_VENDOR) $< -o $@

$(ASM_OBJS): $(BUILD_DIR)/%.o: %.s firmware.mk
	@mkdir -p $(dir $@)
	$(AS) -c $(MCU) $(OPT) -Wall $< -o $@

# Вивід --print-memory-usage зберігається в build/mem/memory_usage.txt (для M04)
$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT)
	@mkdir -p $(BUILD_DIR)/mem
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@ > $(BUILD_DIR)/mem/memory_usage.txt; \
	  st=$$?; cat $(BUILD_DIR)/mem/memory_usage.txt; exit $$st
	$(SZ) $@

# ЛР3 (I06): звіти пам'яті для M04 (методичка §5.4) у build/mem/
#   size.txt          text / data / bss
#   size_sections.txt кожна секція окремо
#   top_symbols.txt   25 найбільших символів (Flash і RAM)
#   top_ram.txt       15 найбільших змінних у RAM (.bss / .data)
#   stack_usage.txt   20 найбільших кадрів стека з файлів .su
mem-report: $(BUILD_DIR)/$(TARGET).elf
	@mkdir -p $(BUILD_DIR)/mem
	$(SZ) $< > $(BUILD_DIR)/mem/size.txt
	$(SZ) -A -d $< > $(BUILD_DIR)/mem/size_sections.txt
	$(PREFIX)nm --size-sort -S --radix=d $< | tail -25 > $(BUILD_DIR)/mem/top_symbols.txt
	$(PREFIX)nm --size-sort -S --radix=d $< | grep -E ' [bBdD] ' | tail -15 > $(BUILD_DIR)/mem/top_ram.txt
	find $(BUILD_DIR) -name '*.su' -exec cat {} + | sort -k2,2n | tail -20 \
	  > $(BUILD_DIR)/mem/stack_usage.txt
	@for f in memory_usage size top_ram stack_usage; do \
	  echo "==== build/mem/$$f.txt"; cat $(BUILD_DIR)/mem/$$f.txt; done

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(CP) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(CP) -O binary -S $< $@

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)
