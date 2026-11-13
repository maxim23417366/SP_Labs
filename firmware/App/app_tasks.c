/**
  ******************************************************************************
  * @file    app_tasks.c
  * @brief   Тіла runtime-задач і реєстрація їх у планувальнику.
  *
  *          Задачі тут — тонкі: виклик точки підключення з smartlock_hooks.h,
  *          яку реалізує власник картки. Тому додати нову задачу = один рядок
  *          Sched_Add().
  ******************************************************************************
  */
#include "app_tasks.h"

#include "app_config.h"
#include "bench.h"
#include "event_queue.h"
#include "platform.h"
#include "sched.h"
#include "smartlock_hooks.h"

/* ---- задачі ------------------------------------------------------------------------
 * Модулі не викликають FSM напряму: buttons / security / env кладуть подію в чергу
 * (SmartLock_PostEvent), fsm забирає її в наступному проході (event_queue.h).
 * Фронти з EXTI (b1 / pir / mpu) з мітками часу підключає I04; до того — false. */
static bool task_fsm(void)
{
    return SmartLock_FsmStep(Platform_Millis());
}

static bool task_buttons(void)
{
    return SmartLock_ButtonsPoll(Platform_Millis(), false);
}

static bool task_security(void)
{
    return SmartLock_SecurityPoll(Platform_Millis(), false, false);
}

static bool task_env(void)
{
    return SmartLock_EnvPoll(Platform_Millis());
}

static bool task_storage(void)
{
    return SmartLock_StoragePoll(Platform_Millis());
}

/* UART: однолітерні команди стенда ЛР2 і стеження за входами (g) */
static bool task_uart(void)
{
    bool did = false;
    int  c;

    for (unsigned n = 0U; (n < UART_RX_PER_PASS) && ((c = Platform_UartGetChar()) >= 0); n++)
    {
        did = true;
        Bench_Command(c);   /* блокувальні діагностичні команди ЛР2 */
    }
    Bench_Watch();
    return did;
}

/* ---- ініціалізація ----------------------------------------------------------------- */
void AppTasks_Init(void)
{
    Perf_Init();
    SmartLock_EventsInit();

    Sched_Init();
    (void)Sched_Add("fsm",      task_fsm,      PERIOD_FSM_US,          SCHED_NO_PROBE);
    (void)Sched_Add("buttons",  task_buttons,  TBD_PERIOD_BUTTONS_US,  SCHED_NO_PROBE);
    (void)Sched_Add("security", task_security, TBD_PERIOD_SECURITY_US, SCHED_NO_PROBE);
    (void)Sched_Add("env",      task_env,      TBD_PERIOD_ENV_US,      SCHED_NO_PROBE);
    (void)Sched_Add("storage",  task_storage,  PERIOD_STORAGE_US,      SCHED_NO_PROBE);
    (void)Sched_Add("uart",     task_uart,     PERIOD_UART_US,         SCHED_NO_PROBE);
    Sched_Start();
}
