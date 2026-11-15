/**
  * @file    test_sched.c
  * @brief   ЛР3 (I02): каркас Super Loop без плати — планувальник (sched.c),
  *          час (perf.c), черга подій FSM (event_queue.c), таблиця задач
  *          (app_tasks.c), слабкі точки підключення (smartlock_hooks_weak.c)
  *          і службові GPIO для аналізатора (I05).
  *
  *          Час — fake_platform: Platform_Cycles() = now_ms · 1000 + cycles_step на
  *          кожне зчитування, 1 такт = 1 мкс (Platform_CpuHz() = 1 МГц).
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#include "app.h"
#include "app_config.h"
#include "app_tasks.h"
#include "event_queue.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "perf.h"
#include "platform.h"
#include "sched.h"
#include "smartlock_hooks.h"

/* ---- тестові задачі --------------------------------------------------------------- */
static unsigned n_periodic;
static unsigned n_event;
static bool     event_has_work;
static uint32_t work_ms;   /* скільки «триває» periodic */

static bool periodic(void)
{
  n_periodic++;
  fake_platform.now_ms += work_ms;
  return true;
}

static bool event_task(void)
{
  n_event++;
  return event_has_work;
}

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_NACK;
  app_devices.baro_status = DEV_ERR_NACK;
  n_periodic = 0U;
  n_event = 0U;
  event_has_work = false;
  work_ms = 0U;
  Perf_Init();
  Sched_Init();
}

void tearDown(void)
{
}

/* ---- perf ------------------------------------------------------------------------ */
static void test_perf_conversions_and_stats(void)
{
  perf_stat_t s = PERF_STAT_INIT("x");

  TEST_ASSERT_EQUAL_UINT32(250U, Perf_CyclesToUs(250U));
  TEST_ASSERT_EQUAL_UINT32(10000U, Perf_UsToCycles(10000U));
  TEST_ASSERT_EQUAL_UINT32(0U, Perf_StatAvg(&s));
  Perf_StatAdd(&s, 30U);
  Perf_StatAdd(&s, 10U);
  Perf_StatAdd(&s, 20U);
  TEST_ASSERT_EQUAL_UINT32(3U, s.count);
  TEST_ASSERT_EQUAL_UINT32(10U, s.min);
  TEST_ASSERT_EQUAL_UINT32(30U, s.max);
  TEST_ASSERT_EQUAL_UINT32(20U, Perf_StatAvg(&s));
  Perf_StatReset(&s);
  TEST_ASSERT_EQUAL_UINT32(0U, s.count);
}

static void test_perf_time64_survives_counter_wrap(void)
{
  /* 32-бітний лічильник при 1 МГц переповнюється за ~71,6 хв: 4 300 000 мс */
  fake_platform.now_ms = 4294000U;          /* 4 294 000 000 тактів — біля межі 2^32 */
  Perf_Init();
  fake_platform.now_ms += 2000U;             /* лічильник перейшов через 0 */
  TEST_ASSERT_EQUAL_UINT64(2000000ULL, Perf_TimeUs64());
  fake_platform.now_ms += 1000U;
  TEST_ASSERT_EQUAL_UINT64(3000000ULL, Perf_TimeUs64());
}

/* ---- планувальник ---------------------------------------------------------------- */
static void test_add_rejects_bad_tasks_and_full_table(void)
{
  TEST_ASSERT_EQUAL_INT(-1, Sched_Add("null", NULL, 0U, SCHED_NO_PROBE));
  TEST_ASSERT_EQUAL_INT(-1, Sched_Add("long", event_task, SCHED_MAX_PERIOD_US + 1U, SCHED_NO_PROBE));
  for (int i = 0; i < SCHED_MAX_TASKS; i++)
  {
    TEST_ASSERT_EQUAL_INT(i, Sched_Add("t", event_task, 0U, SCHED_NO_PROBE));
  }
  TEST_ASSERT_EQUAL_INT(-1, Sched_Add("extra", event_task, 0U, SCHED_NO_PROBE));
  TEST_ASSERT_EQUAL_INT(SCHED_MAX_TASKS, Sched_Count());
  TEST_ASSERT_NULL(Sched_Task(SCHED_MAX_TASKS));
  TEST_ASSERT_NULL(Sched_Task(-1));
}

