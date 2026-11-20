/**
  * @file    test_sched.c
  * @brief   ЛР3 (I02): каркас Super Loop без плати — планувальник (sched.c),
  *          час (perf.c), черга подій FSM (event_queue.c), таблиця задач
  *          (app_tasks.c), службові GPIO для аналізатора (I05) і точки підключення
  *          (smartlock_hooks.h), заміщені сильними модулями sl_*.c; слабкі
  *          (smartlock_hooks_weak.c) — у test_hooks_weak.c.
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
#include "cmd.h"
#include "event_queue.h"
#include "evt.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "perf.h"
#include "platform.h"
#include "sched.h"
#include "sl_security.h"
#include "sl_env.h"
#include "sl_buttons.h"
#include "sl_camera_link.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "smartlock_hooks.h"
#include "stats.h"

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
  /* I09: camera — подієва задача між env і storage (metrics-spec M-01) */
  static const char *const names[] = { "fsm", "buttons", "security", "env", "camera", "storage", "mem", "uart" };

  AppTasks_Init();
  TEST_ASSERT_TRUE(Sched_Started());
  TEST_ASSERT_EQUAL_INT(8, Sched_Count());
  for (int i = 0; i < 8; i++)
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

/* Фізичний фронт → buttons/security (опитування) → черга → fsm з імпульсом PB1.
 * I04: на платі той самий фронт дає і переривання EXTI — тест подає його через
 * fake_platform_raise_event(), PB1 (REACT) — у момент, коли задача забирає подію.
 * ЛР3: хуки сильні (sl_buttons, sl_security): B1 стає подією лише після антидребезгу
 * (TBD_BTN_DEBOUNCE_MS), PIR — з наступного після базового зразка опитування, геркон
 * один раз повідомляє початковий стан. Тому проходи йдуть з кроком часу. */
static void test_input_edges_reach_fsm_with_react_pulse(void)
{
  uint32_t posted;

  SmartLock_JournalInit();                  /* модулі sl_* зберігають стан між тестами */
  SmartLock_FsmInit();
  SmartLock_ButtonsInit();
  SmartLock_SecurityInit();
  SmartLock_EnvInit();
  AppTasks_Init();                          /* B1 відпущена (1), PIR у спокої (0) */
  fake_platform.inputs &= ~PLATFORM_IN_DOOR;   /* магніт поруч: двері зачинені */
  fake_platform.now_ms = 10U;
  Sched_RunOnce();                          /* перше опитування — база, без фронтів */
  TEST_ASSERT_EQUAL_UINT32(0U, fake_platform.probe_rises[PLATFORM_PROBE_REACT]);

  fake_platform.inputs &= ~PLATFORM_IN_LOCK;   /* B1 натиснули */
  fake_platform.inputs |= PLATFORM_IN_PIR;     /* PIR спрацював */
  fake_platform_raise_event(PLATFORM_EV_LOCK_B1, 15000U);   /* EXTI цих фронтів (I04) */
  fake_platform_raise_event(PLATFORM_EV_PIR, 15000U);
  for (uint32_t t = 20U; t <= 200U; t += 10U)  /* > антидребезг B1 і геркона */
  {
    fake_platform.now_ms = t;
    Sched_RunOnce();
  }
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());           /* fsm забрала все */
  TEST_ASSERT_TRUE(fake_platform.probe_rises[PLATFORM_PROBE_REACT] >= 1U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());     /* LOCK_REQUEST дійшов */
  TEST_ASSERT_TRUE(SmartLock_EventsPosted() >= 3U);   /* LOCK_REQUEST, MOTION, DOOR_CLOSED */

  posted = SmartLock_EventsPosted();        /* рівні не змінюються — нових подій немає */
  for (uint32_t t = 210U; t <= 400U; t += 10U)
  {
    fake_platform.now_ms = t;
    Sched_RunOnce();
  }
  TEST_ASSERT_EQUAL_UINT32(posted, SmartLock_EventsPosted());
  /* I09: PB2 / PB12 — лише захоплення і запис кадру; у UNLOCKED кадр не запитується */
  TEST_ASSERT_EQUAL_UINT32(0U, fake_platform.probe_rises[PLATFORM_PROBE_CAMERA]);
  TEST_ASSERT_EQUAL_UINT32(0U, fake_platform.probe_rises[PLATFORM_PROBE_STORAGE]);
}

