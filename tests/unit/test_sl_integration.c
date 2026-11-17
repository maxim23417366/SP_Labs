/**
  * @file    test_sl_integration.c
  * @brief   ЛР3 (K07/K08/K09, Коваль): наскрізні сценарії SmartLock через
  *          справжній Super Loop і справжній журнал W25Q64 без плати.
  *
  *          Усе працює разом, як на платі: Storage_Init() + Frames_Init() +
  *          AppTasks_Init(), як App_Init(), і Sched_RunOnce() по одному проходу на
  *          мілісекунду; справжні sl_buttons / sl_security / sl_env / sl_fsm /
  *          sl_journal / sl_camera_link / sl_camera_frames, бекенд журналу —
  *          storage.c + storage_ops.c (H09), кадри — frames.c (H10), а W25Q64 —
  *          fake_w25q64 (модель NOR-Flash у RAM) через справжній драйвер.
  *
  *          Тест керує лише «фізикою»: fake_platform (рівні кнопок, геркона, PIR;
  *          фронти B1 і PIR ще й як переривання EXTI — I04; кадр DHT11; час) і
  *          fake_sensors (MPU-6050, BMP180 на fake_i2c). Читає виходи: команди
  *          серво (fake_platform.servo_log), записи журналу, прочитані назад з
  *          W25Q64 (flash_log), рядки UART (EVT,id=... і EVT t_us=...). Камера —
  *          слабка заглушка sl_camera_link.c: модуля I09 ще немає, кадр подає тест
  *          через SmartLock_CameraStoreFrame().
  *
  *          Сценарії відповідають docs/lab_3/e2e-test-cases.md (E2E-01…06) і
  *          рядкам FR-02…FR-15 docs/lab_3/fr-verification-matrix.md; апаратні
  *          частини (реальний дребезг, поріг удару, час аналізатором) — на стенді.
  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

#include "app.h"
#include "app_config.h"
#include "app_tasks.h"
#include "event_queue.h"
#include "fake_delay.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_sensors.h"
#include "fake_w25q64.h"
#include "flash_log.h"
#include "frames.h"
#include "perf.h"
#include "platform.h"
#include "sched.h"
#include "sl_buttons.h"
#include "sl_camera_frames.h"
#include "sl_camera_link.h"
#include "sl_env.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "sl_security.h"
#include "smartlock_hooks.h"
#include "storage.h"

/* ---- перехоплення stdout --------------------------------------------------------- */
static char out[1 << 20];

static const char *capture(void (*fn)(void))
{
  FILE *tmp = tmpfile();
  int saved;
  size_t n = 0U;

  out[0] = '\0';
  if (tmp == NULL) { return out; }
  fflush(stdout);
  saved = dup(1);
  dup2(fileno(tmp), 1);
  fn();
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  rewind(tmp);
  n = fread(out, 1U, sizeof(out) - 1U, tmp);
  out[n] = '\0';
  fclose(tmp);
  return out;
}

/* ---- W25Q64: записи журналу, прочитані назад з Flash ------------------------------------ */
#define FLASH_LEN 256U

typedef struct
{
  uint32_t seq;
  uint32_t t_ms;
  uint8_t  type;    /* код Flash, flash-layout.md розділ 3.2 */
  uint16_t boot;
  uint32_t arg;
  uint32_t arg2;
  uint32_t frame;
} flash_rec_t;

static flash_rec_t flash[FLASH_LEN];   /* усі дійсні записи, крім BOOT, від найстарішого */
static unsigned    flash_n;
static unsigned    flash_boots;
static unsigned    flash_damaged;

static void flash_read(void)
{
  flash_log_iter_t it;
  flash_log_entry_t e;
  flash_log_slot_t state;

  flash_n = 0U;
  flash_boots = 0U;
  flash_damaged = 0U;
  flash_log_iter_begin(Storage_Journal(), &it);
  while ((flash_log_next(Storage_Journal(), &it, &e, &state, NULL) == DEV_OK) &&
         (state != FLASH_LOG_SLOT_EMPTY))
  {
    if (state != FLASH_LOG_SLOT_VALID)
    {
      flash_damaged++;
      continue;
    }
    if (e.type == FLASH_LOG_BOOT)
    {
      flash_boots++;
      continue;
    }
    if (flash_n < FLASH_LEN)
    {
      flash[flash_n] = (flash_rec_t){ e.seq, e.t_ms, e.type, e.boot, e.arg, e.arg2, e.frame_id };
    }
    flash_n++;
  }
}

/* Скільки записів (крім BOOT) зараз у Flash */
static unsigned flash_count(void)
{
  flash_read();
  return flash_n;
}

static unsigned count_type(sl_log_type_t type)
{
  unsigned n = 0U;

  flash_read();
  for (unsigned i = 0U; (i < flash_n) && (i < FLASH_LEN); i++)
  {
    if (flash[i].type == Storage_FlashType(type)) { n++; }
  }
  return n;
}

/* n-й (з 0) запис типу type у Flash; NULL — немає */
static const flash_rec_t *find(sl_log_type_t type, unsigned nth)
{
  flash_read();
  for (unsigned i = 0U; (i < flash_n) && (i < FLASH_LEN); i++)
  {
    if ((flash[i].type == Storage_FlashType(type)) && (nth-- == 0U)) { return &flash[i]; }
  }
  return NULL;
}

/* ---- час і Super Loop --------------------------------------------------------------- */
#define T0_MS 1000U

static uint32_t now(void)
{
  return fake_platform.now_ms;
}

/* Проходи Super Loop по одному на мс, доки не настане t_end (різниця uint32 —
 * працює і через переповнення Platform_Millis()) */