static void test_periodic_runs_on_time_event_runs_every_pass(void)
{
  TEST_ASSERT_EQUAL_INT(0, Sched_Add("per", periodic, 10000U, SCHED_NO_PROBE));   /* 10 мс */
  TEST_ASSERT_EQUAL_INT(1, Sched_Add("evt", event_task, 0U, SCHED_NO_PROBE));
  TEST_ASSERT_FALSE(Sched_Started());
  Sched_Start();
  TEST_ASSERT_TRUE(Sched_Started());

  /* перший запуск періодичної — через один період після старту */
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(0U, n_periodic);
  TEST_ASSERT_EQUAL_UINT(1U, n_event);

  fake_platform.now_ms = 9U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(0U, n_periodic);
  fake_platform.now_ms = 10U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(1U, n_periodic);
  fake_platform.now_ms = 20U;
  Sched_RunOnce();
  fake_platform.now_ms = 30U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(3U, n_periodic);
  TEST_ASSERT_EQUAL_UINT(5U, n_event);
  TEST_ASSERT_EQUAL_UINT32(5U, Sched_Loops());

  const sched_task_t *t = Sched_Task(0);
  TEST_ASSERT_EQUAL_STRING("per", t->name);
  TEST_ASSERT_EQUAL_UINT32(2U, t->jit_n);   /* два інтервали між трьома запусками */
  TEST_ASSERT_EQUAL_INT32(0, t->jit_min);
  TEST_ASSERT_EQUAL_INT32(0, t->jit_max);
  TEST_ASSERT_EQUAL_UINT32(2U, t->jit_hist[0]);
  TEST_ASSERT_EQUAL_UINT32(0U, t->missed);
}

static void test_late_run_counts_jitter_and_missed_without_catch_up(void)
{
  (void)Sched_Add("per", periodic, 10000U, SCHED_NO_PROBE);
  Sched_Start();
  fake_platform.now_ms = 10U;
  Sched_RunOnce();                     /* запуск у 10 мс */
  fake_platform.now_ms = 23U;
  Sched_RunOnce();                     /* запізнення 3 мс */
  fake_platform.now_ms = 75U;
  Sched_RunOnce();                     /* інтервал 52 мс > 2 періодів */
  TEST_ASSERT_EQUAL_UINT(3U, n_periodic);
  /* без «боргу»: наступний запуск — точка сітки 80 мс, а не серія підряд */
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(3U, n_periodic);
  fake_platform.now_ms = 80U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(4U, n_periodic);

  const sched_task_t *t = Sched_Task(0);
  TEST_ASSERT_EQUAL_UINT32(1U, t->missed);
  TEST_ASSERT_EQUAL_INT32(-5000, t->jit_min);   /* 75 → 80: інтервал 5 мс */
  TEST_ASSERT_EQUAL_INT32(42000, t->jit_max);   /* 23 → 75: 52 мс − 10 мс */
  /* |джитер|: 3 мс і 5 мс → кошик <10 мс; 42 мс → кошик <100 мс */
  TEST_ASSERT_EQUAL_UINT32(2U, t->jit_hist[2]);
  TEST_ASSERT_EQUAL_UINT32(1U, t->jit_hist[3]);

  Sched_ResetStats();
  TEST_ASSERT_EQUAL_UINT32(0U, Sched_Task(0)->jit_n);
  TEST_ASSERT_EQUAL_UINT32(0U, Sched_Loops());
  TEST_ASSERT_EQUAL_UINT32(0U, Sched_LoopStat()->count);
}

