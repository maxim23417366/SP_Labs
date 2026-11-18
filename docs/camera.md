# Камера OV7670 на STM32F411 без DCMI (ЛР3: I07–I08, FR-07)

> Автор: Непорожній М.О. (дистанційно, без стенда). Фізичну роботу камери і реальний
> кадр перевіряє Колодницький у **H14** за інструкцією §6. До H14 статус FR-07 —
> «код готовий, апаратно не підтверджено».

## 1. Що реалізовано в I07 і де

| Частина | Файл | Шар |
|---|---|---|
| PID/VER, скидання, CLKRC, таблиця QQVGA RGB565 по SCCB | `firmware/Devices/ov7670.c/h` | Devices (лише `dev_bus.h`) |
| Захоплення кадру по DVP GPIO: `Platform_CameraCapture()` | `firmware/Platform/camera_dvp.c` + `camera_dvp_port.h`, API — `platform.h` | Platform |
| Формат кадру і параметри захоплення (`CAMERA_*`, `TBD_CAMERA_*`) | `firmware/App/app_config.h` | App |
| **Команда запуску на платі `CAM TEST`**: init камери + один кадр у буфер 38 400 Б | `firmware/App/cam_test.c/h`, виклик з `cmd.c`, `STAT name=camera` у `stats.c` | App |
| Хостові тести драйвера, алгоритму і команди | `tests/unit/test_ov7670.c`, `tests/unit/test_camera_dvp.c` (модель сигналів `tests/doubles/fake_dvp.c`), `tests/unit/test_cam_test.c` (`tests/doubles/fake_camera.c`) | тести |

SCCB не допускає repeated START, тому драйвер не користується `dev_i2c_bus_t::read_reg`,
а отримує дві функції — на платі це `Platform_SccbRead/Write` з ЛР2.
Піни DVP вже налаштовані CubeMX (`Core/Src/gpio.c`): D0–D7 = PC0–PC7, VSYNC = PC8,
HREF = PC10, PCLK = PA10, XCLK = PA8 (MCO1, 24 МГц), RESET = PC11, PWDN = PC12.

Передача кадру на ПК (`CAM PATTERN`, `CAM EXPORT`, `tools/frame_decode.py`) — I08, §5.
Камера як задача Super Loop (черга запитів від FSM, запис кадру у Flash з ID події) — I09;
обидві картки використовують той самий буфер кадру з `cam_test.c`.

**Команда запуску.** Рядок `CAM TEST` + Enter у терміналі 115200 8N1:

```
CAM,init,status=ok,pid=0x76,ver=0x73,clkrc=0x0F,failed_reg=0x00
CAM,result,status=ok,bytes=38400,lines=120,short=0,vglitch=0,capture_us=<мкс>,crc=0x<CRC-32>
EVT t_us=<мкс> name=capture_start
EVT t_us=<мкс> name=capture_done
```

Перший `CAM TEST` ініціалізує камеру (рядок `CAM,init`), наступні лише знімають кадр.
Якщо ініціалізація не вдалась — кадр не знімається, а `status` / `failed_reg` показують причину.
Тривалість вдалих захоплень — `STAT name=camera` у `STATS` (metrics-spec §3.2, M-01).
Команда блокує цикл на час кадру (≈ 0,5–1,1 с при CLKRC = 0x0F, §3) — для стендової
перевірки це прийнятно; у задачі Super Loop (I09) захоплення стоїть окремим проходом.

## 2. Формат кадру і RAM

STM32F411RE має **128 КБ RAM** і не має DCMI, а камера — FIFO, тож кадр має повністю
вміститися в RAM, поки процесор вичитує його з GPIO.

