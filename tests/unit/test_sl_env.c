/**
  * @file    test_sl_env.c
  * @brief   ЛР3 (K06): DHT11 / BMP180, ENV_WARNING — sl_env.c без плати.
  *
  *          Збирається з усім App/ і fake_platform (tests/Makefile, APP_TESTS).
  *          Окремий запуск: make -C tests run-sl_env
  *
  *          Залізо: fake_platform (now_ms, dht_script / dht_values), fake_sensors
  *          (BMP180 через app_devices), fake_i2c (помилки шини). Події
  *          перевіряються через SmartLock_TakeEvent(), рядок UART — перехопленням
  *          stdout. Числа меж і тривалості беруться з TBD_ENV_* (sl_env.h): тест
  *          перевіряє ЛОГІКУ, а не конкретні тимчасові значення.
  */
#include <string.h>
#include <unistd.h>

#include "unity.h"

#include "app.h"
#include "app_config.h"
#include "event_queue.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_sensors.h"
#include "sl_env.h"
#include "smartlock_hooks.h"

#define DUR   TBD_ENV_WARN_DURATION_MS
#define T_HI  TBD_ENV_T_MAX_C
#define T_LO  TBD_ENV_T_MIN_C
#define RH_HI TBD_ENV_RH_MAX_PCT
#define RH_LO TBD_ENV_RH_MIN_PCT
#define T_OK  ((T_HI + T_LO) / 2)
#define RH_OK ((RH_HI + RH_LO) / 2)

/* ---- перехоплення stdout (між початком і кінцем не можна TEST_ASSERT) ----------- */
static char out[256];

static const char *poll_at(uint32_t now)
{
  FILE *tmp = tmpfile();
  int saved;
  size_t n;

  out[0] = '\0';
  if (tmp == NULL) { return out; }
  fflush(stdout);
  saved = dup(1);
  dup2(fileno(tmp), 1);
  (void)SmartLock_EnvPoll(now);
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  rewind(tmp);
  n = fread(out, 1U, sizeof(out) - 1U, tmp);
  out[n] = '\0';
  fclose(tmp);
  return out;
}

static void set_dht(int rh, int t)
{
  fake_platform_dht_values((uint8_t)rh, (uint8_t)t);
}

static void dht_fail(void)
{
  fake_platform.dht_status = DEV_ERR_TIMEOUT;
}

/* Усі події з черги: скільки їх і остання (для перевірки маски) */
static unsigned drain(uint32_t *last_arg)
{
  sl_event_t ev;
  unsigned n = 0U;

  while (SmartLock_TakeEvent(&ev))
  {
    TEST_ASSERT_EQUAL_INT(SL_EV_ENV_WARNING, ev.type);   /* модуль не постить нічого іншого */
    if (last_arg != NULL) { *last_arg = ev.arg; }
    n++;
  }
  return n;
}

/* Опитування з кроком TBD_PERIOD_ENV_US у [from, to] включно */
static void run(uint32_t from, uint32_t to)
{
  for (uint32_t t = from; t <= to; t += TBD_PERIOD_ENV_US / 1000U)
  {
    (void)poll_at(t);
  }
}

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_STATE;
  app_devices.baro_status = DEV_ERR_STATE;
  SmartLock_EventsInit();
  SmartLock_EnvInit();
  set_dht(RH_OK, T_OK);
}

void tearDown(void)
{
}

static void test_init_posts_nothing(void)
{
  SmartLock_EnvInit();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_FALSE(SmartLock_EnvLastDht(NULL, NULL));
  TEST_ASSERT_FALSE(SmartLock_EnvLastPressure(NULL));
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvActiveMask());
}

