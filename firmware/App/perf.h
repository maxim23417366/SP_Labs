/**
  ******************************************************************************
  * @file    perf.h
  * @brief   ЛР3 (I02): час Super Loop на DWT CYCCNT — мінімум для планувальника.
  *
  *          Час усередині — у тактах ядра (≈10,4 нс при 96 МГц); у мкс
  *          перераховується лише при виведенні. Різниця двох 32-бітних значень
  *          коректна і після одного переповнення (інтервал < ~44,7 с).
  *          Perf_TimeUs64() — 64-бітне розширення; його оновлює кожен прохід
  *          Sched_RunOnce(). ЛИШЕ з головного циклу (не з ISR).
  *
  *          I03–I05 доповнюють модуль: затримка подія→реакція, формат STAT, EVT.
  ******************************************************************************
  */
#ifndef PERF_H
#define PERF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

typedef struct
{
    const char *name;
    uint32_t    count;
    uint32_t    min;
    uint32_t    max;
    uint64_t    sum;
} perf_stat_t;

#define PERF_STAT_INIT(n) { (n), 0U, 0U, 0U, 0U }

void     Perf_Init(void);

static inline uint32_t Perf_Now(void)
{
    return Platform_Cycles();
}

uint32_t Perf_CyclesToUs(uint32_t cycles);
uint32_t Perf_UsToCycles(uint32_t us);

/* Мікросекунди від Perf_Init(), 64 біти. Лише з головного циклу. */
uint64_t Perf_TimeUs64(void);

void     Perf_StatReset(perf_stat_t *s);
void     Perf_StatAdd(perf_stat_t *s, uint32_t value);
uint32_t Perf_StatAvg(const perf_stat_t *s);

#endif /* PERF_H */
