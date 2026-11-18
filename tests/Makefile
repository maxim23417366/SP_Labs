# -----------------------------------------------------------------------------
# Хостові тести: звичайний gcc, без ARM-тулчейну і без плати.
# З кореня репозиторію запускаються в Docker: make test / make coverage
#
#   make run       — periph_check (ЛР1) + unit-тести драйверів і App/ (Unity)
#                    + тест форматування self-test (selftest)
#                    + ЛР3 (I07): OV7670 і алгоритм захоплення DVP (Platform/camera_dvp.c)
#   make coverage  — unit-тести + покриття firmware/Devices/*.c (поріг COV_MIN)
#                    і firmware/App/*.c (поріг COV_MIN_APP), gcov
#   make run-<name> — зібрати і запустити один тест, напр. make run-sl_fsm
#                    (unit/test_<name>.c); run-hooks_weak — слабкі хуки без sl_*
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

DRIVERS := bmp180 dht11 lsm303d mpu6050 w25q64 crc32 flash_log frame_store ov7670
DOUBLES := fake_i2c mock_spi fake_delay fake_w25q64

# Модулі Devices/, що працюють поверх інших: тест лінкується і з ними
DRV_DEPS_flash_log   := w25q64 crc32
DRV_DEPS_frame_store := w25q64 crc32

DRV_OBJS  := $(DRIVERS:%=$(BUILD)/dev/%.o)
DBL_OBJS  := $(DOUBLES:%=$(BUILD)/doubles/%.o)
UNITY_OBJ := $(BUILD)/vendor/unity.o
DRV_BINS  := $(DRIVERS:%=$(BUILD)/test_%)

# Тести App/ (перевірки стенда і команди без обладнання): кожен лінкується з усім App/,
# усіма драйверами і fake_platform (імітація platform.h)
# ЛР3 (K02–K09): модулі sl_* і їхні тести; кожен власник картки редагує лише свої файли
# ЛР3 (I04): evt — подієві мітки EVT; (I07): cam_test — CAM TEST, fake_camera;
# (I08): frame_export — експорт кадру на ПК
APP_SRCS  := app bench self_test stand_test perf sched app_tasks smartlock_hooks_weak event_queue stats cmd evt cam_test frame_export \
             sl_journal sl_camera_link sl_fsm sl_buttons sl_security sl_env storage storage_ops frames \
             sl_camera_frames
APP_TESTS := stand_test app sched fakes_lr3 sl_journal sl_fsm sl_buttons sl_security sl_env storage frames cam_test \
             sl_camera_frames sl_integration frame_export