static void run_to(uint32_t t_end)
{
  while ((int32_t)(t_end - fake_platform.now_ms) > 0)
  {
    fake_platform.now_ms++;
    Sched_RunOnce();
  }
}

static void run_for(uint32_t ms)
{
  run_to(now() + ms);
}

static uint32_t s_cap_ms;

static void run_cap(void)
{
  run_for(s_cap_ms);
}

/* Прогнати ms мілісекунд із перехопленням UART */
static const char *run_captured(uint32_t ms)
{
  s_cap_ms = ms;
  return capture(run_cap);
}

/* ---- входи -------------------------------------------------------------------------- */
/* Рівень входу як на платі; фронти B1 (спадний) і PIR (наростаючий) ще й дають
 * переривання EXTI з міткою часу (I04): 1 такт fake_platform = 1 мкс */
static void set_level(uint32_t mask, int level)
{
  bool was = ((fake_platform.inputs & mask) != 0U);

  fake_platform_set_inputs(mask, level);
  if (((mask & PLATFORM_IN_LOCK) != 0U) && was && (level == 0))
  {
    fake_platform_raise_event(PLATFORM_EV_LOCK_B1, now() * 1000U);
  }
  if (((mask & PLATFORM_IN_PIR) != 0U) && !was && (level != 0))
  {
    fake_platform_raise_event(PLATFORM_EV_PIR, now() * 1000U);
  }
}

static void door(bool closed)
{
  set_level(PLATFORM_IN_DOOR, closed ? 0 : 1);   /* магніт поруч = 0 */
}

static void hold(uint32_t mask, bool pressed, uint32_t ms)
{
  set_level(mask, pressed ? 0 : 1);   /* pull-up: натиснуто = 0 */
  run_for(ms);
}

/* Чисте натискання: 60 мс утримання (> антидребезгу 20 мс + період 10 мс), 60 мс пауза */
static void press(uint32_t mask)
{
  hold(mask, true, 60U);
  hold(mask, false, 60U);
}

/* Сітка опитування buttons: від AppTasks_Init() кожні TBD_PERIOD_BUTTONS_US */
static uint32_t s_grid_ms;

static void align_to_poll(void)
{
  while (((now() - s_grid_ms) % (TBD_PERIOD_BUTTONS_US / 1000U)) != 0U)
  {
    run_for(1U);
  }
}

/* Натискання з дребезгом на фронті і на спаді. Сегменти коротші за антидребезг
 * (20 мс), але деякі довші за період опитування (10 мс): опитування бачить той
 * самий рівень двічі поспіль. Від початку (вирівняно до опитування):
 *   натискання: P 3, R 2, P 17 (опитування 10, 20), R 23 (30, 40), далі P —
 *               останній відскок на +45 мс;
 *   відпускання: R 22 (10, 20), P 23 (30, 40), далі R.
 * Антидребезг коротший за 23 мс (або «два однакові опитування») дав би тут
 * зайву подію / символ. Для B1 кожен спадний фронт — ще й EXTI (I04): мітка
 * першого, решта — OVR, як дребезг на платі. */
#define BOUNCE_LAST_MS 45U

static void press_bouncy(uint32_t mask)
{
  static const uint32_t down[] = { 3U, 2U, 17U, 23U };   /* натиснуто / відпущено по черзі */
  static const uint32_t up[]   = { 22U, 23U };          /* відпущено / натиснуто по черзі */

  align_to_poll();
  for (unsigned i = 0U; i < sizeof(down) / sizeof(down[0]); i++)
  {
    hold(mask, (i % 2U) == 0U, down[i]);
  }
  hold(mask, true, 80U);
  align_to_poll();
  for (unsigned i = 0U; i < sizeof(up) / sizeof(up[0]); i++)
  {
    hold(mask, (i % 2U) != 0U, up[i]);
  }
  hold(mask, false, 80U);
}

static const uint32_t code_mask[4] = { PLATFORM_IN_BTN1, PLATFORM_IN_BTN2, PLATFORM_IN_BTN3, PLATFORM_IN_BTN4 };

static void enter_code(const uint8_t *symbols, unsigned n, bool bouncy)
{
  for (unsigned i = 0U; i < n; i++)
  {
    if (bouncy)
    {
      press_bouncy(code_mask[symbols[i] - 1U]);
    }
    else
    {
      press(code_mask[symbols[i] - 1U]);
    }
  }
}

static const uint8_t code_ok[TBD_CODE_LEN] = TBD_CODE_VALUE;

static void enter_wrong_code(void)
{
  uint8_t wrong[TBD_CODE_LEN];

  for (unsigned i = 0U; i < TBD_CODE_LEN; i++)
  {
    wrong[i] = (uint8_t)((code_ok[i] % 4U) + 1U);   /* кожен символ інший */
  }
  enter_code(wrong, TBD_CODE_LEN, false);
}

/* ---- виходи ------------------------------------------------------------------------- */
static unsigned servo_count(uint32_t pulse)
{
  unsigned n = 0U;

  for (unsigned i = 0U; (i < fake_platform.servo_set_calls) && (i < FAKE_SERVO_LOG_LEN); i++)
  {
    if (fake_platform.servo_log[i] == pulse) { n++; }
  }
  return n;
}

/* Момент (now_ms) першої команди серво pulse; 0 — не було */
static uint32_t servo_first_ms(uint32_t pulse)
{
  for (unsigned i = 0U; (i < fake_platform.servo_set_calls) && (i < FAKE_SERVO_LOG_LEN); i++)
  {
    if (fake_platform.servo_log[i] == pulse) { return fake_platform.servo_log_ms[i]; }
  }
  return 0U;
}

