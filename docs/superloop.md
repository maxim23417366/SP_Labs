# Super Loop SmartLock — вимірювання (ЛР3)

> Автор: Непорожній М.О. Цей файл доповнюють картки I02–I06.

## 2. Лічильник тактів і STATS (I03)

`firmware/App/perf.c`: DWT `CYCCNT` (вмикає `Platform_IoInit`), статистика
count/min/avg/max у тактах, перерахунок у мкс лише при виводі. Кожна задача в
`Sched_RunOnce()` обгорнута виміром; окремо — тривалість повного проходу (`loop`),
джитер періодичних задач і CPU Load (`sched.c`, I02).

Команда `STATS` (рядок у UART, `\r` або `\n` наприкінці):

```
STATS_BEGIN git=<hash> t_us=<64 біти> loops=<проходів>
STAT name=loop n=.. min_us=.. avg_us=.. max_us=..
CPU name=cpu_load windows=.. min_pm=.. avg_pm=.. max_pm=..
STAT name=fsm n=.. min_us=.. avg_us=.. max_us=..
POLL name=fsm polls=.. runs=..
STAT name=buttons ...
JITTER name=buttons period_us=10000 min_us=.. max_us=.. missed=..
JHIST name=buttons n=.. h_lt100us=.. h_lt1ms=.. h_lt10ms=.. h_lt100ms=.. h_ge100ms=..
... (security, env, mem — JITTER + JHIST; storage, uart — POLL)
STAT name=btn_b1_lat|pir_lat|mpu_lat n=.. min_us=.. avg_us=.. max_us=..   (I04)
OVR name=btn_b1|pir|mpu count=..             (I04: повторні фронти до обробки)
STAT name=evq_wait ...                       (подія в черзі FSM: Post → Take)
EVQ len=8 pending=.. max_depth=.. posted=.. dropped=..
UART rx_overrun=.. cmd_overflow=.. cmd_dropped=.. rx_peak=..   (rx_overrun — I04; DMA-прийом — docs/lab_3/uart-rx.md)
EVTLOG pending=.. dropped=..                 (I04)
STACK reserved=.. used=.. free=..
HEAP arena=.. used=.. free=..
STATS_END
```

Окремо, без команди, раз на секунду (коли закривається вікно CPU Load):

```
LOAD pct=<0..100>
```

**Контракт з парсером.** Рядки `STAT`, `JITTER`, `LOAD`, `STACK`, `HEAP` і банер `BUILD`
— точно за `docs/metrics-spec.md` §3 (регулярні вирази M03). `CPU`, `JHIST`, `POLL`, `EVQ`,
`UART` — додаткові: парсер специфікації їх пропускає, а для звіту вони дають
гістограму джитера, кількість опитувань і стан черги.

* `STAT n` — лише запуски з корисною роботою; `POLL polls` — скільки разів задачу
  викликали взагалі.
* `JITTER` — інтервал між фактичними запусками мінус період: min/max у мкс, `missed` —
  інтервал понад 2 періоди (методичка §5.1, приклад 3). `JHIST` — гістограма модуля
  джитера в 5 кошиках (M-04).
* `LOAD pct` (%, metrics-spec M-05) — останнє вікно 1 с; `CPU` (‰) у STATS — min/avg/max
  за всі вікна. Частка тактів корисної роботи у вікні 1 с. Задача, що лише перевірила
  порожню чергу, не рахується — інакше super loop завжди показував би 1000 ‰.
* `STATS RAW` — ті самі STAT у тактах (`min_cyc=`) для перевірки округлення.
* `STATS RESET` — обнулити все на початку сценарію.
* `MARK <n>` — мітка сценарію в EVT-лозі (`name=mark_<n>`, I04).
* `MEM` — лише рядки `STACK` і `HEAP`; `PING <n>` → `PONG seq=<n> t_us=<час МК>` (потік команд
  з ПК, методичка §6.2.3); `HELP`.
* Рядок з великої латинської літери — команда ЛР3; малі літери, цифри і `?` ідуть у
  команди стенда ЛР2, як і раніше. Задовгий рядок (> 47 символів) не виконується і
  рахується в `UART cmd_overflow`.