APP_DBLS  := fake_platform fake_sensors fake_camera
APP_OBJS  := $(APP_SRCS:%=$(BUILD)/app/%.o) $(APP_DBLS:%=$(BUILD)/doubles/%.o)
APP_BINS  := $(APP_TESTS:%=$(BUILD)/test_%)
APP_HDRS  := $(wildcard $(FW_APP)/*.h) $(FW_PLAT)/platform.h
COV_MIN_APP ?= 90

# Слабкі точки підключення (smartlock_hooks_weak.c): окремий бінарник БЕЗ модулів
# sl_* — інакше лінкер бере сильні реалізації і слабкі ніде не виконуються
HOOKS_WEAK_SRCS := $(filter-out sl_%,$(APP_SRCS))
HOOKS_WEAK_OBJS := $(HOOKS_WEAK_SRCS:%=$(BUILD)/app/%.o) $(APP_DBLS:%=$(BUILD)/doubles/%.o)
HOOKS_WEAK_BIN  := $(BUILD)/test_hooks_weak

# ЛР3 (I07): алгоритм захоплення кадру з Platform/ на моделі сигналів DVP (fake_dvp)
DVP_BIN   := $(BUILD)/test_camera_dvp
DVP_OBJS  := $(BUILD)/plat/camera_dvp.o $(BUILD)/doubles/fake_dvp.o $(BUILD)/doubles/fake_platform.o

BINS      := $(DRV_BINS) $(APP_BINS) $(HOOKS_WEAK_BIN) $(DVP_BIN)

DEV_HDRS  := $(wildcard $(FW_DEV)/*.h)
DBL_HDRS  := $(wildcard doubles/*.h)

.PHONY: all run periph unit selftest coverage tools clean
.SECONDARY: $(DRV_OBJS) $(DBL_OBJS) $(APP_OBJS) $(UNITY_OBJ) $(DVP_OBJS)

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

# ЛР3 (I08): C → лог → Python → PNG. test_frame_export експортує тестовий кадр через
# задачу uart Super Loop у build/frame_export_host.log; frame_decode.py відновлює його,
# перевіряє CRC і звіряє побайтно з малюнком CAM PATTERN. Потрібен python3.
PYTHON ?= python3
tools: $(BUILD)/test_frame_export
	./$(BUILD)/test_frame_export
	$(PYTHON) ../tools/test_frame_decode.py
	$(PYTHON) ../tools/frame_decode.py $(BUILD)/frame_export_host.log \
		--out $(BUILD)/frame_export_host.png --expect-test-pattern

unit: $(BINS)
	@set -e; for t in $(BINS); do echo; echo "======== $$t"; ./$$t; done

run-%: $(BUILD)/test_%
	./$<

$(BUILD)/dev/%.o: $(FW_DEV)/%.c $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_OWN) $(COVFLAGS) -c $< -o $@

$(BUILD)/doubles/%.o: doubles/%.c $(DBL_HDRS) $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_OWN) -c $< -o $@

$(BUILD)/app/%.o: $(FW_APP)/%.c $(APP_HDRS) $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_APP) $(COVFLAGS) -c $< -o $@

$(APP_DBLS:%=$(BUILD)/doubles/%.o) $(BUILD)/doubles/fake_dvp.o: $(BUILD)/doubles/%.o: doubles/%.c $(DBL_HDRS) $(APP_HDRS) $(DEV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_APP) -c $< -o $@

# ЛР3 (I07): справжній Platform/camera_dvp.c, але з fake_dvp_port.h замість регістрів GPIO
$(BUILD)/plat/camera_dvp.o: $(FW_PLAT)/camera_dvp.c $(FW_PLAT)/platform.h doubles/fake_dvp_port.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_APP) -DPLATFORM_DVP_FAKE $(COVFLAGS) -c $< -o $@

$(DVP_BIN): unit/test_camera_dvp.c $(DVP_OBJS) $(DBL_OBJS) $(UNITY_OBJ)
	$(CC) $(CFLAGS_APP) $(COVFLAGS) $< $(DVP_OBJS) $(DBL_OBJS) $(UNITY_OBJ) -o $@

$(UNITY_OBJ): $(UNITY)/unity.c $(UNITY)/unity.h $(UNITY)/unity_internals.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_VENDOR) -c $< -o $@

# Тест sl_journal підставляє власний SmartLock_JournalOps() — без storage_ops.o
APP_OBJS_sl_journal := $(filter-out $(BUILD)/app/storage_ops.o,$(APP_OBJS))

.SECONDEXPANSION:
$(APP_BINS): $(BUILD)/test_%: unit/test_%.c $$(or $$(APP_OBJS_$$*),$(APP_OBJS)) $(DRV_OBJS) $(DBL_OBJS) $(UNITY_OBJ) $(wildcard vectors/*.h)
	$(CC) $(CFLAGS_APP) $(COVFLAGS) $< $(or $(APP_OBJS_$*),$(APP_OBJS)) $(DRV_OBJS) $(DBL_OBJS) $(UNITY_OBJ) -o $@

$(HOOKS_WEAK_BIN): unit/test_hooks_weak.c $(HOOKS_WEAK_OBJS) $(DRV_OBJS) $(DBL_OBJS) $(UNITY_OBJ)
	$(CC) $(CFLAGS_APP) $(COVFLAGS) $< $(HOOKS_WEAK_OBJS) $(DRV_OBJS) $(DBL_OBJS) $(UNITY_OBJ) -o $@

$(DRV_BINS): $(BUILD)/test_%: unit/test_%.c $(BUILD)/dev/%.o $$(addprefix $(BUILD)/dev/,$$(addsuffix .o,$$(DRV_DEPS_$$*))) $(DBL_OBJS) $(UNITY_OBJ) $(wildcard vectors/*.h)
	$(CC) $(CFLAGS_OWN) $(COVFLAGS) $< $(BUILD)/dev/$*.o $(addprefix $(BUILD)/dev/,$(addsuffix .o,$(DRV_DEPS_$*))) $(DBL_OBJS) $(UNITY_OBJ) -o $@

# Покриття рахується для шару драйверів і App/: тести і test doubles не входять.
# Підсумок по рядках порівнюється з COV_MIN / COV_MIN_APP; гілки виводяться для звіту.
# Файл без гілок не має рядка "Taken at least once" — тоді рядок звіту
# завершується при переході до наступного файлу.
coverage:
	rm -f $(BUILD)/dev/*.gcda $(BUILD)/app/*.gcda $(BUILD)/plat/*.gcda
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
	@echo
	@echo "======== coverage: firmware/Platform/camera_dvp.c (gcov, line threshold $(COV_MIN_APP)%)"
	@gcov -n -b -o $(BUILD)/plat $(FW_PLAT)/camera_dvp.c | awk -v min=$(COV_MIN_APP) ' \
	  /^File / { if (keep && got_l && !got_b) printf "\n"; f = $$2; gsub(/\047/, "", f); sub(/.*\//, "", f); \
	    keep = ($$2 ~ /Platform\/[^\/]*\.c\047$$/); got_l = 0; got_b = 0; next } \
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