| Формат | Байт на кадр | Частка 128 КБ | Рішення |
|---|---|---|---|
| VGA 640×480 RGB565 | 614 400 | 469 % | не вміщається |
| QVGA 320×240 RGB565 | 153 600 | 117 % | не вміщається |
| QVGA 320×240 лише Y (YUV422, кожен 2-й байт) | 76 800 | 59 % | вміщається, але без кольору і майже без запасу для решти прошивки |
| **QQVGA 160×120 RGB565** | **38 400** | **29 %** | **обрано**: колір, достатньо для розпізнавання сцени (FR-07, PRD D.2) |
| QQVGA 160×120 лише Y | 19 200 | 15 % | резерв, якщо RAM не вистачить (M04) |

До I07 прошивка займала 9 256 Б RAM (`main` з I04 і картками K/H, `make mem-report`);
з буфером кадру `s_frame` (38 400 Б, `cam_test.c`) — ≈ 47,7 КБ, тобто ≈ 36 % від 128 КБ.
Запас достатній.

`docs/metrics/theoretical-estimates.md` §1 рахує частки від 96 КБ (39,1 % для QQVGA RGB565);
у STM32F411RE 128 КБ SRAM, тож правильна частка — 29,3 %. Вибір формату той самий: QQVGA RGB565.

## 3. Частоти й час (очікування до H14)

Тактування: XCLK = 24 МГц з MCO1 (PA8, ЛР1). Внутрішній такт камери
`f_int = XCLK / (CLKRC[5:0] + 1)`; кадр у VGA-таймінгу — 784 × 510 тактів пікселя по
2 PCLK (RGB565) = 799 680 тактів f_int (OV7670 datasheet, «VGA Frame Timing»).

| CLKRC | f_int | Кадр | Захоплення (очікування VSYNC + кадр) |
|---|---|---|---|
| 0x00 | 24 МГц | 33 мс (30 кадрів/с — те, що бачив self-test ЛР2) | PCLK ≈ 24 МГц: байт кожні ~42 нс = 4 такти ядра — опитування GPIO не встигає |
| 0x07 | 3 МГц | 0,27 с | 0,27–0,53 с |
| **0x0F** (`TBD_CAMERA_CLKRC`) | **1,5 МГц** | **0,53 с** | **0,53–1,07 с** |
| 0x1F (команда `k` ЛР2) | 750 кГц | 1,07 с | 1,07–2,13 с |

**Чому не 33–65 мс з `theoretical-estimates.md` §2.1.** Той розрахунок бере заводську
частоту кадрів (30 Гц при CLKRC = 0). Для захоплення через GPIO її потрібно знизити
щонайменше в 8–16 разів, тож база для M07 — рядок 0x0F цієї таблиці.

**Скільки встигає процесор.** Внутрішній цикл на байт — два опитування PCLK, читання
`GPIOC->IDR`, запис у RAM: орієнтовно 20–30 тактів при 96 МГц, тобто стабільна вибірка
до ~3 МГц PCLK. Для QQVGA дільники COM14 / 0x73 ще зменшують PCLK на активних лініях
(очікувано /4) — точне значення дає PulseView у H14.

**Маскування переривань.** SysTick (1 кГц) чи EXTI посеред лінії зсунули б вибірку на
кілька фронтів PCLK. Тому `irq_lock_per_line = 1`: переривання вимикаються від початку
HREF до його кінця і вмикаються в гасінні між лініями. Лінія QQVGA = 320 байтів; поки
вона коротша за 1 мс, відкладений SysTick лише зсувається, а не губиться. Якщо в H14
лінія виявиться довшою за 1 мс (повільніший CLKRC), `Platform_Millis()` почне відставати —
це видно за `t_us` у EVT проти мілісекунд, і тоді треба або пришвидшити CLKRC, або
вимкнути маскування (і прийняти ризик SHORT_FRAME).

## 4. Алгоритм захоплення (`Platform_CameraCapture`)

