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

#include <stdio.h>

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

/* ---- I06: пам'ять (методичка §5.4) -------------------------------------------------
 * Формат — docs/metrics-spec.md §3.6 (M-07, M-08):
 *   STACK reserved=<Б> used=<Б> free=<Б>
 *   HEAP arena=<Б> used=<Б> free=<Б>
 * used стека — найглибша точка з моменту Platform_StackPaint() у main(), тож вона лише
 * росте. Нова пара рядків з'являється в лозі в момент, коли сценарій уперше дійшов
 * до глибшого виклику: останні STACK/HEAP у лозі S1–S3 = максимум за сценарій. */
static uint32_t s_mem_stack;
static uint32_t s_mem_arena;

void AppTasks_PrintMem(void)
{
    platform_heap_t h = { 0U, 0U, 0U };

    uint32_t reserved = Platform_StackReservedBytes();

    Platform_HeapInfo(&h);
    s_mem_stack = Platform_StackUsedBytes();
    s_mem_arena = h.arena;
    printf("STACK reserved=%lu used=%lu free=%lu\r\n", (unsigned long)reserved,
           (unsigned long)s_mem_stack,
           (unsigned long)((reserved > s_mem_stack) ? (reserved - s_mem_stack) : 0U));
    printf("HEAP arena=%lu used=%lu free=%lu\r\n", (unsigned long)h.arena,
           (unsigned long)h.used, (unsigned long)h.free);
}

static bool task_mem(void)
{
    platform_heap_t h = { 0U, 0U, 0U };

    Platform_HeapInfo(&h);
    if ((Platform_StackUsedBytes() > s_mem_stack) || (h.arena > s_mem_arena))
    {
        AppTasks_PrintMem();
        return true;
    }
    return false;
}

/* ---- ініціалізація ----------------------------------------------------------------- */
void AppTasks_Init(void)
{
    Perf_Init();
    SmartLock_EventsInit();
    s_prev_in = Platform_ReadInputs();
    s_mem_stack = 0U;
    s_mem_arena = 0U;

    Sched_Init();
    (void)Sched_Add("fsm",      task_fsm,      PERIOD_FSM_US,          SCHED_NO_PROBE);
    (void)Sched_Add("buttons",  task_buttons,  TBD_PERIOD_BUTTONS_US,  SCHED_NO_PROBE);
    /* PB2 / PB12 до I09 показують роботу security і env (найдовші періодичні задачі);
     * у I09 ці канали переходять на захоплення кадру і запис у Flash. */
    (void)Sched_Add("security", task_security, TBD_PERIOD_SECURITY_US, (int)PLATFORM_PROBE_CAMERA);
    (void)Sched_Add("env",      task_env,      TBD_PERIOD_ENV_US,      (int)PLATFORM_PROBE_STORAGE);
    (void)Sched_Add("storage",  task_storage,  PERIOD_STORAGE_US,      SCHED_NO_PROBE);
    (void)Sched_Add("mem",      task_mem,      TBD_PERIOD_MEM_US,      SCHED_NO_PROBE);
    (void)Sched_Add("uart",     task_uart,     PERIOD_UART_US,         SCHED_NO_PROBE);
    Sched_Start();
}
