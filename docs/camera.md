# Камера OV7670 на STM32F411 без DCMI (ЛР3: I07–I09, FR-07)

> Автор: Непорожній М.О. (дистанційно, без стенда). Фізичну роботу камери і реальний
> кадр перевіряє Колодницький у **H14** за інструкцією §7. До H14 статус FR-07 —
> «код готовий, апаратно не підтверджено».

## 1. Що реалізовано в I07 і де

| Частина | Файл | Шар |
|---|---|---|
| PID/VER, скидання, CLKRC, таблиця QQVGA RGB565 + AEC/AGC/AWB, гамма, кольорова матриця (§8) по SCCB; перестановка байтів RGB565 | `firmware/Devices/ov7670.c/h` | Devices (лише `dev_bus.h`) |
| Захоплення кадру по DVP GPIO: `Platform_CameraCapture()` | `firmware/Platform/camera_dvp.c` + `camera_dvp_port.h`, API — `platform.h` | Platform |
| Формат кадру і параметри захоплення (`CAMERA_*`, `TBD_CAMERA_*`) | `firmware/App/app_config.h` | App |
| **Команда запуску на платі `CAM TEST`**: init камери + один кадр у буфер 38 400 Б | `firmware/App/cam_test.c/h`, виклик з `cmd.c`, `STAT name=camera` у `stats.c` | App |
| Хостові тести драйвера, алгоритму і команди | `tests/unit/test_ov7670.c`, `tests/unit/test_camera_dvp.c` (модель сигналів `tests/doubles/fake_dvp.c`), `tests/unit/test_cam_test.c` (`tests/doubles/fake_camera.c`) | тести |

SCCB не допускає repeated START, тому драйвер не користується `dev_i2c_bus_t::read_reg`,
а отримує дві функції — на платі це `Platform_SccbRead/Write` з ЛР2.
Піни DVP вже налаштовані CubeMX (`Core/Src/gpio.c`): D0–D7 = PC0–PC7, VSYNC = PC8,
HREF = PC10, PCLK = PA10, XCLK = PA8 (MCO1, 24 МГц), RESET = PC11, PWDN = PC12.

Передача кадру на ПК (`CAM PATTERN`, `CAM EXPORT`, `tools/frame_decode.py`) — I08, §5.
Камера як задача Super Loop (запит від FSM, запис кадру у Flash з ID події) — I09, §6;
обидві картки використовують той самий буфер кадру з `cam_test.c`.

**Команда запуску.** Рядок `CAM TEST` + Enter у терміналі 115200 8N1:

```
CAM,init,status=ok,pid=0x76,ver=0x73,clkrc=0x0F,failed_reg=0x00
CAM,result,status=ok,bytes=38400,lines=120,short=0,vglitch=0,capture_us=<мкс>,crc=0x<CRC-32>,settle_ms=<мс>
EVT t_us=<мкс> name=capture_start
EVT t_us=<мкс> name=capture_done
```

З виправленням §8 камера ініціалізується ще при старті (`App_Init`, рядок `CAM,init` після
self-test), щоб експозиція і баланс білого встигли зійтися. `settle_ms` — скільки
`CAM TEST` чекав сходження (> 0 лише якщо кадр просять раніше ніж через 5 с після
init). Якщо камери при старті не було, перший `CAM TEST` ініціалізує її (рядок `CAM,init`).
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

- `id` — ID події журналу: для кадру тривоги або кадру, прочитаного `CAM LOAD`, це `event_seq`
  (I09, §6); для стендових кадрів `CAM TEST` / `CAM PATTERN` — 0.
- Піксель RGB565 — 2 байти, **старший першим**, рядки кадру зверху вниз. OV7670 віддає
  молодший байт першим, тож прошивка переставляє байти одразу після захоплення
  (`CAMERA_RGB565_SWAP`, §8). Кадри, зняті до виправлення, — `frame_decode.py --swap-bytes`.
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

## 6. Кадр охоронної події у W25Q64 (I09)

Точка підключення — `docs/lab_3/journal-camera-integration.md` §4 (Коваль, K08). Файли I09:

