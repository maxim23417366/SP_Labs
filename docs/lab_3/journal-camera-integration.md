# Інтеграція журналу (K07 → H09) і камери (K08 → I09/I10)

Каркас: `firmware/App/sl_journal.{h,c}`, `sl_camera_link.{h,c}`, `sl_fsm.c`. Тести: `tests/unit/test_sl_journal.c`, `test_sl_fsm.c`.
Власникам блокерів не потрібно правити App/: достатньо визначити сильні функції (лінкер відкине слабкі).

## 1. Для Колодницького (H09): сильна `SmartLock_JournalOps()`

```c
typedef struct {
    dev_status_t (*append)(const sl_log_rec_t *rec);
    dev_status_t (*last_seq)(uint32_t *last_seq);   /* може бути NULL */
} sl_journal_ops_t;
const sl_journal_ops_t *SmartLock_JournalOps(void);  /* повертає static const */
```
Ініціалізуйте ОБА поля (`{ f, NULL }`): `-Wextra -Werror` забороняє пропущені поля.

- `append` — одна порція роботи за виклик, не частіше одного разу за прохід (задача `storage`, після `fsm` і `camera`). `DEV_OK` — збережено.
  `DEV_ERR_TIMEOUT` / `DEV_ERR_BUS` — тимчасово (Flash зайнята, збій SPI): запис лишається на чолі черги,
  повтор через `TBD_JOURNAL_RETRY_MS` (100 мс), до `TBD_JOURNAL_RETRY_MAX` (5) разів. Будь-який інший код (журнал заповнений,
  `DEV_ERR_STATE`, `DEV_ERR_PARAM`) — остаточна відмова: UART `flash=err`, запис не повторюється, йдемо далі.
  Стирання сектора (до 400 мс, на стенді 64 мс, лог 14) краще виносити в окрему порцію/прохід (наприклад, стирати наступний сектор наперед).
- `last_seq` — повернути найбільший `seq` у Flash (0, якщо журнал порожній). Має бути дешевою: порахуйте у своєму init (скан при старті),
  тут віддайте кеш. Викликається один раз, перед першим записом (`SmartLock_LogEvent` сам, або явно `SmartLock_JournalRestore()` після init Flash).
  Не вдалось — журнал почне з 1 (`SmartLock_JournalSeqRestored() == false`), E2E-06 тоді не пройде.
- Журнал сам не перечитує Flash: читання для UART-дампу/FR-08 — ваше. `SmartLock_JournalRecUnpack()` розбирає слот.

### Рекомендований запис у Flash: 32 байти (little-endian)
| Зсув | Поле | Примітка |
|---|---|---|
| 0..3 | `seq` | 1, 2, 3…; 0 і 0xFFFFFFFF недійсні |
| 4..7 | `t_ms` | `Platform_Millis()` у момент події |
| 8 | `type` | `sl_log_type_t`; значення незмінні, нові — в кінець |
| 9..11, 20..27 | резерв | 0xFF |
| 12..15 | `arg` | за типом (таблиця нижче) |
| 16..19 | `ref` | PHOTO / PHOTO_ERR: `seq` події; інакше 0 |
| 28..31 | CRC-32 за байтами 0..27 | рахує H09 |

8 записів на сторінку 256 Б (запис не перетинає межу), 128 на сектор, 100 подій ≈ 3,2 КБ. Порожній слот = 0xFF; рахуйте `last_seq` за останнім слотом з вірним CRC.
Готові `SmartLock_JournalRecPack()` / `Unpack()` заповнюють/розбирають байти 0..27 (CRC лишається 0xFF для вас).
Невизначеність, яку вирішує H09: політика при збої живлення посеред запису (запис з CRC ≠ дійсний — пропустити слот), заповнення журналу (кільце чи стоп).

## 2. Для Непорожнього (I09/I10): камера
1. Визначте сильну `bool SmartLock_CameraRequest(uint32_t event_id)` (обгортка над `Camera_Request`) — заглушка з `sl_camera_link.c` відкинеться.
   Не чекає: лише ставить запит у чергу. `true` — прийнято; `false` — зайнята попереднім кадром / камери немає (журнал сам запише `PHOTO_ERR`/`REJECTED`, повторів немає).
