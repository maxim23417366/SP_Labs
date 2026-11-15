# Super Loop SmartLock — вимірювання (ЛР3)

> Автор: Непорожній М.О. Цей файл доповнюють картки I02–I06; розділи 1–3 (каркас,
> STATS, затримка/джитер/CPU) додаються з картками I02–I04.

## 4. Службові GPIO для PulseView (I05)

| Канал | Пін | Роз'єм Nucleo | Що показує |
|---|---|---|---|
| `PLATFORM_PROBE_LOOP` | PB0 | CN8-4 (A3) | 1 на час кожного проходу `Sched_RunOnce()`: ширина = тривалість проходу, проміжки — період циклу |
| `PLATFORM_PROBE_REACT` | PB1 | CN10-24 | 1, поки задача fsm обробляє події з черги (реакція на B1 / PIR). У I04 імпульс переноситься в момент, коли цикл забирає подію з ISR |
| `PLATFORM_PROBE_CAMERA` | PB2 | CN10-22 | до I09 — 1 на час задачі security (раз на 20 мс); з I09 — захоплення кадру |
| `PLATFORM_PROBE_STORAGE` | PB12 | CN10-16 | до I09 — 1 на час задачі env (раз на 2 с, DHT11 + BMP180); з I09 — `write_frame` у W25Q64 |

* Усі на GPIOB: `PROBE_ON/OFF` — один запис у `GPIOB->BSRR` без HAL (кілька тактів).
* Піни вільні за `docs/pinout.md`: PB0 звільнено в ЛР2 (тумблер LOCK), PB1/PB2/PB12 не
  використовуються. Налаштовує `Platform_IoInit()` (у `firmware.ioc` поки не внесено —
  при наступній перегенерації CubeMX додати їх як GPIO_Output, H02).
* **Шлях події до I04:** buttons (раз на 10 мс) / security (раз на 20 мс) порівнюють
  рівень входу з попереднім опитуванням → кладуть подію в чергу → fsm у наступному
  проході забирає її з імпульсом PB1. Тому затримка на аналізаторі = до одного періоду
  опитування + до одного проходу циклу. I04 замінює опитування мітками часу з EXTI.
* **Повна затримка реакції**: канал аналізатора на фізичний сигнал (PC13 для B1 або
  OUT PIR на PA0) + канал на PB1. Відстань між фронтами = дребезг + вхід у переривання +
  очікування задачі в циклі. Порівняння з `LAT` (I04) — M06.
* Будь-якій задачі можна дати власний канал: останній аргумент `Sched_Add()` —
  `PLATFORM_PROBE_*` замість `SCHED_NO_PROBE`; планувальник сам обгорне виклик задачі.
* Хостові тести: `test_sched.c` (`test_loop_probe_pulses_every_pass`,
  `test_task_probe_wraps_only_its_task`, `test_react_probe_only_when_fsm_has_events`,
  `test_input_edges_reach_fsm_with_react_pulse`).
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
| Фактична глибина стека | stack painting: `Platform_StackPaint()` першим рядком `main()`, шаблон `0xDEADBEEF` | `STACK reserved= used= free=` у UART: при старті (після self-test) і щоразу, коли максимум зріс — задача `mem` перевіряє раз на 1 с (команди `MEM` і `STATS` додає I03) |
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
перший рядок UART-логу — `BUILD,...,git=<хеш>,...`. Без git — `git=nogit`.

Формати рядків — `docs/metrics-spec.md` §3.6 (M-07, M-08).

Перевірено без плати: `test_app.c` (рядки `STACK` / `HEAP` при старті), `test_sched.c`
(`test_mem_task_prints_stack_heap_only_on_new_maximum`); логіка
`firmware.mk` (`all`, `mem-report`, мітка `.git_hash_*`) — прогоном із заглушками
тулчейну. Числа FLASH / RAM і `stack_used` — з CI і зі стенда (H11, M04).