static void test_cpu_load_counts_only_useful_work(void)
{
  (void)Sched_Add("per", periodic, 100000U, SCHED_NO_PROBE);   /* 100 мс */
  (void)Sched_Add("evt", event_task, 0U, SCHED_NO_PROBE);
  Sched_Start();
  work_ms = 25U;   /* кожен запуск periodic «триває» 25 мс */

  /* вікно 1 с: 10 запусків по 25 мс = 250 мс роботи → 250 ‰; порожні опитування evt — ні */
  for (uint32_t ms = 0U; ms <= 1000U; ms++)
  {
    if (fake_platform.now_ms < ms)
    {
      fake_platform.now_ms = ms;
    }
    Sched_RunOnce();
  }
  TEST_ASSERT_EQUAL_UINT32(1U, Sched_CpuStat()->count);
  TEST_ASSERT_UINT32_WITHIN(30U, 250U, Sched_CpuStat()->max);
  TEST_ASSERT_EQUAL_UINT32(0U, Sched_Task(1)->exec.count);   /* жодної корисної роботи */
  TEST_ASSERT_TRUE(Sched_Task(1)->polls > 900U);

  /* та сама задача з роботою — потрапляє в exec */
  event_has_work = true;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT32(1U, Sched_Task(1)->exec.count);
}

/* ---- черга подій FSM --------------------------------------------------------------- */
static void test_event_queue_fifo_overflow_and_wait(void)
{
  sl_event_t ev;

  SmartLock_EventsInit();
  TEST_ASSERT_FALSE(SmartLock_TakeEvent(&ev));
  TEST_ASSERT_FALSE(SmartLock_PostEvent(SL_EV_NONE, 0U));
  TEST_ASSERT_FALSE(SmartLock_PostEvent(SL_EV_COUNT, 0U));

  TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_MOTION, 1U));
  TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_TAMPER, 2U));
  for (unsigned i = 2U; i < TBD_EVQ_LEN; i++)
  {
    TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_DOOR_OPEN, i));
  }
  TEST_ASSERT_FALSE(SmartLock_PostEvent(SL_EV_ENV_WARNING, 99U));   /* черга повна */
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EventsDropped());
  TEST_ASSERT_EQUAL_UINT32(TBD_EVQ_LEN, SmartLock_EventsPosted());
  TEST_ASSERT_EQUAL_UINT(TBD_EVQ_LEN, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT(TBD_EVQ_LEN, SmartLock_EventsMaxDepth());

  fake_platform.now_ms += 3U;
  TEST_ASSERT_FALSE(SmartLock_TakeEvent(NULL));
  TEST_ASSERT_TRUE(SmartLock_TakeEvent(&ev));
  TEST_ASSERT_EQUAL(SL_EV_MOTION, ev.type);
  TEST_ASSERT_EQUAL_UINT32(1U, ev.arg);
  TEST_ASSERT_TRUE(SmartLock_TakeEvent(&ev));
  TEST_ASSERT_EQUAL(SL_EV_TAMPER, ev.type);
  TEST_ASSERT_EQUAL_UINT32(3000U, SmartLock_EventsWaitStat()->max);   /* 3 мс у черзі */

  TEST_ASSERT_EQUAL_STRING("motion", SmartLock_EventName(SL_EV_MOTION));
  TEST_ASSERT_EQUAL_STRING("env_warning", SmartLock_EventName(SL_EV_ENV_WARNING));
  TEST_ASSERT_EQUAL_STRING("?", SmartLock_EventName(SL_EV_COUNT));

  SmartLock_EventsResetStats();
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EventsDropped());
  TEST_ASSERT_EQUAL_UINT(TBD_EVQ_LEN - 2U, SmartLock_EventsMaxDepth());
}

/* ---- таблиця задач SmartLock і слабкі точки підключення --------------------------- */
static void test_app_tasks_table_order(void)
{
  static const char *const names[] = { "fsm", "buttons", "security", "env", "storage", "mem", "uart" };

  AppTasks_Init();
  TEST_ASSERT_TRUE(Sched_Started());
  TEST_ASSERT_EQUAL_INT(7, Sched_Count());
  for (int i = 0; i < 7; i++)
  {
    TEST_ASSERT_EQUAL_STRING(names[i], Sched_Task(i)->name);
  }
  TEST_ASSERT_EQUAL_UINT32(TBD_PERIOD_BUTTONS_US, Sched_Task(1)->period_us);
  TEST_ASSERT_EQUAL_UINT32(TBD_PERIOD_SECURITY_US, Sched_Task(2)->period_us);
  TEST_ASSERT_EQUAL_UINT32(TBD_PERIOD_ENV_US, Sched_Task(3)->period_us);
  TEST_ASSERT_EQUAL_UINT32(0U, Sched_Task(0)->period_us);
}