| Частина | Файл |
|---|---|
| Сильна `SmartLock_CameraRequest()` (замінює слабку заглушку `sl_camera_link.c`), задача `camera` | `firmware/App/camera.c/h` |
| Задача `camera` у Super Loop (подієва, між `env` і `storage`) | `app_tasks.c`, слабка `SmartLock_CameraPoll()` у `smartlock_hooks_weak.c` |
| Спільне захоплення для `CAM TEST` і тривоги, `CAM LOAD <frame_id>`, `STAT name=flash_frame` | `cam_test.c/h`, `stats.c` |
| Host-тест | `tests/unit/test_camera.c` (6): окремий бінарник з `camera.o` (`APP_OBJS_camera`), у решті тестів App/ лишається заглушка, на яку спираються тести K08 |

**Шлях:**

```
FSM (LOCKED, MOTION / TAMPER) ── запис у журнал (ID = seq) ── SmartLock_CameraLinkStart(id)
  └► SmartLock_CameraRequest(id)                  camera.c: лише запам'ятовує запит
задача camera (той самий прохід, після fsm):
  CamTest_Capture()                    PB2 ↑  EVT capture_start / capture_done, STAT camera
  SmartLock_CameraStoreFrame(id, RGB565, 160, 120, buf, 38400)
                                       PB12 ↑ EVT flash_wr_start / flash_wr_done, STAT flash_frame
    write_frame (H10): заголовок кадру event_seq = id
    → SmartLock_OnCameraResult → журнал PHOTO {arg = frame_id, ref = id}  (у Flash PHOTO_SAVED)
  або SmartLock_OnCameraResult(id, 0, SL_CAM_ERR_CAPTURE) → PHOTO_ERR {arg = 2, ref = id}
CAM,photo,event=<id>,status=ok|capture_err|store_err,cap=<статус>,capture_us=..,store_us=..
```

- **Рівно один результат на запит:** `PHOTO` або `PHOTO_ERR` з `ref` = ID події. Модуль камери
  сам `PHOTO` не логує (правило K08).
- **Один запит за раз.** Поки попередній не оброблено або буфер кадру передається на ПК
  (`CAM EXPORT`), `SmartLock_CameraRequest()` повертає `false` — журнал запише `PHOTO_ERR`
  REJECTED (arg = 1). На практиці запит обробляється в тому самому проході, тож друга подія
  відхиляється лише під час експорту.
- **Блокування циклу.** Захоплення ≈ 0,5–1,1 с (§3) + запис кадру ≈ 0,74–0,94 с
  (`flash-layout.md` §4.2) — разом до ≈ 2 с один прохід. Це в межах
  `TBD_CAM_RESULT_TIMEOUT_MS` (10 с), але на цей час інші задачі стоять: події кнопок і PIR
  не губляться (ISR, черга `TBD_EVQ_LEN`), їхню затримку покаже `STAT *_lat`. Розбити запис
  на порції (`frame_store_begin/append/commit`) — кандидат на оптимізацію після вимірів H14.
- **Зв'язок в обидва боки:** заголовок кадру → `event_seq` = ID події; запис `PHOTO_SAVED`
  у Flash → seq події і `frame_id`. Після рестарту кадр читається командою
  `CAM LOAD <frame_id>`:
  `CAM,load,frame=<n>,status=ok|no_frame|crc|param|flash_err,event=<seq>,w=160,h=120,len=38400,crc=0x...`,
  а `CAM EXPORT` передає його з `id` = ID події.

**Host-тест** (`test_camera`, справжні `camera.c`, `sl_camera_*`, `sl_journal` → `storage.c`,
`frames.c` → `frame_store` → драйвер W25Q64 над `fake_w25q64`; підмінено лише DVP і SCCB):
- `test_alarm_frame_written_and_read_back_bytewise` — кадр → `write_frame` → `read_frame`
  **побайтно** (38 400 Б), заголовок `event_seq` = ID події, `PHOTO,arg=1,ref=<id>` у журналі,
  `PHOTO_SAVED` у Flash з тим самим seq і `frame_id`; EVT і `STAT flash_frame`; PB2 / PB12;
- `test_cam_load_after_restart_and_export_with_event_id` — після «рестарту» `CAM LOAD`
  повертає той самий кадр побайтно, `CAM EXPORT` — з `id` події;
- `test_capture_error_gives_photo_err_capture`, `test_store_error_gives_photo_err_store` —
  `PHOTO_ERR` CAPTURE / STORE з `ref` = ID події, кадру у Flash немає;
- `test_request_rules` — один запит за раз, відмова під час експорту і для ID = 0;
- `test_super_loop_motion_in_locked_gives_linked_photo` — повний шлях у Super Loop:
  LOCK_REQUEST → 15 с → LOCKED → MOTION → FSM → задача camera → Flash → `PHOTO` з `ref` = ID
  MOTION, `alarm_triggered` у EVT.

