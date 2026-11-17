/**
  ******************************************************************************
  * @file    sl_fsm.h
  * @brief   ЛР3 (K02 + ядро K03/K04/K05, Коваль): FSM SmartLock.
  *
  *          Стани FR-01: UNLOCKED, EXIT_DELAY, LOCKED. EXIT_DELAY має дві
  *          внутрішні умови (PRD, «Уточнення поведінки»): відлік 15 с триває /
  *          відлік завершено, чекаємо DOOR_CLOSED — це не окремі стани.
  *
  *          Сильна SmartLock_FsmStep() (smartlock_hooks.h): забирає всі події з
  *          черги (event_queue.h) у порядку надходження, потім перевіряє час
  *          відліку. Логіка не торкається GPIO: виходи — лише через вузькі
  *          функції Platform_ServoSetPulseUs() (засувка), SmartLock_LogEvent()
  *          (журнал, sl_journal.h) і SmartLock_CameraRequest() (фото,
  *          sl_camera_link.h).
  *
  *          Реакція на події:
  *            UNLOCKED   LOCK_REQUEST → EXIT_DELAY, журнал LOCK_ACTIVATED, старт 15 с
  *            EXIT_DELAY LOCK_REQUEST ігнорується (відлік не перезапускається, E2E-01);
  *                       після 15 с: двері ЗАЧИНЕНІ → серво LOCK, LOCKED, журнал LOCKED;
  *                       інакше — лишаємось в EXIT_DELAY (серво UNLOCK) і чекаємо
  *                       DOOR_CLOSED без нового відліку (FR-13)
  *            LOCKED     MOTION / TAMPER → журнал + запит фото (FR-05..07);
  *                       DOOR_OPEN → журнал FORCED_OPEN, стан LOCKED, без фото (FR-13);
  *                       CODE_OK → серво UNLOCK, UNLOCKED, журнал ACCESS_GRANTED, UNLOCKED;
  *                       CODE_WRONG → журнал WRONG_CODE, стан LOCKED (FR-10)
  *            будь-який  ENV_WARNING → журнал, стан не змінюється (FR-14);
  *                       DOOR_OPEN / DOOR_CLOSED оновлюють відомий стан дверей
  *            поза LOCKED MOTION / TAMPER / CODE_* ігноруються без журналу;
  *                       після CODE_OK автоблокування немає (FR-11)
  *
  *          Стан дверей до першої DOOR_* невідомий і вважається ВІДЧИНЕНИМ
  *          (не блокувати наосліп). Антидребезг геркона — у security (K05).
  ******************************************************************************
  */
#ifndef SL_FSM_H
#define SL_FSM_H

#include <stdbool.h>
#include <stdint.h>

/* FR-03 / PRD D.2: затримка на вихід 15 с ± 0,1 с (від прийнятого B1 до LOCK) */
#define SL_EXIT_DELAY_MS          15000U

/* Положення засувки SG92R, мкс імпульсу (TIM3 CH1, 50 Гц).
 * TBD: робочі імпульси 1000 / 2000 мкс — docs/pinout.md §4 і logs/06_servo_pwm.txt,
 * але яке з них LOCK, а яке UNLOCK, залежить від механіки макета дверей —
 * перевірити на стенді і погодити (PRD цього не задає). */
#define TBD_SERVO_LOCK_US         2000U
#define TBD_SERVO_UNLOCK_US       1000U

/* TBD: PRD (FR-13) вимагає розрізняти DOOR_OPEN / DOOR_CLOSED, але не каже,
 * чи писати їх у журнал W25Q64 (там явно лише FORCED_OPEN). 1 — писати (корисно
 * для E2E-04 і FR-12), 0 — лише оновлювати стан дверей у FSM. */
#ifndef TBD_SL_LOG_DOOR_EVENTS
#define TBD_SL_LOG_DOOR_EVENTS    1
#endif

/* arg події SL_EV_LOCK_REQUEST: Platform_Millis() прийнятого натискання B1
 * (від нього рахуються 15 с) або SL_LOCK_REQ_T_NOW — від моменту, коли FSM
 * забрала подію з черги. Мітка «з майбутнього» або старша за 15 с вважається
 * недійсною і теж замінюється моментом обробки. */
#define SL_LOCK_REQ_T_NOW         0U

typedef enum
{
    SL_STATE_UNLOCKED = 0,
    SL_STATE_EXIT_DELAY,
    SL_STATE_LOCKED,
    SL_STATE_COUNT
} sl_state_t;

typedef enum
{
    SL_DOOR_UNKNOWN = 0,   /* до першої DOOR_* — для FSM те саме, що OPEN */
    SL_DOOR_OPEN,
    SL_DOOR_CLOSED
} sl_door_t;

/* UNLOCKED, двері невідомі, команда серво UNLOCK. Без журналу. Викликається
 * сама при першому SmartLock_FsmStep(); тести викликають явно для скидання. */
void SmartLock_FsmInit(void);

/* ---- геттери для тестів і діагностики (UART STATS / I10) --------------------- */
sl_state_t  SmartLock_FsmState(void);
const char *SmartLock_FsmStateName(sl_state_t state);
sl_door_t   SmartLock_FsmDoor(void);

/* Скільки мс лишилось до кінця відліку; 0 — поза EXIT_DELAY або відлік завершено */
uint32_t    SmartLock_FsmExitDelayRemainingMs(uint32_t now_ms);

/* EXIT_DELAY, відлік завершено, двері відчинені — чекаємо DOOR_CLOSED */
bool        SmartLock_FsmWaitingDoor(void);

uint32_t    SmartLock_FsmIgnored(void);          /* події, що в поточному стані не мають сенсу */
uint32_t    SmartLock_FsmCameraRejected(void);   /* SmartLock_CameraRequest() повернула false */

#endif /* SL_FSM_H */
