/**
  ******************************************************************************
  * @file    sl_security.h
  * @brief   ЛР3 (K05, Коваль): PIR, MPU-6050, геркон — FR-05, FR-06, FR-13.
  *
  *          ЗАГОТОВКА. Модуль реалізує сильну SmartLock_SecurityPoll()
  *          (smartlock_hooks.h); доки її немає, працює слабка зі
  *          smartlock_hooks_weak.c. FSM напряму не викликається — лише події:
  *
  *            SL_EV_MOTION       PIR: рух (FR-05). arg = 0.
  *            SL_EV_TAMPER       MPU-6050: удар / вібрація понад поріг (FR-06).
  *                               arg = величина, що перевищила поріг (мg).
  *            SL_EV_DOOR_OPEN    геркон: стабільно відчинено (FR-13). arg = 0.
  *            SL_EV_DOOR_CLOSED  геркон: стабільно зачинено (FR-13). arg = 0.
  *
  *          Модуль не знає стану FSM: MOTION / TAMPER постяться в будь-якому
  *          стані, FSM реагує лише в LOCKED. DOOR_* — лише при зміні стабільного
  *          (після антидребезгу) стану, і ОДИН раз після старту — початковий
  *          стан: до першої DOOR_* FSM вважає двері відчиненими і не блокує.
  *          Короткі нестабільні переходи не повинні давати DOOR_* (PRD,
  *          «Уточнення поведінки»), бо DOOR_OPEN у LOCKED = FORCED_OPEN.
  *
  *          Входи: Platform_ReadInputs(): PLATFORM_IN_DOOR — 0 = магніт поруч
  *          (зачинено), 1 = відчинено (docs/pinout.md, logs/04); PLATFORM_IN_PIR —
  *          1 = рух. MPU-6050 — app_devices.mpu (app.h), mpu6050_read_raw();
  *          при app_devices.mpu_status != DEV_OK — без TAMPER і без зависання.
  *          pir_edge / mpu_edge у хуку — фронти з EXTI, підключає I04 (до того
  *          false). Поріг TAMPER і антидребезг геркона — TBD_ у цьому файлі.
  ******************************************************************************
  */
#ifndef SL_SECURITY_H
#define SL_SECURITY_H

/* Скидання внутрішнього стану модуля (тести; на платі — лінива ініціалізація) */
void SmartLock_SecurityInit(void);

#endif /* SL_SECURITY_H */