/* ---- UART і норма -------------------------------------------------------------- */
static void test_normal_line_without_baro(void)
{
  const uint8_t frame[5] = { 45U, 0U, 25U, 7U, 77U };   /* 25,7 °C, 45 % */

  fake_platform_dht_frame(frame);
  TEST_ASSERT_EQUAL_STRING("ENV,dht=ok,t_c=25.7,rh=45,bmp=err\r\n", poll_at(1000U));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

static void test_normal_line_with_baro_has_pressure_only(void)
{
  int32_t p = 0;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_bmp_attach());
  set_dht(45, 25);
  TEST_ASSERT_EQUAL_STRING("ENV,dht=ok,t_c=25.0,rh=45,bmp=ok,p_pa=69964\r\n", poll_at(0U));
  /* температура BMP180 (15,0 °C) окремо не виводиться, t_c — лише одна (DHT11) */
  TEST_ASSERT_NULL(strstr(out, "15.0"));
  TEST_ASSERT_NULL(strstr(strstr(out, "t_c=") + 1, "t_c="));
  TEST_ASSERT_TRUE(SmartLock_EnvLastPressure(&p));
  TEST_ASSERT_EQUAL_INT32(FAKE_BMP180_DS_P_PA, p);

  /* будь-який тиск подій і тривог не дає */
  fake_sensors_bmp_set_raw(FAKE_BMP180_DS_UT, FAKE_BMP180_DS_UP + 4000);
  run(2000U, 3U * DUR);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvWarningsPosted());
}

static void test_negative_temperature_and_getters(void)
{
  const uint8_t frame[5] = { 50U, 0U, 3U, 0x85U, (uint8_t)(50U + 3U + 0x85U) };   /* -3,5 °C: знак — біт 7 байта 3 */
  int16_t t_dc = 0;
  uint8_t rh = 0U;

  fake_platform_dht_frame(frame);
  TEST_ASSERT_EQUAL_STRING("ENV,dht=ok,t_c=-3.5,rh=50,bmp=err\r\n", poll_at(0U));
  TEST_ASSERT_TRUE(SmartLock_EnvLastDht(&t_dc, &rh));
  TEST_ASSERT_EQUAL_INT16(-35, t_dc);
  TEST_ASSERT_EQUAL_UINT8(50U, rh);
}

/* ---- перевищення T ------------------------------------------------------------- */
static void test_short_excursion_no_event(void)
{
  set_dht(RH_OK, T_HI + 5);
  poll_at(0U);
  poll_at(DUR - 1U);
  set_dht(RH_OK, T_OK);          /* повернення в норму до кінця тривалості */
  poll_at(DUR);
  set_dht(RH_OK, T_HI + 5);
  poll_at(DUR + 1000U);          /* нове перевищення — відлік з нуля */
  poll_at(DUR + 1000U + DUR - 1U);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

static void test_sustained_posts_exactly_one_event_with_mask(void)
{
  uint32_t arg = 0U;

  set_dht(RH_OK, T_HI + 3);
  poll_at(1000U);
  poll_at(1000U + DUR - 1U);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());   /* на 1 мс раніше — ще ні */
  poll_at(1000U + DUR);                                    /* рівно тривалість */
  run(1000U + DUR + 2000U, 1000U + 4U * DUR);              /* далі подій немає */
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, arg);
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, SmartLock_EnvActiveMask());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvWarningsPosted());
}

static void test_return_to_normal_then_second_excursion(void)
{
  uint32_t arg = 0U;

  set_dht(RH_OK, T_LO - 2);          /* нижня межа */
  run(0U, DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, arg);

  set_dht(RH_OK, T_OK);
  run(DUR + 2000U, DUR + 10000U);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvActiveMask());

  set_dht(RH_OK, T_LO - 2);          /* нове перевищення: подія лише після повної тривалості */
  poll_at(200000U);
  poll_at(200000U + DUR - 1U);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  poll_at(200000U + DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, arg);
}

