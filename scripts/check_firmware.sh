#!/bin/sh
# -----------------------------------------------------------------------------
# ЛР3 (I10): перевірка зібраної єдиної прошивки firmware/build/firmware.elf.
# Запуск з кореня репозиторію після make build: make fw-check (у Docker).
#
#   1. артефакти elf / hex / bin є;
#   2. без RTOS: у прошивці немає символів планувальників RTOS (FreeRTOS / CMSIS-RTOS,
#      ThreadX, Zephyr), у firmware.ioc немає FREERTOS, у дереві немає Middlewares/;
#   3. Super Loop: main → App_Init → App_Loop → Sched_RunOnce лінковано;
#   4. усі точки підключення (smartlock_hooks.h) і запит кадру — СИЛЬНІ реалізації
#      модулів (T), а не слабкі заглушки (W): це одна прошивка з усіма картками;
#   5. git-хеш: мітка build/.git_hash_<хеш> збігається, і в .elf є рядок банера
#      «BUILD,fw=SmartLock,lab=LR3,git=<хеш>,» (App_PrintBuild(), app.c) — з префіксом
#      і комою після хеша, тож не підходить ні коротший хеш, ні <хеш>-dirty, ні випадковий
#      збіг байтів. Запасний варіант (інший формат банера): окремий рядок C, рівно
#      <хеш> між нулями, — з попередженням.
#
# Змінні: GIT_HASH (з кореневого Makefile), REQUIRE_GIT=1 — «nogit» є помилкою (CI),
#         NM (за замовчуванням arm-none-eabi-nm), FW (каталог прошивки).
# -----------------------------------------------------------------------------

FW=${FW:-firmware}
NM=${NM:-arm-none-eabi-nm}
ELF=$FW/build/firmware.elf
GIT_HASH=${GIT_HASH:-nogit}
REQUIRE_GIT=${REQUIRE_GIT:-0}

status=0
ok()   { echo "fw-check: OK   — $*"; }
warn() { echo "fw-check: WARN — $*"; }
fail() { echo "fw-check: FAIL — $*"; status=1; }

# 1. Артефакти
missing=""
for f in "$ELF" "$FW/build/firmware.hex" "$FW/build/firmware.bin"; do
  [ -s "$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
  fail "немає артефактів:$missing (спочатку make build)"
  exit 1
fi
ok "артефакти: firmware.elf, firmware.hex, firmware.bin"

SYMS=$("$NM" --defined-only "$ELF") || { fail "$NM не прочитав $ELF"; exit 1; }

# 2. Без RTOS
RTOS='xTaskCreate|xTaskCreateStatic|vTaskStartScheduler|xQueueGenericCreate|osKernelInitialize|osKernelStart|osThreadNew|osThreadCreate|tx_kernel_enter|tx_thread_create|k_thread_create|z_cstart'
hits=$(echo "$SYMS" | awk '{print $NF}' | grep -E "^($RTOS)$")
if [ -n "$hits" ]; then
  fail "у прошивці є символи RTOS: $(echo $hits)"
elif grep -qi 'FREERTOS' "$FW"/*.ioc 2>/dev/null || [ -d "$FW/Middlewares" ]; then
  fail "RTOS у конфігурації CubeMX (firmware.ioc / Middlewares/)"
else
  ok "без RTOS: немає символів FreeRTOS / CMSIS-RTOS / ThreadX / Zephyr і Middlewares/"
fi

# T / t — визначено в цьому .elf (сильний), W / w — слабкий
sym_type() { echo "$SYMS" | awk -v s="$1" '$NF == s {print $(NF-1); exit}'; }

# 3. Super Loop
loop_missing=""
for s in main App_Init App_Loop Sched_RunOnce AppTasks_Init; do
  [ "$(sym_type "$s")" = "T" ] || loop_missing="$loop_missing $s"
done
if [ -n "$loop_missing" ]; then
  fail "немає ланцюжка Super Loop:$loop_missing"
else
  ok "Super Loop: main -> App_Init -> while(1) App_Loop -> Sched_RunOnce"
fi

# 4. Сильні модулі (хто власник — smartlock_hooks.h)
weak=""
for s in SmartLock_FsmStep SmartLock_ButtonsPoll SmartLock_SecurityPoll SmartLock_EnvPoll \
         SmartLock_StoragePoll SmartLock_CameraPoll SmartLock_CameraRequest SmartLock_PrintDiag \
         Camera_PrintDiag; do
  t=$(sym_type "$s")
  [ "$t" = "T" ] || weak="$weak $s(${t:-none})"
done
if [ -n "$weak" ]; then
  fail "слабкі або відсутні реалізації модулів:$weak"
else
  ok "усі 9 точок підключення — сильні реалізації модулів (FSM, buttons, security, env, journal, camera, diag)"
fi

# 5. git-хеш
# Префікс — як прошивка друкує хеш у першому рядку UART і на VER (app.c, App_PrintBuild)
BANNER_PREFIX='BUILD,fw=SmartLock,lab=LR3,git='
stamp=$(ls "$FW"/build/.git_hash_* 2>/dev/null | sed 's|.*/\.git_hash_||')
if [ "$GIT_HASH" = "nogit" ] && [ "$REQUIRE_GIT" = "1" ]; then
  fail "git-хеш невідомий (nogit): збирайте з клону репозиторію, не з розпакованого ZIP"
elif [ "$stamp" != "$GIT_HASH" ]; then
  fail "збірка з хешем '$stamp', а зараз GIT_HASH='$GIT_HASH' — make build ще раз"
elif LC_ALL=C grep -q -a -F "$BANNER_PREFIX$GIT_HASH," "$ELF"; then
  case "$GIT_HASH" in
    nogit)   warn "git-хеш невідомий (nogit): каталог без .git (ZIP?) — для звіту збирайте з клону" ;;
    *-dirty) warn "git=$GIT_HASH: є незакомічені зміни — для звіту збирайте чистий коміт" ;;
    *)       ok "git=$GIT_HASH у firmware.elf (рядок BUILD; той самий GIT_HASH — STATS_BEGIN, запис BOOT)" ;;
  esac
elif tr '\000' '\n' < "$ELF" | LC_ALL=C grep -q -a -x -F "$GIT_HASH"; then
  warn "рядка '$BANNER_PREFIX$GIT_HASH,' немає (інший формат банера?), але є окремий рядок '$GIT_HASH'"
else
  fail "рядка '$BANNER_PREFIX$GIT_HASH,' (і окремого рядка '$GIT_HASH') немає в firmware.elf"
fi

if [ $status -eq 0 ]; then
  echo "fw-check: PASS — єдина прошивка STM32F411, Super Loop без RTOS, git=$GIT_HASH"
fi
exit $status