static void expect_no_losses(void)
{
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EventsDropped());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_ButtonsDropped());
  TEST_ASSERT_EQUAL_UINT32(0U, SlSecurity_EventsLost());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalDropped());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalUnsaved());
  TEST_ASSERT_EQUAL_UINT32(0U, Storage_Errors());
  TEST_ASSERT_TRUE(SmartLock_JournalMaxDepth() < TBD_JOURNAL_FIFO_LEN - TBD_JOURNAL_FIFO_RESERVE);
}

/* Тип sl_journal за іменем з рядка EVT,...,name=<ІМ'Я>,... */
static sl_log_type_t type_by_name(const char *name, size_t len)
{
  for (unsigned t = 1U; t < (unsigned)SL_LOG_COUNT; t++)
  {
    const char *n = SmartLock_LogTypeName((sl_log_type_t)t);

    if ((strlen(n) == len) && (strncmp(n, name, len) == 0)) { return (sl_log_type_t)t; }
  }
  return SL_LOG_NONE;
}

/* ---- старт і блокування ---------------------------------------------------------------- */
static void mount(void)
{
  Storage_Init();
  Frames_Init();
}

/* Скидання МК: RAM модулів — нулі, вміст W25Q64 той самий; як App_Init() */
static void boot(uint32_t t0, bool door_closed)
{
  fake_platform.now_ms = t0;
  door(door_closed);
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  SmartLock_ButtonsInit();
  SmartLock_SecurityInit();
  SmartLock_EnvInit();
  (void)capture(mount);               /* LOG,MOUNT; BOOT; нумерація з Flash */
  TEST_ASSERT_TRUE(Storage_Ready());
  AppTasks_Init();                    /* час, черга, таблиця задач */
  s_grid_ms = t0;
  run_for(300U);                      /* security фіксує початковий стан дверей */
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL(door_closed ? SL_DOOR_CLOSED : SL_DOOR_OPEN, SmartLock_FsmDoor());
}

/* B1 → чекаємо, поки FSM прийме LOCK_REQUEST; повертає момент, від якого FSM рахує 15 с */
static uint32_t press_b1(bool bouncy)
{
  if (bouncy) { press_bouncy(PLATFORM_IN_LOCK); } else { press(PLATFORM_IN_LOCK); }
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  return now() - (SL_EXIT_DELAY_MS - SmartLock_FsmExitDelayRemainingMs(now()));
}

static void lock_with_b1(void)
{
  (void)press_b1(false);
  run_for(SL_EXIT_DELAY_MS);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  run_for(20U);   /* storage вивантажує LOCK_ACTIVATED, LOCKED */
}

static uint8_t frame_buf[1200];

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  fake_w25q64_reset();
  fake_delay_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_NACK;
  app_devices.baro_status = DEV_ERR_NACK;
  app_devices.flash_status = w25q64_init(&app_devices.flash, fake_w25q64_bus(), fake_delay_ms);
  TEST_ASSERT_EQUAL(DEV_OK, app_devices.flash_status);
  memset(flash, 0, sizeof(flash));
  flash_n = 0U;
  SmartLock_EventsInit();
  SmartLock_CameraStubReset();
  for (unsigned i = 0U; i < sizeof(frame_buf); i++)
  {
    frame_buf[i] = (uint8_t)(i ^ 0x5AU);
  }
}

void tearDown(void)
{
  TEST_ASSERT_EQUAL_UINT(0U, fake_w25q64_protocol_errors());
}

/* ---- E2E-01 / FR-02, FR-03, FR-04: B1 з дребезгом → 15 с → LOCK за зачинених дверей --- */
static void b1_bounce_then_15s_lock(uint32_t t0)
{
  uint32_t t_press;
  uint32_t t_acc;
  const flash_rec_t *locked;

  boot(t0, true);
  fake_platform.servo_set_calls = 0U;   /* лише команди сценарію */
  align_to_poll();
  t_press = now();
  /* дребезг 45 мс, утримання, дребезг відпускання: це одне натискання */
  t_acc = press_b1(true);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_FsmIgnored());   /* жодної другої LOCK_REQUEST від дребезгу */
  TEST_ASSERT_TRUE(AppTasks_Latency(0)->st.count >= 1U);   /* I04: фронт B1 пройшов через EXTI */

  /* повторні натискання B1 у EXIT_DELAY не перезапускають відлік (E2E-01) */
  run_for(5000U);
  press(PLATFORM_IN_LOCK);
  press_bouncy(PLATFORM_IN_LOCK);
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_FsmIgnored());   /* два натискання — дві проігноровані */
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS - (now() - t_acc), SmartLock_FsmExitDelayRemainingMs(now()));
  run_to(t_acc + SL_EXIT_DELAY_MS - 1U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(0U, servo_count(TBD_SERVO_LOCK_US));   /* до кінця відліку — UNLOCK */
  run_for(2U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(1U, servo_count(TBD_SERVO_LOCK_US));
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS, servo_first_ms(TBD_SERVO_LOCK_US) - t_acc);   /* ±1 прохід = 1 мс */

  /* від першого дотику контакту: не менше антидребезгу 20 мс і не більше
   * останнього відскоку + антидребезгу 20 мс + фази опитування 10 мс + прохід */
  TEST_ASSERT_TRUE((servo_first_ms(TBD_SERVO_LOCK_US) - t_press) >= SL_EXIT_DELAY_MS + TBD_BTN_DEBOUNCE_MS);
  TEST_ASSERT_TRUE((servo_first_ms(TBD_SERVO_LOCK_US) - t_press) <=
                   SL_EXIT_DELAY_MS + BOUNCE_LAST_MS + TBD_BTN_DEBOUNCE_MS + (TBD_PERIOD_BUTTONS_US / 1000U) + 1U);

  run_for(20U);
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_LOCK_ACTIVATED));   /* одне натискання = одна команда */
  locked = find(SL_LOG_LOCKED, 0U);
  TEST_ASSERT_NOT_NULL(locked);
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS, locked->arg);         /* фактична затримка в журналі */
  expect_no_losses();
}