## 7. Інструкція для H14 (Колодницький, стенд)

На стенді перевіряються сигнали камери, реальне захоплення кадру командою `CAM TEST`,
передача кадру на ПК (`CAM EXPORT`, I08), кадр тривоги у W25Q64 з ID події (I09) і числа,
від яких залежать параметри `TBD_CAMERA_*`.

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
6. Натиснути `RESET` (повертає заводський CLKRC після `k`), зачекати ≥ 5 с (сходження
   AEC/AWB, §8), набрати **`CAM TEST`** + Enter.
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
10. **Кадр тривоги (I09).** B1 → вийти, зачинити двері → через 15 с `LOCKED`. Помахати
   перед PIR. У лозі: `EVT,id=<N>,...,name=MOTION`, `CAM,photo,event=<N>,status=ok,...`,
   `EVT,...,name=PHOTO,arg=<frame>,ref=<N>,flash=ok`. PulseView: PB2 — захоплення,
   PB12 — запис кадру; ширина імпульсів = `capture_us` / `store_us`.
11. Вимкнути живлення, увімкнути. `CAM LOAD <frame>` → `status=ok,event=<N>`; `CAM EXPORT`
   і `frame_decode.py` → PNG з `id=<N>` у виводі скрипта. Команда `d` (журнал) показує
   `PHOTO_SAVED` з тим самим `<N>` і `<frame>`.
12. Дві події поспіль під час `CAM EXPORT` (≈ 9 с) → друга дає `PHOTO_ERR,arg=1` (REJECTED).
   `STATS` після серії: `STAT name=camera`, `STAT name=flash_frame`, `STAT name=pir_lat`
   (затримка PIR під час зйомки і запису кадру — для M-03, сценарій S3).
13. **Експозиція й колір (§8).** Кадр звичайної сцени і кадр TAMPER (стенд повернуто до
   вікна / лампи) — `CAM TEST` + `CAM EXPORT` + `frame_decode.py`: сцена розпізнавана, без
   пересвічення, білий папір — білий. Перевірити `CAM REG 13` → `val=0xE7` (AEC, AGC, AWB
   увімкнено). Якщо кадр темний/світлий — підібрати `CAM REG 24 <AEW>` і `CAM REG 25 <AEB>`
   (ціль AEC, §8) без перепрошивки і записати значення в таблицю. Зберегти PNG у
   `docs/lab_3/img/`.
14. Результати — у таблицю M07 (теорія з §3, §5 і §6 проти виміру) і в `app_config.h` /
   `TBD_CAM_RESULT_TIMEOUT_MS`, якщо числа розходяться більш ніж на 10–20 %.

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
| `CAM,photo,...,status=store_err` | Flash не змонтована (`FRM,MOUNT` при старті) або помилка запису — `LOG,ERR` у лозі |
| `PHOTO_ERR,arg=4` (TIMEOUT) | кадр не встиг за `TBD_CAM_RESULT_TIMEOUT_MS` — порівняти `capture_us + store_us` з 10 с |
| `CAM,load,...,status=no_frame` | кадр з цим номером перезаписано (кільце з 126 слотів, `flash-layout.md`) або номер не той — взяти `arg` з рядка `PHOTO` |
| кадр «зсунутий» | `short=` > 0 у `CAM,result` — див. `short_frame` |
| «райдужні» смуги замість сцени, CRC цілий | порядок байтів пікселя: кадр знятий до виправлення §8 (напр. зі старого слоту W25Q64 через `CAM LOAD`) — `frame_decode.py --swap-bytes`; новий кадр з прошивки з `CAMERA_RGB565_SWAP = 1` не має цього ефекту |
| кадр пересвічений / майже білий | `CAM REG 13` має дати `0xE7`; `settle_ms` = 0 (інакше кадр знято до сходження AEC); нижча ціль AEC — `CAM REG 24 70`, `CAM REG 25 20` |
| кадр зелений / синій відтінок | AWB ще не зійшовся — зачекати кілька секунд і повторити `CAM TEST`; підсилення каналів AWB видно в `CAM REG 01` (синій) і `CAM REG 02` (червоний) |
| `CAM,photo` кадр однаково пересвічений одразу після старту | тривога раніше ніж через 5 с після `CAM,init` — `TBD_CAMERA_AEC_SETTLE_MS` дочекається, але перевірити, що рядок `CAM,init,status=ok` є після self-test |

