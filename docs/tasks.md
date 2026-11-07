# Runtime-задачі прошивки SmartLock (ЛР3, Super Loop без RTOS)

> Картка **I01** (Непорожній). Це перелік задач **виконання прошивки** — що і коли
> викликає головний цикл на STM32F411, — а не задачі людей і не GitHub Issues
> (розподіл робіт команди — у `SmartLock_LR3_Завдання.pdf`).
>
> **Правило чисел.** Число, якого немає в PRD (`docs/PRD.md`) або datasheet, не
> вигадується: такий параметр позначено **TBD**, а в коді має префікс `TBD_`
> (`firmware/App/app_config.h`) — це тимчасове значення для bring-up і вимірювань,
> а не вимога. Оцінки тривалості з datasheet і розрахунку — це **очікування**;
> фактичні числа дають сценарії S1–S3 (H11–H13) і STATS.

Код: `firmware/App/sched.c` (планувальник), `firmware/App/app_tasks.c` (таблиця задач),
`firmware/App/smartlock_hooks.h` (точки підключення логіки), `firmware/App/app.c` (`App_Loop`).

## 1. Модель виконання

| Параметр | Значення в прошивці |
|---|---|
| Архітектура | Super loop з мітками часу + переривання лише для фіксації подій (методичка §4.1). Без RTOS, без перемикання контексту, один стек |
| Головний цикл | `main()` → `App_Loop()` → `Sched_RunOnce()`: задачі по черзі в порядку таблиці §2 |
| Джерело часу | DWT `CYCCNT`, 32 біти, 96 МГц (1 такт ≈ 10,4 нс). Переповнення кожні 2³²/96·10⁶ ≈ **44,7 с**; для довгих інтервалів — 64-бітне розширення `Perf_TimeUs64()`, яке оновлює кожен прохід циклу |
| Періодична задача | запускається, коли `now − t_next ≥ 0`; наступний запуск — найближча точка сітки в майбутньому (без серії «наздоганяючих» запусків) |
| Подієва задача | `period = 0`: викликається в кожному проході, сама перевіряє свою чергу / прапорець |
| Пріоритет | = порядок у таблиці. Витіснення немає: термінова задача чекає, доки завершиться поточна |
| Корисна робота | задача повертає `true`, якщо щось зробила; порожнє опитування не входить у CPU Load (§5.1, приклад 4) |
| Заборонено в задачах | `HAL_Delay`, очікування у `while` без межі, FreeRTOS/інші планувальники. Виняток — наявні драйвери/діагностика ЛР2, перелічені в §4 як **блокувальні місця** (саме їх і вимірює ЛР3) |

## 2. Таблиця runtime-задач

Порядок рядків = порядок у проході циклу (`AppTasks_Init()`).

