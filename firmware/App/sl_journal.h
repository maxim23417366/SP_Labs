/**
  ******************************************************************************
  * @file    sl_journal.h
  * @brief   ЛР3 (K07, Коваль): журнал подій SmartLock — RAM-FIFO і задача storage.
  *
  *          FR-08, FR-10, FR-12, FR-13, FR-14. Хто завгодно в головному циклі
  *          (FSM, зв'язок з камерою sl_camera_link — K08) викликає SmartLock_LogEvent(): запис
  *          {seq, t_ms, type, arg, ref} стає в RAM-FIFO і отримує ID (seq).
  *          Задача storage (SmartLock_StoragePoll, сильна реалізація тут) за
  *          один прохід забирає ОДИН запис, друкує рядок EVT у UART (FR-12) і
  *          передає його бекенду журналу Flash через sl_journal_ops_t.append.
  *
  *          Рядок UART:
  *            EVT,id=<seq>,t_ms=<мс>,name=<ТИП>,arg=<arg>[,ref=<ref>],flash=ok|err
  *          (стиль ключ=значення, як BUILD / ENV / TIME; імена типів — як у PRD).
  *
  *          Рядок журналу відрізняється від часових міток I04 (evt.h,
  *          "EVT t_us=... name=...") комою після EVT: регулярний вираз
  *          docs/metrics/metrics-spec.md §3.3 (^EVT t_us=...) його не захоплює.
  *
  *          Бекенд: слабка SmartLock_JournalOps() повертає «Flash недоступний»
  *          (append → DEV_ERR_STATE), такі записи лише рахуються (unsaved).
  *          Журнал W25Q64 (H09, Колодницький, App/storage.c) визначає сильну
  *          SmartLock_JournalOps() у App/storage_ops.c — без правок цього модуля.
  *          Формат запису у Flash, CRC і коди типів — storage.c і
  *          docs/lab_3/flash-layout.md, розділ 3 (Storage_FlashType()).
  *
  *          Нумерація після рестарту: Storage_Init() після монтування передає
  *          останній seq з Flash у SmartLock_JournalSetLastSeq() — ID у рядку EVT
  *          і seq у Flash збігаються і не повторюються (E2E-06).
  *
  *          Відмова бекенда: запис виходить з FIFO одразу, у UART — flash=err,
  *          лічильник unsaved. Повторів тут немає навмисно: storage.c сам
  *          повторює стирання сектора наступним записом і вимикає журнал після
  *          TBD_STORAGE_ERASE_RETRIES невдач, а невдалий запис уже витратив seq
  *          (flash-layout.md, розділ 3.3), тож повтор того самого запису дав би
  *          DEV_ERR_PARAM. Недоступна Flash не гальмує UART-журнал.
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

#define SL_LOG_ID_NONE         0U   /* «запису немає» (FIFO повна або невірний тип) */

/* Типи записів. У Flash пишеться не це значення, а код з flash-layout.md, розділ 3.2
 * (Storage_FlashType() у storage.c): новий тип додавати в кінець і туди ж. */
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
    SL_LOG_PHOTO_ERR,        /* кадр не отримано / не збережено (K08): arg = sl_cam_status_t, ref = seq події MOTION/TAMPER */
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

/* Вузький інтерфейс до сховища журналу (H09, storage.c). Викликається лише з
 * задачі storage, не частіше одного append за прохід. append не чекає довше за
 * одну порцію роботи (запис ≤ 2 мс; раз на 128 записів — стирання сектора).
 * DEV_OK — запис збережено; будь-який інший код — запис не збережено. */
typedef struct
{
    dev_status_t (*append)(const sl_log_rec_t *rec);
} sl_journal_ops_t;

/* Бекенд журналу. Слабка реалізація — «Flash недоступний»; H09 визначає сильну. */
const sl_journal_ops_t *SmartLock_JournalOps(void);

/* Порожня FIFO, лічильники 0, наступний seq = 1. Стан після скидання МК
 * (нульова ініціалізація) такий самий, тому на платі виклик не обов'язковий. */
void SmartLock_JournalInit(void);

/* Новий запис. Повертає його ID (seq) або SL_LOG_ID_NONE, якщо тип невірний
 * або FIFO повна (dropped++). ID передається в SmartLock_CameraLinkStart(). */
uint32_t SmartLock_LogEvent(sl_log_type_t type, uint32_t arg);
uint32_t SmartLock_LogEventRef(sl_log_type_t type, uint32_t arg, uint32_t ref);

/* Відновлення нумерації після перезапуску (H09: останній seq з Flash), щоб ID
 * не повторювались (E2E-06). Наступний запис отримає last_seq + 1. Викликає
 * storage.c після монтування і після записів поза цією чергою (BOOT, стенд). */
void SmartLock_JournalSetLastSeq(uint32_t last_seq);

/* ---- діагностика і тести --------------------------------------------------- */
unsigned    SmartLock_JournalPending(void);
unsigned    SmartLock_JournalMaxDepth(void);
uint32_t    SmartLock_JournalLastSeq(void);
uint32_t    SmartLock_JournalDropped(void);   /* відкинуто: FIFO повна */
uint32_t    SmartLock_JournalStored(void);    /* бекенд прийняв (DEV_OK) */
uint32_t    SmartLock_JournalUnsaved(void);   /* бекенд відмовив / недоступний */

/* i-й запис від найстаршого, без вилучення. false — такого немає. */
bool        SmartLock_JournalPeek(unsigned i, sl_log_rec_t *out);

const char *SmartLock_LogTypeName(sl_log_type_t type);

#endif /* SL_JOURNAL_H */