```
дочекатися VSYNC=0 → VSYNC=1 → VSYNC=0         (початок кадру; тайм-аут на кожен фронт)
    імпульс коротший за TBD_CAMERA_VSYNC_MIN_US (1 мс) — завада: vsync_glitches++,
    чекати наступний; загальна межа 3 × TBD_CAMERA_VSYNC_TIMEOUT_US
для кожної з 120 ліній:
    дочекатися HREF=1 (переривання увімкнені)   тайм-аут → HREF_TIMEOUT
    [маскувати переривання]
    320 разів: фронт PCLK 0→1 → прочитати GPIOC->IDR → якщо HREF=0 — лінія коротша
    пропустити зайві байти до HREF=0             (кадр ширший за налаштований)
    [відновити переривання]
результат: bytes, lines, short_lines, vsync_glitches, cycles (DWT)
```

| Статус | Значення | Що перевірити на стенді |
|---|---|---|
| `vsync_timeout` | кадрової синхронізації немає | XCLK на PA8 (24 МГц), PWDN = 0 (PC12), RESET = 1 (PC11), живлення 3,3 В |
| `href_timeout` | лінія не почалась | таблиця вікна (HSTART…VREF), COM7 |
| `pclk_timeout` | PCLK не тактує | PA10, COM10[5] (PCLK у гасінні) |
| `short_frame` | хоча б одна лінія коротша за 320 Б | масштабування (COM14, 0x72, 0x73), повільніший CLKRC |

Алгоритм перевірено на ПК на моделі сигналів (`tests/doubles/fake_dvp.c`): повний кадр
побайтно, старт посеред кадру, ширші лінії, повільний PCLK, коротка лінія, кожен тайм-аут,
баланс маскування переривань (`tests/unit/test_camera_dvp.c`, 7 тестів, зокрема відкидання завад на VSYNC).
Драйвер SCCB — `tests/unit/test_ov7670.c` (5 тестів: PID/VER, відсутня камера, порядок
скидання → CLKRC → таблиця, зупинка на регістрі з помилкою, перевірка параметрів).

## 5. Формування й експорт кадру на ПК (I08)

| Частина | Файл |
|---|---|
| Формат рядків, CRC-32, один рядок за прохід | `firmware/App/frame_export.c/h` (виклик `FrameExport_Step()` у задачі `uart`, `app_tasks.c`) |
| Команди `CAM PATTERN`, `CAM EXPORT`, блокування буфера на час експорту | `firmware/App/cam_test.c` |
| Скрипт відновлення: лог → перевірка → PNG | `tools/frame_decode.py` (лише стандартна бібліотека Python 3) |
| Приклад логу і відновленого кадру | `docs/examples/frame_export_sample.log`, `frame_export_sample.png` |
| Host-тести | `tests/unit/test_frame_export.c` (6), `tools/test_frame_decode.py` (5), ціль `make tools-test` |

**Формат.** UART той самий, що для EVT, STATS і команд, тож кадр іде текстом —
рядок у Super Loop друкується цілком і не змішується з іншими:

```
FRAME_BEGIN id=<event> seq=0 w=160 h=120 fmt=RGB565 len=38400 crc32=<hex8> chunk=32
FRM off=0 d=<64 hex-символи>
FRM off=32 d=...
...                                      ← між FRM можуть бути EVT / LOAD / відповіді
FRAME_END id=<event> len=38400 crc32=<hex8> lines=1200
```

- `id` — ID події журналу (I09). Для стендових кадрів `CAM TEST` / `CAM PATTERN` — 0.
- Піксель RGB565 — 2 байти, **старший першим** (так віддає OV7670), рядки кадру зверху вниз.
- `off` дозволяє скрипту знайти загублений рядок, `crc32` (CRC-32/zlib, той самий
  `Devices/crc32.c`, що в `CAM,result` і `frame_store`) — перевірити кадр цілком.
  CRC у `CAM,result,...,crc=` і в `FRAME_BEGIN` однакові для того самого кадру.
- EVT `export_start_<id>` / `export_done_<id>` дають час експорту (metrics-spec §3.3).

