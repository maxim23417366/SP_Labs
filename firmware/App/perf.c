/**
  ******************************************************************************
  * @file    perf.c
  * @brief   Лічильник тактів, статистика min/avg/max, 64-бітний час.
  ******************************************************************************
  */
#include "perf.h"

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
