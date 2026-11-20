# ЛР3 — єдина прошивка SmartLock: збірка, перевірка, стенд (I10)

Одна прошивка STM32F411 (Nucleo-F411RE), без RTOS: усі картки ЛР3 (K02–K08, H09/H10,
I02–I09) зібрані в один `firmware.elf`, який збирає CI на кожен pull request.
Документ відповідає на три питання: **що** входить у прошивку, **як** її зібрати й
перевірити без плати, і **як** запустити її на стенді та зберегти логи для звіту.

## 1. Склад прошивки

```
main.c (CubeMX)  HAL_Init → SystemClock_Config → MX_*_Init
                 App_Init()          BUILD,git=…  → self-test ЛР2 → журнал і кадри W25Q64
                                     → CAM,init (OV7670, AEC/AWB) → таблиця задач → STACK/HEAP
                 while (1) App_Loop()   Platform_Poll → Sched_RunOnce → App_SensorService
```

RTOS немає: немає потоків, черг ОС і перемикання контексту. Одна гілка виконання —
нескінченний цикл у `main.c`, переривання (EXTI) лише ставлять мітки часу і прапорці,
які забирають задачі циклу (I04). Байти команд приймає DMA у кільце 2048 Б
(`Platform/uart_rx.c`, `docs/lab_3/uart-rx.md`) — без втрат під паузами циклу.

**Задачі Super Loop** (`app_tasks.c`, порядок = порядок у `STATS`):

| # | Задача | Період | Що робить | Модуль (картка) |
|---|---|---|---|---|
| 0 | `fsm` | кожен прохід | FSM замка, EXIT_DELAY 15 с, тривога | `sl_fsm.c`, `sl_camera_link.c` (K02–K05, K08) |
| 1 | `buttons` | 10 мс | B1 (LOCK), код з 4 кнопок, антидребезг | `sl_buttons.c` (K03/K04) |
| 2 | `security` | 20 мс | PIR, MPU-6050, геркон | `sl_security.c` (K05) |
| 3 | `env` | 2 с | DHT11, BMP180, ENV_WARNING | `sl_env.c` (K06) |
| 4 | `camera` | за запитом | кадр охоронної події → W25Q64 | `camera.c` (I09) |
| 5 | `storage` | кожен прохід | черга журналу → W25Q64 | `sl_journal.c` → `storage.c` (K07, H09) |
| 6 | `mem` | 1 с | `STACK` / `HEAP` при новому максимумі | `app_tasks.c` (I06) |
| 7 | `uart` | кожен прохід | команди, `EVT`, `LOAD`, експорт кадру | `cmd.c`, `evt.c`, `frame_export.c` (I03, I04, I08) |

Кожна задача — сильна реалізація точки підключення з `smartlock_hooks.h`; слабкі
заглушки (`smartlock_hooks_weak.c`) у прошивку не потрапляють. Це перевіряє
`make fw-check` (розділ 3) по символах готового `firmware.elf`.

## 2. Що потрібно

* **Клон репозиторію** (`git clone`), а не розпакований ZIP: без каталогу `.git` хеш
  збірки буде `nogit` (розділ 4).
* Docker Desktop (образ `stm32-builder` з `docker/Dockerfile`: `arm-none-eabi-gcc`,
  `gcc`, `gcov`, `python3`). Тулчейн на ПК ставити не треба.
* Git Bash (Windows) або будь-який shell з `make` (Linux, macOS).
* Для стенда: STM32CubeProgrammer (Windows) або `st-flash` (Linux) і термінал 115200 8N1
  з записом у файл (PuTTY, `picocom`, `python -m serial.tools.miniterm`). Шлях до
  прошивальника і COM-порт — у `local.mk` (шаблон `local.mk.example`).

## 3. Збірка й перевірка на ПК

Усе одразу — ті самі кроки, що в CI (`.github/workflows/ci.yml`):

```sh
cd SP_Labs              # клон, не ZIP
git pull
make version            # який хеш отримає прошивка: GIT_HASH=<хеш>
make lab3 2>&1 | tee lab3_$(git rev-parse --short HEAD).log
```

Останній рядок при успіху:

```
LAB3: PASS git=<хеш> — check-deps, build, fw-check, mem-report, host-checks (test, coverage, tools, python)
```

Кроки окремо (якщо щось упало — запускати той, що впав):