* Вивід STATS блокує цикл на ~30 рядків × ~4 мс ≈ 0,12 с, тому його викликають між
  фазами сценарію, а не всередині вимірюваного інтервалу.

Формат збігається з прикладом методички §5.1, тож парсер M03 розбирає
`STAT` і без знання інших рядків. Хостові тести — `tests/unit/test_sched.c`
(`test_stats_prints_every_metric_line`, `test_line_commands`, `test_load_line_once_per_cpu_window`,
`test_uart_routes_lines_and_lr2_letters`, `test_perf_format_helpers`).

## 3. Часові події і затримка реакції (I04)

**EVT** (`firmware/App/evt.c`, методичка §5.2):

```
EVT t_us=10234567 name=btn_b1_press
EVT t_us=10244567 name=btn_b1_handled
EVT t_us=10254590 name=fsm_lock_request
EVT t_us=12000100 name=mark_1
```

* `t_us` — мікросекунди **МК** від старту, 64 біти: 32-бітний DWT переповнюється
  кожні 44,7 с, розширення оновлюється в кожному проході. Тест: перехід лічильника
  через 0 — час без збою (`test_perf_time64_survives_counter_wrap`).
  Обмеження: прохід циклу має бути коротшим за 44,7 с.
* `Evt_Log` лише пише в RAM-буфер (64 записи); друк — `uart`-задачею по 2 рядки за
  прохід, тож вивід (~3,5 мс на рядок) не спотворює вимірюваних інтервалів.
  Переповнення — `EVTLOG dropped`.

| Подія | Звідки | Пара для затримки |
|---|---|---|
| `btn_b1_press`, `pir_trig`, `mpu_int` | момент фронту з EXTI (перераховано на шкалу `t_us`) | → `btn_b1_handled`, `pir_handled`, `mpu_handled` |
| `btn_b1_handled`, `pir_handled`, `mpu_handled` | задача забрала подію (імпульс PB1) | |
| `fsm_lock_request`, `fsm_motion`, `fsm_tamper`, … | FSM забрала подію з черги | від `*_handled` того ж фронту |
| `uart_cmd_rx` → `uart_cmd_tx` | рядкова команда UART отримана → виконана | обробка команди (M-03) |
| `mark_<n>`, `stats_reset`, `mem_peak` | `MARK <n>`, `STATS RESET`, новий максимум стека/heap | |

Формат і імена — `docs/metrics-spec.md` §3.3: регулярний вираз `^EVT t_us=(\d+) name=(\S+)$`,
тому число, де воно потрібне, — суфікс імені (`mark_42`), а не окреме поле.
`alarm_triggered` (рішення про тривогу після `pir_handled`) логує FSM (`sl_fsm.c`, K07): лише для MOTION,
прийнятої в стані LOCKED (`docs/lab_3/journal-camera-integration.md`).

**Затримка подія → реакція** (`STAT name=btn_b1_lat|pir_lat|mpu_lat`, M-03): ISR
записує такт DWT першого фронту (повторні до обробки — рядок `OVR name=... count=`, це
дребезг або повільний цикл), задача обчислює `now − t_irq`. p99 (M-03) рахує парсер з
пар `btn_b1_press` → `btn_b1_handled` в EVT-лозі. Ширина вимірює лише те, що бачить прошивка; затримку від фізичного
фронту дає аналізатор (§4).

Обмеження `*_lat`, підтверджене стендом: читання DHT11 вимикає переривання на ~4 мс
(з 24–25 мс читання, `docs/lab_3/logs/13_sensor_read_timing.txt`). Фронт B1/PIR у цей
час чекає в EXTI pending, і ISR ставить мітку вже після ввімкнення переривань — `*_lat`
занижує затримку до ~4 мс. Тому `*_lat` порівнюється з PulseView (M06), а в звіті S3
вказується, чи була подія під час ENV.

Події камери (`capture_*`, `flash_write_*`, `cam_*`, `export_*`) додають I07–I09.

