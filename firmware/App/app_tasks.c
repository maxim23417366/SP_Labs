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

/* ---- фронти входів опитуванням -------------------------------------------------------
 * До I04 (мітки часу з EXTI) фронт B1 і PIR задача бачить, порівнюючи рівень зі
 * своїм попереднім опитуванням. Затримка «фізичний фронт → реакція» тоді містить
 * до одного періоду опитування — саме її видно на аналізаторі (PC13/PA0 → PB1). */
static uint32_t s_prev_in;

static bool fell(uint32_t now, uint32_t bit)   /* 1 → 0: B1 натиснута (активний 0) */
{
    bool e = ((s_prev_in & bit) != 0U) && ((now & bit) == 0U);

    s_prev_in = (s_prev_in & ~bit) | (now & bit);
    return e;
}

static bool rose(uint32_t now, uint32_t bit)   /* 0 → 1: PIR побачив рух */
{
    bool e = ((s_prev_in & bit) == 0U) && ((now & bit) != 0U);

    s_prev_in = (s_prev_in & ~bit) | (now & bit);
    return e;
}

/* ---- задачі ------------------------------------------------------------------------
 * Модулі не викликають FSM напряму: buttons / security / env кладуть подію в чергу
 * (SmartLock_PostEvent), fsm забирає її в наступному проході (event_queue.h). */
/* PB1 (REACT) — FSM обробляє події з черги: фронт на аналізаторі відносно фронту
 * PIR / B1 показує затримку реакції незалежно від прошивки (M06). */
static bool task_fsm(void)
{
    bool react = (SmartLock_EventsPending() != 0U);
    bool did;

    if (react)
    {
        PROBE_ON(PLATFORM_PROBE_REACT);
    }
    did = SmartLock_FsmStep(Platform_Millis());
    if (react)
    {
        PROBE_OFF(PLATFORM_PROBE_REACT);
    }
    return did;
}

static bool task_buttons(void)
{
    return SmartLock_ButtonsPoll(Platform_Millis(), fell(Platform_ReadInputs(), PLATFORM_IN_LOCK));
}

static bool task_security(void)
{
    uint32_t in = Platform_ReadInputs();
    bool pir = rose(in, PLATFORM_IN_PIR);
    bool mpu = rose(in, PLATFORM_IN_MPUINT);

    return SmartLock_SecurityPoll(Platform_Millis(), pir, mpu);
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
    s_prev_in = Platform_ReadInputs();

    Sched_Init();
    (void)Sched_Add("fsm",      task_fsm,      PERIOD_FSM_US,          SCHED_NO_PROBE);
    (void)Sched_Add("buttons",  task_buttons,  TBD_PERIOD_BUTTONS_US,  SCHED_NO_PROBE);
    /* PB2 / PB12 до I09 показують роботу security і env (найдовші періодичні задачі);
     * у I09 ці канали переходять на захоплення кадру і запис у Flash. */
    (void)Sched_Add("security", task_security, TBD_PERIOD_SECURITY_US, (int)PLATFORM_PROBE_CAMERA);
    (void)Sched_Add("env",      task_env,      TBD_PERIOD_ENV_US,      (int)PLATFORM_PROBE_STORAGE);
    (void)Sched_Add("storage",  task_storage,  PERIOD_STORAGE_US,      SCHED_NO_PROBE);
    (void)Sched_Add("uart",     task_uart,     PERIOD_UART_US,         SCHED_NO_PROBE);
    Sched_Start();
}
