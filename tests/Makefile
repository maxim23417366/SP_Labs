# -----------------------------------------------------------------------------
# Хостові тести: звичайний gcc, без ARM-тулчейну і без плати.
# З кореня репозиторію запускаються в Docker: make test / make coverage
#
#   make run       — periph_check (ЛР1) + unit-тести драйверів (Unity)
#   make coverage  — unit-тести + покриття firmware/Devices/*.c (gcov), поріг COV_MIN
#   make clean
# -----------------------------------------------------------------------------

FW_DEV  := ../firmware/Devices
UNITY   := vendor/unity
BUILD   := build
COV_MIN ?= 90

CC := gcc
# Свій код (драйвери, test doubles, тести): усі попередження — помилки
CFLAGS_OWN    := -std=gnu11 -Wall -Wextra -Werror -O0 -g -I$(FW_DEV) -Idoubles -Ivectors -I$(UNITY)
# Вендорський Unity: попередження вимкнені цілеспрямовано і лише для нього
CFLAGS_VENDOR := -std=gnu11 -O0 -g -w
COVFLAGS      := --coverage

DRIVERS := bmp180 dht11 lsm303d mpu6050 w25q64
DOUBLES := fake_i2c mock_spi fake_delay

DRV_OBJS  := $(DRIVERS:%=$(BUILD)/dev/%.o)
DBL_OBJS  := $(DOUBLES:%=$(BUILD)/doubles/%.o)
UNITY_OBJ := $(BUILD)/vendor/unity.o
BINS      := $(DRIVERS:%=$(BUILD)/test_%)

DEV_HDRS  := $(wildcard $(FW_DEV)/*.h)
DBL_HDRS  := $(wildcard doubles/*.h)

.PHONY: all run periph unit selftest coverage clean
.SECONDARY: $(DRV_OBJS) $(DBL_OBJS) $(UNITY_OBJ)

all: run

run: periph unit selftest

selftest:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Werror \
		-I../firmware/App -I../firmware/Devices \
		test_self_test.c ../firmware/App/self_test.c \
		-o $(BUILD)/test_self_test
	./$(BUILD)/test_self_test

periph:
	$(MAKE) -C periph_check run

unit: $(BINS)
	@set -e; for t in $(BINS); do echo; echo "======== $$t"; ./$$t; done

$(BUILD)/dev/%.o: $(FW_DEV)/%.c $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_OWN) $(COVFLAGS) -c $< -o $@

$(BUILD)/doubles/%.o: doubles/%.c $(DBL_HDRS) $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_OWN) -c $< -o $@

$(UNITY_OBJ): $(UNITY)/unity.c $(UNITY)/unity.h $(UNITY)/unity_internals.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_VENDOR) -c $< -o $@

$(BUILD)/test_%: unit/test_%.c $(BUILD)/dev/%.o $(DBL_OBJS) $(UNITY_OBJ) $(wildcard vectors/*.h)
	$(CC) $(CFLAGS_OWN) $(COVFLAGS) $< $(BUILD)/dev/$*.o $(DBL_OBJS) $(UNITY_OBJ) -o $@

# Покриття рахується лише для шару драйверів: тести і test doubles не входять.
# Підсумок по рядках порівнюється з COV_MIN; гілки виводяться для звіту.
coverage:
	rm -f $(BUILD)/dev/*.gcda
	$(MAKE) unit
	@echo
	@echo "======== coverage: firmware/Devices (gcov, line threshold $(COV_MIN)%)"
	@gcov -n -b -o $(BUILD)/dev $(DRIVERS:%=$(FW_DEV)/%.c) | awk -v min=$(COV_MIN) ' \
	  /^File / { f = $$2; gsub(/\047/, "", f); sub(/.*\//, "", f); \
	    keep = ($$2 ~ /Devices\/[^\/]*\.c\047$$/); got_l = 0; got_b = 0; next } \
	  keep && !got_l && /^Lines executed:/ { p = substr($$2, 10) + 0; n = $$4; got_l = 1; \
	    printf "  %-12s lines %6.2f%% of %4d", f, p, n; hit += p * n / 100; tot += n; next } \
	  keep && !got_b && /^Taken at least once:/ { got_b = 1; \
	    printf "   branches %6.2f%% of %4d\n", substr($$4, 6) + 0, $$6; next } \
	  END { pct = tot ? 100 * hit / tot : 0; \
	    ok = (pct >= min); \
	    printf "  %-12s lines %6.2f%% of %4d -> %s\n", "TOTAL", pct, tot, ok ? "OK" : "BELOW THRESHOLD"; \
	    exit !ok }'

clean:
	rm -rf $(BUILD)
	$(MAKE) -C periph_check clean
