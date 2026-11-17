/**
  ******************************************************************************
  * @file    evt.h
  * @brief   ЛР3 (I04): часові події для аналізу на хості (методичка §5.2).
  *
  *          Формат рядка: EVT t_us=<мкс МК, 64 біти> name=<подія>[_<число>]
  *          (docs/metrics-spec.md §3.3: ^EVT t_us=(\d+) name=(\S+)$; аргумент —
  *          суфікс імені, наприклад mark_42)
  *          Час — з лічильника МК, а не час прибуття рядка на ПК.
  *
  *          Evt_Log() лише кладе запис у кільцевий буфер RAM (десятки тактів).
  *          Друкує uart-задача по EVT_FLUSH_PER_PASS рядків за прохід, тож вивід
  *          (≈3,5 мс на 40 байт при 115200, блокувальний) не потрапляє всередину
  *          вимірюваних інтервалів. Переповнення буфера — лічильник dropped.
  *          Викликати лише з головного циклу (64-бітний час не атомарний).
  ******************************************************************************
  */
#ifndef EVT_H
#define EVT_H

#include <stddef.h>
#include <stdint.h>

#define EVT_NOARG 0xFFFFFFFFUL

void     Evt_Init(void);
void     Evt_Log(const char *name);
void     Evt_LogArg(const char *name, uint32_t arg);
/* Подія з відомим моментом у минулому (наприклад, фронт, зафіксований в ISR) */
void     Evt_LogAt(uint64_t t_us, const char *name, uint32_t arg);

/* Друкує до max_lines записів; повертає скільки надруковано. */
unsigned Evt_Flush(unsigned max_lines);
unsigned Evt_Pending(void);
uint32_t Evt_Dropped(void);

/* Один рядок без "\r\n" — для тестів і парсера */
int      Evt_Format(char *buf, size_t size, uint64_t t_us, const char *name, uint32_t arg);

#endif /* EVT_H */
