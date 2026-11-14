/**
  ******************************************************************************
  * @file    sl_journal.c
  * @brief   ЛР3 (K07): RAM-FIFO журналу і задача storage (див. sl_journal.h).
  ******************************************************************************
  */
#include "sl_journal.h"

#include <stddef.h>
#include <stdio.h>

#include "platform.h"
#include "smartlock_hooks.h"

static sl_log_rec_t s_fifo[TBD_JOURNAL_FIFO_LEN];
static unsigned     s_head;
static unsigned     s_tail;
static unsigned     s_count;
static unsigned     s_max_depth;
static uint32_t     s_last_seq;
static uint32_t     s_dropped;
static uint32_t     s_stored;
static uint32_t     s_unsaved;
static uint32_t     s_retries;
static unsigned     s_retry_n;        /* невдалих спроб append для запису на чолі FIFO */
static uint32_t     s_retry_at_ms;    /* час останньої невдалої спроби */
static bool         s_degraded;       /* остаточна відмова: Flash не працює, повторів немає */
static bool         s_seq_ready;      /* нумерацію відновлено або вже розпочато */
static bool         s_restored;       /* last_seq прийнято з бекенда */

/* ---- бекенд за замовчуванням: журналу у Flash ще немає (H09) ------------------ */
static dev_status_t flash_unavailable_append(const sl_log_rec_t *rec)
{
    (void)rec;
    return DEV_ERR_STATE;
}

__attribute__((weak)) const sl_journal_ops_t *SmartLock_JournalOps(void)
{
    static const sl_journal_ops_t ops = { flash_unavailable_append, NULL };   /* без last_seq: нумерація з 1 */

    return &ops;
}

/* ---- FIFO ------------------------------------------------------------------------ */
void SmartLock_JournalInit(void)
{
    s_head = 0U;
    s_tail = 0U;
    s_count = 0U;
    s_max_depth = 0U;
    s_last_seq = 0U;
    s_dropped = 0U;
    s_stored = 0U;
    s_unsaved = 0U;
    s_retries = 0U;
    s_retry_n = 0U;
    s_retry_at_ms = 0U;
    s_degraded = false;
    s_seq_ready = false;
    s_restored = false;
}

void SmartLock_JournalSetLastSeq(uint32_t last_seq)
{
    s_last_seq = last_seq;
    s_seq_ready = true;
    s_restored = true;
}

dev_status_t SmartLock_JournalRestore(void)
{
    const sl_journal_ops_t *ops;
    uint32_t last = 0U;
    dev_status_t st;

    if (s_seq_ready)
    {
        return DEV_ERR_STATE;   /* вже відновлено або перший ID видано */
    }
    ops = SmartLock_JournalOps();
    if ((ops == NULL) || (ops->last_seq == NULL))
    {
        return DEV_ERR_STATE;
    }
    st = ops->last_seq(&last);
    if (st == DEV_OK)
    {
        SmartLock_JournalSetLastSeq(last);
    }
    return st;
}

bool SmartLock_JournalSeqRestored(void)
{
    return s_restored;
}

/* Бінарний вигляд запису (little-endian), див. SL_LOG_REC_BYTES у заголовку */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void SmartLock_JournalRecPack(const sl_log_rec_t *rec, uint8_t out[SL_LOG_REC_BYTES])
{
    for (unsigned i = 0U; i < SL_LOG_REC_BYTES; i++)
    {
        out[i] = 0xFFU;   /* резерв і місце CRC лишаються як у стертій Flash */
    }
    put_u32(&out[0], rec->seq);
    put_u32(&out[4], rec->t_ms);
    out[8] = rec->type;
    put_u32(&out[12], rec->arg);
    put_u32(&out[16], rec->ref);
}

bool SmartLock_JournalRecUnpack(const uint8_t in[SL_LOG_REC_BYTES], sl_log_rec_t *rec)
{
    uint32_t seq;

    if ((in == NULL) || (rec == NULL))
    {
        return false;
    }
    seq = get_u32(&in[0]);
    if ((seq == SL_LOG_ID_NONE) || (seq == 0xFFFFFFFFU) ||
        (in[8] == (uint8_t)SL_LOG_NONE) || (in[8] >= (uint8_t)SL_LOG_COUNT))
    {
        return false;
    }
    rec->seq = seq;
    rec->t_ms = get_u32(&in[4]);
    rec->type = in[8];
    rec->arg = get_u32(&in[12]);
    rec->ref = get_u32(&in[16]);
    return true;
}

/* Перший запис після скидання: підхопити останній seq з Flash (один раз).
 * Не вийшло — починаємо з 1, SmartLock_JournalSeqRestored() покаже false. */
static void ensure_seq_ready(void)
{
    if (!s_seq_ready)
    {
        (void)SmartLock_JournalRestore();
        s_seq_ready = true;
    }
}