/* ---- гістерезис ---------------------------------------------------------------- */
static void test_value_on_boundary_never_starts_countdown(void)
{
  set_dht(RH_OK, T_HI);             /* рівно на межі: ще норма */
  run(0U, 3U * DUR);
  set_dht(RH_HI, T_OK);   /* RH рівно на верхній межі */
  run(3U * DUR + 2000U, 6U * DUR);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

static void test_hysteresis_band_keeps_countdown_and_blocks_repeat(void)
{
  uint32_t arg = 0U;

  set_dht(RH_OK, T_HI + 1);
  poll_at(0U);
  set_dht(RH_OK, T_HI);              /* на межі, але в смузі гістерезису: відлік тече */
  run(2000U, DUR - 2000U);
  poll_at(DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));

  /* шум на межі після події: 41, 40, 41, 40 ... — жодної нової події */
  for (uint32_t t = DUR + 2000U; t < DUR + 200000U; t += 4000U)
  {
    set_dht(RH_OK, T_HI + 1);
    poll_at(t);
    set_dht(RH_OK, T_HI);
    poll_at(t + 2000U);
  }
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());

  /* смуга не відпускає: T_HI - hyst + 1 ще «поза» */
  set_dht(RH_OK, T_HI - TBD_ENV_T_HYST_C + 1);
  poll_at(300000U);
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, SmartLock_EnvActiveMask());
  /* вийшов углиб на гістерезис — норма */
  set_dht(RH_OK, T_HI - TBD_ENV_T_HYST_C);
  poll_at(302000U);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvActiveMask());
}

static void test_hysteresis_low_side(void)
{
  uint32_t arg = 0U;

  set_dht(RH_OK, T_LO - 1);
  poll_at(0U);
  set_dht(RH_OK, T_LO + TBD_ENV_T_HYST_C - 1);   /* ще в смузі гістерезису */
  poll_at(DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  set_dht(RH_OK, T_LO + TBD_ENV_T_HYST_C);       /* глибина гістерезису — норма */
  poll_at(DUR + 2000U);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvActiveMask());
}

/* ---- T і RH окремо і разом ------------------------------------------------------- */
static void test_rh_alone_high_and_low(void)
{
  uint32_t arg = 0U;

  set_dht(RH_HI + 5, T_OK);
  run(0U, DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_RH, arg);

  set_dht(RH_OK, T_OK);
  poll_at(DUR + 2000U);
  set_dht(RH_LO - 5, T_OK);
  run(DUR + 4000U, 2U * DUR + 4000U);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_RH, arg);
}

static void test_t_and_rh_together_one_event(void)
{
  uint32_t arg = 0U;

  set_dht(RH_HI + 5, T_HI + 5);
  run(0U, 2U * DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T | SL_ENV_WARN_RH, arg);
}

/* Зміна складу під час активної тривоги (sl_env.h, п. 5) */
static void test_mask_change_during_active_alarm(void)
{
  uint32_t arg = 0U;

  set_dht(RH_OK, T_HI + 5);
  run(0U, DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, arg);

  /* RH доєднався, але ще не дозрів — події немає */
  set_dht(RH_HI + 5, T_HI + 5);
  poll_at(DUR + 2000U);
  poll_at(DUR + 2000U + DUR - 1U);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  /* дозрів: ще одна подія з ПОВНОЮ маскою, лише одна */
  poll_at(DUR + 2000U + DUR);
  run(2U * DUR + 4000U, 3U * DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T | SL_ENV_WARN_RH, arg);

  /* RH повернувся, T лишається: події немає, T не оголошується вдруге */
  set_dht(RH_OK, T_HI + 5);
  run(3U * DUR + 2000U, 5U * DUR);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, SmartLock_EnvActiveMask());

  /* RH знову виходить і дозріває — нова подія з T|RH */
  set_dht(RH_HI + 5, T_HI + 5);
  poll_at(6U * DUR);
  poll_at(7U * DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T | SL_ENV_WARN_RH, arg);

  /* лишився лише RH, T повернулась: зменшення складу — без події */
  set_dht(RH_HI + 5, T_OK);
  run(7U * DUR + 2000U, 9U * DUR);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_RH, SmartLock_EnvActiveMask());
}

/* ---- відмови DHT11 ----------------------------------------------------------------- */
static void test_dht_error_at_start(void)
{
  dht_fail();
  TEST_ASSERT_EQUAL_STRING("ENV,dht=err,bmp=err\r\n", poll_at(0U));
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvDhtErrors());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvDhtErrStreak());
  TEST_ASSERT_FALSE(SmartLock_EnvLastDht(NULL, NULL));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());

  set_dht(RH_OK, T_OK);               /* відновлення: серія скидається */
  poll_at(2000U);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvDhtErrStreak());
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvDhtErrors());
  TEST_ASSERT_TRUE(SmartLock_EnvLastDht(NULL, NULL));
}

