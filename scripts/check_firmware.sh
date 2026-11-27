#!/bin/sh
# -----------------------------------------------------------------------------
# ЛР4 (H06): перевірка зібраної єдиної прошивки firmware/build/firmware.elf.
# Запуск з кореня репозиторію після make build: make fw-check (у Docker).
# (ЛР3 — правило «без RTOS» — лишилося в тегу lab3, CI збирає його окремою задачею.)
#
#   1. артефакти elf / hex / bin є;
#   2. FreeRTOS присутній: у прошивці є xTaskCreate (або xTaskCreateStatic) і
#      vTaskStartScheduler, у дереві є firmware/Middlewares/FreeRTOS; інших RTOS
#      (ThreadX, Zephyr) немає. Ядро лише частково (символи без каталогу чи навпаки) —
#      завжди помилка. Ядра немає зовсім: RTOS_REQUIRED=1 — помилка, RTOS_REQUIRED=0 —
#      попередження (перехідний режим до K02, поки в main ще Super Loop);
#   3. старт: з FreeRTOS — main → App_Init → vTaskStartScheduler лінковано;
#      без нього (перехід) — ланцюжок Super Loop ЛР3 (main → App_Init → App_Loop → Sched_RunOnce);
#   4. усі точки підключення (smartlock_hooks.h) і запит кадру — СИЛЬНІ реалізації
#      модулів (T), а не слабкі заглушки (W): це одна прошивка з усіма картками;
#   5. git-хеш: мітка build/.git_hash_<хеш> збігається, і в .elf є рядок банера
#      «BUILD,fw=SmartLock,lab=LR3|LR4,git=<хеш>,» (App_PrintBuild(), app.c) — з префіксом
#      і комою після хеша, тож не підходить ні коротший хеш, ні <хеш>-dirty, ні випадковий
#      збіг байтів. Запасний варіант (інший формат банера): окремий рядок C, рівно
#      <хеш> між нулями, — з попередженням.
#
# Змінні: GIT_HASH (з кореневого Makefile), REQUIRE_GIT=1 — «nogit» є помилкою (CI),
#         RTOS_REQUIRED=1 — прошивка без FreeRTOS є помилкою (після K02; за замовчуванням 1),
#         NM (за замовчуванням arm-none-eabi-nm), FW (каталог прошивки).
# -----------------------------------------------------------------------------

FW=${FW:-firmware}
NM=${NM:-arm-none-eabi-nm}
ELF=$FW/build/firmware.elf
GIT_HASH=${GIT_HASH:-nogit}
REQUIRE_GIT=${REQUIRE_GIT:-0}
RTOS_REQUIRED=${RTOS_REQUIRED:-1}

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

# Імена визначених символів (без адрес і типів)
NAMES=$(echo "$SYMS" | awk '{print $NF}')
has_sym() { echo "$NAMES" | grep -q -x -F "$1"; }

# 2. FreeRTOS присутній
rtos=0
create=""
for s in xTaskCreate xTaskCreateStatic; do
  has_sym "$s" && create="$create $s"
done
has_sym vTaskStartScheduler && sched=1 || sched=0
[ -d "$FW/Middlewares/FreeRTOS" ] && kdir=1 || kdir=0
OTHER_RTOS='tx_kernel_enter|tx_thread_create|k_thread_create|z_cstart'
other=$(echo "$NAMES" | grep -E "^($OTHER_RTOS)$")
if [ -n "$other" ]; then
  fail "у прошивці інша RTOS (не FreeRTOS): $(echo $other)"
fi
if [ -n "$create" ] && [ $sched -eq 1 ] && [ $kdir -eq 1 ]; then
  rtos=1
  ok "FreeRTOS:$create, vTaskStartScheduler, $FW/Middlewares/FreeRTOS"
elif [ -z "$create" ] && [ $sched -eq 0 ] && [ $kdir -eq 0 ]; then
  if [ "$RTOS_REQUIRED" = "1" ]; then
    fail "FreeRTOS немає: ні xTaskCreate / vTaskStartScheduler, ні $FW/Middlewares/FreeRTOS"
  else
    warn "FreeRTOS ще немає (RTOS_REQUIRED=0, перехід до K02) — перевіряю як Super Loop ЛР3"
  fi
else
  miss=""
  [ -n "$create" ] || miss="$miss xTaskCreate/xTaskCreateStatic"
  [ $sched -eq 1 ] || miss="$miss vTaskStartScheduler"
  [ $kdir -eq 1 ]  || miss="$miss $FW/Middlewares/FreeRTOS/"
  fail "FreeRTOS підключено частково, бракує:$miss"
fi

# T / t — визначено в цьому .elf (сильний), W / w — слабкий
sym_type() { echo "$SYMS" | awk -v s="$1" '$NF == s {print $(NF-1); exit}'; }

# 3. Старт: ядро FreeRTOS або (перехід) Super Loop ЛР3
start_missing=""
if [ $rtos -eq 1 ]; then
  chain="main App_Init vTaskStartScheduler"
  chain_msg="старт: main -> App_Init -> vTaskStartScheduler"
else
  chain="main App_Init App_Loop Sched_RunOnce AppTasks_Init"
  chain_msg="Super Loop: main -> App_Init -> while(1) App_Loop -> Sched_RunOnce"
fi
for s in $chain; do
  [ "$(sym_type "$s")" = "T" ] || start_missing="$start_missing $s"
done
if [ -n "$start_missing" ]; then
  fail "немає ланцюжка старту:$start_missing"
else
  ok "$chain_msg"
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
# ЛР3 (Super Loop) і ЛР4 (FreeRTOS) — обидва номери лаби; хеш — [0-9a-f], «-dirty», «nogit»
BANNER_PREFIX='BUILD,fw=SmartLock,lab=LR[34],git='
stamp=$(ls "$FW"/build/.git_hash_* 2>/dev/null | sed 's|.*/\.git_hash_||')
if [ "$GIT_HASH" = "nogit" ] && [ "$REQUIRE_GIT" = "1" ]; then
  fail "git-хеш невідомий (nogit): збирайте з клону репозиторію, не з розпакованого ZIP"
elif [ "$stamp" != "$GIT_HASH" ]; then
  fail "збірка з хешем '$stamp', а зараз GIT_HASH='$GIT_HASH' — make build ще раз"
elif LC_ALL=C grep -q -a -E "$BANNER_PREFIX$GIT_HASH," "$ELF"; then
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
  if [ $rtos -eq 1 ]; then
    echo "fw-check: PASS — єдина прошивка STM32F411, FreeRTOS, git=$GIT_HASH"
  else
    echo "fw-check: PASS — єдина прошивка STM32F411, Super Loop (FreeRTOS ще немає), git=$GIT_HASH"
  fi
fi
exit $status
