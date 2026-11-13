/**
  ******************************************************************************
  * @file    event_queue.h
  * @brief   ЛР3 (I02): черга подій до FSM — модулі повідомляють, FSM вирішує.
  *
  *          security / buttons / env / storage НЕ викликають FSM напряму, а
  *          кладуть подію в чергу (SmartLock_PostEvent). FSM у наступному проході
  *          забирає події по одній (SmartLock_TakeEvent) і сама вирішує, що з ними
  *          робити (наприклад, MOTION має сенс лише в LOCKED). Так:
  *            - модулі не знають про стан FSM (окрема відповідальність);
  *            - кілька подій в одному проході не губляться (FIFO);
  *            - у ЛР4 черга 1:1 переходить у FreeRTOS:
  *                SmartLock_PostEvent  → xQueueSend(q, &ev, 0)
  *                SmartLock_TakeEvent  → xQueueReceive(q, &ev, timeout) у задачі FSM
  *
  *          Порядок у проході fsm → buttons → security → … → camera, тож:
  *            прохід k   : security бачить PIR → PostEvent(MOTION)
  *            прохід k+1 : fsm забирає MOTION (у LOCKED → Camera_Request(id)),
  *                         camera у тому ж проході знімає кадр
  *
  *          Лише для головного циклу (не з ISR): індекси не атомарні. Переривання
  *          передають події через Platform_TakeEvent() (мітка часу + прапорець).
  *          Переповнення — нова подія відкидається і рахується (dropped у STATS):
  *          довжина черги TBD_EVQ_LEN має бути більшою за max_depth у S1–S3.
  ******************************************************************************
  */
#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "perf.h"

/* Події, на які реагує FSM (PRD: FR-02, FR-05, FR-06, FR-09, FR-10, FR-13, FR-14) */
typedef enum
{
    SL_EV_NONE = 0,
    SL_EV_LOCK_REQUEST,   /* USER B1 — запит на блокування (FR-02) */
    SL_EV_MOTION,         /* PIR (FR-05) */
    SL_EV_TAMPER,         /* MPU-6050 вище порогу (FR-06) */
    SL_EV_DOOR_OPEN,      /* геркон: відчинено (FR-13) */
    SL_EV_DOOR_CLOSED,    /* геркон: зачинено (FR-13) */
    SL_EV_CODE_OK,        /* введено правильний код (FR-10) */
    SL_EV_CODE_WRONG,     /* неправильний код (FR-10) */
    SL_EV_ENV_WARNING,    /* DHT11 поза межами (FR-14) */
    SL_EV_COUNT
} sl_event_type_t;

typedef struct
{
    sl_event_type_t type;
    uint32_t        arg;        /* дані події (ID, код кнопки тощо) */
    uint32_t        t_cycles;   /* такт DWT, коли подію поставили в чергу */
} sl_event_t;

void SmartLock_EventsInit(void);

/* false — черга повна, подію відкинуто (dropped++). */
bool SmartLock_PostEvent(sl_event_type_t type, uint32_t arg);

/* false — черга порожня. Час очікування в черзі додається до STAT evq_wait,
 * момент забирання — EVT name=fsm_event arg=<тип>. */
bool SmartLock_TakeEvent(sl_event_t *ev);

unsigned           SmartLock_EventsPending(void);
unsigned           SmartLock_EventsMaxDepth(void);   /* найбільше заповнення з моменту скидання */
uint32_t           SmartLock_EventsDropped(void);
uint32_t           SmartLock_EventsPosted(void);
const perf_stat_t *SmartLock_EventsWaitStat(void);   /* такти від Post до Take */
void               SmartLock_EventsResetStats(void);

const char *SmartLock_EventName(sl_event_type_t type);

#endif /* EVENT_QUEUE_H */
