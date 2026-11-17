/**
  ******************************************************************************
  * @file    stats.h
  * @brief   ЛР3 (I03): команда STATS — один рядок на кожну метрику, формат
  *          «ключ=значення» для парсера M03 (методичка §5.1):
  *
  *   STATS_BEGIN git=<hash> t_us=<64-біт> loops=<проходів>
  *   STAT name=loop n=.. min_us=.. avg_us=.. max_us=..           — прохід циклу
  *   CPU name=cpu_load windows=.. min_pm=.. avg_pm=.. max_pm=..  — ‰ за вікно 1 с
  *   STAT name=<задача> n=.. min_us=.. avg_us=.. max_us=..       — час виконання
  *   JITTER name=<задача> period_us=.. min_us=.. max_us=.. missed=..   — періодичні
  *   JHIST name=<задача> n=.. h_lt100us=.. h_lt1ms=.. h_lt10ms=.. h_lt100ms=.. h_ge100ms=..
  *   POLL name=<задача> polls=.. runs=..                         — подієві задачі
  *   STAT name=btn_b1_lat|pir_lat|mpu_lat n=.. min_us=.. avg_us=.. max_us=..   (I04)
  *   OVR name=btn_b1|pir|mpu count=..   — повторні фронти до обробки (I04)
  *   STAT name=evq_wait ...                                      — подія в черзі FSM
  *   EVQ len=.. pending=.. max_depth=.. posted=.. dropped=..
  *   UART rx_overrun=.. cmd_overflow=..
  *   EVTLOG pending=.. dropped=..                                (I04)
  *   STACK reserved=.. used=.. free=..
  *   HEAP arena=.. used=.. free=..
  *   STATS_END
  *
  *   Формати STAT / JITTER / LOAD / STACK / HEAP — docs/metrics-spec.md §3 (контракт
  *   парсера M03). Рядки CPU, JHIST, POLL, OVR, EVQ, UART, EVTLOG — додаткові, парсер їх пропускає.
  *
  *   raw_cycles = true: STAT-рядки в тактах (min_cyc=..), для перевірки округлень.
  ******************************************************************************
  */
#ifndef STATS_H
#define STATS_H

#include <stdbool.h>

void Stats_Print(bool raw_cycles);

/* Обнулити всі лічильники (STATS RESET): початок сценарію з чистого аркуша */
void Stats_Reset(void);

/* «LOAD pct=N» раз на секунду (metrics-spec §3.5); викликає uart-задача.
 * true — рядок надруковано. */
bool Stats_PollLoad(void);

#endif /* STATS_H */