| Команда | Що перевіряє | Ознака успіху |
|---|---|---|
| `make check-deps` | правила залежностей Core / Platform / Devices / App | 4 × `check-deps: OK` |
| `make build` | `arm-none-eabi-gcc -Wall -Wextra -Werror`, лінкування під STM32F411 (512 КБ Flash, 128 КБ RAM) | `Memory region … RAM / FLASH`, `firmware.elf`, `.hex`, `.bin` |
| `make fw-check` | **єдина прошивка**: без RTOS, Super Loop лінковано, усі 9 точок підключення сильні, git-хеш у `.elf` | `fw-check: PASS — єдина прошивка STM32F411, Super Loop без RTOS, git=<хеш>` |
| `make mem-report` | пам'ять для M04: `firmware/build/mem/*.txt` | файли `size.txt`, `top_ram.txt`, `stack_usage.txt` … |
| `make test` | host-тести на моделях заліза: `periph_check`, 29 наборів Unity, `self_test` | кожен набір `N Tests 0 Failures`, наприкінці `ALL TESTS PASSED` |
| `make coverage` | покриття gcov: Devices, App ≥ 90 % рядків, `camera_dvp.c` | усі `TOTAL … -> OK` |
| `make tools-test` | експорт кадру C → UART-лог → `tools/frame_decode.py` → PNG | `frame_decode: OK` |
| `make host-checks` | I11: test + coverage + tools-test + Python-тести одним запуском, лог `tests/build/I11_host_checks.log` (`docs/lab_3/host-tests.md`) | `I11 HOST CHECKS: PASS — …` |

Що саме перевіряє `make fw-check` (`scripts/check_firmware.sh`, `arm-none-eabi-nm`):

```
fw-check: OK   — артефакти: firmware.elf, firmware.hex, firmware.bin
fw-check: OK   — без RTOS: немає символів FreeRTOS / CMSIS-RTOS / ThreadX / Zephyr і Middlewares/
fw-check: OK   — Super Loop: main -> App_Init -> while(1) App_Loop -> Sched_RunOnce
fw-check: OK   — усі 9 точок підключення — сильні реалізації модулів (FSM, buttons, security, env, journal, camera, diag)
fw-check: OK   — git=<хеш> у firmware.elf (рядок BUILD; той самий GIT_HASH — STATS_BEGIN, запис BOOT)
fw-check: PASS — єдина прошивка STM32F411, Super Loop без RTOS, git=<хеш>
```

Якщо модуль якоїсь картки не потрапив у збірку, його точка підключення лишається
слабкою (`W`) — `fw-check` назве її: `FAIL — слабкі або відсутні реалізації модулів: …`.

**Host-тест єдиної прошивки** — `tests/unit/test_firmware.c` (`test_firmware`). Він
лінкує рівно ті `App/*.c`, що й `firmware.mk` (wildcard), і крутить `App_Loop()` як
`main.c`:

| Тест | Перевіряє |
|---|---|
| `test_boot_banner_tasks_and_strong_modules` | перший рядок `BUILD,…,git=<GIT_HASH>`, `CAM,init,status=ok` при старті, 8 задач у порядку таблиці, FSM / камера / DIAG — сильні |
| `test_ver_and_help` | `VER` = рядок `BUILD`; `HELP` містить `VER` і `CAM LOAD` |
| `test_locked_motion_gives_photo_in_flash_and_clean_stats` | LOCK → 15 с → LOCKED → PIR → `alarm_triggered` → `CAM,photo` → `PHOTO` у журналі → `CAM LOAD 1` з тим самим ID події; `STATS`: `DIAG` без втрат |
| `test_one_hour_of_super_loop` | 360 000 проходів = 1 год модельного часу: час монотонний (±1 с), черги, журнал, EVT без втрат |

**Орієнтири пам'яті** (`make mem-report` на `main` з I09 і фіксом UART): FLASH 65 484 Б
(12,5 %), RAM 50 008 Б (38,2 %), з них буфер кадру `s_frame` 38 400 Б і кільце прийому
UART `s_buf` 2048 Б; найглибший кадр стека — `dht11_line`, 376 Б. Таблиця AEC/AWB камери
і I10 разом додають < 1,5 КБ Flash (≈ 200 Б таблиці, рядки `DIAG`, `VER`) і не додають RAM.

## 4. Git-хеш збірки

Кореневий `Makefile` на ПК бере `git rev-parse --short HEAD` (+ `-dirty`, якщо є
незакомічені зміни відстежуваних файлів) і передає в Docker як `GIT_HASH`
(`make version` показує його). Хеш потрапляє в прошивку як рядок і видний:

| Де | Рядок |
|---|---|
| перший рядок UART після скидання | `BUILD,fw=SmartLock,lab=LR3,git=<хеш>,cpu_hz=96000000,loop=superloop` |
| команда `VER` (термінал відкрито після старту) | той самий рядок `BUILD` |
| `STATS` | `STATS_BEGIN git=<хеш> t_us=… loops=…` |
| журнал W25Q64 | запис `BOOT` (arg — згортка хеша, `storage.c`) |

Зміна хеша перезбирає всі файли, що його друкують (`firmware.mk`: `GIT_USERS` — кожен
`App/*.c` з `GIT_HASH`), тож `BUILD` і `STATS_BEGIN` не можуть показати різні хеші.

| Хеш | Що означає | Для звіту |
|---|---|---|
| `a1b2c3d` | збірка точно з коміту `a1b2c3d` | так |
| `a1b2c3d-dirty` | коміт `a1b2c3d` + незакомічені правки | ні — закомітити й перезібрати |
| `nogit` | збирали з каталогу без `.git` (ZIP) | ні — збирати з клону |

CI запускає `make fw-check REQUIRE_GIT=1`: там `nogit` — помилка. Локально — лише
попередження. `fw-check` шукає в `.elf` не голий хеш, а рядок банера
`BUILD,fw=SmartLock,lab=LR3,git=<хеш>,` (у `App_PrintBuild()` хеш вклеєно в рядок
формату): коротший хеш, `<хеш>-dirty` чи випадковий збіг байтів не пройдуть.

**Хеш у CI** (`.github/workflows/ci.yml`): на pull request збирається **голова PR**
(`github.event.pull_request.head.sha`), а не тимчасовий коміт злиття `refs/pull/N/merge`,
на push у `main` — `github.sha`. Короткий хеш (`git rev-parse --short`) передається
явно: `make build|fw-check|mem-report|host-checks GIT_HASH=<хеш>`. Локально
`GIT_HASH` не задають — його обчислює `Makefile` (вище).

**Відповідність плата ↔ коміт:** `VER` у терміналі → `git=<хеш>` → `git show <хеш>`
(або сторінка коміту на GitHub). Артефакт CI **`firmware-<хеш>`** (Actions → запуск →
Artifacts): `firmware.elf`, `.hex`, `.bin`, `.map`, `mem/*.txt` (`memory_usage.txt` з
`--print-memory-usage` і звіти `make mem-report`) і `I11_host_checks.log` — та сама
прошивка, що й на платі з цим хешем.

## 5. Стенд

### 5.1 Підключення і прошивання

1. Схема — `docs/pinout.md` (таблиця §1, схема §5). Серво — від окремого 5 В, земля
   спільна. Камера OV7670: XCLK = PA8, D0–D7 = PC0–PC7, PCLK = PA10, VSYNC = PC8.
2. Nucleo через USB (ST-LINK). `make flash` — збирає і прошиває
   (`STM32_Programmer_CLI -c port=SWD -w … -v -rst` / `st-flash`). Або
   STM32CubeProgrammer вручну: `firmware/build/firmware.hex`, «Verify», «Run after».
3. Термінал 115200 8N1, **запис у файл** з самого початку (PuTTY: Session → Logging →
   All session output; Linux: `picocom -b 115200 --imap lfcrlf /dev/ttyACM0 | tee log.txt`).
   Ім'я файлу з хешем: `docs/lab_3/logs/I10_stand_<хеш>.txt`.
4. Натиснути чорну кнопку RESET на Nucleo.

### 5.2 Сценарій перевірки (≈ 20 хв + 1 год без втручання)

Це перевірка єдиної прошивки за сценарієм типу **S2** (номінальна робота + година
циклу). Простій **S1** і найгірший випадок **S3** — §5.5, на тій самій збірці.

