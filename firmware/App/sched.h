/**
  ******************************************************************************
  * @file    sched.h
  * @brief   ЛР3 (I02): кооперативний Super Loop з мітками часу (методичка §4.1).
  *
  *          НЕ RTOS: немає витіснення, немає окремих стеків, немає блокування
  *          з timeout. Задача — звичайна функція, яку цикл викликає по черзі:
  *            - period_us = 0  — у кожному проході (подієва: сама перевіряє чергу);
  *            - period_us > 0  — коли настав її час (мітка часу на DWT).
  *          Порядок додавання = порядок перевірки в проході (це і є «пріоритет»).
  *
  *          Вимірювання на кожну задачу: час виконання min/avg/max, кількість
  *          викликів, джитер і пропущені запуски (періодичні), CPU Load (вікно 1 с).
  *          Задача повертає true, якщо виконала корисну роботу: виклик, який лише
  *          перевірив, що робити нічого, у CPU Load не потрапляє (§5.1, приклад 4).
  ******************************************************************************
  */
#ifndef SCHED_H
#define SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "perf.h"

#define SCHED_MAX_TASKS      10
#define SCHED_MAX_PERIOD_US  20000000U   /* знакове порівняння тактів коректне до ~22 с */
#define SCHED_NO_PROBE       (-1)   /* службові GPIO підключає I05 */

/* Гістограма |джитера|: <100 мкс, <1 мс, <10 мс, <100 мс, ≥100 мс */
#define SCHED_JIT_BUCKETS    5U

typedef bool (*sched_fn_t)(void);

typedef struct
{
    const char  *name;
    sched_fn_t   fn;
    uint32_t     period_us;
    int          probe;          /* канал службового GPIO (I05) або SCHED_NO_PROBE */

    uint32_t     period_cyc;
    uint32_t     t_next;         /* такт наступного запуску (періодичні) */
    uint32_t     t_last;         /* такт попереднього фактичного запуску */
    bool         started;

    perf_stat_t  exec;           /* такти; лише запуски з корисною роботою */
    uint32_t     polls;          /* скільки разів викликано fn */

    int32_t      jit_min;        /* такти: (фактичний інтервал − період) */
    int32_t      jit_max;
    uint32_t     jit_n;
    uint32_t     missed;         /* інтервал > 2 періодів (§5.1, приклад 3) */
    uint32_t     jit_hist[SCHED_JIT_BUCKETS];
} sched_task_t;

void Sched_Init(void);

/* Повертає індекс задачі або -1 (таблиця повна, fn == NULL, завеликий період). */
int  Sched_Add(const char *name, sched_fn_t fn, uint32_t period_us, int probe);

/* Перший запуск періодичної задачі — через один період після старту, щоб усі
 * задачі не запускались в одному проході одразу після ініціалізації. */
void Sched_Start(void);
bool Sched_Started(void);

/* Один прохід Super Loop: усі задачі по черзі. Викликається з App_Loop(). */
void Sched_RunOnce(void);

void Sched_ResetStats(void);

int                 Sched_Count(void);
const sched_task_t *Sched_Task(int idx);
const perf_stat_t  *Sched_LoopStat(void);   /* такти одного проходу */
const perf_stat_t  *Sched_CpuStat(void);    /* завантаження за вікно, ‰ (проміле) */
uint32_t            Sched_Loops(void);

#endif /* SCHED_H */