| # | Задача (`app_tasks.c`) | FR | Період / тригер | Дедлайн | Виклики API | ISR / polling | Блокувальні місця |
|---|---|---|---|---|---|---|---|
| 1 | `fsm` → `SmartLock_FsmStep()` | 01–04, 10, 11, 13 | кожен прохід: забирає події з черги `SmartLock_TakeEvent()` | **FR-03: 15 с ± 0,1 с** від прийнятого B1 до команди LOCK (PRD D.2). Інші — TBD | FSM (K02), `SmartLock_TakeEvent`, `Platform_ServoSetPulseUs`, `Evt_Log`, `Camera_Request` (K08) | черга подій (не ISR) | немає, якщо FSM лише змінює стан (правило §1) |
| 2 | `buttons` → `SmartLock_ButtonsPoll()` | 02, 09, 10 | **TBD**: `TBD_PERIOD_BUTTONS_US` = 10 мс | TBD: PRD не задає часу реакції на кнопку; вимірюється (`LAT b1_react`, аналізатор) | `Platform_ReadInputs`, `Platform_TakeEvent(B1)`, `SmartLock_PostEvent` | B1 — EXTI15_10 (лише мітка часу), 4 кнопки — polling з антидребезгом (K03/K04) | немає |
| 3 | `security` → `SmartLock_SecurityPoll()` | 05, 06, 13 | **TBD**: `TBD_PERIOD_SECURITY_US` = 20 мс | TBD (поріг і час реакції FR-06 не затверджені) | `mpu6050_read_raw`, `Platform_ReadInputs` (геркон), `Platform_TakeEvent(PIR, MPU_INT)`, `SmartLock_PostEvent` | PIR — EXTI0, MPU_INT — EXTI9_5 (мітки часу); MPU і геркон — polling | I²C HAL з тайм-аутом 10 мс на транзакцію (`BOARD_I2C_TIMEOUT_MS`); у нормі 14 Б ≈ 1,7 мс |
| 4 | `env` → `SmartLock_EnvPoll()` | 14, 15 | **TBD**: `TBD_PERIOD_ENV_US` = 2 с (DHT11 — не частіше 1 разу/с за datasheet) | немає (моніторинг); поріг і тривалість ENV_WARNING — TBD | `Platform_Dht11Capture`, `dht11_decode`, `bmp180_read`, `printf` | polling | DHT11: старт 20 мс `HAL_Delay` + ~4 мс кадру **з вимкненими перериваннями**; BMP180: два перетворення з `HAL_Delay` ≈ 9–10 мс |
| 5 | `camera` → `Camera_Task()` | 07, 08 | подія: черга `Camera_Request()` (MOTION/TAMPER у LOCKED, `CAM SNAP`) | TBD (PRD вимагає виміряти час захоплення, але не задає межу) | `Platform_CameraCapture`, `Crc32_Update`, `FrameStore_GetOps()->write_frame` (H10) | polling черги; захоплення — **зайнятий цикл** по GPIO | **критичне**: кадр без DCMI займає процесор до 2 кадрів (очікування VSYNC + кадр); запис у Flash — окремим проходом (§4) |
| 6 | `storage` → `SmartLock_StoragePoll()` | 08, 10, 12–14 | подія: черга записів журналу (K07/H09) | TBD | W25Q64 `append` журналу (H09) | polling | стирання сектора W25Q64: типово 45 мс, до 400 мс (tSE) |
| 7 | `uart` (в `app_tasks.c`) | 12, 14, 15 | кожен прохід | TBD: PRD не задає час відповіді на команду | `Platform_UartGetChar`, `Cmd_Feed` (STATS, MEM, PING, CAM…), `Bench_Command` (ЛР2), `Evt_Flush`, `FrameExport_Step` | RX — **polling** регістра USART2 (переповнення рахує `UART rx_overrun`), TX — блокувальний `HAL_UART_Transmit` | 1 символ ≈ 86,8 мкс при 115200 8N1; EVT-рядок ~3,5 мс; рядок кадру FRM ~7 мс; повний STATS ~0,2 с; діагностика ЛР2 `h` ≈ 8 с, `m` ≈ 1 с |

**Зв'язок задач з FSM — лише через чергу подій** (`firmware/App/event_queue.h`):
buttons / security / env кладуть подію (`SL_EV_LOCK_REQUEST`, `SL_EV_MOTION`,
`SL_EV_TAMPER`, `SL_EV_DOOR_OPEN/CLOSED`, `SL_EV_CODE_OK/WRONG`, `SL_EV_ENV_WARNING`)
викликом `SmartLock_PostEvent()`, а fsm у наступному проході забирає їх по одній і сама
вирішує, що робити (MOTION має сенс лише в LOCKED). Прямих викликів FSM з інших
модулів немає. Черга FIFO на **TBD** `TBD_EVQ_LEN` = 8 подій: кілька подій в одному
проході не губляться, переповнення рахується (`EVQ dropped`), час у черзі — `STAT
name=evq_wait`. У ЛР4 черга переходить у FreeRTOS: `xQueueSend` / `xQueueReceive`.