| Крок | Дія | Очікувано в лозі |
|---|---|---|
| 1 | RESET | перший рядок `BUILD,…,git=<хеш>,cpu_hz=96000000,loop=superloop`; self-test ЛР2 (`ST,…`, `ST,SUMMARY,…`); `LOG,MOUNT,result=ok,…`; `CAM,init,status=ok,pid=0x76,ver=0x73,…`; `STACK …`, `HEAP …` |
| 2 | `VER` + Enter | той самий `BUILD`; `<хеш>` = `make version` / `git rev-parse --short HEAD` |
| 3 | `STATS` | `STATS_BEGIN git=<хеш>`; `STAT name=loop`; по `STAT` на кожну з 8 задач; `EVQ … dropped=0`; 7 рядків `DIAG name=fsm/buttons/security/env/journal/camlink/camera`; `STATS_END` |
| 4 | B1 (синя), двері зачинені (магніт біля геркона) | `EVT,…,name=LOCK_ACTIVATED`, через 15 с — `LOCKED`, серво в LOCK |
| 5 | рух перед PIR | `EVT,id=<N>,…,name=MOTION`, `EVT t_us=… name=alarm_triggered`, `CAM,photo,event=<N>,status=ok,…`, `EVT,…,name=PHOTO,arg=<кадр>,ref=<N>,flash=ok` |
| 6 | код 1-3-2-4 (кнопки коду, `TBD_CODE_VALUE`) | `ACCESS_GRANTED`, `UNLOCKED`, серво в UNLOCK |
| 7 | `CAM LOAD <кадр>`, `CAM EXPORT` | `CAM,load,…,status=ok,event=<N>`; на ПК `python3 tools/frame_decode.py log.txt --out frame.png` → `OK` |
| 8 | вимкнути живлення, увімкнути; `CAM LOAD <кадр>` | той самий `event=<N>` (кадр і зв'язок з подією пережили перезапуск) |
| 9 | залишити стенд на ≥ 1 год, потім `STATS` | `loops=` ≈ 1 год / період циклу; `DIAG`: `dropped=0`, `lost=0`, `unsaved=0`, `post_failed=0`, `timeouts=0`; `LOAD pct=` щосекунди |

Детальні кроки камери (PulseView, `CAM TEST`, час експорту) — `docs/camera.md` §7
(H14). Службові піни для аналізатора: PB0 — прохід циклу, PB1 — реакція на подію,
PB2 — захоплення кадру, PB12 — запис кадру у W25Q64 (`docs/superloop.md` §4).

**Рядки `DIAG` у `STATS`** (I10, `sl_diag.c`): лічильники модулів, які до I10 були
видні лише в тестах (`fsm-access-control.md`):

```
DIAG name=fsm state=UNLOCKED ignored=0 cam_rejected=0
DIAG name=buttons dropped=0 late=0 key_lost=0
DIAG name=security mpu_ok=1 mpu_err=0 mpu_zero=0 lost=0 door_retries=0
DIAG name=env dht_err=0 bmp_err=0 warnings=0 post_failed=0
DIAG name=journal pending=0 max_depth=2 stored=5 unsaved=0 dropped=0 last_seq=6
DIAG name=camlink requested=1 rejected=0 ok=1 failed=0 timeouts=0 unknown=0 no_id=0 pending=0
DIAG name=camera accepted=1 rejected=0 stored=1 capture_err=0 store_err=0 pending=0
```

Будь-яке ненульове `dropped` / `lost` / `unsaved` / `post_failed` / `timeouts` після
сценарію — дефект, який треба описати в звіті (I12).

### 5.3 Логи для звіту

| Файл | Звідки |
|---|---|
| `lab3_<хеш>.log` | `make lab3 2>&1 \| tee …` (розділ 3) |
| `docs/lab_3/logs/I10_stand_<хеш>.txt` | термінал під час 5.2 |
| `firmware/build/mem/*.txt` | `make mem-report` — для M04 |
| PNG кадру | `tools/frame_decode.py` (крок 7) |
| `docs/metrics/S1_uart_raw.txt`, `S2_…`, `S3_…` + умови | прогони S1–S3 (§5.5, `test-scenarios.md`) |
| `docs/lab_3/logs/I11_host_checks_<хеш>.log` | артефакт CI `firmware-<хеш>` (`host-tests.md` §4) |

Хеш у назвах файлів, у `BUILD` і в `lab3_….log` має бути один і той самий.

### 5.4 Якщо не вийшло

| Симптом | Дія |
|---|---|
| `git=nogit` | збирали з ZIP — `git clone`, `make build` з клону |
| `git=…-dirty` | `git status` — закомітити або відкотити правки, перезібрати |
| `fw-check: FAIL — збірка з хешем '…'` | після коміту не перезібрали — `make build` (або `make lab3`) |
| `fw-check: FAIL — слабкі … реалізації` | модуль картки не в гілці — `git log`, злити гілку картки |
| у терміналі нічого | COM-порт ST-LINK VCP, 115200 8N1; RESET після відкриття терміналу або `VER` |
| `BUILD` є, далі зависання | self-test чекає шину I2C — `docs/lab_3/driver-audit.md`; живлення модулів |
| `LOG,MOUNT,err=…` | W25Q64: CS# = PB10, SPI2 PB13–15; без Flash журнал і кадри вимкнено (`flash=err`) |
| `CAM,photo,…,status=capture_err` | камера — `docs/camera.md` §7 «Якщо не вийшло» |
| `DIAG name=modules present=0` | зібрано без модулів `sl_*` (тестова збірка) — у прошивці так бути не може, `make fw-check` |

### 5.5 Сценарії вимірювань S1–S3 на тій самій збірці

Протоколи — `docs/lab_3/test-scenarios.md` (S1, S2, S3), визначення сценаріїв і
стрес-сценаріїв §6.2 — `docs/metrics/metrics-spec.md` §4, формати рядків — там само §3.
Тут лише те, що стосується **цієї** прошивки; протоколи не дублюються.

**Спільне для S1, S2, S3:**

1. Одна збірка: `make lab3` один раз, прошити, далі **не перезбирати** між прогонами.
   Хеш — з першого рядка `BUILD,…,git=<хеш>` (або `VER`); він має бути однаковим у
   всіх трьох логах і без `-dirty` / `nogit` (§4).
2. Запис UART з першого байта. Порядок запуску, команди запису, куди класти файли і
   реєстр прогонів — `docs/lab_3/bench-runs.md` (Колодницький, PR #40): там
   `scripts/uart_capture.py` пише сирий `<name>_uart_raw.txt`, ts-файл і
   `<name>_conditions.md` з хешем з `BUILD`. Поки PR #40 не злито — термінал із
   записом у файл (§5.1, крок 3). Після цієї збірки в `bench-runs.md` змінюються
   передумови «Єдина прошивка» і «PASS збірки, CI і host-тестів» (I10, I11).
3. Що записати в умови прогону (`test-scenarios.md`, «Спільні умови»): git-хеш, дата,
   тривалість, початковий стан FSM, 115200 8N1, порт, що підключено / відключено
   (камера, Flash), час і вид кожної ручної дії, команди, що надсилались.
4. Наприкінці — `STATS` (рядки `STAT`, `JITTER`, `CPU`, `EVQ`, `UART`, `DIAG`) і `MEM`.
   `STATS` сам друкує ≈ 2,3 КБ ≈ 0,2 с (блокувальний `printf`) — у S1 надсилати лише
   наприкінці.

**S1 — простій** (`test-scenarios.md` §1): RESET → дочекатися `CAM,init`, `STACK` →
`UNLOCKED`, двері закриті, нічого не торкатися 10 хв, жодних команд → `STATS`, `MEM`.
Очікувано: немає `MOTION` / `TAMPER` / `CAM,photo`; `STAT name=camera n=0`,
`STAT name=flash_frame n=0`; щосекунди `LOAD pct=…`, раз на 2 с `ENV,…`.

**S3 — найгірший випадок** (`test-scenarios.md` §3): `LOCKED` (B1 → 15 с), далі
одночасно:

| Складова | Як отримати на цій прошивці |
|---|---|
| довга операція камера + Flash | подія в `LOCKED` (PIR / удар MPU-6050) → задача `camera`: захоплення ≈ 1,0–2,1 с + запис кадру у W25Q64 ≈ 0,83–0,86 с одним проходом (логи 21, 23); `CAM TEST` — лише захоплення; `CAM EXPORT` — ≈ 10,6 с передачі по UART (кадр тривоги в цей час відхиляється, `PHOTO_ERR arg=1`, лог 23) |
| UART-навантаження | `scripts/uart_capture.py … --name S3 --plan S3 --load-rate 20 --load-profile mixed --char-gap 0 --retries 1` — запис і навантаження одним процесом, бо порт один (`docs/lab_3/bench-runs.md`, PR #40); без пауз — `--load-burst` (методичка §6.2.3). Профілі ті самі, що в `scripts/uart_load.py` (Собкович, `docs/metrics/uart-load-guide.md`) |
| зовнішні події | 30 окремих PIR / B1 під час довгих операцій, моменти — PulseView: PA0 (PIR), PC13 (B1), PB1 (реакція), PB2 (захоплення), PB12 (запис кадру) (§5.2, `docs/superloop.md` §4) |

Окремий термінал на той самий порт не потрібен (і не відкриється): усе пише `uart_capture.py`,
підсумок навантаження (надіслано, `PONG`, фактичний темп) — у `S3_conditions.md`. Записати: параметри
навантаження (`--load-rate`, `--load-profile`, `--load-burst`), підсумок `PING`/`PONG`,
рядок `UART rx_overrun=… cmd_overflow=… cmd_dropped=… rx_peak=…`, усі `CAM,photo`
(`capture_us`, `store_us`, `aec=`), `OVR name=pir|btn_b1|mpu`, `DIAG name=camlink|camera`,
`.sr` PulseView з частотою дискретизації і каналами. Що з цього очікувано погіршиться
на цій прошивці — `docs/lab_3/host-tests.md` §7.
