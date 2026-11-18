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
#include "cmd.h"
#include "event_queue.h"
#include "evt.h"
#include "frame_export.h"
#include "platform.h"
#include "sched.h"
#include "smartlock_hooks.h"
#include "stats.h"

#include <stdio.h>

/* ---- I04: події з ISR і затримка реакції --------------------------------------------
 * ISR (platform_io.c) запам'ятовує такт DWT першого фронту B1 / PIR / MPU_INT. Задача,
 * що відповідає за вхід, забирає подію: затримка «фронт → реакція» йде в LAT, на PB1
 * (REACT) — імпульс, у EVT — два записи: момент фронту і момент реакції. */
enum
{
    LAT_B1 = 0,
    LAT_PIR,
    LAT_MPU,
    LAT_COUNT
};

static perf_latency_t s_lat[LAT_COUNT];

/* Імена — docs/metrics-spec.md §3.3: <подія> → <обробка>. alarm_triggered після
 * pir_handled логує FSM (K-модулі), бо рішення про тривогу приймає вона. */
static const char *const s_lat_evt_irq[LAT_COUNT]   = { "btn_b1_press", "pir_trig", "mpu_int" };
static const char *const s_lat_evt_react[LAT_COUNT] = { "btn_b1_handled", "pir_handled", "mpu_handled" };

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

static bool task_storage(void)
{
    return SmartLock_StoragePoll(Platform_Millis());
}

/* UART: рядкові команди ЛР3 (STATS, MEM, PING — cmd.c) і однолітерні команди
 * стенда ЛР2, стеження за входами (g) */
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
    if (Stats_PollLoad())   /* LOAD pct= раз на секунду (metrics-spec §3.5) */
    {
        did = true;
    }
    if (Evt_Flush(EVT_FLUSH_PER_PASS) != 0U)   /* I04: EVT друкуються порціями */
    {
        did = true;
    }
    if (FrameExport_Step())   /* I08: один рядок кадру за прохід (CAM EXPORT) */
    {
        did = true;
    }
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
        Evt_Log("mem_peak");   /* I04: момент нового максимуму — у шкалі t_us */
        AppTasks_PrintMem();
        return true;
    }
    return false;
}

/* ---- ініціалізація ----------------------------------------------------------------- */
void AppTasks_Init(void)
{
    Perf_Init();
    Evt_Init();
    SmartLock_EventsInit();
    Cmd_Reset();
    AppTasks_ResetLatency();
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

void AppTasks_ResetLatency(void)
{
    /* STAT name=<...>_lat — затримки M-03 (metrics-spec §3.2) */
    Perf_LatencyInit(&s_lat[LAT_B1],  "btn_b1_lat", PLATFORM_EV_LOCK_B1);
    Perf_LatencyInit(&s_lat[LAT_PIR], "pir_lat",    PLATFORM_EV_PIR);
    Perf_LatencyInit(&s_lat[LAT_MPU], "mpu_lat",    PLATFORM_EV_MPU_INT);
}

int AppTasks_LatencyCount(void)
{
    return LAT_COUNT;
}

const perf_latency_t *AppTasks_Latency(int idx)
{
    return ((idx >= 0) && (idx < LAT_COUNT)) ? &s_lat[idx] : NULL;
}