**PB1 (REACT)** тепер імпульс у момент, коли задача забирає подію з ISR (`take()` в
`app_tasks.c`), а не на час роботи fsm, як у I05: фронт PC13 / PA0 → фронт PB1 на
аналізаторі = та сама затримка, що `STAT name=*_lat`, але виміряна незалежно від прошивки.

Хостові тести (`tests/unit/test_sched.c`): `test_isr_events_give_latency_evt_and_reach_fsm`
(фронт B1 і PIR → `btn_b1_lat` 7 мс / `pir_lat` 5 мс, `OVR` від дребезгу, EVT
`btn_b1_press` / `btn_b1_handled`, `fsm_*`), `test_react_probe_on_event_take`, `test_evt_format_ring_and_drop`
(64-бітний час, переповнення буфера), `test_mark_ping_and_uart_overrun_in_stats`.

## 4. Службові GPIO для PulseView (I05)

| Канал | Пін | Роз'єм Nucleo | Що показує |
|---|---|---|---|
| `PLATFORM_PROBE_LOOP` | PB0 | CN8-4 (A3) | 1 на час кожного проходу `Sched_RunOnce()`: ширина = тривалість проходу, проміжки — період циклу |
| `PLATFORM_PROBE_REACT` | PB1 | CN10-24 | короткий імпульс, коли задача забрала подію B1 / PIR / MPU_INT з ISR (I04; в I05 — на час роботи fsm з подіями) |
| `PLATFORM_PROBE_CAMERA` | PB2 | CN10-22 | з I09 — 1 на час захоплення кадру охоронної події (`camera.c`); до I09 показував задачу security |
| `PLATFORM_PROBE_STORAGE` | PB12 | CN10-16 | з I09 — 1 на час запису кадру у W25Q64 (`SmartLock_CameraStoreFrame`); до I09 показував задачу env |

* Усі на GPIOB: `PROBE_ON/OFF` — один запис у `GPIOB->BSRR` без HAL (кілька тактів).
* Піни вільні за `docs/pinout.md`: PB0 звільнено в ЛР2 (тумблер LOCK), PB1/PB2/PB12 не
  використовуються. Налаштовує `Platform_IoInit()` (у `firmware.ioc` поки не внесено —
  при наступній перегенерації CubeMX додати їх як GPIO_Output, H02).
* **Шлях події:** фронт → ISR ставить мітку DWT → buttons (раз на 10 мс) / security
  (раз на 20 мс) забирає подію з імпульсом PB1 → кладе подію в чергу → fsm у
  наступному проході. На аналізаторі PC13/PA0 → PB1 = до одного періоду опитування.
  До I04 фронт виявлявся порівнянням рівня з попереднім опитуванням.
* **Повна затримка реакції**: канал аналізатора на фізичний сигнал (PC13 для B1 або
  OUT PIR на PA0) + канал на PB1. Відстань між фронтами = дребезг + вхід у переривання +
  очікування задачі в циклі. Порівняння зі `STAT name=*_lat` (I04) — M06.
* Будь-якій задачі можна дати власний канал: останній аргумент `Sched_Add()` —
  `PLATFORM_PROBE_*` замість `SCHED_NO_PROBE`; планувальник сам обгорне виклик задачі.
* Хостові тести: `test_sched.c` (`test_loop_probe_pulses_every_pass`,
  `test_task_probe_wraps_only_its_task`, `test_react_probe_on_event_take`).
* **Конфлікти пінів:** у `docs/pinout.md` PB0 — «Вільний» (`SPARE_PB0`, вхід у
  `firmware.ioc`; `Platform_IoInit()` переводить його у вихід уже після `MX_GPIO_Init()`),
  PB1 / PB2 / PB12 не використовуються ні в таблиці, ні в `firmware.ioc`. SPI2 Flash —
  PB13/14/15 + CS на PB10, тож PB12 (апаратний NSS SPI2) вільний. PB2 = BOOT1, але при
  BOOT0 = 0 (Nucleo) він ігнорується, а виходом стає лише після старту. Фізично на
  бредборді ці контакти мають бути порожні (перевіряє стенд, H02).

Запис (8 каналів, fx2lafw, як у ЛР2):

