# ЛР3 — host-тести і лог CI (I11)

Усе, що можна перевірити без плати, запускається **однією командою** і дає **один лог** з
підсумком — той самий локально і в CI на кожен pull request. Лог іде в звіт (I12) як
доказ: які тести, скільки, з яким покриттям і на якому коміті пройшли.

## 1. Запуск

```sh
cd SP_Labs                       # клон репозиторію (не ZIP — інакше git=nogit)
make host-checks                 # Docker-образ stm32-builder, ~1 хв
```

Останні рядки при успіху:

```
I11 HOST CHECKS: PASS — 29 наборів Unity / 446 тестів, periph_check, self_test, покриття, tools, python
лог: tests/build/I11_host_checks.log
```

Код виходу 0 — усе пройшло, 1 — хоч одна перевірка впала (і в лозі рядки `FAIL: …`).
`make lab3` (I10) викликає `host-checks` після збірки і `fw-check`.

Без Docker (Linux / WSL з `gcc`, `gcov`, `python3`):
`python3 scripts/host_checks.py --git $(git rev-parse --short HEAD)`.

## 2. Що виконується

`scripts/host_checks.py`, кроки по черзі (падіння одного не зупиняє наступні — у лозі
видно все):

| Крок | Команда | Що перевіряє |
|---|---|---|
| `deps` | `scripts/check_deps.sh` | Devices/ без HAL, App/ без HAL і внутрішніх файлів Platform, Platform/ без App/ |
| `unit` | `make -C tests periph selftest coverage` | `periph_check` (ЛР1, моки HAL), усі 29 наборів Unity, `self_test`, покриття gcov з порогом 90 % рядків |
| `tools` | `make -C tests tools` | кадр із прошивки (C) → UART-лог → `tools/frame_decode.py` → PNG, побайтно з тестовим малюнком |
| `python` | `unittest discover` у `scripts/` і `tools/` | Python-інструменти стенда: розбір UART-логів, навантаження UART, декодер кадру, сам `host_checks.py` |

## 3. Лог `I11_host_checks.log`

```
I11 HOST CHECKS git=<хеш> utc=<час UTC>
  tool: gcc …  / gcov … / Python … / arm-none-eabi-gcc …
==================== I11 крок: deps ====================
$ <команда>
<повний вивід>
… (unit, tools, python)
==================== I11: ПІДСУМОК HOST-ПЕРЕВІРОК ====================
  deps    exit=0   0.0 s           ← код виходу і час кожного кроку
  Unity: 29 наборів, 446 тестів, 0 падінь
    test_bmp180   14 тестів  0 падінь   ← по рядку на набір
    …
  periph_check: 82 перевірок, 0 падінь
  self_test: PASS
  Покриття (рядки):  firmware/Devices 99.88 % …  OK
  tools: frame_decode OK
  python: 20 тестів, OK
I11 HOST CHECKS: PASS — …
```

Підсумок рахує сам скрипт з виводу тестів і **не пропускає** падінь, які легко
проґавити очима: набір з `Failures > 0`; набір, що аварійно завершився без рядка
`N Tests N Failures` (segfault); покриття нижче порогу; `periph_check`, `self_test`,
`frame_decode` чи Python з помилкою. Це перевіряють `scripts/test_host_checks.py`
(8 тестів на зразках виводу).

## 4. Лог з CI

На кожен PR і push у `main` (`.github/workflows/ci.yml`): крок
**Host checks - tests, coverage, tools, python (I11)** і артефакт **`I11_host_checks`**.

1. GitHub → **Actions** → запуск потрібного коміту (у назві — повідомлення коміту).
2. Унизу сторінки запуску — **Artifacts** → `I11_host_checks` (zip з
   `I11_host_checks.log`). Артефакт зберігається і тоді, коли крок упав (`if: always()`).
3. Для звіту — покласти лог у `docs/lab_3/logs/I11_host_checks.log` (або
   `I11_host_checks_<хеш>.log`). Хеш у першому рядку логу CI — коміт злиття PR або
   коміт `main`; його й наводити у звіті.

## 5. Набори тестів

