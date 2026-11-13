/**
  ******************************************************************************
  * @file    smartlock_hooks.h
  * @brief   ЛР3 (I02/I10): точки підключення прикладної логіки до Super Loop.
  *
  *          Каркас циклу (sched.c, app_tasks.c) — Непорожній; логіка всередині
  *          задач — власники карток. Кожна функція має СЛАБКУ реалізацію за
  *          замовчуванням (smartlock_hooks_weak.c). Модуль, що визначає функцію з
  *          тим самим ім'ям, автоматично її замінює — без правок циклу:
  *
  *            SmartLock_FsmStep        — K02–K05 (Коваль): FSM, EXIT_DELAY, LOCK
  *            SmartLock_ButtonsPoll    — K03/K04 (Коваль): B1, код з 4 кнопок, антидребезг
  *            SmartLock_SecurityPoll   — K05 (Коваль): PIR, MPU-6050, геркон
  *            SmartLock_EnvPoll        — K06 (Коваль): DHT11/BMP180, ENV_WARNING
  *            SmartLock_StoragePoll    — K07/H09: черга записів журналу W25Q64
  *
  *          Правила для всіх: функція не чекає (без HAL_Delay і очікувань у
  *          циклі), повертає true, якщо виконала корисну роботу, і викликається
  *          лише з головного циклу.
  *
  *          Зв'язок модулів з FSM — лише через чергу подій (event_queue.h):
  *          buttons / security / env викликають SmartLock_PostEvent(), FSM у
  *          SmartLock_FsmStep() забирає події SmartLock_TakeEvent(). Прямих
  *          викликів FSM з інших модулів немає.
  ******************************************************************************
  */
#ifndef SMARTLOCK_HOOKS_H
#define SMARTLOCK_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

/* Кожен прохід циклу. now_ms — Platform_Millis(). Забирає події з черги
 * SmartLock_TakeEvent() (event_queue.h) і змінює стан. */
bool SmartLock_FsmStep(uint32_t now_ms);

/* Раз на TBD_PERIOD_BUTTONS_US. b1_edge — був фронт USER B1 (мітки часу з EXTI
 * підключає I04; до того false). Сирий рівень входів — Platform_ReadInputs(). */
bool SmartLock_ButtonsPoll(uint32_t now_ms, bool b1_edge);

/* Раз на TBD_PERIOD_SECURITY_US. pir_edge / mpu_edge — фронти з EXTI (з I04). */
bool SmartLock_SecurityPoll(uint32_t now_ms, bool pir_edge, bool mpu_edge);

/* Раз на TBD_PERIOD_ENV_US */
bool SmartLock_EnvPoll(uint32_t now_ms);

/* Кожен прохід: одна порція роботи з чергою журналу */
bool SmartLock_StoragePoll(uint32_t now_ms);

#endif /* SMARTLOCK_HOOKS_H */
