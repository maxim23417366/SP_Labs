/**
  ******************************************************************************
  * @file    sl_fsm.c
  * @brief   ЛР3 (K02 + ядро K03/K04/K05): FSM SmartLock (див. sl_fsm.h).
  *
  *          Чиста логіка: вхід — події з черги і now_ms, вихід — три вузькі дії
  *          (серво, журнал, запит фото через sl_camera_link.c). Нічого не чекає і не читає GPIO.
  *          Час — різниця uint32 у мс, тому відлік коректний і через
  *          переповнення Platform_Millis() (~49,7 доби).
  ******************************************************************************
  */
#include "sl_fsm.h"

#include "app_config.h"
#include "event_queue.h"
#include "evt.h"
#include "platform.h"
#include "sl_camera_link.h"
#include "sl_journal.h"
#include "smartlock_hooks.h"

typedef struct
{
    bool       started;        /* SmartLock_FsmInit() вже виконано */
    sl_state_t state;
    sl_door_t  door;
    uint32_t   t_start_ms;     /* початок відліку 15 с */
    bool       delay_done;     /* EXIT_DELAY: відлік завершено, чекаємо DOOR_CLOSED */
    uint32_t   ignored;
    uint32_t   cam_rejected;
} sl_fsm_t;

static sl_fsm_t s_fsm;

/* ---- вихідні дії: єдині місця, де FSM торкається зовнішнього світу ------------ */
static void out_servo(uint32_t pulse_us)
{
    Platform_ServoSetPulseUs(pulse_us);
}

static uint32_t out_log(sl_log_type_t type, uint32_t arg)
{
    return SmartLock_LogEvent(type, arg);
}

/* Один кадр на охоронну подію. Результат (PHOTO / PHOTO_ERR) пише sl_camera_link.c;
 * тут лише рахуємо, скільки разів камера не прийняла запит. event_id = 0 (подію не
 * записано, FIFO повна) — кадр не запитується, link веде окремий лічильник. */
static void out_photo(uint32_t event_id, uint32_t now_ms)
{
    if (!SmartLock_CameraLinkStart(event_id, now_ms) && (event_id != SL_LOG_ID_NONE))
    {
        s_fsm.cam_rejected++;
    }
}

/* ---- переходи ------------------------------------------------------------------- */
void SmartLock_FsmInit(void)
{
    s_fsm.started = true;
    s_fsm.state = SL_STATE_UNLOCKED;
    s_fsm.door = SL_DOOR_UNKNOWN;
    s_fsm.t_start_ms = 0U;
    s_fsm.delay_done = false;
    s_fsm.ignored = 0U;
    s_fsm.cam_rejected = 0U;
    SmartLock_CameraLinkInit();
    out_servo(TBD_SERVO_UNLOCK_US);   /* стан UNLOCKED = засувка UNLOCK */
}

static void enter_exit_delay(uint32_t now_ms, uint32_t stamp_ms)
{
    uint32_t t0 = now_ms;

    /* мітка прийнятого B1 — лише якщо вона не з майбутнього і не старша за відлік */
    if ((stamp_ms != SL_LOCK_REQ_T_NOW) && ((uint32_t)(now_ms - stamp_ms) < SL_EXIT_DELAY_MS))
    {
        t0 = stamp_ms;
    }
    s_fsm.state = SL_STATE_EXIT_DELAY;
    s_fsm.t_start_ms = t0;
    s_fsm.delay_done = false;
    (void)out_log(SL_LOG_LOCK_ACTIVATED, SL_EXIT_DELAY_MS);
}

static void enter_locked(uint32_t now_ms)
{
    out_servo(TBD_SERVO_LOCK_US);
    s_fsm.state = SL_STATE_LOCKED;
    s_fsm.delay_done = false;
    (void)out_log(SL_LOG_LOCKED, (uint32_t)(now_ms - s_fsm.t_start_ms));
}

static void enter_unlocked_by_code(uint32_t code_arg)
{
    out_servo(TBD_SERVO_UNLOCK_US);
    s_fsm.state = SL_STATE_UNLOCKED;
    (void)out_log(SL_LOG_ACCESS_GRANTED, code_arg);
    (void)out_log(SL_LOG_UNLOCKED, 0U);
}

/* Геркон: діє лише зміна стану (повтор того самого — ігнорується) */
static void on_door(sl_door_t door, uint32_t arg)
{
    if (s_fsm.door == door)
    {
        s_fsm.ignored++;
        return;
    }
    s_fsm.door = door;
    if ((door == SL_DOOR_OPEN) && (s_fsm.state == SL_STATE_LOCKED))
    {
        /* без попереднього CODE_OK: стан лишається LOCKED, фото не запускаємо */
        (void)out_log(SL_LOG_FORCED_OPEN, 0U);
        return;
    }
    if (TBD_SL_LOG_DOOR_EVENTS != 0)
    {
        (void)out_log((door == SL_DOOR_OPEN) ? SL_LOG_DOOR_OPEN : SL_LOG_DOOR_CLOSED, arg);
    }
}

/* Охоронна подія: лише в LOCKED — запис журналу і кадр з посиланням на нього.
 * MOTION у LOCKED — рішення про тривогу: EVT alarm_triggered (I04, пара
 * pir_trig → alarm_triggered, docs/metrics/metrics-spec.md §3.3). Для TAMPER
 * специфікація пари не задає — лише mpu_int → mpu_handled з app_tasks.c. */