## 8. Порядок байтів і експозиція (виправлення після стенда, лог 23)

На `main` `1c2da0a` кадри знімались, зберігались і передавались без втрат (CRC
збігається), але зображення було непридатним для FR-07
(`docs/lab_3/logs/23_camera_bench_main.txt`, `docs/lab_3/img/camera_main_frame21_motion215_*.png`,
PR `lab3/camera-bench`):

1. **Порядок байтів пікселя.** Декодер вважає RGB565 «старший байт першим», а OV7670
   з нашою таблицею (TSLB і COM13[0] — значення за замовчуванням) віддає **молодший першим**
   (GGGBBBBB, потім RRRRRGGG; у Linux цей режим — `MEDIA_BUS_FMT_RGB565_2X8_LE`). Звідси
   «райдужні» смуги, а ті самі байти з переставленою парою — плавна сцена.
2. **Пересвічення.** Таблиця вмикала лише формат і вікно QQVGA. Після скидання датчик
   працює з фіксованою експозицією, без AGC і балансу білого: кадр пересвічений, кадр
   TAMPER майже білий.

**Виправлення:**

| Що | Де | Як |
|---|---|---|
| порядок байтів | `ov7670_rgb565_swap_bytes()` (`Devices/ov7670.c`), виклик у `CamTest_Capture()` після `Platform_CameraCapture()`; `CAMERA_RGB565_SWAP = 1` (`app_config.h`) | переставити байти кожної пари одразу після кадру: ~0,3 мс на 38 400 Б, поза циклом опитування PCLK (його таймінг не змінюється). У буфері, CRC, W25Q64 і `CAM EXPORT` — старший байт першим, як каже формат і як чекає декодер |
| старі кадри | `tools/frame_decode.py --swap-bytes` | кадри у W25Q64, зняті до виправлення (`CAM LOAD`), декодуються з перестановкою |
| AEC / AGC / AWB | таблиця `ov7670_qqvga_rgb565_table()` | після формату QQVGA: гамма-крива, параметри AEC (цільова яскравість AEW/AEB/VPT, гістограмний AEC), стеля підсилення ×4, AWB, кольорова матриця RGB, `COM8 = 0xE7` — усе ввімкнено. Значення — з драйвера Linux `drivers/media/i2c/ov7670.c` |
| сходження автоматики | `CamTest_Init()` у `App_Init()`; `TBD_CAMERA_AEC_SETTLE_MS = 5000` | камера налаштовується при старті й далі працює сама, AEC/AWB сходяться за кілька кадрів (кадр ≈ 0,53 с). До першої тривоги минає ≥ EXIT_DELAY 15 с. Якщо кадр просять раніше ніж через 5 с після init — `CAM TEST` / задача camera чекають решту (`settle_ms` у `CAM,result`) |
| підбір на стенді | `CAM REG <hex> [<hex>]` | читання / запис будь-якого регістра без перепрошивки: `CAM,reg,op=read,reg=0x13,val=0xE7,status=ok` |

**Свідомо НЕ взято з драйвера Linux** (і перевіряє `test_ov7670`):

| Регістр | Чому |
|---|---|
| DBLV `0x6B` = `0x4A` | PLL ×4 — PCLK став би вчетверо швидшим, а опитування GPIO не встигає (§3) |
| TSLB `0x3A`, COM13[0] | порядок байтів — його вже вирішує перестановка в прошивці; зміна регістра без стенда могла б «перевернути» його вдруге |
| NT_CTRL `0xA4` | автоматичне зниження частоти кадрів у темряві змінило б період VSYNC і `TBD_CAMERA_VSYNC_TIMEOUT_US` |
| MVFP `0x1E`, COM10, `0x70`–`0x73`, `0xA2` | дзеркалення, полярність синхросигналів і масштаб QQVGA — без змін |

**Перевірка без стенда:** `test_ov7670` (автоматика ввімкнена останнім записом COM8, формат
QQVGA не змінився, заборонених регістрів немає; перестановка байтів), `test_cam_test`
(байти пікселя переставлені, `settle_ms`, init при старті, `CAM REG`), `test_camera`
(у W25Q64 — старший байт першим), `tools/test_frame_decode.py` (`--swap-bytes` для старих кадрів).
**На стенді** — §7 крок 13: кадр звичайної сцени і кадр TAMPER, після чого FR-07
переоцінюється за `fr-verification-matrix.md`.

