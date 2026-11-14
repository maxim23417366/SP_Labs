/**
  * @file    test_sl_integration.c
  * @brief   ЛР3 (K09, Коваль): наскрізні сценарії SmartLock через справжній
  *          Super Loop без плати.
  *
  *          Усе працює разом, як на платі: AppTasks_Init() + Sched_RunOnce() по
  *          одному проходу на мілісекунду, справжні sl_buttons / sl_security /
  *          sl_env / sl_fsm / sl_journal / sl_camera_link. Тест керує лише
  *          «фізикою» через fake_platform (рівні кнопок, геркона, PIR, кадр DHT11,
  *          час) і fake_sensors (MPU-6050, BMP180 на fake_i2c), а читає виходи:
  *          команди серво (fake_platform.servo_log) і записи, які storage передав
  *          бекенду журналу (сильна SmartLock_JournalOps() цього тесту = «Flash»).
  *          Камера — слабка заглушка sl_camera_link.c (SmartLock_CameraStub*).
  *
  *          Сценарії відповідають docs/lab_3/e2e-test-cases.md (E2E-01…04) і
  *          рядкам FR-02…FR-14 docs/lab_3/fr-verification-matrix.md; апаратні
  *          частини (реальний дребезг, поріг удару, час аналізатором) — на стенді.
  */
#include <string.h>

#include "unity.h"

#include "app.h"
#include "app_config.h"
#include "app_tasks.h"
#include "event_queue.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_sensors.h"
#include "perf.h"
#include "platform.h"
#include "sched.h"
#include "sl_buttons.h"
#include "sl_camera_link.h"
#include "sl_env.h"
#include "sl_fsm.h"
#include "sl_journal.h"
#include "sl_security.h"
#include "smartlock_hooks.h"

/* ---- «Flash»: усе, що storage передав бекенду журналу ------------------------------- */
#define FLASH_LEN 256U
static sl_log_rec_t flash[FLASH_LEN];
static unsigned     flash_n;

static dev_status_t flash_append(const sl_log_rec_t *rec)
{
  if (flash_n < FLASH_LEN)
  {
    flash[flash_n] = *rec;
  }
  flash_n++;
  return DEV_OK;
}

const sl_journal_ops_t *SmartLock_JournalOps(void)
{
  static const sl_journal_ops_t ops = { flash_append, NULL };

  return &ops;
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

/* ---- входи -------------------------------------------------------------------------- */
static void door(bool closed)
{
  fake_platform_set_inputs(PLATFORM_IN_DOOR, closed ? 0 : 1);   /* магніт поруч = 0 */
}

static void hold(uint32_t mask, bool pressed, uint32_t ms)
{
  fake_platform_set_inputs(mask, pressed ? 0 : 1);   /* pull-up: натиснуто = 0 */
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
 * зайву подію / символ. */
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
static unsigned count_type(sl_log_type_t type)
{
  unsigned n = 0U;

  for (unsigned i = 0U; (i < flash_n) && (i < FLASH_LEN); i++)
  {
    if (flash[i].type == (uint8_t)type) { n++; }
  }
  return n;
}

/* n-й (з 0) запис типу type; NULL — немає */
static const sl_log_rec_t *find(sl_log_type_t type, unsigned nth)
{
  for (unsigned i = 0U; (i < flash_n) && (i < FLASH_LEN); i++)
  {
    if ((flash[i].type == (uint8_t)type) && (nth-- == 0U)) { return &flash[i]; }
  }
  return NULL;
}

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
  TEST_ASSERT_TRUE(SmartLock_JournalMaxDepth() < TBD_JOURNAL_FIFO_LEN - TBD_JOURNAL_FIFO_RESERVE);
}

/* ---- старт і блокування ---------------------------------------------------------------- */
static void boot(uint32_t t0, bool door_closed)
{
  fake_platform.now_ms = t0;
  door(door_closed);
  AppTasks_Init();                    /* час, черга, таблиця задач, як App_Init() */
  s_grid_ms = t0;
  run_for(300U);                      /* security фіксує початковий стан дверей */
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL(door_closed ? SL_DOOR_CLOSED : SL_DOOR_OPEN, SmartLock_FsmDoor());
}

/* B1 → чекаємо, поки FSM прийме LOCK_REQUEST; повертає момент входу в EXIT_DELAY */
static uint32_t press_b1(bool bouncy)
{
  if (bouncy) { press_bouncy(PLATFORM_IN_LOCK); } else { press(PLATFORM_IN_LOCK); }
  TEST_ASSERT_EQUAL(SL_STATE_EXIT_DELAY, SmartLock_FsmState());
  /* мітка прийнятого B1, від якої FSM рахує 15 с */
  return now() - (SL_EXIT_DELAY_MS - SmartLock_FsmExitDelayRemainingMs(now()));
}

static void lock_with_b1(void)
{
  (void)press_b1(false);
  run_for(SL_EXIT_DELAY_MS);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  run_for(20U);   /* storage вивантажує LOCK_ACTIVATED, LOCKED */
}

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_NACK;
  app_devices.baro_status = DEV_ERR_NACK;
  memset(flash, 0, sizeof(flash));
  flash_n = 0U;
  SmartLock_JournalInit();
  SmartLock_FsmInit();
  SmartLock_ButtonsInit();
  SmartLock_SecurityInit();
  SmartLock_EnvInit();
  SmartLock_CameraStubReset();
  fake_platform.servo_set_calls = 0U;   /* лише команди сценарію */
}