2. Для КОЖНОГО прийнятого запиту викликайте рівно один раз `SmartLock_OnCameraResult(event_id, frame_id, status)` з головного циклу (прохід k+2 з tasks.md §4),
   коли відомий результат запису кадру в Flash: `SL_CAM_OK` + `frame_id` (ID з `FrameStore`) → запис `PHOTO`; `SL_CAM_ERR_CAPTURE` / `SL_CAM_ERR_STORE` → `PHOTO_ERR`.
   Виклик усередині `SmartLock_CameraRequest` теж допустимий.
3. Обмеження: один кадр за раз; FSM не блокується (захоплення — у `Camera_Task`). Не прийшов результат за `TBD_CAM_RESULT_TIMEOUT_MS` (10 с) — журнал
   запише `PHOTO_ERR`/`TIMEOUT`, пізня відповідь ігнорується (лічильник `unknown`, кадр у Flash лишиться без посилання — вирішіть з H10, чи звільняти).
   До 4 запитів можуть чекати результат одночасно (`TBD_CAMLINK_PENDING_MAX`). Поки камери немає: `SmartLock_CameraStubSetAccept(true)` у тестах.

## 3. Подія → запис журналу → фото
| Подія (стан) | Запис (`arg`) | Фото |
|---|---|---|
| LOCK_REQUEST (UNLOCKED) | `LOCK_ACTIVATED` (15000 мс) | ні |
| кінець відліку, двері зачинені | `LOCKED` (фактична затримка, мс) | ні |
| CODE_OK (LOCKED) | `ACCESS_GRANTED` (з події), потім `UNLOCKED` | ні |
| CODE_WRONG (LOCKED) | `WRONG_CODE` | ні |
| MOTION / TAMPER (LOCKED) | `MOTION` / `TAMPER` (з події), потім `PHOTO` (arg = ID кадру, ref = ID події) або `PHOTO_ERR` (arg = `sl_cam_status_t`, ref = ID події) | так, 1 кадр, рівно 1 запис-закриття |
| DOOR_OPEN у LOCKED без коду | `FORCED_OPEN` | ні |
| DOOR_OPEN / DOOR_CLOSED інакше | `DOOR_OPEN` / `DOOR_CLOSED` (за `TBD_SL_LOG_DOOR_EVENTS`) | ні |
| ENV_WARNING (будь-який стан) | `ENV_WARNING` (з події) | ні |
| вимірювання BMP180 / DHT11 | немає (FR-15) | ні |

MOTION / TAMPER поза LOCKED ігноруються. Якщо подію не прийнято в журнал (FIFO повна, ID = 0) — кадр не запитується (`skipped_no_id`).
Останні `TBD_JOURNAL_FIFO_RESERVE` (2) місць FIFO — лише для `PHOTO`/`PHOTO_ERR`; якщо й їх немає — запис чекає і повторюється кожен прохід.
`flash=ok|err` у рядку `EVT,...` — підсумок `append` для запису; перед ним (при повторах) UART мовчить.

## 4. Лишається перевірити на стенді (H13/H14)
- FR-08: 100 подій → вимкнення живлення → дамп; порядок, ID без дублів і пропусків, CRC; кадр після перезапуску (E2E-06).
- E2E-05: MOTION під час стирання/запису сектора — не втрачається, `TBD_JOURNAL_FIFO_LEN` 16 і `RETRY_*` достатні (`SmartLock_JournalMaxDepth()`, `Dropped()`).
- Реальний час `append` і стирання; чи вкладаються 5 × 100 мс повторів; чи не змінюється `LAT b1_react` через `storage`.
- `TBD_CAM_RESULT_TIMEOUT_MS` за вимірами захоплення (0,53–1,07 с) + запису кадру (до 4,6 с); реальна поведінка при двох MOTION поспіль (камера зайнята).
- Відмова Flash: журнал UART продовжується, система не зависає; живлення, обірване посеред `append`.