static void test_posted_event_reaches_fsm_next_pass(void)
{
  AppTasks_Init();
  /* «security» у проході k кладе MOTION; fsm стоїть першою і забирає її в k+1 */
  TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_MOTION, 0U));
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_EventsPending());
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(1U, Sched_Task(0)->exec.count);   /* fsm мала роботу */
}

static void test_weak_hooks(void)
{
  SmartLock_EventsInit();
  TEST_ASSERT_TRUE(SmartLock_ButtonsPoll(0U, true));
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(0U, true, false));
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_EventsPending());   /* LOCK_REQUEST, MOTION */
  TEST_ASSERT_TRUE(SmartLock_FsmStep(0U));
  TEST_ASSERT_FALSE(SmartLock_FsmStep(0U));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_FALSE(SmartLock_StoragePoll(0U));

  /* MPU-6050 є — security читає 14 байт; DHT11 відповідає — ENV,dht=ok */
  app_devices.mpu_status = DEV_OK;
  app_devices.mpu.bus = Platform_SensorI2c();
  app_devices.mpu.addr7 = MPU6050_I2C_ADDR;
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(0U, false, false));
  fake_platform.dht_status = DEV_OK;
  for (unsigned i = 0U; i < 40U; i++)
  {
    fake_platform.dht_high_us[i] = 24U;   /* усі біти 0: контрольна сума 0 збігається */
  }
  TEST_ASSERT_TRUE(SmartLock_EnvPoll(0U));
  fake_platform.dht_status = DEV_ERR_TIMEOUT;
  TEST_ASSERT_TRUE(SmartLock_EnvPoll(0U));
}

/* ---- I05: службові GPIO ---------------------------------------------------------- */
static void test_loop_probe_pulses_every_pass(void)
{
  (void)Sched_Add("evt", event_task, 0U, SCHED_NO_PROBE);
  Sched_Start();
  for (int i = 0; i < 5; i++)
  {
    Sched_RunOnce();
  }
  TEST_ASSERT_EQUAL_UINT32(5U, fake_platform.probe_rises[PLATFORM_PROBE_LOOP]);
  TEST_ASSERT_EQUAL_UINT8(0U, fake_platform.probe_level[PLATFORM_PROBE_LOOP]);   /* імпульс закрито */
}

static unsigned probe_seen_in_task;

static bool probed_task(void)
{
  probe_seen_in_task = fake_platform.probe_level[PLATFORM_PROBE_CAMERA];
  return true;
}

static void test_task_probe_wraps_only_its_task(void)
{
  (void)Sched_Add("cam", probed_task, 0U, (int)PLATFORM_PROBE_CAMERA);
  Sched_Start();
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(1U, probe_seen_in_task);                  /* 1 під час задачі */
  TEST_ASSERT_EQUAL_UINT8(0U, fake_platform.probe_level[PLATFORM_PROBE_CAMERA]);
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.probe_rises[PLATFORM_PROBE_CAMERA]);
  Platform_ProbeWrite(PLATFORM_PROBE_COUNT, 1U);                   /* неіснуючий канал ігнорується */
}

static void test_react_probe_only_when_fsm_has_events(void)
{
  AppTasks_Init();
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT32(0U, fake_platform.probe_rises[PLATFORM_PROBE_REACT]);   /* черга порожня */
  (void)SmartLock_PostEvent(SL_EV_MOTION, 0U);
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.probe_rises[PLATFORM_PROBE_REACT]);
  TEST_ASSERT_EQUAL_UINT8(0U, fake_platform.probe_level[PLATFORM_PROBE_REACT]);
}