void tearDown(void)
{
}

/* ---- E2E-01 / FR-02, FR-03, FR-04: B1 з дребезгом → 15 с → LOCK за зачинених дверей --- */
static void b1_bounce_then_15s_lock(uint32_t t0)
{
  uint32_t t_press;
  uint32_t t_acc;
  const sl_log_rec_t *locked;

  boot(t0, true);
  align_to_poll();
  t_press = now();
  /* дребезг 45 мс, утримання, дребезг відпускання: це одне натискання */
  t_acc = press_b1(true);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_FsmIgnored());   /* жодної другої LOCK_REQUEST від дребезгу */

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

/* ---- E2E-02 / FR-05..07: PIR + MPU одночасно в LOCKED → два записи, два кадри --------- */
static void test_pir_and_mpu_together_give_two_records_and_two_photos(void)
{
  const sl_log_rec_t *motion;
  const sl_log_rec_t *tamper;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  SmartLock_CameraStubSetAccept(true);
  boot(T0_MS, true);
  lock_with_b1();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_CameraStubCalls());

  /* в одну мілісекунду: рух перед PIR і удар +0,5 g по Z */
  fake_platform_set_inputs(PLATFORM_IN_PIR, 1);
  fake_sensors_mpu_set_accel(0, 0, (int16_t)(MPU6050_ACCEL_LSB_PER_G + (MPU6050_ACCEL_LSB_PER_G / 2)));
  run_for(60U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  motion = find(SL_LOG_MOTION, 0U);
  tamper = find(SL_LOG_TAMPER, 0U);
  TEST_ASSERT_NOT_NULL(motion);
  TEST_ASSERT_NOT_NULL(tamper);
  TEST_ASSERT_EQUAL_UINT32(500U - TBD_TAMPER_THRESHOLD_MG, tamper->arg);
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_CameraStubCalls());   /* по одному кадру на подію */
  TEST_ASSERT_EQUAL_UINT32(tamper->seq, SmartLock_CameraStubLastId());
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_CameraLinkPending());

  /* камера (I09) повідомляє результати — по одному PHOTO з посиланням на подію */
  SmartLock_OnCameraResult(motion->seq, 11U, SL_CAM_OK);
  SmartLock_OnCameraResult(tamper->seq, 12U, SL_CAM_OK);
  fake_platform_set_inputs(PLATFORM_IN_PIR, 0);
  run_for(TBD_CAM_RESULT_TIMEOUT_MS + 1000U);   /* тайм-аут уже нічого не додає */
  TEST_ASSERT_EQUAL_UINT(2U, count_type(SL_LOG_PHOTO));
  TEST_ASSERT_EQUAL_UINT(0U, count_type(SL_LOG_PHOTO_ERR));
  TEST_ASSERT_EQUAL_UINT32(motion->seq, find(SL_LOG_PHOTO, 0U)->ref);
  TEST_ASSERT_EQUAL_UINT32(11U, find(SL_LOG_PHOTO, 0U)->arg);
  TEST_ASSERT_EQUAL_UINT32(tamper->seq, find(SL_LOG_PHOTO, 1U)->ref);
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
  records = flash_n;
  run_for(10000U);
  TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());   /* у UNLOCKED — без LOCK */
  TEST_ASSERT_EQUAL_UINT(servo_calls, fake_platform.servo_set_calls);
  TEST_ASSERT_EQUAL_UINT(records, flash_n);
  TEST_ASSERT_FALSE(SlSecurity_MpuHealthy());
  TEST_ASSERT_TRUE(SmartLock_EnvDhtErrors() > 0U);
  TEST_ASSERT_TRUE(SmartLock_EnvBmpErrors() > 0U);

  /* охорона працює без датчиків моніторингу; у LOCKED відмови теж не дають UNLOCK */
  lock_with_b1();
  servo_calls = fake_platform.servo_set_calls;
  records = flash_n;
  run_for(30000U);
  TEST_ASSERT_EQUAL(SL_STATE_LOCKED, SmartLock_FsmState());
  TEST_ASSERT_EQUAL_UINT(servo_calls, fake_platform.servo_set_calls);
  TEST_ASSERT_EQUAL_UINT32(TBD_SERVO_LOCK_US, fake_platform.servo_pulse_us);
  TEST_ASSERT_EQUAL_UINT(records, flash_n);                    /* ні ENV_WARNING, ні TAMPER */
  expect_no_losses();
}

