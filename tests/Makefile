# -----------------------------------------------------------------------------
# Хостові тести: звичайний gcc, без ARM-тулчейну і без плати.
# З кореня репозиторію запускаються в Docker: make test
#
#   make run       — periph_check (ЛР1) + unit-тести драйверів (Unity)
#   make clean
# -----------------------------------------------------------------------------

FW_DEV  := ../firmware/Devices
UNITY   := vendor/unity
BUILD   := build

CC := gcc
# Свій код (драйвери, test doubles, тести): усі попередження — помилки
CFLAGS_OWN    := -std=gnu11 -Wall -Wextra -Werror -O0 -g -I$(FW_DEV) -Idoubles -Ivectors -I$(UNITY)
# Вендорський Unity: попередження вимкнені цілеспрямовано і лише для нього
CFLAGS_VENDOR := -std=gnu11 -O0 -g -w

DRIVERS := bmp180 mpu6050 w25q64
DOUBLES := fake_i2c mock_spi fake_delay

DRV_OBJS  := $(DRIVERS:%=$(BUILD)/dev/%.o)
DBL_OBJS  := $(DOUBLES:%=$(BUILD)/doubles/%.o)
UNITY_OBJ := $(BUILD)/vendor/unity.o
BINS      := $(DRIVERS:%=$(BUILD)/test_%)

DEV_HDRS  := $(wildcard $(FW_DEV)/*.h)
DBL_HDRS  := $(wildcard doubles/*.h)

.PHONY: all run periph unit clean
.SECONDARY: $(DRV_OBJS) $(DBL_OBJS) $(UNITY_OBJ)

all: run

run: periph unit

periph:
	$(MAKE) -C periph_check run

unit: $(BINS)
	@set -e; for t in $(BINS); do echo; echo "======== $$t"; ./$$t; done

$(BUILD)/dev/%.o: $(FW_DEV)/%.c $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_OWN) -c $< -o $@

$(BUILD)/doubles/%.o: doubles/%.c $(DBL_HDRS) $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_OWN) -c $< -o $@

$(UNITY_OBJ): $(UNITY)/unity.c $(UNITY)/unity.h $(UNITY)/unity_internals.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_VENDOR) -c $< -o $@

$(BUILD)/test_%: unit/test_%.c $(BUILD)/dev/%.o $(DBL_OBJS) $(UNITY_OBJ) $(wildcard vectors/*.h)
	$(CC) $(CFLAGS_OWN) $< $(BUILD)/dev/$*.o $(DBL_OBJS) $(UNITY_OBJ) -o $@

clean:
	rm -rf $(BUILD)
	$(MAKE) -C periph_check clean