static void test_b1_with_bounce_locks_after_15s_with_door_closed(void)
{
  b1_bounce_then_15s_lock(T0_MS);
}

static void test_b1_15s_across_millis_overflow(void)
{
  b1_bounce_then_15s_lock(0xFFFFFFFFU - 8000U);   /* Platform_Millis() переповнюється посеред відліку */
}

/* ---- E2E-04 / FR-13: двері відчинені після відліку → LOCK лише після закриття -------- */
static void test_open_door_after_countdown_locks_only_after_close(void)
{
  uint32_t t_acc;
  uint32_t t_close;

  boot(T0_MS, false);
  fake_platform.servo_set_calls = 0U;
  t_acc = press_b1(false);
  run_for(SL_EXIT_DELAY_MS + 1000U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_TRUE(SmartLock_FsmWaitingDoor());
  TEST_ASSERT_EQUAL_UINT(0U, servo_count(TBD_SERVO_LOCK_US));

  /* короткий нестабільний контакт геркона (50 мс < антидребезгу) — не закриття */
  door(true);
  run_for(50U);
  door(false);
  run_for(1000U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(0U, servo_count(TBD_SERVO_LOCK_US));

  /* стабільне закриття — LOCK після антидребезгу 100 мс, без другого відліку 15 с */
  door(true);
  t_close = now();
  run_for(TBD_DOOR_DEBOUNCE_MS - 1U);
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  run_for(40U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_UINT32_WITHIN(21U, TBD_DOOR_DEBOUNCE_MS + 20U, servo_first_ms(TBD_SERVO_LOCK_US) - t_close);
  run_for(20U);
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_LOCK_ACTIVATED));
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_DOOR_CLOSED));
  TEST_ASSERT_TRUE(find(SL_LOG_LOCKED, 0U)->arg > (t_close - t_acc));   /* реальна затримка > 15 с */
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_FORCED_OPEN));
  expect_no_losses();
}

/* ---- E2E-03 / FR-10, FR-11: правильний код → UNLOCK, ACCESS_GRANTED, без автоблокування */
static void test_correct_code_unlocks_without_relock(void)
{
  unsigned lock_cmds;

  boot(T0_MS, true);
  lock_with_b1();
  lock_cmds = servo_count(TBD_SERVO_LOCK_US);
  enter_code(code_ok, TBD_CODE_LEN, true);   /* з дребезгом: жодного зайвого символу */
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_UNLOCK_US, fake_platform.servo_pulse_us);
  run_for(20U);
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_ACCESS_GRANTED));
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_UNLOCKED));
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_WRONG_CODE));
  TEST_ASSERT_TRUE(find(SL_LOG_UNLOCKED, 0U)->seq > find(SL_LOG_ACCESS_GRANTED, 0U)->seq);

  /* FR-11 / PRD D.2: 5 хв без B1 — UNLOCKED, нових команд LOCK немає */
  run_for(5U * 60U * 1000U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(lock_cmds, servo_count(TBD_SERVO_LOCK_US));
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_LOCK_ACTIVATED));
  expect_no_losses();
}

/* ---- E2E-03 / FR-10: неправильний код → WRONG_CODE, LOCKED ------------------------------ */
static void test_wrong_code_keeps_locked(void)
{
  unsigned servo_calls;

  boot(T0_MS, true);
  lock_with_b1();
  servo_calls = fake_platform.servo_set_calls;
  enter_wrong_code();
  run_for(20U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(servo_calls, fake_platform.servo_set_calls);   /* засувку ніхто не чіпав */
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, fake_platform.servo_pulse_us);
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_WRONG_CODE));
  TEST_ASSERT_EQUAL_UINT32(TBD_CODE_LEN, find(SL_LOG_WRONG_CODE, 0U)->arg);
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_ACCESS_GRANTED));

  /* неповний код і пауза > TBD_CODE_TIMEOUT_MS — введення скидається без події */
  enter_code(code_ok, TBD_CODE_LEN - 1U, false);
  run_for(TBD_CODE_TIMEOUT_MS + 100U);
  enter_code(&code_ok[TBD_CODE_LEN - 1U], 1U, false);
  run_for(20U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_WRONG_CODE));
  TEST_ASSERT_EQUAL_UINT(1U, SmartLock_ButtonsCodeProgress());
  expect_no_losses();
}

/* ---- E2E-02 / FR-05..08: PIR (EXTI) + MPU одночасно в LOCKED → два записи, два кадри,
 *      зв'язок подія ↔ кадр у W25Q64; I04: pir_trig → pir_handled → alarm_triggered ---- */
static uint64_t evt_t_us(const char *o, const char *name)
{
  char key[48];
  const char *p;
  const char *line;

  (void)snprintf(key, sizeof(key), " name=%s\r\n", name);
  p = strstr(o, key);
  if (p == NULL) { return 0U; }
  line = p;
  while ((line > o) && (line[-1] != '\n')) { line--; }
  TEST_ASSERT_EQUAL_INT(0, strncmp(line, "EVT t_us=", 9));
  return strtoull(line + 9, NULL, 10);
}

