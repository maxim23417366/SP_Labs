/**
  ******************************************************************************
  * @file    sl_journal.h
  * @brief   ЛР3 (K07, Коваль): журнал подій SmartLock — каркас у RAM.
  *
  *          FR-08, FR-10, FR-12, FR-13, FR-14. Хто завгодно в головному циклі
  *          (FSM, пізніше камера K08) викликає SmartLock_LogEvent(): запис
  *          {seq, t_ms, type, arg, ref} стає в RAM-FIFO і отримує ID (seq).
  *          Задача storage (SmartLock_StoragePoll, сильна реалізація тут) за
  *          один прохід забирає ОДИН запис, друкує рядок EVT у UART (FR-12) і
  *          передає його бекенду журналу Flash через sl_journal_ops_t.append.
  *
  *          Рядок UART:
  *            EVT,id=<seq>,t_ms=<мс>,name=<ТИП>,arg=<arg>[,ref=<ref>],flash=ok|err
  *          (стиль ключ=значення, як BUILD / ENV / TIME; імена типів — як у PRD).
  *
  *          Бекенд: слабка SmartLock_JournalOps() повертає «Flash недоступний»
  *          (append → DEV_ERR_STATE), такі записи лише рахуються (unsaved).
  *          Журнал W25Q64 (H09, Колодницький) визначає сильну
  *          SmartLock_JournalOps() — без правок цього модуля. CRC, адреси й
  *          розміщення у Flash — H08/H09, не тут; тут лише рекомендований
  *          бінарний вигляд запису (SL_LOG_REC_BYTES, SmartLock_JournalRecPack).
  *
  *          Відмова бекенда (K07): DEV_ERR_TIMEOUT / DEV_ERR_BUS — «Flash зайнята
  *          / збій SPI», запис лишається на чолі FIFO, повтор не раніше ніж через
  *          TBD_JOURNAL_RETRY_MS, не більше TBD_JOURNAL_RETRY_MAX разів; потім
  *          flash=err і йдемо далі. Інший код — одразу flash=err. Після першої
  *          остаточної відмови (режим «Flash не працює») повторів немає, доки
  *          append не вдасться знову: недоступна Flash не гальмує UART-журнал.
  *          Задача storage не чекає: у паузі між повторами повертає false.
  *
  *          Переповнення FIFO: новий запис відкидається і рахується (dropped),
  *          нічого не блокує; seq при цьому все одно витрачається, тож втрата
  *          видна в UART / Flash як пропуск ID. Останні TBD_JOURNAL_FIFO_RESERVE
  *          місць — лише для PHOTO / PHOTO_ERR: вони закривають уже записану
  *          охоронну подію (K08) і не мають губитись разом зі звичайними.
  *
  *          Лише для головного циклу (не з ISR): індекси не атомарні.
  ******************************************************************************
  */
#ifndef SL_JOURNAL_H
#define SL_JOURNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "dev_bus.h"

/* TBD: PRD не задає. Має перекривати найбільшу серію записів між двома
 * проходами storage (один запис за прохід): CODE_OK дає 2, MOTION + TAMPER + 2 фото
 * — 4. 16 — запас для bring-up; уточнити за SmartLock_JournalMaxDepth() у S1–S3. */
#define TBD_JOURNAL_FIFO_LEN       16U

/* TBD: скільки останніх місць FIFO віддано записам PHOTO / PHOTO_ERR (К08).
 * Достатньо кількості кадрів, що чекають результату (TBD_CAMLINK_PENDING_MAX
 * у sl_camera_link.h, зараз 4), але FIFO 16 — беремо 2 як компроміс; уточнити за
 * SmartLock_JournalMaxDepth() і SmartLock_JournalDropped() у S1–S3. */
#define TBD_JOURNAL_FIFO_RESERVE   2U

/* TBD: повтор append після DEV_ERR_TIMEOUT / DEV_ERR_BUS. Стирання сектора
 * W25Q64 — до 400 мс (tSE, datasheet; на стенді 64 мс, лог 14), тож
 * 5 × 100 мс перекриває одне стирання з запасом. Уточнити з H09 (H13). */
