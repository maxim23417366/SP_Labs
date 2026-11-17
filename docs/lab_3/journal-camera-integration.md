# Журнал (K07 + H09), фото (K08 + H10 → I09) і мітки часу (I04)

Код K07/K08: `firmware/App/sl_journal.{h,c}`, `sl_camera_link.{h,c}`, `sl_camera_frames.{h,c}`, `sl_fsm.c`.
Бекенди колег: `App/storage.c` + `storage_ops.c` (H09, журнал W25Q64), `App/frames.c` + `Devices/frame_store.c` (H10, кадри), `App/evt.c` (I04).
Тести: `tests/unit/test_sl_journal.c`, `test_sl_fsm.c`, `test_storage.c`, `test_sl_camera_frames.c`, `test_sl_integration.c`.

## 1. Що вже інтегровано

| З ким | Як з'єднано | Перевірено host-тестом |
|---|---|---|
| H09 (Колодницький), журнал W25Q64 | `sl_journal.c` за прохід передає один запис у `SmartLock_JournalOps()->append()`; сильна реалізація — `storage_ops.c` → `Storage_Append()` | `test_storage`, `test_sl_integration` (fake_w25q64) |
| H09, нумерація | `Storage_Init()` після монтування (і після `BOOT` / стендових записів) викликає `SmartLock_JournalSetLastSeq()`; ID у рядку `EVT,id=` = `seq` у Flash, після рестарту — продовження | `test_sl_journal`, `test_storage`, `test_sl_integration` |
| H10, кадри | `SmartLock_CameraStoreFrame()` → `FrameStore_GetOps()->write_frame()` із `event_seq` = ID події → `SmartLock_OnCameraResult()` → рівно один `PHOTO` / `PHOTO_ERR` | `test_sl_camera_frames`, `test_sl_integration` |
| I04 (Непорожній), EVT | FSM на MOTION, прийняту в LOCKED, пише `Evt_Log("alarm_triggered")` — пара `pir_trig → alarm_triggered` (metrics-spec §3.3); фронти B1 / PIR з EXTI приходять у `b1_edge` / `pir_edge` | `test_sl_fsm`, `test_sl_integration` |

`sl_journal_ops_t` має одне поле `append` (як ініціалізує `storage_ops.c`). Окремого `last_seq` в ops і `SmartLock_JournalRestore()` немає: нумерацію відновлює `SmartLock_JournalSetLastSeq()` з `storage.c`.

**Відмова Flash.** `append` ≠ `DEV_OK` → запис виходить з черги, у UART `flash=err`, лічильник `SmartLock_JournalUnsaved()`. Повторів у `sl_journal` немає: за `flash-layout.md`, розділ 3.3, невдалий запис уже витратив `seq` (повтор дав би `DEV_ERR_PARAM`), стирання повторює наступний запис, а після `TBD_STORAGE_ERASE_RETRIES` (3) невдач `storage.c` вимикає журнал (`LOG,ERR,op=disable`) і `append` одразу повертає `DEV_ERR_STATE`. Замок і UART-журнал працюють далі (`test_flash_failure_keeps_lock_and_uart_journal_running`).

## 2. Рядки UART: два різні `EVT`

| Рядок | Хто друкує | Формат | Для чого |
|---|---|---|---|
| журнал (K07) | `sl_journal.c`, задача storage | `EVT,id=<seq>,t_ms=<мс>,name=<ТИП>,arg=<arg>[,ref=<ref>],flash=ok\|err` | FR-12, звірка з `LOG,seq=` (команда `d`) |
| мітки часу (I04) | `evt.c`, задача uart | `EVT t_us=<мкс> name=<подія>` | M-03, затримки за `docs/metrics/metrics-spec.md` §3.3 |

Після `EVT` журнал має кому, мітки часу — пробіл, тож регулярний вираз специфікації `^EVT t_us=(\d+) name=(\S+)$` рядків журналу не захоплює, а парсер журналу шукає `EVT,id=`. Формат журналу не змінено: на нього вже спираються `storage.c`, `test_storage.c`, `flash-layout.md` і стендові логи 16, 18, 19. Примітку для парсера додано в metrics-spec §3.3.

## 3. Подія → запис журналу → Flash → фото