static void test_pir_and_mpu_together_give_two_records_and_two_linked_frames(void)
{
  flash_rec_t motion;
  flash_rec_t tamper;
  const flash_rec_t *ph;
  frame_header_t h;
  static uint8_t rd[sizeof(frame_buf)];
  const char *o;
  uint64_t t_trig;
  uint64_t t_handled;
  uint64_t t_alarm;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  SmartLock_CameraStubSetAccept(true);
  boot(T0_MS, true);
  lock_with_b1();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraStubCalls());

  /* в одну мілісекунду: рух перед PIR (фронт ще й як EXTI) і удар +0,5 g по Z */
  set_level(PLATFORM_IN_PIR, 1);
  fake_sensors_mpu_set_accel(0, 0, (int16_t)(MPU6050_ACCEL_LSB_PER_G + (MPU6050_ACCEL_LSB_PER_G / 2)));
  o = run_captured(60U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());

  /* I04 / metrics-spec §3.3: пара pir_trig → alarm_triggered у шкалі t_us МК */
  t_trig = evt_t_us(o, "pir_trig");
  t_handled = evt_t_us(o, "pir_handled");
  t_alarm = evt_t_us(o, "alarm_triggered");
  TEST_ASSERT_TRUE(t_trig > 0U);
  TEST_ASSERT_TRUE(t_handled >= t_trig);
  TEST_ASSERT_TRUE(t_alarm >= t_handled);
  TEST_ASSERT_TRUE((t_alarm - t_trig) <= (uint64_t)TBD_PERIOD_SECURITY_US + 2000U);   /* опитування + прохід fsm */
  TEST_ASSERT_EQUAL_UINT32(1U, AppTasks_Latency(1)->st.count);                       /* STAT pir_lat */

  TEST_ASSERT_NOT_NULL(find(SL_LOG_MOTION, 0U));
  motion = *find(SL_LOG_MOTION, 0U);
  TEST_ASSERT_NOT_NULL(find(SL_LOG_TAMPER, 0U));
  tamper = *find(SL_LOG_TAMPER, 0U);
  TEST_ASSERT_EQUAL_UINT32(500U - TBD_TAMPER_THRESHOLD_MG, tamper.arg);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_CameraStubCalls());   /* по одному кадру на подію */
  TEST_ASSERT_EQUAL_UINT32(tamper.seq, SmartLock_CameraStubLastId());
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_CameraLinkPending());

  /* «камера» (I09) зберегла обидва кадри через frames.c; тайм-аут уже нічого не додає */
  TEST_ASSERT_EQUAL(DEV_OK, SmartLock_CameraStoreFrame(motion.seq, FRAME_FORMAT_Y8, 40U, 30U,
                                                       frame_buf, sizeof(frame_buf)));
  TEST_ASSERT_EQUAL(DEV_OK, SmartLock_CameraStoreFrame(tamper.seq, FRAME_FORMAT_Y8, 40U, 30U,
                                                       frame_buf, sizeof(frame_buf)));
  set_level(PLATFORM_IN_PIR, 0);
  run_for(TBD_CAM_RESULT_TIMEOUT_MS + 1000U);
  TEST_ASSERT_EQUAL_UINT(2U, count_type(SL_LOG_PHOTO));
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_PHOTO_ERR));
  ph = find(SL_LOG_PHOTO, 0U);
  TEST_ASSERT_EQUAL_UINT32(motion.seq, ph->arg);             /* PHOTO_SAVED.arg = seq події */
  TEST_ASSERT_EQUAL(DEV_OK, FrameStore_GetOps()->read_frame(ph->frame, &h, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_UINT32(motion.seq, h.event_seq);         /* заголовок кадру → та сама подія */
  TEST_ASSERT_EQUAL_MEMORY(frame_buf, rd, sizeof(rd));
  ph = find(SL_LOG_PHOTO, 1U);
  TEST_ASSERT_EQUAL_UINT32(tamper.seq, ph->arg);
  TEST_ASSERT_EQUAL(DEV_OK, FrameStore_GetOps()->read_frame(ph->frame, &h, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_UINT32(tamper.seq, h.event_seq);
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_MOTION));
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_TAMPER));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  expect_no_losses();
}

/* ---- FR-13: відкриття геркона в LOCKED → FORCED_OPEN без фото ------------------------- */
static void test_door_open_in_locked_is_forced_open_without_photo(void)
{
  boot(T0_MS, true);
  SmartLock_CameraStubSetAccept(true);
  lock_with_b1();

  /* короткий відскок дверей (50 мс) — не відкриття */
  door(false);
  run_for(50U);
  door(true);
  run_for(500U);
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_FORCED_OPEN));

  door(false);
  run_for(300U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, fake_platform.servo_pulse_us);
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_FORCED_OPEN));
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_UNLOCKED));
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_DOOR_OPEN));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraStubCalls());
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_PHOTO) + count_type(SL_LOG_PHOTO_ERR));

  /* після авторизованого розблокування відкриття дверей — не FORCED_OPEN */
  door(true);
  run_for(300U);
  enter_code(code_ok, TBD_CODE_LEN, false);
  door(false);
  run_for(300U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_FORCED_OPEN));
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_DOOR_OPEN));
  expect_no_losses();
}

