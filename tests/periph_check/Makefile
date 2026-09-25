# Тести periph_check.c на ПК (звичайний gcc, без ARM і без плати).
# Запуск з кореня репозиторію: make test  (у Docker)

FW_DIR := ../../firmware
BUILD  := build
BIN    := $(BUILD)/test_periph_check

CC     := gcc
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O0 -g -Imocks -I$(FW_DIR)/Core/Inc

SRCS := test_periph_check.c mocks/mock_hal.c $(FW_DIR)/Core/Src/periph_check.c
DEPS := $(SRCS) $(wildcard mocks/*.h) $(FW_DIR)/Core/Inc/periph_check.h

.PHONY: all run clean

all: run

$(BIN): $(DEPS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD)