uint32_t SmartLock_LogEventRef(sl_log_type_t type, uint32_t arg, uint32_t ref)
{
    sl_log_rec_t *r;
    bool closing = (type == SL_LOG_PHOTO) || (type == SL_LOG_PHOTO_ERR);
    unsigned limit = closing ? TBD_JOURNAL_FIFO_LEN : (TBD_JOURNAL_FIFO_LEN - TBD_JOURNAL_FIFO_RESERVE);

    if ((type <= SL_LOG_NONE) || (type >= SL_LOG_COUNT))
    {
        return SL_LOG_ID_NONE;
    }
    ensure_seq_ready();
    s_last_seq++;
    if (s_last_seq == SL_LOG_ID_NONE)   /* після 2^32 записів — пропустити 0 */
    {
        s_last_seq = 1U;
    }
    if (s_count >= limit)
    {
        s_dropped++;
        return SL_LOG_ID_NONE;
    }
    r = &s_fifo[s_head];
    r->seq = s_last_seq;
    r->t_ms = Platform_Millis();
    r->arg = arg;
    r->ref = ref;
    r->type = (uint8_t)type;
    s_head = (s_head + 1U) % TBD_JOURNAL_FIFO_LEN;
    s_count++;
    if (s_count > s_max_depth)
    {
        s_max_depth = s_count;
    }
    return s_last_seq;
}

uint32_t SmartLock_LogEvent(sl_log_type_t type, uint32_t arg)
{
    return SmartLock_LogEventRef(type, arg, 0U);
}

/* ---- задача storage: одна порція за прохід ---------------------------------------- */
/* Тимчасова відмова — Flash зайнята (WIP) або збій SPI; решта кодів остаточні */
static bool is_transient(dev_status_t st)
{
    return (st == DEV_ERR_TIMEOUT) || (st == DEV_ERR_BUS);
}

bool SmartLock_StoragePoll(uint32_t now_ms)
{
    const sl_journal_ops_t *ops;
    sl_log_rec_t rec;
    dev_status_t st = DEV_ERR_STATE;

    if (s_count == 0U)
    {
        return false;
    }
    if ((s_retry_n > 0U) && ((uint32_t)(now_ms - s_retry_at_ms) < TBD_JOURNAL_RETRY_MS))
    {
        return false;   /* чекаємо між повторами, не блокуючи цикл */
    }
    rec = s_fifo[s_tail];

    ops = SmartLock_JournalOps();
    if ((ops != NULL) && (ops->append != NULL))
    {
        st = ops->append(&rec);
    }
    if (is_transient(st) && !s_degraded && (s_retry_n < TBD_JOURNAL_RETRY_MAX))
    {
        s_retry_n++;
        s_retries++;
        s_retry_at_ms = now_ms;
        return true;   /* запис лишився на чолі FIFO, UART-рядок — після підсумку */
    }
    s_tail = (s_tail + 1U) % TBD_JOURNAL_FIFO_LEN;
    s_count--;
    s_retry_n = 0U;
    if (st == DEV_OK)
    {
        s_stored++;
        s_degraded = false;
    }
    else
    {
        s_unsaved++;
        s_degraded = true;
    }

    printf("EVT,id=%lu,t_ms=%lu,name=%s,arg=%lu", (unsigned long)rec.seq, (unsigned long)rec.t_ms,
           SmartLock_LogTypeName((sl_log_type_t)rec.type), (unsigned long)rec.arg);
    if (rec.ref != 0U)
    {
        printf(",ref=%lu", (unsigned long)rec.ref);
    }
    printf(",flash=%s\r\n", (st == DEV_OK) ? "ok" : "err");
    return true;
}

/* ---- діагностика ---------------------------------------------------------------- */
unsigned SmartLock_JournalPending(void)
{
    return s_count;
}

unsigned SmartLock_JournalMaxDepth(void)
{
    return s_max_depth;
}

uint32_t SmartLock_JournalLastSeq(void)
{
    return s_last_seq;
}

uint32_t SmartLock_JournalDropped(void)
{
    return s_dropped;
}

uint32_t SmartLock_JournalStored(void)
{
    return s_stored;
}

uint32_t SmartLock_JournalUnsaved(void)
{
    return s_unsaved;
}

uint32_t SmartLock_JournalRetries(void)
{
    return s_retries;
}

bool SmartLock_JournalPeek(unsigned i, sl_log_rec_t *out)
{
    if ((out == NULL) || (i >= s_count))
    {
        return false;
    }
    *out = s_fifo[(s_tail + i) % TBD_JOURNAL_FIFO_LEN];
    return true;
}

const char *SmartLock_LogTypeName(sl_log_type_t type)
{
    static const char *const names[SL_LOG_COUNT] =
    {
        "NONE", "LOCK_ACTIVATED", "LOCKED", "UNLOCKED", "ACCESS_GRANTED", "WRONG_CODE",
        "MOTION", "TAMPER", "FORCED_OPEN", "ENV_WARNING", "DOOR_OPEN", "DOOR_CLOSED", "PHOTO", "PHOTO_ERR",
    };

    return ((unsigned)type < (unsigned)SL_LOG_COUNT) ? names[type] : "?";
}