/* ---- FR-14, FR-15, FR-06: відмова DHT11 / BMP180 / MPU не змінює стан засувки --------- */
static void test_sensor_failures_do_not_move_the_latch(void)
{
  unsigned servo_calls;
  unsigned records;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_bmp_attach());
  fake_platform_dht_values(45U, 22U);
  boot(T0_MS, true);
  run_for(3000U);
  TEST_ASSERT_TRUE(SmartLock_EnvLastPressure(NULL));
  TEST_ASSERT_TRUE(SmartLock_EnvLastDht(NULL, NULL));

  /* усі три зникають: DHT11 не відповідає, MPU-6050 і BMP180 зникли з шини */
  fake_platform.dht_status = DEV_ERR_TIMEOUT;
  fake_i2c_reset();
  servo_calls = fake_platform.servo_set_calls;
  records = flash_count();
  run_for(10000U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());   /* у UNLOCKED — без LOCK */
  TEST_ASSERT_EQUAL_UINT(servo_calls, fake_platform.servo_set_calls);
  TEST_ASSERT_EQUAL_UINT(records, flash_count());
  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());
  TEST_ASSERT_TRUE(SmartLock_EnvDhtErrors() > 0U);
  TEST_ASSERT_TRUE(SmartLock_EnvBmpErrors() > 0U);

  /* охорона працює без датчиків моніторингу; у LOCKED відмови теж не дають UNLOCK */
  lock_with_b1();
  servo_calls = fake_platform.servo_set_calls;
  records = flash_count();
  run_for(30000U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(servo_calls, fake_platform.servo_set_calls);
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, fake_platform.servo_pulse_us);
  TEST_ASSERT_EQUAL_UINT(records, flash_count());   /* ні ENV_WARNING, ні TAMPER */
  expect_no_losses();
}

/* ---- FR-15: BMP180 вимірюється і друкується, але записів у W25Q64 не створює --------- */
static void test_bmp180_creates_no_flash_records(void)
{
  unsigned records;
  const char *o;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_bmp_attach());
  fake_platform_dht_values(45U, 22U);
  boot(T0_MS, true);
  lock_with_b1();
  records = flash_count();

  fake_sensors_bmp_set_raw(FAKE_BMP180_DS_UT, FAKE_BMP180_DS_UP + 8000);   /* різка зміна тиску */
  o = run_captured(10U * (TBD_PERIOD_ENV_US / 1000U));
  TEST_ASSERT_NOT_NULL(strstr(o, "bmp=ok"));                               /* вимір у UART є */
  TEST_ASSERT_NULL(strstr(o, "EVT,id="));                                   /* у журнал — нічого */
  TEST_ASSERT_EQUAL_UINT(records, flash_count());
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  expect_no_losses();
}

/* ---- FR-14: тривале перевищення DHT11 → ENV_WARNING; коротке — ні -------------------- */
static void test_sustained_dht11_excess_gives_env_warning(void)
{
  uint32_t t_out;
  const flash_rec_t *w;

  fake_platform_dht_values(45U, 22U);
  boot(T0_MS, true);
  lock_with_b1();

  /* коротке перевищення (30 с < TBD_ENV_WARN_DURATION_MS) */
  fake_platform_dht_values(45U, (uint8_t)(TBD_ENV_T_MAX_C + 5));
  run_for(30000U);
  fake_platform_dht_values(45U, 22U);
  run_for(5000U);
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_ENV_WARNING));

  /* тривале перевищення — рівно одне ENV_WARNING з маскою T, стан і засувка ті самі */
  fake_platform_dht_values(45U, (uint8_t)(TBD_ENV_T_MAX_C + 5));
  t_out = now();
  run_for(TBD_ENV_WARN_DURATION_MS + 10000U);
  TEST_ASSERT_EQUAL_UINT(1U, count_type(SL_LOG_ENV_WARNING));
  w = find(SL_LOG_ENV_WARNING, 0U);
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, w->arg);
  /* зразок раз на TBD_PERIOD_ENV_US: подія не раніше тривалості і не пізніше +2 періодів */
  TEST_ASSERT_TRUE((w->t_ms - t_out) >= TBD_ENV_WARN_DURATION_MS);
  TEST_ASSERT_TRUE((w->t_ms - t_out) <= TBD_ENV_WARN_DURATION_MS + 2U * (TBD_PERIOD_ENV_US / 1000U));
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, fake_platform.servo_pulse_us);
  expect_no_losses();
}

/* Увесь UART сценарію: рядки EVT,id= друкує storage у тих самих проходах */
static char   s_uart[1 << 20];
static size_t s_uart_used;

static void uart_add(const char *o)
{
  size_t l = strlen(o);

  TEST_ASSERT_TRUE(s_uart_used + l < sizeof(s_uart));
  memcpy(&s_uart[s_uart_used], o, l + 1U);
  s_uart_used += l;
}

static void act_press_lock(void)
{
  press(PLATFORM_IN_LOCK);
}

static void act_code_ok(void)
{
  enter_code(code_ok, TBD_CODE_LEN, false);
}

/* ---- K07 / FR-08, FR-12, E2E-06: усі прикладні події → W25Q64 у порядку, з тими самими
 *      ID, що в UART; після рестарту нумерація продовжується ------------------------------ */