/* ---- I06: рядки STACK/HEAP лише при зростанні максимуму --------------------------------- */
static char out[4096];

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

/* Вивід довільної дії (Cmd_Execute) — для перевірки формату рядків I03 */
static const char *s_cmd_line;

static void run_cmd(void)
{
  Cmd_Execute(s_cmd_line);
}

static const char *capture_cmd(const char *line)
{
  FILE *tmp = tmpfile();
  int saved;
  size_t n;

  out[0] = '\0';
  if (tmp == NULL) { return out; }
  s_cmd_line = line;
  fflush(stdout);
  saved = dup(1);
  dup2(fileno(tmp), 1);
  run_cmd();
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
  {
    const char *o = capture_pass();

    TEST_ASSERT_NOT_NULL(strstr(o, "STACK reserved=1024 used=712 free=312\r\n"
                                   "HEAP arena=1460 used=0 free=0\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(o, "EVT t_us=2000000 name=mem_peak\r\n"));   /* I04: момент максимуму */
  }

  fake_platform.heap.arena = 2000U;                     /* heap теж стежиться */
  fake_platform.now_ms = 3000U;
  TEST_ASSERT_NOT_NULL(strstr(capture_pass(), "HEAP arena=2000 "));
  fake_platform.now_ms = 4000U;
  TEST_ASSERT_NULL(strstr(capture_pass(), "STACK "));
}

/* ---- I03: STATS і рядкові команди -------------------------------------------------- */
static void test_perf_format_helpers(void)
{
  char b[24];
  perf_stat_t s = PERF_STAT_INIT("x");

  TEST_ASSERT_EQUAL_UINT(1U, (unsigned)Perf_FormatU64(b, sizeof(b), 0U));
  TEST_ASSERT_EQUAL_STRING("0", b);
  TEST_ASSERT_EQUAL_UINT(20U, (unsigned)Perf_FormatU64(b, sizeof(b), 18446744073709551615ULL));
  TEST_ASSERT_EQUAL_STRING("18446744073709551615", b);
  TEST_ASSERT_EQUAL_UINT(0U, (unsigned)Perf_FormatU64(b, 3U, 12345U));   /* не вміщається */
  TEST_ASSERT_EQUAL_STRING("", b);
  TEST_ASSERT_EQUAL_UINT(0U, (unsigned)Perf_FormatU64(b, 0U, 1U));
  TEST_ASSERT_EQUAL_INT32(-5, Perf_CyclesToUsSigned(-5));                 /* 1 такт = 1 мкс */

  Perf_StatAdd(&s, 100U);
  Perf_StatAdd(&s, 300U);
  (void)Perf_FormatStat(b, sizeof(b), &s, false);
  TEST_ASSERT_EQUAL_STRING_LEN("STAT name=x n=2 min_us=", b, 22U);
  {
    char l[96];

    (void)Perf_FormatStat(l, sizeof(l), &s, false);
    TEST_ASSERT_EQUAL_STRING("STAT name=x n=2 min_us=100 avg_us=200 max_us=300", l);
    (void)Perf_FormatStat(l, sizeof(l), &s, true);
    TEST_ASSERT_EQUAL_STRING("STAT name=x n=2 min_cyc=100 avg_cyc=200 max_cyc=300", l);
  }
}

static void test_stats_prints_every_metric_line(void)
{
  const char *o;

  AppTasks_Init();
  for (uint32_t ms = 10U; ms <= 40U; ms += 10U)
  {
    fake_platform.now_ms = ms;
    Sched_RunOnce();
  }
  o = capture_cmd("STATS");
  TEST_ASSERT_NOT_NULL(strstr(o, "STATS_BEGIN git=nogit t_us=40000 loops=4\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=loop n=4 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "CPU name=cpu_load windows="));
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=fsm "));
  /* runs залежить від модулів sl_*: сильна FSM на першому проході ініціалізується (серво UNLOCK) */
  TEST_ASSERT_NOT_NULL(strstr(o, "POLL name=fsm polls=4 runs="));
  /* формат JITTER — docs/metrics-spec.md §3.4 */
  TEST_ASSERT_NOT_NULL(strstr(o, "JITTER name=buttons period_us=10000 min_us=0 max_us=0 missed=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "JHIST name=buttons n=3 h_lt100us=3 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "JITTER name=security period_us=20000 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "JITTER name=env period_us=2000000 min_us=0 max_us=0 missed=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "JHIST name=env n=0 "));   /* 2 с ще не минули */
  TEST_ASSERT_NOT_NULL(strstr(o, "POLL name=uart "));
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=evq_wait n=0 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "EVQ len=8 pending=0 max_depth=0 posted=0 dropped=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "UART rx_overrun=0 cmd_overflow=0 cmd_dropped=0 rx_peak=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "STACK reserved=1024 used=0 free=1024\r\nHEAP arena=0 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "STATS_END\r\n"));

  o = capture_cmd("STATS RAW");
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=loop n=4 min_cyc="));

  o = capture_cmd("STATS RESET");
  TEST_ASSERT_EQUAL_STRING("STATS,reset\r\n", o);
  TEST_ASSERT_EQUAL_UINT32(0U, Sched_Loops());
  o = capture_cmd("STATS");
  TEST_ASSERT_NOT_NULL(strstr(o, "loops=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=loop n=0 min_us=0 avg_us=0 max_us=0\r\n"));
}

static void test_line_commands(void)
{
  const char *o;

  AppTasks_Init();
  fake_platform.now_ms = 1234U;
  o = capture_cmd("PING 7");
  TEST_ASSERT_EQUAL_STRING("PONG seq=7 t_us=1234000\r\n", o);
  o = capture_cmd("MEM");
  TEST_ASSERT_EQUAL_STRING("STACK reserved=1024 used=0 free=1024\r\nHEAP arena=0 used=0 free=0\r\n", o);
  o = capture_cmd("HELP");
  TEST_ASSERT_NOT_NULL(strstr(o, "STATS [RAW|RESET] | MEM | PING <n> | MARK <n> | VER | HELP"));
  o = capture_cmd("VER");   /* I10: рядок BUILD з git-хешем */
  TEST_ASSERT_EQUAL_STRING("BUILD,fw=SmartLock,lab=LR3,git=" GIT_HASH ",cpu_hz=1000000,loop=superloop\r\n", o);
  o = capture_cmd("FOO");
  TEST_ASSERT_EQUAL_STRING("# unknown command 'FOO', HELP for list\r\n", o);
}

/* metrics-spec §3.5: LOAD pct= раз на секунду, з uart-задачі */
static void test_load_line_once_per_cpu_window(void)
{
  const char *o;

  AppTasks_Init();
  fake_platform.now_ms = 500U;
  TEST_ASSERT_NULL(strstr(capture_pass(), "LOAD "));         /* вікно ще не закрите */
  fake_platform.now_ms = 1000U;
  (void)capture_pass();                                      /* вікно 1 с закривається в кінці проходу */
  TEST_ASSERT_EQUAL_UINT32(1U, Sched_CpuStat()->count);
  o = capture_pass();                                        /* uart наступного проходу друкує */
  TEST_ASSERT_NOT_NULL(strstr(o, "LOAD pct="));
  fake_platform.now_ms = 1100U;
  TEST_ASSERT_NULL(strstr(capture_pass(), "LOAD "));         /* лише раз за вікно */
  fake_platform.now_ms = 2100U;
  (void)capture_pass();
  TEST_ASSERT_NOT_NULL(strstr(capture_pass(), "LOAD pct="));
  (void)capture_cmd("STATS RESET");                          /* лічильник вікон з нуля */
  fake_platform.now_ms = 3200U;
  (void)capture_pass();
  TEST_ASSERT_NOT_NULL(strstr(capture_pass(), "LOAD pct="));
}

static void test_uart_routes_lines_and_lr2_letters(void)
{
  const char *o;

  AppTasks_Init();
  /* рядок з великої літери — Cmd, усе інше — Bench_Command (ЛР2) */
  fake_platform.uart_rx = "PING 3\r?";
  fake_platform.uart_pos = 0U;
  o = capture_pass();
  TEST_ASSERT_NOT_NULL(strstr(o, "PONG seq=3 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "# commands:"));   /* довідка ЛР2 на '?' */

  /* задовгий рядок не виконується і рахується */
  fake_platform.uart_rx = "STATSXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n";
  fake_platform.uart_pos = 0U;
  o = capture_pass();
  if (Cmd_LineOverflows() == 0U)
  {
    o = capture_pass();   /* решта символів — наступного проходу (UART_RX_PER_PASS) */
  }
  TEST_ASSERT_NOT_NULL(strstr(o, "# command too long (>47)"));
  TEST_ASSERT_EQUAL_UINT32(1U, Cmd_LineOverflows());
}

/* ---- I04: події з ISR → LAT, EVT, імпульс PB1, черга FSM --------------------------- */
static void test_react_probe_on_event_take(void)
{
  AppTasks_Init();
  fake_platform.now_ms = 10U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT32(0U, fake_platform.probe_rises[PLATFORM_PROBE_REACT]);   /* подій немає */
  fake_platform_raise_event(PLATFORM_EV_LOCK_B1, 10000U);
  fake_platform.now_ms = 20U;
  Sched_RunOnce();                                     /* buttons забирає подію */
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.probe_rises[PLATFORM_PROBE_REACT]);
  TEST_ASSERT_EQUAL_UINT8(0U, fake_platform.probe_level[PLATFORM_PROBE_REACT]);
}

/* Черга FSM і кільце EVT порожні: у наступних проходах друкується лише те, що
 * породив сам тест, а не модулі sl_* (K/H-картки) з попередніх проходів. */
static void quiesce(void)
{
  sl_event_t ev;

  while (SmartLock_TakeEvent(&ev))
  {
  }
  Evt_Init();
}

/* Фронт B1 і PIR в ISR → buttons/security забирають (LAT, EVT, PB1) → хуки.
 * Що хук робить з фронтом (антидребезг K03, рішення FSM K05), тут не перевіряється —
 * це тести sl_buttons / sl_security / sl_fsm і test_input_edges_reach_fsm_with_react_pulse. */
static void test_isr_events_give_latency_evt_and_reach_fsm(void)
{
  const char *o;
  const perf_latency_t *b1;

  AppTasks_Init();
  fake_platform.now_ms = 10U;
  Sched_RunOnce();

  fake_platform_raise_event(PLATFORM_EV_LOCK_B1, 13000U);   /* фронт у 13 мс */
  fake_platform_raise_event(PLATFORM_EV_LOCK_B1, 13500U);   /* дребезг до обробки */
  fake_platform_raise_event(PLATFORM_EV_PIR, 15000U);
  fake_platform.now_ms = 20U;                          /* buttons (10 мс) і security (20 мс) */
  quiesce();
  o = capture_pass();                                  /* у цьому ж проході uart друкує 2 EVT */
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT t_us=13000 name=btn_b1_press\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT t_us=20000 name=btn_b1_handled\r\n"));
  TEST_ASSERT_EQUAL_UINT32(2U, fake_platform.probe_rises[PLATFORM_PROBE_REACT]);   /* B1 і PIR */

  b1 = AppTasks_Latency(0);
  TEST_ASSERT_EQUAL_STRING("btn_b1_lat", b1->st.name);
  TEST_ASSERT_EQUAL_UINT32(1U, b1->st.count);
  TEST_ASSERT_EQUAL_UINT32(7000U, b1->st.max);         /* 20 мс − 13 мс, 1 такт = 1 мкс */
  TEST_ASSERT_EQUAL_UINT32(5000U, AppTasks_Latency(1)->st.max);
  TEST_ASSERT_EQUAL_UINT32(1U, Platform_EventOverruns(PLATFORM_EV_LOCK_B1));
  TEST_ASSERT_NULL(AppTasks_Latency(3));
  TEST_ASSERT_NULL(AppTasks_Latency(-1));

  o = capture_pass();                                  /* наступний прохід: решта EVT */
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT t_us=15000 name=pir_trig\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT t_us=20000 name=pir_handled\r\n"));

  /* момент, коли FSM забирає подію з черги, — EVT fsm_<подія> (event_queue.c);
   * забирає тест, бо скільки подій за прохід бере FSM — справа K02 */
  quiesce();
  (void)SmartLock_PostEvent(SL_EV_LOCK_REQUEST, 0U);
  (void)SmartLock_PostEvent(SL_EV_MOTION, 0U);
  {
    sl_event_t ev;

    while (SmartLock_TakeEvent(&ev))
    {
    }
  }
  o = capture_pass();
  TEST_ASSERT_NOT_NULL(strstr(o, "name=fsm_lock_request\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "name=fsm_motion\r\n"));

  o = capture_cmd("STATS");
  /* metrics-spec §3.2: затримка — звичайний STAT; повторні фронти — OVR */
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=btn_b1_lat n=1 min_us=7000 avg_us=7000 max_us=7000\r\n"
                                 "OVR name=btn_b1 count=1\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=pir_lat n=1 min_us=5000 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "STAT name=mpu_lat n=0 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "OVR name=mpu count=0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "EVTLOG pending="));
  (void)capture_cmd("STATS RESET");
  TEST_ASSERT_EQUAL_UINT32(0U, AppTasks_Latency(0)->st.count);
}

/* ---- I04: EVT — формат, 64-бітний час, кільцевий буфер ---------------------------- */
static void test_evt_format_ring_and_drop(void)
{
  char line[96];
  const char *o;

  Evt_Init();
  (void)Evt_Format(line, sizeof(line), 4294967296000ULL, "x", EVT_NOARG);
  TEST_ASSERT_EQUAL_STRING("EVT t_us=4294967296000 name=x", line);   /* > 32 біт */
  (void)Evt_Format(line, sizeof(line), 5U, "mark", 3U);
  TEST_ASSERT_EQUAL_STRING("EVT t_us=5 name=mark_3", line);   /* без пробілу: \\S+ */

  for (unsigned i = 0U; i < EVT_RING_LEN + 3U; i++)
  {
    Evt_LogArg("e", i);
  }
  TEST_ASSERT_EQUAL_UINT(EVT_RING_LEN, Evt_Pending());
  TEST_ASSERT_EQUAL_UINT32(3U, Evt_Dropped());          /* старі записи цінніші */
  {
    FILE *tmp = tmpfile();
    int saved;
    size_t n;

    TEST_ASSERT_NOT_NULL(tmp);
    fflush(stdout);
    saved = dup(1);
    dup2(fileno(tmp), 1);
    TEST_ASSERT_EQUAL_UINT(2U, Evt_Flush(2U));
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    rewind(tmp);
    n = fread(out, 1U, sizeof(out) - 1U, tmp);
    out[n] = '\0';
    fclose(tmp);
  }
  o = out;
  TEST_ASSERT_NOT_NULL(strstr(o, "name=e_0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "name=e_1\r\n"));
  TEST_ASSERT_EQUAL_UINT(EVT_RING_LEN - 2U, Evt_Pending());
}

static void test_mark_uart_cmd_evt_and_uart_overrun(void)
{
  const char *o;

  AppTasks_Init();
  fake_platform.now_ms = 5U;
  /* рядок з UART: uart_cmd_rx → mark_42 → uart_cmd_tx (metrics-spec §3.3, M-03) */
  fake_platform.uart_rx = "MARK 42\r";
  fake_platform.uart_pos = 0U;
  o = capture_pass();                          /* друк — 2 EVT за прохід */
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT t_us=5000 name=uart_cmd_rx\r\nEVT t_us=5000 name=mark_42\r\n"));
  o = capture_pass();
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT t_us=5000 name=uart_cmd_tx\r\n"));

  fake_platform.uart_overruns = 3U;
  fake_platform.uart_rx_peak  = 120U;
  o = capture_cmd("STATS");
  TEST_ASSERT_NOT_NULL(strstr(o, "UART rx_overrun=3 cmd_overflow=0 cmd_dropped=0 rx_peak=120\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "EVTLOG pending=0 dropped=0\r\n"));
}

/* Прийом загубив байти (PLATFORM_UART_LOST, у fake — '\xFE'): зіпсований рядок не
 * виконується зі зміненим змістом і не склеюється з наступним, а дає ERR; наступна
 * ціла команда виконується. Випадки з журналу дефекту (20_uart_rx_loss.txt). */
static void test_uart_rx_lost_drops_damaged_line(void)
{
  const char *o;

  AppTasks_Init();
  fake_platform.now_ms = 5U;

  /* «PING 12» без «2»: не PONG seq=1 */
  fake_platform.uart_rx  = "PING 1\xFE\rPING 13\r";
  fake_platform.uart_pos = 0U;
  o = capture_pass();
  TEST_ASSERT_NULL(strstr(o, "PONG seq=1 "));
  TEST_ASSERT_NOT_NULL(strstr(o, "ERR,cmd=dropped,reason=rx_lost\r\nPONG seq=13 "));

  /* загублений '\r' між командами: не «unknown command 'MMARK 2'»; уламок відкинуто
   * до найближчого кінця рядка, далі — нормально */
  fake_platform.uart_rx  = "MARK 1\xFEMARK 2\rPING 5\r";
  fake_platform.uart_pos = 0U;
  o = capture_pass();
  TEST_ASSERT_NULL(strstr(o, "unknown command"));
  TEST_ASSERT_NULL(strstr(o, "name=mark_"));
  TEST_ASSERT_NOT_NULL(strstr(o, "ERR,cmd=dropped,reason=rx_lost\r\nPONG seq=5 "));

  /* загублено початок рядка («P»): «ING 12» не стає командою; дві втрати поспіль — одна відповідь */
  fake_platform.uart_rx  = "\xFE\xFEING 12\r\nPING 6\r";
  fake_platform.uart_pos = 0U;
  o = capture_pass();
  TEST_ASSERT_NULL(strstr(o, "unknown command"));
  TEST_ASSERT_NOT_NULL(strstr(o, "ERR,cmd=dropped,reason=rx_lost\r\nPONG seq=6 "));

  TEST_ASSERT_EQUAL_UINT32(3U, Cmd_Dropped());
  o = capture_cmd("STATS");
  TEST_ASSERT_NOT_NULL(strstr(o, " cmd_dropped=3 "));
  Cmd_Reset();
  TEST_ASSERT_EQUAL_UINT32(0U, Cmd_Dropped());
}

/* ЛР3 (K09): усі п'ять точок підключення заміщені сильними модулями sl_*.c (слабкі
 * реалізації перевіряє окремий бінарник test_hooks_weak). Ті самі прямі виклики
 * хуків, що й у test_hooks_weak.c, але з контрактом сильних модулів: подія — лише зі
 * стабільного рівня, FSM забирає всі події, storage вивантажує по запису за прохід,
 * MPU-6050 і DHT11 справді читаються. */
static void test_strong_hooks_replace_weak(void)
{
  SmartLock_EventsInit();
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  SmartLock_ButtonsInit();
  SmartLock_SecurityInit();
  SmartLock_EnvInit();
  fake_platform_set_inputs(PLATFORM_IN_DOOR, 0);   /* магніт поруч: двері зачинені */

  /* перший виклик лише запам'ятовує рівні; фронт B1 з EXTI без стабільного рівня
   * (завада) — не подія (слабкий хук тут клав LOCK_REQUEST одразу) */
  TEST_ASSERT_FALSE(SmartLock_ButtonsPoll(0U, true));
  TEST_ASSERT_FALSE(SmartLock_ButtonsPoll(10U, true));
  TEST_ASSERT_FALSE(SmartLock_SecurityPoll(0U, false, false));   /* база PIR і геркона */
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());

  /* B1 утримується 20 мс — рівно одна LOCK_REQUEST з міткою прийняття */
  fake_platform_set_inputs(PLATFORM_IN_LOCK, 0);
  TEST_ASSERT_FALSE(SmartLock_ButtonsPoll(20U, false));
  TEST_ASSERT_TRUE(SmartLock_ButtonsPoll(40U, false));
  TEST_ASSERT_FALSE(SmartLock_ButtonsPoll(50U, false));          /* утримання — без автоповтору */
  /* геркон стабільний 100 мс — DOOR_CLOSED; фронт PIR з EXTI — MOTION */
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(100U, true, false));
  TEST_ASSERT_EQUAL_UINT(3U, SmartLock_EventsPending());          /* LOCK_REQUEST, DOOR_CLOSED, MOTION */

  TEST_ASSERT_TRUE(SmartLock_FsmStep(100U));
  TEST_ASSERT_FALSE(SmartLock_FsmStep(100U));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  /* LOCK_REQUEST і DOOR_CLOSED дали два записи (MOTION поза LOCKED ігнорується),
   * storage вивантажує по одному за прохід */
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS - 60U, SmartLock_FsmExitDelayRemainingMs(100U));   /* від 40 мс */
  TEST_ASSERT_TRUE(SmartLock_StoragePoll(100U));
  TEST_ASSERT_TRUE(SmartLock_StoragePoll(100U));
  TEST_ASSERT_FALSE(SmartLock_StoragePoll(100U));
  TEST_ASSERT_FALSE(SmartLock_CameraRequest(1U));   /* слабка заглушка: камери ще немає */

  /* MPU-6050 «є», але не відповідає — security все одно звертається до шини */
  app_devices.mpu_status = DEV_OK;
  app_devices.mpu.bus = Platform_SensorI2c();
  app_devices.mpu.addr7 = MPU6050_I2C_ADDR;
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(120U, false, false));
  TEST_ASSERT_EQUAL_UINT32(1U, SlSecurity_MpuErrors());

  /* DHT11 відповідає — валідні T/RH; потім тайм-аут — лише лічильник. Кадр не
   * нульовий: нульовий DHT11 відкидає з PR #31 (датчик без живлення). */
  fake_platform_dht_values(45U, 22U);
  TEST_ASSERT_TRUE(SmartLock_EnvPoll(0U));
  TEST_ASSERT_TRUE(SmartLock_EnvLastDht(NULL, NULL));
  fake_platform.dht_status = DEV_ERR_TIMEOUT;
  TEST_ASSERT_TRUE(SmartLock_EnvPoll(0U));
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvDhtErrors());
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());   /* відмова DHT11 подій не дає */
}

static void test_uart_task_runs_lr2_commands(void)
{
  AppTasks_Init();
  fake_platform.uart_rx = "?";
  fake_platform.uart_pos = 0U;
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(1U, (unsigned)fake_platform.uart_pos);   /* символ забрала uart-задача */
  TEST_ASSERT_EQUAL_STRING("uart", Sched_Task(7)->name);
  TEST_ASSERT_EQUAL_UINT32(1U, Sched_Task(7)->exec.count);
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
  RUN_TEST(test_strong_hooks_replace_weak);
  RUN_TEST(test_uart_task_runs_lr2_commands);
  RUN_TEST(test_loop_probe_pulses_every_pass);
  RUN_TEST(test_task_probe_wraps_only_its_task);
  RUN_TEST(test_react_probe_on_event_take);
  RUN_TEST(test_input_edges_reach_fsm_with_react_pulse);
  RUN_TEST(test_isr_events_give_latency_evt_and_reach_fsm);
  RUN_TEST(test_evt_format_ring_and_drop);
  RUN_TEST(test_mark_uart_cmd_evt_and_uart_overrun);
  RUN_TEST(test_uart_rx_lost_drops_damaged_line);
  RUN_TEST(test_mem_task_prints_stack_heap_only_on_new_maximum);
  RUN_TEST(test_perf_format_helpers);
  RUN_TEST(test_stats_prints_every_metric_line);
  RUN_TEST(test_line_commands);
  RUN_TEST(test_uart_routes_lines_and_lr2_letters);
  RUN_TEST(test_load_line_once_per_cpu_window);
  return UNITY_END();
}