**Час.** Рядок FRM ≈ 82 символи (16 + 64 hex + CRLF) ≈ 7,1 мс при 115200 8N1 (86,8 мкс на
символ). 1200 рядків ≈ **8,5 с** на кадр замість 3,3 с для двійкових 38 400 Б
(`theoretical-estimates.md` §2.3) — ціна текстового формату. Цикл при цьому не стоїть:
задача `uart` друкує один рядок за прохід, решта задач працює між рядками.
Поки йде експорт, буфер заблоковано: `CAM TEST` / `CAM PATTERN` / `CAM EXPORT` відповідають
`CAM,busy,op=..,reason=export`.

**Команди.**

| Команда | Що робить | Відповідь |
|---|---|---|
| `CAM PATTERN` | відомий кадр без камери: 8 кольорових смуг зверху, градієнт сірого знизу | `CAM,pattern,bytes=38400,crc=0x...` |
| `CAM EXPORT` | останній кадр (`CAM TEST` або `CAM PATTERN`) на ПК | `FRAME_BEGIN ...` … `FRAME_END ...`; без кадру — `CAM,export,status=no_frame` |

**Відновлення на ПК** (Python 3, сторонні бібліотеки не потрібні):

```
python3 tools/frame_decode.py <лог> --out frame.png                      # кадр камери
python3 tools/frame_decode.py <лог> --out frame.png --expect-test-pattern  # після CAM PATTERN
python3 tools/frame_decode.py <лог> --index 0 --raw frame.bin            # перший кадр у лозі, сирі байти
```

Виводить `frame_decode: OK` і код 0, якщо кадр цілий; інакше `FAIL` з причиною
(`missing ... range(s)`, `crc32 ... != expected`, `no FRAME_END`, `test pattern mismatch`)
і код 1. Без Python на ПК: `make tools-test` проганяє той самий скрипт у Docker.

**Host-перевірка без плати** (`make tools-test`): `test_frame_export` проганяє
`CAM PATTERN` + `CAM EXPORT` через задачу `uart` Super Loop, звіряє відновлені байти з
малюнком і зберігає весь вивід у `tests/build/frame_export_host.log`; далі
`frame_decode.py` відновлює з нього PNG і звіряє побайтно (C → лог → Python → PNG).
Цим логом і є `docs/examples/frame_export_sample.log` (з EVT між рядками кадру).

## 6. Інструкція для H14 (Колодницький, стенд)

На стенді перевіряються сигнали камери, реальне захоплення кадру командою `CAM TEST`,
передача кадру на ПК (`CAM EXPORT`, I08) і числа, від яких залежать параметри
`TBD_CAMERA_*`. Запис кадру у Flash з ID події — після I09.

**Аналізатор** (fx2lafw, 12 МГц, як `cam_vsync_href_pclk.sr` у ЛР2):
D0 = PC8 VSYNC, D1 = PC10 HREF, D2 = PA10 PCLK, D3 = PC0 (D0 камери). Земля спільна.

**Кроки:**

1. Прошивка з `make flash`, термінал 115200 8N1 із записом у файл
   (`docs/metrics/H14_camera_i07.log`). Перший рядок — `BUILD,...,git=<hash>`.
2. Команда ЛР2 `c` — PID/VER камери по SCCB: очікувано `0x76` / `0x73`.
3. PulseView при заводському CLKRC: частота VSYNC (~30 Гц), період PCLK, тривалість HREF.
   Зберегти `.sr`.
4. Команда ЛР2 `k` (повільний PCLK, CLKRC = 0x1F): ті самі виміри. Перерахувати для
   `TBD_CAMERA_CLKRC = 0x0F`: очікувано VSYNC ≈ 1,9 Гц, кадр ≈ 0,53 с (§3).
5. Ширина імпульсу VSYNC — має бути більшою за `TBD_CAMERA_VSYNC_MIN_US` (1 мс);
   кількість завад на VSYNC під час роботи руками біля стенда (лог 15 ЛР3).
