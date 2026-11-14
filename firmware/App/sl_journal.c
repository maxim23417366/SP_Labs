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

/* ---- бекенд за замовчуванням: журналу у Flash ще немає (H09) ------------------ */
static dev_status_t flash_unavailable_append(const sl_log_rec_t *rec)
{
    (void)rec;
    return DEV_ERR_STATE;
}

__attribute__((weak)) const sl_journal_ops_t *SmartLock_JournalOps(void)
{
    static const sl_journal_ops_t ops = { flash_unavailable_append };

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
}

void SmartLock_JournalSetLastSeq(uint32_t last_seq)
{
    s_last_seq = last_seq;
}

uint32_t SmartLock_LogEventRef(sl_log_type_t type, uint32_t arg, uint32_t ref)
{
    sl_log_rec_t *r;

    if ((type <= SL_LOG_NONE) || (type >= SL_LOG_COUNT))
    {
        return SL_LOG_ID_NONE;
    }
    s_last_seq++;
    if (s_last_seq == SL_LOG_ID_NONE)   /* після 2^32 записів — пропустити 0 */
    {
        s_last_seq = 1U;
    }
    if (s_count >= TBD_JOURNAL_FIFO_LEN)
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
bool SmartLock_StoragePoll(uint32_t now_ms)
{
    const sl_journal_ops_t *ops;
    sl_log_rec_t rec;
    dev_status_t st = DEV_ERR_STATE;

    (void)now_ms;
    if (s_count == 0U)
    {
        return false;
    }
    rec = s_fifo[s_tail];
    s_tail = (s_tail + 1U) % TBD_JOURNAL_FIFO_LEN;
    s_count--;

    ops = SmartLock_JournalOps();
    if ((ops != NULL) && (ops->append != NULL))
    {
        st = ops->append(&rec);
    }
    if (st == DEV_OK)
    {
        s_stored++;
    }
    else
    {
        s_unsaved++;
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
        "MOTION", "TAMPER", "FORCED_OPEN", "ENV_WARNING", "DOOR_OPEN", "DOOR_CLOSED", "PHOTO",
    };

    return ((unsigned)type < (unsigned)SL_LOG_COUNT) ? names[type] : "?";
}
