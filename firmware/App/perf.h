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
  *          I03 додає формат STAT; I04 — затримку подія→реакція і мітки часу ISR.
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
int32_t  Perf_CyclesToUsSigned(int32_t cycles);   /* I03: джитер буває від'ємним */
uint32_t Perf_UsToCycles(uint32_t us);

/* Мікросекунди від Perf_Init(), 64 біти. Лише з головного циклу. */
uint64_t Perf_TimeUs64(void);

void     Perf_StatReset(perf_stat_t *s);
void     Perf_StatAdd(perf_stat_t *s, uint32_t value);
uint32_t Perf_StatAvg(const perf_stat_t *s);

/* I03: "STAT name=<n> n=.. min_us=.. avg_us=.. max_us=.." (raw — у тактах: min_cyc=..)
 * Повертає довжину як snprintf. */
int Perf_FormatStat(char *buf, size_t size, const perf_stat_t *s, bool raw_cycles);

/* I03: uint64_t у десятковий рядок без %llu (newlib-nano його не підтримує, див.
 * проблему №11 ЛР2 з %zu). Повертає довжину або 0, якщо не вмістилось. */
size_t Perf_FormatU64(char *buf, size_t size, uint64_t value);

#endif /* PERF_H */
