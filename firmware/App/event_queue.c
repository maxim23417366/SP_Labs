/**
  ******************************************************************************
  * @file    event_queue.c
  * @brief   Кільцевий буфер подій FSM (див. event_queue.h).
  ******************************************************************************
  */
#include "event_queue.h"

#include "app_config.h"

static sl_event_t  s_q[TBD_EVQ_LEN];
static unsigned    s_head;
static unsigned    s_tail;
static unsigned    s_count;
static unsigned    s_max_depth;
static uint32_t    s_dropped;
static uint32_t    s_posted;
static perf_stat_t s_wait = PERF_STAT_INIT("evq_wait");

void SmartLock_EventsInit(void)
{
    s_head = 0U;
    s_tail = 0U;
    s_count = 0U;
    SmartLock_EventsResetStats();
}

void SmartLock_EventsResetStats(void)
{
    s_max_depth = s_count;
    s_dropped = 0U;
    s_posted = 0U;
    Perf_StatReset(&s_wait);
}

bool SmartLock_PostEvent(sl_event_type_t type, uint32_t arg)
{
    if ((type <= SL_EV_NONE) || (type >= SL_EV_COUNT))
    {
        return false;
    }
    if (s_count >= TBD_EVQ_LEN)
    {
        s_dropped++;
        return false;
    }
    s_q[s_head].type = type;
    s_q[s_head].arg = arg;
    s_q[s_head].t_cycles = Perf_Now();
    s_head = (s_head + 1U) % TBD_EVQ_LEN;
    s_count++;
    s_posted++;
    if (s_count > s_max_depth)
    {
        s_max_depth = s_count;
    }
    return true;
}

bool SmartLock_TakeEvent(sl_event_t *ev)
{
    if ((s_count == 0U) || (ev == NULL))
    {
        return false;
    }
    *ev = s_q[s_tail];
    s_tail = (s_tail + 1U) % TBD_EVQ_LEN;
    s_count--;
    Perf_StatAdd(&s_wait, Perf_Now() - ev->t_cycles);
    return true;
}

unsigned SmartLock_EventsPending(void)
{
    return s_count;
}

unsigned SmartLock_EventsMaxDepth(void)
{
    return s_max_depth;
}

uint32_t SmartLock_EventsDropped(void)
{
    return s_dropped;
}

uint32_t SmartLock_EventsPosted(void)
{
    return s_posted;
}

const perf_stat_t *SmartLock_EventsWaitStat(void)
{
    return &s_wait;
}

const char *SmartLock_EventName(sl_event_type_t type)
{
    static const char *const names[SL_EV_COUNT] =
    {
        "none", "lock_request", "motion", "tamper", "door_open", "door_closed",
        "code_ok", "code_wrong", "env_warning",
    };

    return (type < SL_EV_COUNT) ? names[type] : "?";
}