Поза таблицею: `Platform_Poll()` перед кожним проходом (трафік ЛР1 командою `l`,
за замовчуванням вимкнено) і вікно CPU Load (1 с) усередині `Sched_RunOnce()`.

### Відповідність FR → задача

| FR | Задача | FR | Задача |
|---|---|---|---|
| FR-01 стани | fsm | FR-09 введення коду | buttons |
| FR-02 B1 | buttons → черга → fsm | FR-10 код / WRONG_CODE | buttons → черга → fsm → storage |
| FR-03 15 с | fsm (час — `Platform_Millis`) | FR-11 без автоблокування | fsm |
| FR-04 SG92R LOCK | fsm | FR-12 UART | uart |
| FR-05 PIR → MOTION | security → черга → fsm | FR-13 геркон, FORCED_OPEN | security → черга → fsm → storage |
| FR-06 MPU → TAMPER | security → черга → fsm | FR-14 DHT11, ENV_WARNING | env → черга → fsm → storage |
| FR-07 фото | fsm → camera | FR-15 BMP180 тиск | env → uart |
| FR-08 журнал, фото у Flash | storage, camera | | |

## 3. Переривання (що робить ISR)

| ISR | Джерело | Дія в ISR | Хто обробляє |
|---|---|---|---|
| `EXTI15_10` | USER B1, PC13, спад | такт DWT першого фронту + прапорець (`Platform_TakeEvent`), повторні фронти — `overruns` | buttons |
| `EXTI0` | PIR, PA0, фронт | те саме | security |
| `EXTI9_5` | MPU_INT PB5 / VSYNC PC8 | MPU_INT — мітка часу; VSYNC — лічильник кадрів (self-test ЛР2) | security / стенд |
| `SysTick` | 1 кГц | `HAL_IncTick` (`Platform_Millis`) | усі |

Жодної роботи з шинами в ISR немає: усе, що може чекати (I²C, SPI, UART), — у циклі.
Спільні змінні ISR ↔ цикл — `volatile` + порядок «спершу час, потім прапорець» з
бар'єром компілятора (`platform_io.c`, `ev_mark` / `Platform_TakeEvent`).

## 4. Схема виконання

```
reset ─► HAL_Init, SystemClock 96 МГц, MX_*_Init
         Platform_StackPaint()                 ← перший рядок main(): вимір стека
         App_Init():
           BUILD,...,git=<hash>                ← перший рядок UART-логу
           Platform_BringUp (DWT, службові GPIO PB0/PB1/PB2/PB12, periph_check ЛР1)
           драйвери, self-test ЛР2, довідка
           AppTasks_Init: Perf, EVT, таблиця задач (sched)
           OV7670: PID/VER → QQVGA RGB565 → Camera_Init
           MEM stack_used=... heap_...
┌──────────────────────────────────────────────────────────────────────────┐
│ while (1)  App_Loop():  Platform_Poll(); Sched_RunOnce():                │
│   PB0↑  час64 ← DWT                                                      │
│   fsm ─ buttons* ─ security* ─ env* ─ camera ─ storage ─ uart            │
│    ▲      └─────────┴──────────┘ PostEvent → черга подій (FIFO, 8)       │
│    └── TakeEvent у наступному проході                                    │
│        (* лише коли настав період; джитер / missed / exec для кожної)    │
│   PB0↓  loop min/avg/max; раз на 1 с — CPU Load (‰)                      │
└──────────────────────────────────────────────────────────────────────────┘
ISR: EXTI (B1, PIR, MPU_INT, VSYNC) → мітка часу / лічильник;  SysTick → тік
```

Обробка однієї охоронної події з фото розтягнута на кілька проходів, щоб між
довгими операціями інші задачі отримували процесор. Порядок у проході
`fsm → buttons → security → … → camera`, тож fsm бачить подію security лише в
наступному проході, а camera стоїть після fsm і знімає кадр у тому ж проході, де
FSM його запросила:

```
прохід k   : security бачить PIR (LAT pir_react) → додає MOTION у чергу FSM
прохід k+1 : fsm обробляє MOTION (якщо LOCKED) → Camera_Request(id)   (STAT evq_wait)
             camera — захоплення кадру (PB2) + CRC-32
прохід k+2 : camera — write_frame у W25Q64 (PB12)
             → SmartLock_OnCameraResult (K08)
```

Перевірено хостовим тестом `test_pir_motion_reaches_fsm_next_pass`
(`tests/unit/test_superloop.c`): після проходу k подія в черзі, після k+1 — забрана.

**Найгірша затримка реакції** на подію в цьому циклі:

```
T_react_max ≈ T_період_задачі + max(T_задачі_i)  (найдовша задача, що вже почалась)
```

для B1: до 10 мс (період buttons) + найдовша операція в проході — захоплення кадру
(до ~1,1 с при CLKRC = 0x0F) або запис кадру у Flash (до ~4,6 с за максимумами
datasheet). Значення береться з вимірювань S3 (`LAT b1_react max_us`, PulseView),
а не з формули; формула пояснює, звідки береться максимум.

## 5. Тривалість блокувальних місць: очікування (до вимірювань)

| Операція | Розрахунок / джерело | Очікування |
|---|---|---|
| Захоплення кадру QQVGA RGB565 | VGA-таймінг 784 × 510 × 2 PCLK при f_int = 24 МГц / (CLKRC+1) = 1,5 МГц → 0,53 с кадр; + очікування VSYNC до одного кадру | 0,53 … 1,07 с (CLKRC — TBD, H14) |
| CRC-32 кадру (побітова) | 38 400 Б × 8 біт × ~8 тактів | ~25–30 мс |
| Запис кадру у W25Q64 | запис 32 + 38 400 Б = 10 секторів по 4 КБ, 151 сторінка; SPI 3 МГц → 38 432·8/3·10⁶ ≈ 0,10 с; tSE 45 мс тип. / 400 мс макс., tPP 0,4 мс тип. / 3 мс макс. (W25Q64JV datasheet) | ≈ 0,6 с тип., ≈ 4,6 с у найгіршому |
| Експорт кадру в UART (текст) | 1 200 рядків FRM × ~80 символів × 86,8 мкс | ~8,3 с усього, ~7 мс на прохід |
| DHT11 | 20 мс старт + 40 біт ≈ 4 мс (переривання вимкнені) | ~24 мс |
| BMP180 (oss = 0) | 4,5 мс температура + 4,5 мс тиск + I²C | ~10 мс |
| MPU-6050, 14 Б | (1+1+1+14) × 9 біт / 100 кГц | ~1,7 мс |
| Рядок EVT / ENV у UART | ~40–50 символів × 86,8 мкс | 3,5–4,5 мс |

## 6. Відкриті рішення (TBD) і власники

| Параметр | Де в коді | Хто вирішує |
|---|---|---|
| Періоди buttons / security / env | `TBD_PERIOD_*_US` | Коваль (K03, K05, K06) + Сюкало (L01/L02) |
| Поріг MPU-6050 для TAMPER (FR-06) | — (K05) | Коваль, погоджує Сюкало (L01) |
| Межі й тривалість ENV_WARNING (FR-14) | — (K06) | Коваль, погоджує Сюкало (L01) |
| Вимога до часу реакції на кнопку / подію | — | Сюкало (L01/L02): у PRD числа немає |
| Довжина черги подій FSM | `TBD_EVQ_LEN` | Непорожній (I02) за `EVQ max_depth` у S1–S3 |
| CLKRC, тайм-аути камери, формат кадру | `TBD_CAMERA_*` | Непорожній (I07) за вимірами Колодницького (H14) |
| Розміщення журналу і кадрів у W25Q64 | `FrameStore_GetOps()` | Колодницький (H08/H10) |

Затверджене числом у PRD лише одне: **FR-03 = 15 с ± 0,1 с** (D.2). Решта чисел
у цьому документі — або з datasheet / розрахунку (позначено), або TBD.
