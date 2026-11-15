/**
  ******************************************************************************
  * @file    perf.c
  * @brief   Лічильник тактів, статистика min/avg/max, 64-бітний час.
  ******************************************************************************
  */
#include "perf.h"

#include <stdio.h>

static uint32_t s_last_cycles;
static uint64_t s_total_cycles;

void Perf_Init(void)
{
    s_last_cycles  = Platform_Cycles();
    s_total_cycles = 0U;
}

uint32_t Perf_CyclesToUs(uint32_t cycles)
{
    return (uint32_t)(((uint64_t)cycles * 1000000U) / Platform_CpuHz());
}

int32_t Perf_CyclesToUsSigned(int32_t cycles)
{
    return (int32_t)(((int64_t)cycles * 1000000) / (int64_t)Platform_CpuHz());
}

uint32_t Perf_UsToCycles(uint32_t us)
{
    return (uint32_t)(((uint64_t)us * Platform_CpuHz()) / 1000000U);
}

static uint64_t time_cycles64(void)
{
    uint32_t c = Platform_Cycles();

    s_total_cycles += (uint32_t)(c - s_last_cycles);   /* коректно після переповнення */
    s_last_cycles = c;
    return s_total_cycles;
}

static uint64_t cycles64_to_us(uint64_t c)
{
    uint64_t hz = Platform_CpuHz();

    /* без переповнення c * 1e6: окремо цілі секунди і залишок */
    return (c / hz) * 1000000U + ((c % hz) * 1000000U) / hz;
}

uint64_t Perf_TimeUs64(void)
{
    return cycles64_to_us(time_cycles64());
}

uint64_t Perf_CyclesStampToUs64(uint32_t t_cycles)
{
    uint64_t now   = time_cycles64();
    uint32_t delta = s_last_cycles - t_cycles;   /* скільки тактів тому */

    return cycles64_to_us((delta <= now) ? (now - delta) : 0U);
}

void Perf_StatReset(perf_stat_t *s)
{
    s->count = 0U;
    s->min   = 0U;
    s->max   = 0U;
    s->sum   = 0U;
}

void Perf_StatAdd(perf_stat_t *s, uint32_t value)
{
    if ((s->count == 0U) || (value < s->min))
    {
        s->min = value;
    }
    if (value > s->max)
    {
        s->max = value;
    }
    s->sum += value;
    s->count++;
}

uint32_t Perf_StatAvg(const perf_stat_t *s)
{
    return (s->count != 0U) ? (uint32_t)(s->sum / s->count) : 0U;
}

int Perf_FormatStat(char *buf, size_t size, const perf_stat_t *s, bool raw_cycles)
{
    uint32_t avg = Perf_StatAvg(s);

    if (raw_cycles)
    {
        return snprintf(buf, size, "STAT name=%s n=%lu min_cyc=%lu avg_cyc=%lu max_cyc=%lu",
                        s->name, (unsigned long)s->count, (unsigned long)s->min,
                        (unsigned long)avg, (unsigned long)s->max);
    }
    return snprintf(buf, size, "STAT name=%s n=%lu min_us=%lu avg_us=%lu max_us=%lu",
                    s->name, (unsigned long)s->count,
                    (unsigned long)Perf_CyclesToUs(s->min),
                    (unsigned long)Perf_CyclesToUs(avg),
                    (unsigned long)Perf_CyclesToUs(s->max));
}

size_t Perf_FormatU64(char *buf, size_t size, uint64_t value)
{
    char   tmp[21];
    size_t n = 0U;

    do
    {
        tmp[n++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    } while (value != 0U);

    if (size < n + 1U)
    {
        if (size != 0U)
        {
            buf[0] = '\0';
        }
        return 0U;
    }
    for (size_t i = 0U; i < n; i++)
    {
        buf[i] = tmp[n - 1U - i];
    }
    buf[n] = '\0';
    return n;
}

void Perf_LatencyInit(perf_latency_t *l, const char *name, platform_event_t ev)
{
    l->st.name = name;
    Perf_StatReset(&l->st);
    l->ev = ev;
    l->last_t_cycles = 0U;
}

bool Perf_LatencyTake(perf_latency_t *l, uint64_t *t_event_us)
{
    uint32_t t_ev;

    if (Platform_TakeEvent(l->ev, &t_ev) == 0)
    {
        return false;
    }
    Perf_StatAdd(&l->st, Platform_Cycles() - t_ev);
    l->last_t_cycles = t_ev;
    if (t_event_us != NULL)
    {
        *t_event_us = Perf_CyclesStampToUs64(t_ev);
    }
    return true;
}