static void test_dht_checksum_and_range_errors(void)
{
  const uint8_t bad_sum[5] = { 45U, 0U, 25U, 0U, 1U };
  const uint8_t bad_rng[5] = { 101U, 0U, 25U, 0U, 126U };

  fake_platform_dht_frame(bad_sum);
  TEST_ASSERT_EQUAL_STRING("ENV,dht=err,bmp=err\r\n", poll_at(0U));
  fake_platform_dht_frame(bad_rng);
  TEST_ASSERT_EQUAL_STRING("ENV,dht=err,bmp=err\r\n", poll_at(2000U));
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_EnvDhtErrors());
}

static void test_short_error_mid_excursion_countdown_continues(void)
{
  uint32_t arg = 0U;

  set_dht(RH_OK, T_HI + 5);
  poll_at(0U);
  dht_fail();                         /* поодинокі помилки: не «норма» і не скидання */
  poll_at(20000U);
  poll_at(22000U);
  set_dht(RH_OK, T_HI + 5);
  poll_at(24000U);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvDhtErrStreak());

  dht_fail();                         /* помилка саме в момент дозрівання — події ще немає */
  poll_at(DUR);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  set_dht(RH_OK, T_HI + 5);           /* перший валідний зразок — подія */
  poll_at(DUR + 2000U);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, arg);

  /* помилка після оголошення не дає дубля */
  dht_fail();
  poll_at(DUR + 4000U);
  set_dht(RH_OK, T_HI + 5);
  run(DUR + 6000U, 3U * DUR);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

static void test_long_failure_resets_countdown(void)
{
  uint32_t arg = 0U;
  uint32_t t;

  set_dht(RH_OK, T_HI + 5);
  poll_at(0U);
  dht_fail();
  for (unsigned i = 0U; i < TBD_ENV_DHT_ERR_RESET_COUNT; i++)
  {
    poll_at(10000U + 2000U * i);   /* серія досягає порога скидання */
  }
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvActiveMask());

  set_dht(RH_OK, T_HI + 5);           /* датчик повернувся: відлік з нуля від 30000 */
  t = 30000U;
  poll_at(t);
  poll_at(DUR);                       /* без скидання подія була б уже тут */
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  poll_at(t + DUR - 1U);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  poll_at(t + DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));

  /* тривала відмова після оголошення скидає і «оголошені»: нове перевищення — нова подія */
  dht_fail();
  for (unsigned i = 0U; i < TBD_ENV_DHT_ERR_RESET_COUNT; i++)
  {
    poll_at(t + DUR + 2000U + 2000U * i);
  }
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvActiveMask());
  set_dht(RH_OK, T_HI + 5);
  run(100000U, 100000U + DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
}

static void test_permanent_failure_never_posts(void)
{
  dht_fail();
  run(0U, 5U * DUR);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_TRUE(SmartLock_EnvDhtErrors() > 100U);
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvWarningsPosted());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvActiveMask());
}

/* ---- відмови BMP180 ------------------------------------------------------------------ */
static void test_bmp_not_ok_status_gives_bmp_err(void)
{
  app_devices.baro_status = DEV_ERR_ID;   /* ініціалізація не вдалась */
  TEST_ASSERT_EQUAL_STRING("ENV,dht=ok,t_c=30.0,rh=50,bmp=err\r\n", (set_dht(50, 30), poll_at(0U)));
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvBmpErrors());
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

static void test_bmp_bus_failure_after_init_keeps_last_pressure(void)
{
  int32_t p = 0;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_bmp_attach());
  TEST_ASSERT_TRUE(strstr(poll_at(0U), "bmp=ok,p_pa=") != NULL);

  fake_i2c_reset();                    /* модуль «зник» — baro_status ще DEV_OK */
  TEST_ASSERT_EQUAL_STRING("ENV,dht=ok,t_c=30.0,rh=50,bmp=err\r\n", (set_dht(50, 30), poll_at(2000U)));
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvBmpErrors());
  TEST_ASSERT_TRUE(SmartLock_EnvLastPressure(&p));   /* останнє валідне збережене */
  TEST_ASSERT_EQUAL_INT32(FAKE_BMP180_DS_P_PA, p);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