```
sigrok-cli -d fx2lafw --config samplerate=1m -C D0,D1,D2,D3,D4 \
    --time 20s -o docs/captures/S3_b1_latency.sr
# D0 = PB0 loop, D1 = PB1 react, D2 = PB2 camera, D3 = PB12 flash, D4 = PC13 (B1)
```

1 МГц дає похибку 1 мкс — на порядки менше за вимірювані мілісекунди (§5.3).

## 5. Пам'ять (I06)

| Що | Як отримати | Де результат |
|---|---|---|
| Заповнення FLASH / RAM | лінкер `-Wl,--print-memory-usage` (кожна збірка) | вивід `make build`, `firmware/build/mem/memory_usage.txt` |
| text / data / bss, секції | `make mem-report` | `firmware/build/mem/size.txt`, `size_sections.txt` |
| Найбільші символи | `arm-none-eabi-nm --size-sort` | `top_symbols.txt`, `top_ram.txt` (після I09 першим буде буфер кадру `s_record`, 38 432 Б) |
| Статична оцінка стека | `-fstack-usage` → `.su` для кожного файлу | `stack_usage.txt` (20 найбільших кадрів) |
| Фактична глибина стека | stack painting: `Platform_StackPaint()` першим рядком `main()`, шаблон `0xDEADBEEF` | `STACK reserved= used= free=` у UART: при старті (після self-test), щоразу, коли максимум зріс (задача `mem` перевіряє раз на 1 с), і командами `MEM` / `STATS` |
| Heap | `mallinfo()` (newlib-nano; `printf` бере буфер stdout з heap) | `HEAP arena= used= free=` (поруч зі `STACK`) |

Межі стека — з лінкер-скрипта CubeMX: `_estack`, `_Min_Stack_Size = 0x400` (1 КБ).
Якщо в рядку `STACK` `used = reserved` (`free=0`), шаблон зіпсовано до самого низу: стек, найімовірніше,
вийшов за резерв — збільшити `_Min_Stack_Size` і повторити (методичка §5.4).
Повна глибина = найглибший ланцюжок викликів + кадри ISR, тому статичну оцінку `.su`
порівнюють із виміряною (M04).

`make mem-report` збирає прошивку і кладе всі звіти в `firmware/build/mem/`. CI робить
це на кожен PR і вивантажує артефакт `firmware-<sha>`: `firmware.elf`, `firmware.map`
і `build/mem/` (вкладка Summary запуску CI → Artifacts).

Що брати для M04 (Собкович):

| Для звіту M04 | Звідки |
|---|---|
| FLASH / RAM, % | `memory_usage.txt` (або рядки `Memory region` у лозі кроку build) |
| text / data / bss | `size.txt` |
| найбільші символи | `top_symbols.txt`, змінні в RAM — `top_ram.txt` |
| статичний стек | `stack_usage.txt` (20 найбільших кадрів; `dynamic` — кадр залежить від даних) |
| фактичний стек, heap | рядки `STACK` / `HEAP` в UART-лозі стенда: останні = максимум за сценарій |
| яка це збірка | перший рядок `BUILD,...,git=<хеш>` = хеш коміту, з якого CI зібрав артефакт |

Ідентифікатор збірки: кореневий `Makefile` обчислює `git rev-parse --short HEAD`
(з `-dirty`, якщо є незакомічені зміни) і передає в `firmware.mk` як `GIT_HASH`;
перший рядок UART-логу — `BUILD,...,git=<хеш>,...`. Без git — `git=nogit`. З I10 той самий
рядок дає команда `VER`, а `make fw-check` перевіряє хеш у `firmware.elf`
(`docs/lab_3/lab3-build.md` §4).

Формати рядків — `docs/metrics-spec.md` §3.6 (M-07, M-08).

Перевірено без плати: `test_app.c` (рядки `STACK` / `HEAP` при старті), `test_sched.c`
(`test_mem_task_prints_stack_heap_only_on_new_maximum`); логіка
`firmware.mk` (`all`, `mem-report`, мітка `.git_hash_*`) — прогоном із заглушками
тулчейну. Числа FLASH / RAM і `stack_used` — з CI і зі стенда (H11, M04).