static void on_security(sl_log_type_t type, uint32_t arg, uint32_t now_ms)
{
    if (s_fsm.state != SL_STATE_LOCKED)
    {
        s_fsm.ignored++;
        return;
    }
    if (type == SL_LOG_MOTION)
    {
        Evt_Log("alarm_triggered");
    }
    out_photo(out_log(type, arg), now_ms);
}

static void handle_event(const sl_event_t *ev, uint32_t now_ms)
{
    bool locked = (s_fsm.state == SL_STATE_LOCKED);

    switch (ev->type)
    {
    case SL_EV_LOCK_REQUEST:
        if (s_fsm.state == SL_STATE_UNLOCKED)
        {
            enter_exit_delay(now_ms, ev->arg);
        }
        else
        {
            s_fsm.ignored++;   /* повторний B1 не перезапускає відлік */
        }
        break;
    case SL_EV_MOTION:
        on_security(SL_LOG_MOTION, ev->arg, now_ms);
        break;
    case SL_EV_TAMPER:
        on_security(SL_LOG_TAMPER, ev->arg, now_ms);
        break;
    case SL_EV_DOOR_OPEN:
        on_door(SL_DOOR_OPEN, ev->arg);
        break;
    case SL_EV_DOOR_CLOSED:
        on_door(SL_DOOR_CLOSED, ev->arg);
        break;
    case SL_EV_CODE_OK:
        if (locked)
        {
            enter_unlocked_by_code(ev->arg);
        }
        else
        {
            s_fsm.ignored++;
        }
        break;
    case SL_EV_CODE_WRONG:
        if (locked)
        {
            (void)out_log(SL_LOG_WRONG_CODE, ev->arg);
        }
        else
        {
            s_fsm.ignored++;
        }
        break;
    case SL_EV_ENV_WARNING:
        (void)out_log(SL_LOG_ENV_WARNING, ev->arg);   /* у будь-якому стані, стан не змінює */
        break;
    default:
        s_fsm.ignored++;
        break;
    }
}

/* EXIT_DELAY: кінець відліку і очікування закритих дверей. true — щось змінилось */
static bool exit_delay_tick(uint32_t now_ms)
{
    bool changed = false;

    if (s_fsm.state != SL_STATE_EXIT_DELAY)
    {
        return false;
    }
    if (!s_fsm.delay_done)
    {
        if ((uint32_t)(now_ms - s_fsm.t_start_ms) < SL_EXIT_DELAY_MS)
        {
            return false;
        }
        s_fsm.delay_done = true;   /* фіксується один раз: другого відліку не буде */
        changed = true;
    }
    if (s_fsm.door != SL_DOOR_CLOSED)
    {
        return changed;   /* відчинені або невідомо — засувка лишається UNLOCK */
    }
    enter_locked(now_ms);
    return true;
}

bool SmartLock_FsmStep(uint32_t now_ms)
{
    sl_event_t ev;
    bool did = false;

    if (!s_fsm.started)
    {
        SmartLock_FsmInit();
        did = true;
    }
    /* спершу всі події в порядку надходження, потім час: DOOR_OPEN, що прийшла
     * разом з кінцем відліку, не дасть заблокувати відчинені двері */
    for (unsigned n = 0U; (n < TBD_EVQ_LEN) && SmartLock_TakeEvent(&ev); n++)
    {
        handle_event(&ev, now_ms);
        did = true;
    }
    if (exit_delay_tick(now_ms))
    {
        did = true;
    }
    if (SmartLock_CameraLinkPoll(now_ms))   /* тайм-аути фото і повтор запису PHOTO / PHOTO_ERR */
    {
        did = true;
    }
    return did;
}

/* ---- геттери ---------------------------------------------------------------------- */
sl_state_t SmartLock_FsmState(void)
{
    return s_fsm.state;
}

const char *SmartLock_FsmStateName(sl_state_t state)
{
    static const char *const names[SL_STATE_COUNT] = { "UNLOCKED", "EXIT_DELAY", "LOCKED" };

    return ((unsigned)state < (unsigned)SL_STATE_COUNT) ? names[state] : "?";
}

sl_door_t SmartLock_FsmDoor(void)
{
    return s_fsm.door;
}

uint32_t SmartLock_FsmExitDelayRemainingMs(uint32_t now_ms)
{
    uint32_t elapsed;

    if ((s_fsm.state != SL_STATE_EXIT_DELAY) || s_fsm.delay_done)
    {
        return 0U;
    }
    elapsed = (uint32_t)(now_ms - s_fsm.t_start_ms);
    return (elapsed < SL_EXIT_DELAY_MS) ? (SL_EXIT_DELAY_MS - elapsed) : 0U;
}

bool SmartLock_FsmWaitingDoor(void)
{
    return (s_fsm.state == SL_STATE_EXIT_DELAY) && s_fsm.delay_done;
}

uint32_t SmartLock_FsmIgnored(void)
{
    return s_fsm.ignored;
}

uint32_t SmartLock_FsmCameraRejected(void)
{
    return s_fsm.cam_rejected;
}