static void test_bmp_failure_does_not_disturb_dht_countdown(void)
{
  uint32_t arg = 0U;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_bmp_attach());
  fake_i2c_reset();
  set_dht(RH_OK, T_HI + 5);
  run(0U, DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, arg);
}

/* ---- черга ---------------------------------------------------------------------------- */
static void test_full_queue_retries_next_poll(void)
{
  sl_event_t ev;
  uint32_t   arg = 0U;

  for (unsigned i = 0U; i < TBD_EVQ_LEN; i++)
  {
    TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_LOCK_REQUEST, 0U));
  }
  set_dht(RH_OK, T_HI + 5);
  run(0U, DUR + 2000U);                   /* дозрів на DUR; спроби на DUR і DUR+2000 невдалі */
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_EnvPostFailed());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvWarningsPosted());
  TEST_ASSERT_EQUAL_UINT32(0U, SmartLock_EnvActiveMask());   /* не оголошено — повтор буде */

  TEST_ASSERT_TRUE(SmartLock_TakeEvent(&ev));   /* звільнили місце */
  poll_at(DUR + 4000U);
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvWarningsPosted());
  poll_at(DUR + 6000U);                   /* повторів немає */
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_EnvWarningsPosted());

  for (unsigned i = 0U; i < TBD_EVQ_LEN - 1U; i++)
  {
    TEST_ASSERT_TRUE(SmartLock_TakeEvent(&ev));
    TEST_ASSERT_EQUAL_INT(SL_EV_LOCK_REQUEST, ev.type);
  }
  TEST_ASSERT_TRUE(SmartLock_TakeEvent(&ev));
  TEST_ASSERT_EQUAL_INT(SL_EV_ENV_WARNING, ev.type);
  arg = ev.arg;
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, arg);
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
}

/* ---- переповнення now_ms ----------------------------------------------------------------- */
static void test_now_ms_wraparound(void)
{
  uint32_t arg = 0U;
  uint32_t t0 = 0U - (DUR / 2U);          /* відлік стартує за DUR/2 до переповнення */

  set_dht(RH_OK, T_HI + 5);
  poll_at(t0);
  poll_at(t0 + DUR - 1U);                 /* вже після переповнення, на 1 мс раніше */
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  poll_at(t0 + DUR);
  TEST_ASSERT_EQUAL_UINT(1U, drain(&arg));
  TEST_ASSERT_EQUAL_UINT32(SL_ENV_WARN_T, arg);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_posts_nothing);
  RUN_TEST(test_normal_line_without_baro);
  RUN_TEST(test_normal_line_with_baro_has_pressure_only);
  RUN_TEST(test_negative_temperature_and_getters);
  RUN_TEST(test_short_excursion_no_event);
  RUN_TEST(test_sustained_posts_exactly_one_event_with_mask);
  RUN_TEST(test_return_to_normal_then_second_excursion);
  RUN_TEST(test_value_on_boundary_never_starts_countdown);
  RUN_TEST(test_hysteresis_band_keeps_countdown_and_blocks_repeat);
  RUN_TEST(test_hysteresis_low_side);
  RUN_TEST(test_rh_alone_high_and_low);
  RUN_TEST(test_t_and_rh_together_one_event);
  RUN_TEST(test_mask_change_during_active_alarm);
  RUN_TEST(test_dht_error_at_start);
  RUN_TEST(test_dht_checksum_and_range_errors);
  RUN_TEST(test_short_error_mid_excursion_countdown_continues);
  RUN_TEST(test_long_failure_resets_countdown);
  RUN_TEST(test_permanent_failure_never_posts);
  RUN_TEST(test_bmp_not_ok_status_gives_bmp_err);
  RUN_TEST(test_bmp_bus_failure_after_init_keeps_last_pressure);
  RUN_TEST(test_bmp_failure_does_not_disturb_dht_countdown);
  RUN_TEST(test_full_queue_retries_next_poll);
  RUN_TEST(test_now_ms_wraparound);
  return UNITY_END();
}
