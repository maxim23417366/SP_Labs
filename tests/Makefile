# -----------------------------------------------------------------------------
# Хостові тести: звичайний gcc, без ARM-тулчейну і без плати.
# З кореня репозиторію запускаються в Docker: make test / make coverage
#
#   make run       — periph_check (ЛР1) + unit-тести драйверів і App/ (Unity)
#                    + тест форматування self-test (selftest)
#   make coverage  — unit-тести + покриття firmware/Devices/*.c (поріг COV_MIN)
#                    і firmware/App/*.c (поріг COV_MIN_APP), gcov
#   make clean
# -----------------------------------------------------------------------------

FW_DEV  := ../firmware/Devices
FW_APP  := ../firmware/App
FW_PLAT := ../firmware/Platform
UNITY   := vendor/unity
BUILD   := build
COV_MIN ?= 90

CC := gcc
# Свій код (драйвери, test doubles, тести): усі попередження — помилки
CFLAGS_OWN    := -std=gnu11 -Wall -Wextra -Werror -O0 -g -I$(FW_DEV) -Idoubles -Ivectors -I$(UNITY)
# Вендорський Unity: попередження вимкнені цілеспрямовано і лише для нього
CFLAGS_VENDOR := -std=gnu11 -O0 -g -w
COVFLAGS      := --coverage
# App/ і fake_platform додатково бачать App/*.h і platform.h (лише його з Platform/:
# решта Platform/ залежить від HAL і на ПК не збирається)
CFLAGS_APP     = $(CFLAGS_OWN) -I$(FW_APP) -I$(FW_PLAT)

DRIVERS := bmp180 dht11 lsm303d mpu6050 w25q64
DOUBLES := fake_i2c mock_spi fake_delay

DRV_OBJS  := $(DRIVERS:%=$(BUILD)/dev/%.o)
DBL_OBJS  := $(DOUBLES:%=$(BUILD)/doubles/%.o)
UNITY_OBJ := $(BUILD)/vendor/unity.o
DRV_BINS  := $(DRIVERS:%=$(BUILD)/test_%)

# Тести App/ (перевірки стенда і команди без обладнання): кожен лінкується з усім App/,
# усіма драйверами і fake_platform (імітація platform.h)
APP_SRCS  := app bench self_test stand_test
APP_TESTS := stand_test app
APP_OBJS  := $(APP_SRCS:%=$(BUILD)/app/%.o) $(BUILD)/doubles/fake_platform.o
APP_BINS  := $(APP_TESTS:%=$(BUILD)/test_%)
APP_HDRS  := $(wildcard $(FW_APP)/*.h) $(FW_PLAT)/platform.h
COV_MIN_APP ?= 90

BINS      := $(DRV_BINS) $(APP_BINS)

DEV_HDRS  := $(wildcard $(FW_DEV)/*.h)
DBL_HDRS  := $(wildcard doubles/*.h)

.PHONY: all run periph unit selftest coverage clean
.SECONDARY: $(DRV_OBJS) $(DBL_OBJS) $(APP_OBJS) $(UNITY_OBJ)

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

$(BUILD)/app/%.o: $(FW_APP)/%.c $(APP_HDRS) $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_APP) $(COVFLAGS) -c $< -o $@

$(BUILD)/doubles/fake_platform.o: doubles/fake_platform.c $(DBL_HDRS) $(APP_HDRS) $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_APP) -c $< -o $@

$(UNITY_OBJ): $(UNITY)/unity.c $(UNITY)/unity.h $(UNITY)/unity_internals.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_VENDOR) -c $< -o $@

$(APP_BINS): $(BUILD)/test_%: unit/test_%.c $(APP_OBJS) $(DRV_OBJS) $(DBL_OBJS) $(UNITY_OBJ) $(wildcard vectors/*.h)
	$(CC) $(CFLAGS_APP) $(COVFLAGS) $< $(APP_OBJS) $(DRV_OBJS) $(DBL_OBJS) $(UNITY_OBJ) -o $@

$(DRV_BINS): $(BUILD)/test_%: unit/test_%.c $(BUILD)/dev/%.o $(DBL_OBJS) $(UNITY_OBJ) $(wildcard vectors/*.h)
	$(CC) $(CFLAGS_OWN) $(COVFLAGS) $< $(BUILD)/dev/$*.o $(DBL_OBJS) $(UNITY_OBJ) -o $@

# Покриття рахується для шару драйверів і App/: тести і test doubles не входять.
# Підсумок по рядках порівнюється з COV_MIN / COV_MIN_APP; гілки виводяться для звіту.
# Файл без гілок не має рядка "Taken at least once" — тоді рядок звіту
# завершується при переході до наступного файлу.
coverage:
	rm -f $(BUILD)/dev/*.gcda $(BUILD)/app/*.gcda
	$(MAKE) unit
	@echo
	@echo "======== coverage: firmware/Devices (gcov, line threshold $(COV_MIN)%)"
	@gcov -n -b -o $(BUILD)/dev $(DRIVERS:%=$(FW_DEV)/%.c) | awk -v min=$(COV_MIN) ' \
	  /^File / { if (keep && got_l && !got_b) printf "\n"; f = $$2; gsub(/\047/, "", f); sub(/.*\//, "", f); \
	    keep = ($$2 ~ /Devices\/[^\/]*\.c\047$$/); got_l = 0; got_b = 0; next } \
	  keep && !got_l && /^Lines executed:/ { p = substr($$2, 10) + 0; n = $$4; got_l = 1; \
	    printf "  %-12s lines %6.2f%% of %4d", f, p, n; hit += p * n / 100; tot += n; next } \
	  keep && !got_b && /^Taken at least once:/ { got_b = 1; \
	    printf "   branches %6.2f%% of %4d\n", substr($$4, 6) + 0, $$6; next } \
	  END { if (keep && got_l && !got_b) printf "\n"; pct = tot ? 100 * hit / tot : 0; \
	    ok = (pct >= min); \
	    printf "  %-12s lines %6.2f%% of %4d -> %s\n", "TOTAL", pct, tot, ok ? "OK" : "BELOW THRESHOLD"; \
	    exit !ok }'
	@echo
	@echo "======== coverage: firmware/App (gcov, line threshold $(COV_MIN_APP)%)"
	@gcov -n -b -o $(BUILD)/app $(APP_SRCS:%=$(FW_APP)/%.c) | awk -v min=$(COV_MIN_APP) ' \
	  /^File / { if (keep && got_l && !got_b) printf "\n"; f = $$2; gsub(/\047/, "", f); sub(/.*\//, "", f); \
	    keep = ($$2 ~ /App\/[^\/]*\.c\047$$/); got_l = 0; got_b = 0; next } \
	  keep && !got_l && /^Lines executed:/ { p = substr($$2, 10) + 0; n = $$4; got_l = 1; \
	    printf "  %-12s lines %6.2f%% of %4d", f, p, n; hit += p * n / 100; tot += n; next } \
	  keep && !got_b && /^Taken at least once:/ { got_b = 1; \
	    printf "   branches %6.2f%% of %4d\n", substr($$4, 6) + 0, $$6; next } \
	  END { if (keep && got_l && !got_b) printf "\n"; pct = tot ? 100 * hit / tot : 0; \
	    ok = (pct >= min); \
	    printf "  %-12s lines %6.2f%% of %4d -> %s\n", "TOTAL", pct, tot, ok ? "OK" : "BELOW THRESHOLD"; \
	    exit !ok }'

clean:
	rm -rf $(BUILD)
	$(MAKE) -C periph_check clean