static void test_every_app_event_reaches_w25q64_with_uart_ids_and_survives_restart(void)
{
  static const sl_log_type_t expect[] =
  {
    SL_LOG_DOOR_CLOSED,                                  /* стан дверей на старті */
    SL_LOG_LOCK_ACTIVATED, SL_LOG_LOCKED,
    SL_LOG_WRONG_CODE,
    SL_LOG_MOTION, SL_LOG_PHOTO,                         /* кадр збережено */
    SL_LOG_TAMPER, SL_LOG_PHOTO_ERR,                     /* результату немає — тайм-аут */
    SL_LOG_FORCED_OPEN, SL_LOG_ENV_WARNING,
    SL_LOG_DOOR_CLOSED,                                  /* двері зачинили назад, ще LOCKED */
    SL_LOG_ACCESS_GRANTED, SL_LOG_UNLOCKED,
    SL_LOG_DOOR_OPEN, SL_LOG_DOOR_CLOSED,
  };
  const unsigned n_expect = (unsigned)(sizeof(expect) / sizeof(expect[0]));
  const char *p;
  unsigned n_lines = 0U;
  uint32_t last_seq;
  uint32_t motion_seq;
  uint32_t tamper_seq;
  const flash_rec_t *r;

#define UART_RUN(ms) uart_add(run_captured(ms))
#define UART_DO(fn)  uart_add(capture(fn))

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_bmp_attach());   /* BMP180 працює весь сценарій */
  fake_platform_dht_values(45U, 22U);
  SmartLock_CameraStubSetAccept(true);
  s_uart[0] = '\0';
  s_uart_used = 0U;
  boot(T0_MS, true);                                      /* DOOR_CLOSED — у run_for() boot() */

  UART_DO(act_press_lock);
  UART_RUN(SL_EXIT_DELAY_MS + 50U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  UART_DO(enter_wrong_code);
  UART_RUN(50U);

  set_level(PLATFORM_IN_PIR, 1);                          /* MOTION → кадр */
  UART_RUN(60U);
  set_level(PLATFORM_IN_PIR, 0);
  motion_seq = find(SL_LOG_MOTION, 0U)->seq;
  TEST_ASSERT_EQUAL(DEV_OK, SmartLock_CameraStoreFrame(motion_seq, FRAME_FORMAT_Y8, 40U, 30U,
                                                       frame_buf, sizeof(frame_buf)));
  UART_RUN(50U);

  fake_sensors_mpu_set_accel(0, 0, (int16_t)(MPU6050_ACCEL_LSB_PER_G + (MPU6050_ACCEL_LSB_PER_G / 2)));
  UART_RUN(60U);                                          /* TAMPER, камера мовчить */
  fake_sensors_mpu_set_accel(0, 0, (int16_t)MPU6050_ACCEL_LSB_PER_G);
  tamper_seq = find(SL_LOG_TAMPER, 0U)->seq;
  UART_RUN(TBD_CAM_RESULT_TIMEOUT_MS + 100U);             /* PHOTO_ERR (TIMEOUT) */

  door(false);                                            /* без коду — FORCED_OPEN */
  UART_RUN(300U);
  fake_platform_dht_values(45U, (uint8_t)(TBD_ENV_T_MAX_C + 5));
  UART_RUN(TBD_ENV_WARN_DURATION_MS + 2U * (TBD_PERIOD_ENV_US / 1000U));
  fake_platform_dht_values(45U, 22U);
  door(true);
  UART_RUN(300U);
  UART_DO(act_code_ok);
  UART_RUN(50U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  door(false);
  UART_RUN(300U);
  door(true);
  UART_RUN(300U);
#undef UART_RUN
#undef UART_DO

  /* W25Q64: типи в порядку, seq лише зростає, поля PHOTO / PHOTO_ERR — за flash-layout 3.2 */
  flash_read();
  TEST_ASSERT_EQUAL_UINT(1U, flash_boots);
  TEST_ASSERT_EQUAL_UINT(0U, flash_damaged);
  TEST_ASSERT_EQUAL_UINT(n_expect, flash_n);
  for (unsigned i = 0U; i < n_expect; i++)
  {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(Storage_FlashType(expect[i]), flash[i].type, SmartLock_LogTypeName(expect[i]));
    TEST_ASSERT_EQUAL_UINT16(1U, flash[i].boot);
    if (i > 0U) { TEST_ASSERT_TRUE(flash[i].seq > flash[i - 1U].seq); }
  }
  r = find(SL_LOG_PHOTO, 0U);
  TEST_ASSERT_EQUAL_UINT32(motion_seq, r->arg);
  TEST_ASSERT_EQUAL_UINT32(1U, r->frame);
  r = find(SL_LOG_PHOTO_ERR, 0U);
  TEST_ASSERT_EQUAL_UINT32(tamper_seq, r->arg);
  TEST_ASSERT_EQUAL_UINT32(SL_CAM_ERR_TIMEOUT, r->arg2);
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_NO_FRAME, r->frame);
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, find(SL_LOG_ENV_WARNING, 0U)->arg);

  /* UART: кожен рядок EVT,id= — той самий seq і тип, що й запис у Flash (після першого
   * DOOR_CLOSED, надрукованого до перехоплення), і flash=ok */
  for (p = strstr(s_uart, "EVT,id="); p != NULL; p = strstr(p + 1, "EVT,id="))
  {
    unsigned long id = strtoul(p + 7, NULL, 10);
    const char *name = strstr(p, ",name=") + 6;
    size_t len = strcspn(name, ",");
    sl_log_type_t t = type_by_name(name, len);

    n_lines++;
    TEST_ASSERT_TRUE(n_lines < n_expect);
    TEST_ASSERT_EQUAL_UINT32(flash[n_lines].seq, (uint32_t)id);
    TEST_ASSERT_EQUAL_HEX8(flash[n_lines].type, Storage_FlashType(t));
    TEST_ASSERT_EQUAL_INT(0, strncmp(strchr(p, '\r') - 9, ",flash=ok", 9));
  }
  TEST_ASSERT_EQUAL_UINT(n_expect - 1U, n_lines);
  TEST_ASSERT_NULL(strstr(s_uart, "LOG,ERR"));
  expect_no_losses();
  last_seq = flash[n_expect - 1U].seq;

  /* «рестарт»: скидання МК, повторне монтування. BOOT = last + 1, нумерація далі:
   * початковий стан дверей (DOOR_CLOSED) = last + 2, LOCK_ACTIVATED = last + 3 */
  boot(T0_MS, true);
  flash_read();
  TEST_ASSERT_EQUAL_UINT(2U, flash_boots);
  TEST_ASSERT_EQUAL_UINT(n_expect + 1U, flash_n);         /* старі записи на місці + DOOR_CLOSED */
  TEST_ASSERT_EQUAL_UINT32(last_seq + 2U, flash[n_expect].seq);
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_DOOR_CLOSED, flash[n_expect].type);
  TEST_ASSERT_EQUAL_UINT16(2U, flash[n_expect].boot);
  press(PLATFORM_IN_LOCK);
  run_for(50U);
  r = find(SL_LOG_LOCK_ACTIVATED, 1U);
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_UINT32(last_seq + 3U, r->seq);        /* без дублів і без повернення до 1 */
  TEST_ASSERT_EQUAL_UINT16(2U, r->boot);
  TEST_ASSERT_EQUAL_UINT32(last_seq + 3U, SmartLock_JournalLastSeq());
}