#define TBD_JOURNAL_RETRY_MAX      5U
#define TBD_JOURNAL_RETRY_MS       100U

/* Рекомендований розмір запису у Flash (H09): 32 байти = 8 записів на сторінку
 * 256 Б, запис ніколи не перетинає межу сторінки, 128 записів на сектор 4 КБ.
 * Розкладка (little-endian), байти 28..31 — CRC-32 за байтами 0..27 (рахує H09):
 *   0..3 seq | 4..7 t_ms | 8 type | 9..11 0xFF | 12..15 arg | 16..19 ref |
 *   20..27 0xFF (резерв) | 28..31 CRC-32
 * Порожній (стертий) слот = усі 0xFF; seq 0 і 0xFFFFFFFF недійсні. */
#define SL_LOG_REC_BYTES           32U
#define SL_LOG_REC_CRC_OFFSET      28U

#define SL_LOG_ID_NONE         0U   /* «запису немає» (FIFO повна або невірний тип) */

/* Типи записів. Значення — частина формату Flash (H09): нові додавати в кінець. */
typedef enum
{
    SL_LOG_NONE = 0,
    SL_LOG_LOCK_ACTIVATED,   /* прийнято LOCK, старт 15 с (FR-02/03); arg = затримка, мс */
    SL_LOG_LOCKED,           /* команда LOCK на SG92R, стан LOCKED (FR-04); arg = фактична затримка від старту відліку, мс */
    SL_LOG_UNLOCKED,         /* команда UNLOCK, стан UNLOCKED (FR-10); arg = 0 */
    SL_LOG_ACCESS_GRANTED,   /* правильний код (FR-10); arg — з події CODE_OK */
    SL_LOG_WRONG_CODE,       /* неправильний код (FR-10); arg — з події CODE_WRONG */
    SL_LOG_MOTION,           /* PIR у LOCKED (FR-05); arg — з події */
    SL_LOG_TAMPER,           /* MPU-6050 у LOCKED (FR-06); arg — з події */
    SL_LOG_FORCED_OPEN,      /* двері відчинено в LOCKED без коду (FR-13); arg = 0 */
    SL_LOG_ENV_WARNING,      /* DHT11 тривало поза межами (FR-14); arg — з події */
    SL_LOG_DOOR_OPEN,        /* геркон: відчинено (FR-13), поза LOCKED */
    SL_LOG_DOOR_CLOSED,      /* геркон: зачинено (FR-13) */
    SL_LOG_PHOTO,            /* кадр збережено (FR-07/08, K08): arg = ID кадру, ref = seq події MOTION/TAMPER */
    SL_LOG_PHOTO_ERR,        /* кадр не отримано (K08): arg = sl_cam_status_t, ref = seq події MOTION/TAMPER */
    SL_LOG_COUNT
} sl_log_type_t;

typedef struct
{
    uint32_t seq;     /* ID запису: 1, 2, 3, … (0 = SL_LOG_ID_NONE) */
    uint32_t t_ms;    /* Platform_Millis() у момент SmartLock_LogEvent() */
    uint32_t arg;     /* дані за типом (див. sl_log_type_t) */
    uint32_t ref;     /* посилання на інший запис (PHOTO → подія), інакше 0 */
    uint8_t  type;    /* sl_log_type_t */
} sl_log_rec_t;

