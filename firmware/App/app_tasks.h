/**
  ******************************************************************************
  * @file    app_tasks.h
  * @brief   ЛР3 (I02): runtime-задачі SmartLock у Super Loop (docs/tasks.md).
  *
  *          Порядок у таблиці = порядок перевірки в проході:
  *            fsm → buttons → security → env → storage → mem → uart
  *          (задачу camera додає I09)
  ******************************************************************************
  */
#ifndef APP_TASKS_H
#define APP_TASKS_H

/* Час, черга подій FSM і таблиця задач. Нічого не друкує. */
void AppTasks_Init(void);

/* I06: рядки STACK і HEAP (docs/metrics-spec.md §3.6) — запам'ятовує надруковані
 * значення; далі задача mem друкує нову пару лише тоді, коли максимум зріс. */
void AppTasks_PrintMem(void);

#endif /* APP_TASKS_H */