/* Фізичний фронт → buttons/security (опитування) → черга → fsm з імпульсом PB1 */
static void test_input_edges_reach_fsm_with_react_pulse(void)
{
  AppTasks_Init();                          /* B1 відпущена (1), PIR у спокої (0) */
  fake_platform.now_ms = 10U;
  Sched_RunOnce();                          /* перше опитування — без фронтів */
  TEST_ASSERT_EQUAL_UINT32(0U, fake_platform.probe_rises[PLATFORM_PROBE_REACT]);

  fake_platform.inputs &= ~PLATFORM_IN_LOCK;   /* B1 натиснули */
  fake_platform.inputs |= PLATFORM_IN_PIR;     /* PIR спрацював */
  fake_platform.now_ms = 20U;
  Sched_RunOnce();                          /* buttons і security бачать фронти, кладуть події */
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_EventsPending());
  Sched_RunOnce();                          /* наступний прохід: fsm забирає обидві */
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.probe_rises[PLATFORM_PROBE_REACT]);

  fake_platform.now_ms = 40U;               /* рівень не змінився — нових подій немає */
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_EventsPosted());
  /* security і env мають власні канали PB2 / PB12 */
  TEST_ASSERT_TRUE(fake_platform.probe_rises[PLATFORM_PROBE_CAMERA] >= 2U);
}

/* ---- I06: рядки STACK/HEAP лише при зростанні максимуму --------------------------------- */
static char out[1024];

static const char *capture_pass(void)
{
  FILE *tmp = tmpfile();
  int saved;
  size_t n;

  out[0] = '\0';
  if (tmp == NULL) { return out; }
  fflush(stdout);
  saved = dup(1);
  dup2(fileno(tmp), 1);
  Sched_RunOnce();
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  rewind(tmp);
  n = fread(out, 1U, sizeof(out) - 1U, tmp);
  out[n] = '\0';
  fclose(tmp);
  return out;
}

static void test_mem_task_prints_stack_heap_only_on_new_maximum(void)
{
  AppTasks_Init();
  fake_platform.stack_used = 600U;
  fake_platform.heap.arena = 1460U;
  AppTasks_PrintMem();                                  /* як App_Init() після self-test */

  fake_platform.now_ms = 1000U;                         /* період mem — 1 с */
  TEST_ASSERT_NULL(strstr(capture_pass(), "STACK "));   /* нічого не зросло */

  fake_platform.stack_used = 712U;                      /* сценарій дійшов глибше */
  fake_platform.now_ms = 2000U;
  TEST_ASSERT_NOT_NULL(strstr(capture_pass(),
                       "STACK reserved=1024 used=712 free=312\r\n"
                       "HEAP arena=1460 used=0 free=0\r\n"));

  fake_platform.heap.arena = 2000U;                     /* heap теж стежиться */
  fake_platform.now_ms = 3000U;
  TEST_ASSERT_NOT_NULL(strstr(capture_pass(), "HEAP arena=2000 "));
  fake_platform.now_ms = 4000U;
  TEST_ASSERT_NULL(strstr(capture_pass(), "STACK "));
}

static void test_uart_task_runs_lr2_commands(void)
{
  AppTasks_Init();
  fake_platform.uart_rx = "?";
  fake_platform.uart_pos = 0U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(1U, (unsigned)fake_platform.uart_pos);   /* символ забрала uart-задача */
  TEST_ASSERT_EQUAL_UINT32(1U, Sched_Task(6)->exec.count);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_perf_conversions_and_stats);
  RUN_TEST(test_perf_time64_survives_counter_wrap);
  RUN_TEST(test_add_rejects_bad_tasks_and_full_table);
  RUN_TEST(test_periodic_runs_on_time_event_runs_every_pass);
  RUN_TEST(test_late_run_counts_jitter_and_missed_without_catch_up);
  RUN_TEST(test_cpu_load_counts_only_useful_work);
  RUN_TEST(test_event_queue_fifo_overflow_and_wait);
  RUN_TEST(test_app_tasks_table_order);
  RUN_TEST(test_posted_event_reaches_fsm_next_pass);
  RUN_TEST(test_weak_hooks);
  RUN_TEST(test_uart_task_runs_lr2_commands);
  RUN_TEST(test_loop_probe_pulses_every_pass);
  RUN_TEST(test_task_probe_wraps_only_its_task);
  RUN_TEST(test_react_probe_only_when_fsm_has_events);
  RUN_TEST(test_input_edges_reach_fsm_with_react_pulse);
  RUN_TEST(test_mem_task_prints_stack_heap_only_on_new_maximum);
  return UNITY_END();
}
