/**
  ******************************************************************************
  * @file    sched.c
  * @brief   Таблиця задач Super Loop і вимірювання кожного проходу.
  ******************************************************************************
  */
#include "sched.h"

#include <string.h>

#include "app_config.h"
#include "platform.h"

static sched_task_t s_tasks[SCHED_MAX_TASKS];
static int          s_count;
static bool         s_started;

static perf_stat_t  s_loop = PERF_STAT_INIT("loop");
static perf_stat_t  s_cpu  = PERF_STAT_INIT("cpu_load_pm");
static uint64_t     s_busy_cycles;
static uint32_t     s_window_start;
static uint32_t     s_loops;

static void task_reset_stats(sched_task_t *t)
{
    Perf_StatReset(&t->exec);
    t->polls   = 0U;
    t->jit_min = INT32_MAX;
    t->jit_max = INT32_MIN;
    t->jit_n   = 0U;
    t->missed  = 0U;
    memset(t->jit_hist, 0, sizeof(t->jit_hist));
}

void Sched_Init(void)
{
    memset(s_tasks, 0, sizeof(s_tasks));
    s_count   = 0;
    s_started = false;
    Perf_StatReset(&s_loop);
    Perf_StatReset(&s_cpu);
    s_busy_cycles  = 0U;
    s_loops        = 0U;
    s_window_start = Perf_Now();
}

int Sched_Add(const char *name, sched_fn_t fn, uint32_t period_us, int probe)
{
    sched_task_t *t;

    if ((s_count >= SCHED_MAX_TASKS) || (fn == NULL) || (period_us > SCHED_MAX_PERIOD_US))
    {
        return -1;
    }
    t = &s_tasks[s_count];
    t->name       = name;
    t->fn         = fn;
    t->period_us  = period_us;
    t->probe      = probe;
    t->period_cyc = Perf_UsToCycles(period_us);
    t->exec.name  = name;
    task_reset_stats(t);
    return s_count++;
}

void Sched_Start(void)
{
    uint32_t now = Perf_Now();

    for (int i = 0; i < s_count; i++)
    {
        s_tasks[i].t_next  = now + s_tasks[i].period_cyc;
        s_tasks[i].started = false;
    }
    s_window_start = now;
    s_busy_cycles  = 0U;
    s_started      = true;
}

bool Sched_Started(void)
{
    return s_started;
}

void Sched_ResetStats(void)
{
    for (int i = 0; i < s_count; i++)
    {
        task_reset_stats(&s_tasks[i]);
        s_tasks[i].started = false;   /* перший інтервал після скидання не рахуємо */
    }
    Perf_StatReset(&s_loop);
    Perf_StatReset(&s_cpu);
    s_loops        = 0U;
    s_busy_cycles  = 0U;
    s_window_start = Perf_Now();
}

static void note_jitter(sched_task_t *t, uint32_t now)
{
    if (t->started)
    {
        uint32_t dt  = now - t->t_last;
        int32_t  jit = (int32_t)(dt - t->period_cyc);
        uint32_t abs_us;
        unsigned b;

        if (jit < t->jit_min)
        {
            t->jit_min = jit;
        }
        if (jit > t->jit_max)
        {
            t->jit_max = jit;
        }
        t->jit_n++;
        if (dt > 2U * t->period_cyc)
        {
            t->missed++;
        }

        abs_us = Perf_CyclesToUs((jit < 0) ? (uint32_t)(-(int64_t)jit) : (uint32_t)jit);
        b = (abs_us < 100U) ? 0U : (abs_us < 1000U) ? 1U : (abs_us < 10000U) ? 2U
          : (abs_us < 100000U) ? 3U : 4U;
        t->jit_hist[b]++;
    }
    t->t_last  = now;
    t->started = true;
}

void Sched_RunOnce(void)
{
    uint32_t t_loop = Perf_Now();
    uint32_t window;

    (void)Perf_TimeUs64();   /* тримати 64-бітний час: прохід < 44 с */
    PROBE_ON(PLATFORM_PROBE_LOOP);

    for (int i = 0; i < s_count; i++)
    {
        sched_task_t *t = &s_tasks[i];
        uint32_t t0;
        uint32_t dur;
        bool     did;

        if (t->period_cyc != 0U)
        {
            uint32_t now = Perf_Now();

            if ((int32_t)(now - t->t_next) < 0)
            {
                continue;   /* ще не час */
            }
            note_jitter(t, now);
            /* Без «боргу»: якщо пропустили кілька періодів, наступний запуск —
             * найближча точка сітки в майбутньому, а не серія запусків підряд. */
            t->t_next += ((now - t->t_next) / t->period_cyc + 1U) * t->period_cyc;
        }

        t0 = Perf_Now();
        if (t->probe >= 0)
        {
            PROBE_ON((unsigned)t->probe);
        }
        did = t->fn();
        if (t->probe >= 0)
        {
            PROBE_OFF((unsigned)t->probe);
        }
        dur = Perf_Now() - t0;

        t->polls++;
        /* Періодичний запуск — завжди робота; подієва задача, що лише перевірила
         * порожню чергу, — ні (інакше Super Loop завжди показував би 100 %) */
        if (did || (t->period_cyc != 0U))
        {
            Perf_StatAdd(&t->exec, dur);
            s_busy_cycles += dur;
        }
    }

    PROBE_OFF(PLATFORM_PROBE_LOOP);
    Perf_StatAdd(&s_loop, Perf_Now() - t_loop);
    s_loops++;

    window = Perf_Now() - s_window_start;
    if (window >= Perf_UsToCycles(PERF_CPU_WINDOW_MS * 1000U))
    {
        uint64_t busy = (s_busy_cycles > window) ? window : s_busy_cycles;

        Perf_StatAdd(&s_cpu, (uint32_t)((busy * 1000U) / window));
        s_busy_cycles  = 0U;
        s_window_start = Perf_Now();
    }
}

int Sched_Count(void)
{
    return s_count;
}

const sched_task_t *Sched_Task(int idx)
{
    return ((idx >= 0) && (idx < s_count)) ? &s_tasks[idx] : NULL;
}

const perf_stat_t *Sched_LoopStat(void)
{
    return &s_loop;
}

const perf_stat_t *Sched_CpuStat(void)
{
    return &s_cpu;
}

uint32_t Sched_Loops(void)
{
    return s_loops;
}
