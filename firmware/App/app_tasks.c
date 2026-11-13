/**
  ******************************************************************************
  * @file    app_tasks.c
  * @brief   Тіла runtime-задач і реєстрація їх у планувальнику.
  *
  *          Задачі тут — тонкі: вимірювання (затримка реакції, службові GPIO,
  *          EVT) + виклик точки підключення з smartlock_hooks.h, яку реалізує
  *          власник картки. Тому додати нову задачу = один рядок Sched_Add().
  ******************************************************************************
  */
#include "app_tasks.h"

#include "app_config.h"
#include "bench.h"
#include "camera_service.h"
#include "cmd.h"
#include "event_queue.h"
#include "evt.h"
#include "frame_export.h"
#include "platform.h"
#include "sched.h"
#include "smartlock_hooks.h"

enum
{
    LAT_B1 = 0,
    LAT_PIR,
    LAT_MPU,
    LAT_COUNT
};

static perf_latency_t s_lat[LAT_COUNT];

static const char *const s_lat_evt_irq[LAT_COUNT]   = { "b1_irq", "pir_irq", "mpu_irq" };
static const char *const s_lat_evt_react[LAT_COUNT] = { "b1_react", "pir_react", "mpu_react" };

/* Забрати подію з ISR: затримка в LAT, імпульс REACT на аналізаторі, два EVT —
 * момент фронту (з ISR) і момент реакції. */
static bool take(int i)
{
    uint64_t t_irq = 0U;

    if (!Perf_LatencyTake(&s_lat[i], &t_irq))
    {
        return false;
    }
    PROBE_ON(PLATFORM_PROBE_REACT);
    Evt_LogAt(t_irq, s_lat_evt_irq[i], EVT_NOARG);
    Evt_Log(s_lat_evt_react[i]);
    PROBE_OFF(PLATFORM_PROBE_REACT);
    return true;
}

/* ---- задачі ------------------------------------------------------------------------
 * Модулі не викликають FSM напряму: buttons / security / env кладуть подію в чергу
 * (SmartLock_PostEvent), fsm забирає її в наступному проході (event_queue.h). */
static bool task_fsm(void)
{
    return SmartLock_FsmStep(Platform_Millis());
}

static bool task_buttons(void)
{
    bool b1 = take(LAT_B1);

    return SmartLock_ButtonsPoll(Platform_Millis(), b1) || b1;
}

static bool task_security(void)
{
    bool pir = take(LAT_PIR);
    bool mpu = take(LAT_MPU);

    return SmartLock_SecurityPoll(Platform_Millis(), pir, mpu) || pir || mpu;
}

static bool task_env(void)
{
    return SmartLock_EnvPoll(Platform_Millis());
}

static bool task_camera(void)
{
    return Camera_Task();
}

static bool task_storage(void)
{
    return SmartLock_StoragePoll(Platform_Millis());
}

/* UART: усі прийняті символи (рядкові команди ЛР3 або однолітерні ЛР2),
 * стеження за входами (g), по одному EVT/FRM-рядку за прохід. */
static bool task_uart(void)
{
    bool did = false;
    int  c;

    for (unsigned n = 0U; (n < UART_RX_PER_PASS) && ((c = Platform_UartGetChar()) >= 0); n++)
    {
        did = true;
        if (Cmd_Feed(c) == 0)
        {
            Bench_Command(c);   /* блокувальні діагностичні команди ЛР2 */
        }
    }
    Bench_Watch();
    if (Evt_Flush(EVT_FLUSH_PER_PASS) != 0U)
    {
        did = true;
    }
    if (FrameExport_Step())
    {
        did = true;
    }
    return did;
}

/* ---- ініціалізація ----------------------------------------------------------------- */
void AppTasks_Init(void)
{
    Perf_Init();
    Evt_Init();
    Cmd_Reset();
    SmartLock_EventsInit();
    AppTasks_ResetLatency();

    Sched_Init();
    (void)Sched_Add("fsm",      task_fsm,      PERIOD_FSM_US,          SCHED_NO_PROBE);
    (void)Sched_Add("buttons",  task_buttons,  TBD_PERIOD_BUTTONS_US,  SCHED_NO_PROBE);
    (void)Sched_Add("security", task_security, TBD_PERIOD_SECURITY_US, SCHED_NO_PROBE);
    (void)Sched_Add("env",      task_env,      TBD_PERIOD_ENV_US,      SCHED_NO_PROBE);
    (void)Sched_Add("camera",   task_camera,   PERIOD_CAMERA_US,       SCHED_NO_PROBE);
    (void)Sched_Add("storage",  task_storage,  PERIOD_STORAGE_US,      SCHED_NO_PROBE);
    (void)Sched_Add("uart",     task_uart,     PERIOD_UART_US,         SCHED_NO_PROBE);
    Sched_Start();
}

void AppTasks_ResetLatency(void)
{
    Perf_LatencyInit(&s_lat[LAT_B1],  "b1_react",  PLATFORM_EV_LOCK_B1);
    Perf_LatencyInit(&s_lat[LAT_PIR], "pir_react", PLATFORM_EV_PIR);
    Perf_LatencyInit(&s_lat[LAT_MPU], "mpu_react", PLATFORM_EV_MPU_INT);
}

int AppTasks_LatencyCount(void)
{
    return LAT_COUNT;
}

const perf_latency_t *AppTasks_Latency(int idx)
{
    return ((idx >= 0) && (idx < LAT_COUNT)) ? &s_lat[idx] : NULL;
}
