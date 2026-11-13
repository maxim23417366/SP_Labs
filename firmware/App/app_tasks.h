/**
  ******************************************************************************
  * @file    app_tasks.h
  * @brief   ЛР3 (I02/I10): runtime-задачі SmartLock у Super Loop (docs/tasks.md).
  *
  *          Порядок у таблиці = порядок перевірки в проході:
  *            fsm → buttons → security → env → camera → storage → uart
  ******************************************************************************
  */
#ifndef APP_TASKS_H
#define APP_TASKS_H

#include "perf.h"

/* Час, події, EVT, команди, камера (без SCCB — її налаштовує App_Init) і таблиця
 * задач. Нічого не друкує. */
void AppTasks_Init(void);

/* Затримки «фронт в EXTI → задача забрала подію» (LAT у STATS) */
int                   AppTasks_LatencyCount(void);
const perf_latency_t *AppTasks_Latency(int idx);
void                  AppTasks_ResetLatency(void);

#endif /* APP_TASKS_H */