/* ---- K07: відмова W25Q64 не зупиняє ні замок, ні UART-журнал -------------------------- */
static void test_flash_failure_keeps_lock_and_uart_journal_running(void)
{
  const char *o;
  uint32_t t_acc;

  boot(T0_MS, true);
  fake_platform.servo_set_calls = 0U;
  fake_w25q64_fail_transfers(1000000U, DEV_ERR_BUS);     /* W25Q64 перестала відповідати */
  t_acc = press_b1(false);
  o = run_captured(SL_EXIT_DELAY_MS + 50U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT32(SL_EXIT_DELAY_MS, servo_first_ms(TBD_SERVO_LOCK_US) - t_acc);   /* час не зсунувся */
  TEST_ASSERT_NOT_NULL(strstr(o, ",name=LOCKED,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "flash=err\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,ERR,op=append"));
  TEST_ASSERT_NULL(strstr(o, "flash=ok"));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());   /* черга не застрягла */
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_JournalDropped());
  TEST_ASSERT_TRUE(SmartLock_JournalUnsaved() >= 2U);

  /* W25Q64 ожила — наступні записи знову зберігаються, ID ідуть далі */
  fake_w25q64_fail_transfers(0U, DEV_OK);
  o = capture(enter_wrong_code);
  TEST_ASSERT_NOT_NULL(strstr(o, ",name=WRONG_CODE,"));
  TEST_ASSERT_NOT_NULL(strstr(o, ",flash=ok\r\n"));
  TEST_ASSERT_NOT_NULL(find(SL_LOG_WRONG_CODE, 0U));
  TEST_ASSERT_EQUAL_UINT32(SmartLock_JournalLastSeq(), find(SL_LOG_WRONG_CODE, 0U)->seq);
}

/* ---- FR-08 (частина без плати) / FR-12: 100 подій повного циклу без втрат -------------- */
static void test_hundred_events_full_cycle_without_losses(void)
{
  static const sl_log_type_t cycle[] = {
    SL_LOG_LOCK_ACTIVATED, SL_LOG_LOCKED, SL_LOG_WRONG_CODE, SL_LOG_ACCESS_GRANTED, SL_LOG_UNLOCKED,
  };
  const unsigned per_cycle = sizeof(cycle) / sizeof(cycle[0]);
  const unsigned cycles = 100U / per_cycle;

  boot(T0_MS, true);
  TEST_ASSERT_EQUAL_UINT(1U, flash_count());   /* DOOR_CLOSED старту */
  for (unsigned c = 0U; c < cycles; c++)
  {
    lock_with_b1();
    enter_wrong_code();
    enter_code(code_ok, TBD_CODE_LEN, false);
    TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  }
  run_for(100U);
  flash_read();
  TEST_ASSERT_EQUAL_UINT(1U + 100U, flash_n);
  TEST_ASSERT_EQUAL_UINT(1U, flash_boots);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT32(flash_n, SmartLock_JournalStored());
  for (unsigned i = 0U; i < flash_n; i++)
  {
    TEST_ASSERT_EQUAL_UINT32(i + 2U, flash[i].seq);   /* BOOT = 1, далі без пропусків і дублів */
    if (i > 0U)
    {
      TEST_ASSERT_TRUE((int32_t)(flash[i].t_ms - flash[i - 1U].t_ms) >= 0);   /* порядок у часі */
      TEST_ASSERT_EQUAL_HEX8(Storage_FlashType(cycle[(i - 1U) % per_cycle]), flash[i].type);
    }
  }
  TEST_ASSERT_EQUAL_UINT(cycles, count_type(SL_LOG_LOCKED));
  TEST_ASSERT_EQUAL_UINT(cycles, count_type(SL_LOG_ACCESS_GRANTED));
  expect_no_losses();
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_b1_with_bounce_locks_after_15s_with_door_closed);
  RUN_TEST(test_b1_15s_across_millis_overflow);
  RUN_TEST(test_open_door_after_countdown_locks_only_after_close);
  RUN_TEST(test_correct_code_unlocks_without_relock);
  RUN_TEST(test_wrong_code_keeps_locked);
  RUN_TEST(test_pir_and_mpu_together_give_two_records_and_two_linked_frames);
  RUN_TEST(test_door_open_in_locked_is_forced_open_without_photo);
  RUN_TEST(test_sensor_failures_do_not_move_the_latch);
  RUN_TEST(test_bmp180_creates_no_flash_records);
  RUN_TEST(test_sustained_dht11_excess_gives_env_warning);
  RUN_TEST(test_every_app_event_reaches_w25q64_with_uart_ids_and_survives_restart);
  RUN_TEST(test_flash_failure_keeps_lock_and_uart_journal_running);
  RUN_TEST(test_hundred_events_full_cycle_without_losses);
  return UNITY_END();
}