/* ---- FR-14: тривале перевищення DHT11 → ENV_WARNING; коротке — ні -------------------- */
static void test_sustained_dht11_excess_gives_env_warning(void)
{
  uint32_t t_out;
  const sl_log_rec_t *w;

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

/* ---- FR-08 (частина без плати) / FR-12: 100 подій повного циклу без втрат -------------- */
static void test_hundred_events_full_cycle_without_losses(void)
{
  static const sl_log_type_t cycle[] = {
    SL_LOG_LOCK_ACTIVATED, SL_LOG_LOCKED, SL_LOG_WRONG_CODE, SL_LOG_ACCESS_GRANTED, SL_LOG_UNLOCKED,
  };
  const unsigned per_cycle = sizeof(cycle) / sizeof(cycle[0]);
  const unsigned cycles = 100U / per_cycle;

  boot(T0_MS, true);
  TEST_ASSERT_EQUAL_UINT(1U, flash_n);   /* DOOR_CLOSED старту */
  for (unsigned c = 0U; c < cycles; c++)
  {
    lock_with_b1();
    enter_wrong_code();
    enter_code(code_ok, TBD_CODE_LEN, false);
    TEST_ASSERT_EQUAL(SL_STATE_UNLOCKED, SmartLock_FsmState());
  }
  run_for(100U);
  TEST_ASSERT_EQUAL_UINT(1U + 100U, flash_n);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_JournalPending());
  TEST_ASSERT_EQUAL_UINT32(flash_n, SmartLock_JournalStored());
  for (unsigned i = 0U; i < flash_n; i++)
  {
    TEST_ASSERT_EQUAL_UINT32(i + 1U, flash[i].seq);                       /* без пропусків і дублів */
    if (i > 0U)
    {
      TEST_ASSERT_TRUE((int32_t)(flash[i].t_ms - flash[i - 1U].t_ms) >= 0);   /* порядок у часі */
      TEST_ASSERT_EQUAL_STRING(SmartLock_LogTypeName(cycle[(i - 1U) % per_cycle]),
                               SmartLock_LogTypeName((sl_log_type_t)flash[i].type));
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
  RUN_TEST(test_pir_and_mpu_together_give_two_records_and_two_photos);
  RUN_TEST(test_door_open_in_locked_is_forced_open_without_photo);
  RUN_TEST(test_sensor_failures_do_not_move_the_latch);
  RUN_TEST(test_sustained_dht11_excess_gives_env_warning);
  RUN_TEST(test_hundred_events_full_cycle_without_losses);
  return UNITY_END();
}