6. Натиснути `RESET` (повертає заводський CLKRC після `k`), набрати **`CAM TEST`** + Enter.
   Критерій: `CAM,init,status=ok,pid=0x76,ver=0x73,...` і
   `CAM,result,status=ok,bytes=38400,lines=120,short=0`. Записати `capture_us` і `crc`.
   PulseView під час команди: кількість фронтів PCLK при HREF = 1 на лінію (очікувано 320)
   і тривалість HREF. Зберегти `.sr`.
7. Повторити `CAM TEST` щонайменше 10 разів (методичка §6.3), потім `STATS` —
   рядок `STAT name=camera n=10 min_us=.. avg_us=.. max_us=..`. Сцена нерухома — `crc`
   між кадрами може відрізнятися лише через шум сенсора; `bytes`/`lines` — ні.
8. **Передача на ПК (I08).** Термінал — запис у файл (PuTTY: Session → Logging →
   «All session output»; або `picocom -b 115200 /dev/ttyACM0 | tee H14_frame.log`).
   Спершу `CAM PATTERN`, потім `CAM EXPORT`, дочекатися `FRAME_END` (≈ 9 с).
   На ПК: `python3 tools/frame_decode.py H14_frame.log --out pattern.png --expect-test-pattern`
   → `test pattern matches byte for byte`, `OK`. Це перевіряє лінію UART без камери.
9. `CAM TEST`, потім `CAM EXPORT`; на ПК `python3 tools/frame_decode.py H14_frame.log --out frame.png`
   → `OK`; `crc32` у виводі скрипта = `crc` з `CAM,result`. Зберегти `frame.png` і лог у
   `docs/metrics/`. Час експорту — різниця `t_us` між EVT `export_start_0` і `export_done_0`
   (очікувано ≈ 8,5 с, §5).
10. Результати — у таблицю M07 (теорія з §3 і §5 проти виміру) і в `app_config.h`, якщо
   числа розходяться більш ніж на 10–20 %.

**Якщо не вийшло:**

| Симптом | Дія |
|---|---|
| `c` не бачить 0x21 | XCLK на PA8 (24 МГц), PWDN = 0 (PC12), RESET = 1 (PC11), живлення 3,3 В |
| PCLK > 3 МГц при CLKRC = 0x0F | збільшити `TBD_CAMERA_CLKRC` (опитування GPIO не встигає, §3) |
| HREF довший за 1 мс | маскування переривань на лінію зсуне `Platform_Millis()` (§3) — пришвидшити CLKRC |
| імпульс VSYNC коротший за 1 мс | зменшити `TBD_CAMERA_VSYNC_MIN_US` до половини виміряної ширини |
| багато коротких імпульсів на VSYNC | завади: коротший дріт VSYNC, спільна земля поруч |
| `CAM,init,status=nack` | камера не відповідає по SCCB — XCLK, PWDN/RESET, дроти SDA/SCL |
| `CAM,init,status=timeout`, `failed_reg` ≠ 0x00 | SCCB обірвався на цьому регістрі — повторити; якщо стабільно — знизити частоту I2C або перевірити підтяжки |
| `CAM,result,status=vsync_timeout` | кадрової синхронізації немає — XCLK на PA8, `failed_reg` у `CAM,init` |
| `status=short_frame` або `short=` > 0 | повільніший CLKRC (`TBD_CAMERA_CLKRC` 0x0F → 0x13) або таблиця вікна; байти на лінію — у PulseView |
| `vglitch=` > 0 | завади на VSYNC, кадр при цьому цілий — див. рядок вище |
| `frame_decode: FAIL missing ... range(s)` | термінал загубив рядки: лог писати у файл, а не копіювати з вікна; не друкувати команди під час експорту |
| `frame_decode: FAIL crc32 ...` при `CAM PATTERN` | спотворення на лінії UART — інший кабель / порт, перевірити 115200 8N1 |
| `frame_decode: FAIL no FRAME_END` | лог обірвано до кінця експорту — дочекатися `FRAME_END` |
| кадр «зсунутий» або з кольоровими смугами | `short=` > 0 у `CAM,result` або неправильний порядок байтів — див. `short_frame` |