| Подія (стан) | `sl_journal` (`arg`, `ref`) | Код у Flash (flash-layout 3.2) і поля | Фото |
|---|---|---|---|
| LOCK_REQUEST (UNLOCKED) | `LOCK_ACTIVATED` (15000) | `0x10`, `arg` | ні |
| кінець відліку, двері зачинені | `LOCKED` (фактична затримка, мс) | `0x11`, `arg` | ні |
| CODE_OK (LOCKED) | `ACCESS_GRANTED` (довжина коду), потім `UNLOCKED` (0) | `0x20`, `0x12` | ні |
| CODE_WRONG (LOCKED) | `WRONG_CODE` (довжина коду) | `0x21` | ні |
| MOTION (LOCKED) | `MOTION` (0); EVT `alarm_triggered` | `0x30` | так, 1 кадр |
| TAMPER (LOCKED) | `TAMPER` (мг понад поріг) | `0x31` | так, 1 кадр |
| кадр збережено | `PHOTO` (`arg` = ID кадру, `ref` = ID події) | `0x60` `PHOTO_SAVED`: `arg` = seq події, `frame_id` = ID кадру | — |
| кадру немає | `PHOTO_ERR` (`arg` = `sl_cam_status_t`, `ref` = ID події) | `0x61` `PHOTO_FAILED`: `arg` = seq події, `arg2` = `sl_cam_status_t` (1 REJECTED, 2 CAPTURE, 3 STORE, 4 TIMEOUT) | — |
| DOOR_OPEN у LOCKED без коду | `FORCED_OPEN` (0) | `0x32` | ні |
| DOOR_OPEN / DOOR_CLOSED інакше | `DOOR_OPEN` / `DOOR_CLOSED` (за `TBD_SL_LOG_DOOR_EVENTS`) | `0x40` / `0x41` | ні |
| ENV_WARNING (будь-який стан) | `ENV_WARNING` (маска T/RH) | `0x50` | ні |
| вимірювання BMP180 / DHT11 | немає (FR-15) | — | ні |
| старт прошивки | — (пише `storage.c`) | `0x01` `BOOT` | — |

Узгодження з H09: тип `PHOTO_ERR` не мав відповідника в `Storage_FlashType()` (запис давав `DEV_ERR_PARAM` і `flash=err`). Додано `SL_LOG_PHOTO_ERR → PHOTO_FAILED` і розкладку полів у `Storage_Append()`; у `flash-layout.md` для `0x61` уточнено, що `arg2` для записів застосунку — `sl_cam_status_t` (стендова команда `p` і далі пише туди `dev_status_t`).

Останні `TBD_JOURNAL_FIFO_RESERVE` (2) місць FIFO — лише для `PHOTO`/`PHOTO_ERR`; якщо й їх немає — результат чекає в `sl_camera_link` і записується в наступному проході. Подію, що не потрапила в журнал (ID = 0), без кадру: `skipped_no_id`.

## 4. Що лишається для I09 (Непорожній, камера OV7670)

Модуль камери в `main` ще не злитий (PR #34 не чіпали). Точка підключення однозначна, App/ правити не потрібно:

1. Визначити сильну `bool SmartLock_CameraRequest(uint32_t event_id)` (обгортка над `Camera_Request`) — слабка заглушка з `sl_camera_link.c` відкинеться лінкером. Не чекає: ставить запит у чергу камери. `false` — зайнята / немає камери (журнал сам запише `PHOTO_ERR` REJECTED).
2. Кадр захоплено в буфер — викликати з головного циклу
   `SmartLock_CameraStoreFrame(event_id, FRAME_FORMAT_RGB565, 160, 120, buf, 38400)` (`sl_camera_frames.h`). Вона пише кадр у W25Q64 (`frames.c`) з `event_seq` = `event_id` і закриває запит (`PHOTO` або `PHOTO_ERR` STORE). Якщо на `event_id` уже не чекають (тайм-аут, дубль) — кадр не пишеться.
3. Захоплення не вдалося — `SmartLock_OnCameraResult(event_id, 0, SL_CAM_ERR_CAPTURE)`.
4. Не логувати `PHOTO` самостійно (`SmartLock_LogEventRef`) — інакше два записи на подію (`frames.h` оновлено відповідно).
5. Рішення I09/I10: де виконувати блокувальний запис кадру (≈ 0,74–0,94 с, `flash-layout.md` 4.2) — у задачі camera цілком чи порціями `frame_store_begin/append/commit`; формат кадру (RGB565 / Y8); EVT `capture_start` / `capture_done`, `flash_wr_*` (metrics-spec §3.3) — у модулі камери.

Обмеження: до `TBD_CAMLINK_PENDING_MAX` (4) запитів чекають результату одночасно; результат має прийти за `TBD_CAM_RESULT_TIMEOUT_MS` (10 с), інакше `PHOTO_ERR` TIMEOUT.

## 5. Лишається перевірити на стенді (H13 / H14)

- FR-08 / E2E-06: 100 подій → вимкнення живлення → команда `d`; порядок, ID без дублів (пропуски дозволені лише при `JournalDropped` > 0), `order=ok`; перший `EVT,id=` після рестарту = останній seq + 2 (між ними `BOOT`).
- E2E-05: MOTION під час стирання сектора журналу (раз на 128 записів, 64–400 мс) і запису кадру — подія не втрачається (`TBD_JOURNAL_FIFO_LEN` 16, `TBD_EVQ_LEN` 8), `STAT evq_wait`, `pir_lat`.
- Відмова Flash посеред роботи (від'єднати CS): `flash=err`, `LOG,ERR`, після 3 невдалих стирань `LOG,ERR,op=disable`; замок і UART працюють.
- I04: `pir_trig → alarm_triggered` з реальним HC-SR501 і аналізатором (PA0 → PB1), p99 — парсером.
- H14 (з I09): `TBD_CAM_RESULT_TIMEOUT_MS` за вимірами захоплення (0,53–1,07 с) і запису кадру (до 4,6 с); дві MOTION поспіль (камера зайнята) → другий `PHOTO_FAILED` REJECTED; кадр після перезапуску читається за `frame_id` і має `event_seq` події.
