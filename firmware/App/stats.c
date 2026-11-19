/**
  ******************************************************************************
  * @file    stats.c
  * @brief   Вивід STATS і скидання лічильників.
  ******************************************************************************
  */
#include "stats.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_tasks.h"
#include "cam_test.h"
#include "cmd.h"
#include "event_queue.h"
#include "evt.h"
#include "perf.h"
#include "platform.h"
#include "sched.h"

static void print_stat(const perf_stat_t *s, bool raw)
{
    char line[112];

    (void)Perf_FormatStat(line, sizeof(line), s, raw);
    printf("%s\r\n", line);
}

static void print_task(const sched_task_t *t, bool raw)
{
    print_stat(&t->exec, raw);
    if (t->period_cyc != 0U)
    {
        /* docs/metrics-spec.md §3.4 (M-04) */
        printf("JITTER name=%s period_us=%lu min_us=%ld max_us=%ld missed=%lu\r\n",
               t->name, (unsigned long)t->period_us,
               (long)((t->jit_n != 0U) ? Perf_CyclesToUsSigned(t->jit_min) : 0),
               (long)((t->jit_n != 0U) ? Perf_CyclesToUsSigned(t->jit_max) : 0),
               (unsigned long)t->missed);
        /* гістограма |джитера| — окремим рядком, щоб не ламати формат JITTER */
        printf("JHIST name=%s n=%lu h_lt100us=%lu h_lt1ms=%lu h_lt10ms=%lu h_lt100ms=%lu "
               "h_ge100ms=%lu\r\n",
               t->name, (unsigned long)t->jit_n,
               (unsigned long)t->jit_hist[0], (unsigned long)t->jit_hist[1],
               (unsigned long)t->jit_hist[2], (unsigned long)t->jit_hist[3],
               (unsigned long)t->jit_hist[4]);
    }
    else
    {
        printf("POLL name=%s polls=%lu runs=%lu\r\n", t->name, (unsigned long)t->polls,
               (unsigned long)t->exec.count);
    }
}

void Stats_Print(bool raw)
{
    char t_us[21];
    const perf_stat_t *cpu = Sched_CpuStat();

    (void)Perf_FormatU64(t_us, sizeof(t_us), Perf_TimeUs64());
    printf("STATS_BEGIN git=%s t_us=%s loops=%lu\r\n", GIT_HASH, t_us,
           (unsigned long)Sched_Loops());
    print_stat(Sched_LoopStat(), raw);
    printf("CPU name=cpu_load windows=%lu min_pm=%lu avg_pm=%lu max_pm=%lu\r\n",
           (unsigned long)cpu->count, (unsigned long)cpu->min,
           (unsigned long)Perf_StatAvg(cpu), (unsigned long)cpu->max);

    for (int i = 0; i < Sched_Count(); i++)
    {
        print_task(Sched_Task(i), raw);
    }

    for (int i = 0; i < AppTasks_LatencyCount(); i++)
    {
        const perf_latency_t *l = AppTasks_Latency(i);

        /* затримка — звичайний STAT (metrics-spec §3.2, M-03); повторні фронти до
         * обробки — окремим рядком OVR, щоб рядок STAT лишився за регулярним виразом */
        print_stat(&l->st, raw);
        printf("OVR name=%.*s count=%lu\r\n", (int)(strlen(l->st.name) - 4U), l->st.name,
               (unsigned long)Platform_EventOverruns(l->ev));
    }

    print_stat(CamTest_Stat(), raw);   /* I07: тривалість захоплення кадру (CAM TEST) */
    print_stat(CamTest_StoreStat(), raw);   /* I09: запис кадру у W25Q64 (camera.c) */
    print_stat(SmartLock_EventsWaitStat(), raw);
    printf("EVQ len=%u pending=%u max_depth=%u posted=%lu dropped=%lu\r\n", (unsigned)TBD_EVQ_LEN,
           SmartLock_EventsPending(), SmartLock_EventsMaxDepth(),
           (unsigned long)SmartLock_EventsPosted(), (unsigned long)SmartLock_EventsDropped());
    printf("UART rx_overrun=%lu cmd_overflow=%lu\r\n",
           (unsigned long)Platform_UartOverruns(), (unsigned long)Cmd_LineOverflows());
    printf("EVTLOG pending=%u dropped=%lu\r\n", Evt_Pending(), (unsigned long)Evt_Dropped());
    AppTasks_PrintMem();
    printf("STATS_END\r\n");
}

void Stats_Reset(void)
{
    Sched_ResetStats();
    AppTasks_ResetLatency();
    CamTest_ResetStats();
    SmartLock_EventsResetStats();
}

/* docs/metrics-spec.md §3.5 (M-05): «LOAD pct=<0..100>» раз на секунду — щойно
 * планувальник закрив чергове вікно CPU Load. Один рядок ≈ 1,2 мс у UART раз на
 * секунду (~0,1 % CPU). Вимкнути — TBD_LOAD_PRINT = 0. */
static uint32_t s_load_seen;

bool Stats_PollLoad(void)
{
    uint32_t windows = Sched_CpuStat()->count;

    if (windows < s_load_seen)   /* після STATS RESET лічильник вікон почався з нуля */
    {
        s_load_seen = 0U;
    }
    if ((TBD_LOAD_PRINT == 0U) || (windows == s_load_seen))
    {
        return false;
    }
    s_load_seen = windows;
    printf("LOAD pct=%lu\r\n", (unsigned long)((Sched_CpuLastPm() + 5U) / 10U));
    return true;
}