/* Вузький інтерфейс до сховища журналу (H09). Викликається лише з задачі
 * storage, не частіше одного append за прохід.
 *
 * append   — дописати запис; не чекає довше за одну порцію роботи (сторінка
 *            256 Б ≈ 0,7–2 мс; стирання сектора — окремою порцією, не разом з
 *            записом, якщо це можливо). DEV_OK — запис збережено. DEV_ERR_TIMEOUT
 *            / DEV_ERR_BUS — тимчасово (Flash зайнята, збій SPI): журнал повторить.
 *            Будь-який інший код (журнал заповнений, DEV_ERR_STATE, PARAM) —
 *            остаточна відмова, запис не повторюється.
 * last_seq — необов'язкова (NULL = «починати з 1»). Повертає в *last_seq
 *            найбільший seq, збережений у Flash (0, якщо журнал порожній), щоб
 *            нумерація після перезапуску продовжилась (E2E-06). Має бути
 *            дешевою: значення кешується в init журналу, а не шукається скануванням
 *            при кожному виклику. Викликається один раз, перед першим записом. */
typedef struct
{
    dev_status_t (*append)(const sl_log_rec_t *rec);
    dev_status_t (*last_seq)(uint32_t *last_seq);
} sl_journal_ops_t;

/* Бекенд журналу. Слабка реалізація — «Flash недоступний»; H09 визначає сильну. */
const sl_journal_ops_t *SmartLock_JournalOps(void);

/* Порожня FIFO, лічильники 0, наступний seq = 1 (нумерацію буде відновлено з
 * ops->last_seq перед першим записом). Стан після скидання МК (нульова
 * ініціалізація) такий самий, тому на платі виклик не обов'язковий. */
void SmartLock_JournalInit(void);

/* Новий запис. Повертає його ID (seq) або SL_LOG_ID_NONE, якщо тип невірний
 * або FIFO повна (dropped++). ID передається в SmartLock_CameraRequest().
 * Перший виклик після скидання сам відновлює нумерацію (ops->last_seq). */
uint32_t SmartLock_LogEvent(sl_log_type_t type, uint32_t arg);
uint32_t SmartLock_LogEventRef(sl_log_type_t type, uint32_t arg, uint32_t ref);

/* Відновлення нумерації після перезапуску (H09: останній seq з Flash), щоб ID
 * не повторювались (E2E-06). Наступний запис отримає last_seq + 1. */
void SmartLock_JournalSetLastSeq(uint32_t last_seq);

/* Те саме через ops->last_seq. Можна викликати явно після ініціалізації Flash
 * (I10) — інакше перший SmartLock_LogEvent() зробить це сам. DEV_OK — нумерацію
 * відновлено; DEV_ERR_STATE — немає last_seq у бекенді або нумерація вже
 * розпочата; інший код — помилка бекенда (перший запис ще раз спробує, а далі
 * почне з 1, SmartLock_JournalSeqRestored() = false). */
dev_status_t SmartLock_JournalRestore(void);
bool         SmartLock_JournalSeqRestored(void);

/* Запис <-> байти Flash (розкладка — SL_LOG_REC_BYTES). Pack заповнює байти
 * 0..27, 28..31 лишає 0xFF під CRC (H09). Unpack повертає false для NULL,
 * недійсного seq або типу; CRC не перевіряє. */
void SmartLock_JournalRecPack(const sl_log_rec_t *rec, uint8_t out[SL_LOG_REC_BYTES]);
bool SmartLock_JournalRecUnpack(const uint8_t in[SL_LOG_REC_BYTES], sl_log_rec_t *rec);

/* ---- діагностика і тести --------------------------------------------------- */
unsigned    SmartLock_JournalPending(void);
unsigned    SmartLock_JournalMaxDepth(void);
uint32_t    SmartLock_JournalLastSeq(void);
uint32_t    SmartLock_JournalDropped(void);   /* відкинуто: FIFO повна */
uint32_t    SmartLock_JournalStored(void);    /* бекенд прийняв (DEV_OK) */
uint32_t    SmartLock_JournalUnsaved(void);   /* бекенд відмовив / недоступний (остаточно) */
uint32_t    SmartLock_JournalRetries(void);   /* повторні спроби append після тимчасової відмови */

/* i-й запис від найстаршого, без вилучення. false — такого немає. */
bool        SmartLock_JournalPeek(unsigned i, sl_log_rec_t *out);

const char *SmartLock_LogTypeName(sl_log_type_t type);

#endif /* SL_JOURNAL_H */