| Набір | Тестів | Що перевіряє | Картки |
|---|---|---|---|
| `periph_check` | 82 перев. | перевірка периферії ЛР1 на моках HAL: шини, такти, ШІМ, printf → USART2 | ЛР1 |
| `test_bmp180` | 14 | BMP180: приклад із datasheet, калібрування, помилки шини | ЛР2 |
| `test_dht11` | 13 | DHT11: розбір кадру, контрольна сума, таймаути | ЛР2 |
| `test_lsm303d` | 9 | LSM303D: ідентифікація, автоінкремент, перетворення | ЛР2 |
| `test_mpu6050` | 18 | MPU-6050: ідентифікація, розбір, помилки шини | ЛР2 |
| `test_w25q64` | 20 | W25Q64: JEDEC ID і послідовність команд SPI | ЛР2 |
| `test_crc32` | 4 | CRC-32 (IEEE 802.3), обчислення частинами | H09/I08 |
| `test_flash_log` | 22 | журнал подій у W25Q64 на моделі мікросхеми | H09 |
| `test_frame_store` | 17 | слоти кадрів у W25Q64: запис, читання, перевірка | H10 |
| `test_ov7670` | 7 | OV7670: PID/VER, порядок регістрів, AEC/AGC/AWB, порядок байтів RGB565 | I07, фікс камери |
| `test_stand_test` | 21 | перевірки стенда і звіт self-test | ЛР2 |
| `test_app` | 27 | `App_Init` / `App_Loop`, стендові команди ЛР2 | I02 |
| `test_sched` | 25 | Super Loop: планувальник, час, черга подій, STATS/EVT/MARK, прийом команд з втратами | I02–I06, фікс UART |
| `test_fakes_lr3` | 5 | самоперевірка test doubles для K02–K06 | K02–K06 |
| `test_sl_journal` | 22 | журнал у RAM, ID подій, переповнення, зв'язок подія → фото | K07–K09 |
| `test_sl_fsm` | 38 | FSM: UNLOCKED → EXIT_DELAY → LOCKED → ALARM, коди, таймаути | K02–K05, K09 |
| `test_sl_buttons` | 36 | B1, код з 4 кнопок, антидребезг, переповнення | K03, K04 |
| `test_sl_security` | 40 | PIR, MPU-6050 (TAMPER), геркон, відновлення шини | K05 |
| `test_sl_env` | 23 | DHT11 / BMP180, ENV_WARNING | K06 |
| `test_storage` | 19 | журнал W25Q64 як бекенд: монтування, BOOT, нумерація після рестарту | K07, H09 |
| `test_frames` | 6 | кадри на рівні App, стендові команди `p`, `o` | H10 |
| `test_cam_test` | 7 | `CAM TEST`, init при старті, сходження AEC (`settle_ms`), `CAM REG` | I07, фікс камери |
| `test_sl_camera_frames` | 5 | кадр події → W25Q64 → `PHOTO` / `PHOTO_ERR` | K08, K09 |
| `test_sl_integration` | 13 | наскрізні сценарії через справжній Super Loop і журнал W25Q64 | K07–K09 |
| `test_frame_export` | 6 | формування кадру, `CAM PATTERN` / `CAM EXPORT`, CRC | I08 |
| `test_camera` | 7 | кадр тривоги → W25Q64 → назад побайтно з ID події, `CAM LOAD`, помилки; тривога не чекає AEC (`aec=`) | I09, фікс камери |
| `test_firmware` | 4 | єдина прошивка (усі `App/*.c`): старт, `VER`, `DIAG`, LOCKED → PIR → кадр, 1 год циклу | I10 |
| `test_hooks_weak` | 3 | слабкі точки підключення без модулів | I02, I10 |
| `test_camera_dvp` | 7 | алгоритм захоплення DVP на моделі сигналів | I07 |
| `test_uart_rx` | 8 | прийом USART2 через DMA-кільце на моделі DMA1 Stream5 | фікс UART |
| `self_test` | — | форматування звіту self-test | ЛР2 |
| Python `scripts/` | 14 | `parse_uart_logs.py`, `uart_load.py`, `host_checks.py` | M-метрики, I11 |
| Python `tools/` | 6 | `frame_decode.py`: CRC, пропуски рядків, PNG, `--swap-bytes` | I08, фікс камери |

Кількість тестів — на коміті I11; актуальні числа завжди в підсумку логу.

## 6. Чого host-тести НЕ доводять

Host-тести працюють на моделях заліза (`tests/doubles/`): вони доводять логіку, формати,
обробку помилок і інтеграцію модулів, але не електричні сигнали і не реальні часи.
Це перевіряється на стенді (H14, `docs/camera.md` §7, `docs/lab_3/lab3-build.md` §5):

* реальні частоти й таймінги (PCLK, VSYNC, EXTI-латентність, `LOAD`, `STAT`);
* реальний кадр OV7670 (експозиція, колір — FR-07);
* стійкість шини I2C / SPI до завад і гарячого підключення;
* тривала робота > 1 год (модель у `test_firmware` — лише логіка часу і черг).
