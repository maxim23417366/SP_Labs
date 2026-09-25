# Назва вихідного файлу прошивки
TARGET = firmware

# Папка для скомпільованих бінарних файлів
BUILD_DIR = build

# Крос-компілятор для ARM Cortex-M
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

# Налаштування процесора 
MCU = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

# Прапорці компілятора
CFLAGS = $(MCU) -Wall -fdata-sections -ffunction-sections -O2 --specs=nano.specs --specs=nosys.specs

# Список сирцевих файлів 
C_SOURCES = $(wildcard src/*.c) main.c

# Назва Docker-образу
DOCKER_IMAGE = stm32-builder

# --- Основні цілі ---

.PHONY: all clean docker-build

# Ціль за замовчуванням: збирає прошивку
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex

# Створення папки build та компіляція
$(BUILD_DIR)/$(TARGET).elf: $(C_SOURCES)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(C_SOURCES) -o $@
	$(SIZE) $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

# Очищення зібраних файлів
clean:
	rm -rf $(BUILD_DIR)

# Збірка проєкту всередині Docker-контейнера однією командою
docker-build:
	docker run --rm -v "$(CURDIR)":/workspace $(DOCKER_IMAGE) make all