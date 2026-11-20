/**
  * @file    test_hooks_weak.c
  * @brief   ЛР3 (I02): слабкі точки підключення (smartlock_hooks_weak.c) без
  *          сильних модулів.
  *
  *          У прошивці й решті тестів App/ слабкі хуки заміщені модулями sl_*.c
  *          (K02–K08). Цей бінарник лінкується без sl_*.o (tests/Makefile,
  *          HOOKS_WEAK_SRCS), тож лінкер бере слабкі реалізації — так перевіряється
  *          каркас I02, яким він є до злиття гілок: навантаження на шини і
  *          проходження події від хука до «FSM» через чергу.
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
#include "platform.h"
#include "sched.h"
#include "smartlock_hooks.h"

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_NACK;
  app_devices.baro_status = DEV_ERR_NACK;
  Perf_Init();
  Sched_Init();
  SmartLock_EventsInit();
}

void tearDown(void)
{
}

static void test_weak_hooks(void)
{
  TEST_ASSERT_TRUE(SmartLock_ButtonsPoll(0U, true));         /* фронт B1 = LOCK_REQUEST без антидребезгу */
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(0U, true, false)); /* фронт PIR = MOTION */
  TEST_ASSERT_EQUAL_UINT(2U, SmartLock_EventsPending());
  TEST_ASSERT_TRUE(SmartLock_FsmStep(0U));                   /* слабка FSM лише вибирає чергу */
  TEST_ASSERT_FALSE(SmartLock_FsmStep(0U));
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_FALSE(SmartLock_StoragePoll(0U));
  TEST_ASSERT_TRUE(SmartLock_ButtonsPoll(0U, false));        /* без фронту — лише читання входів */
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());

  /* MPU-6050 «є» — security звертається до шини (14 байт) */
  app_devices.mpu_status = DEV_OK;
  app_devices.mpu.bus = Platform_SensorI2c();
  app_devices.mpu.addr7 = MPU6050_I2C_ADDR;
  TEST_ASSERT_TRUE(SmartLock_SecurityPoll(0U, false, false));
  TEST_ASSERT_TRUE(fake_i2c_log_count() > 0U);

  /* DHT11 відповідає — ENV,dht=ok; BMP180 «є», але не відповідає — bmp=err */
  fake_platform.dht_status = DEV_OK;
  for (unsigned i = 0U; i < 40U; i++)
  {
    fake_platform.dht_high_us[i] = 24U;   /* усі біти 0: контрольна сума 0 збігається */
  }
  app_devices.baro_status = DEV_OK;
  app_devices.baro.bus = Platform_SensorI2c();
  app_devices.baro.delay_ms = Platform_DelayMs;
  TEST_ASSERT_TRUE(SmartLock_EnvPoll(0U));
  fake_platform.dht_status = DEV_ERR_TIMEOUT;
  app_devices.baro_status = DEV_ERR_NACK;
  TEST_ASSERT_TRUE(SmartLock_EnvPoll(0U));
}

/* Через справжній планувальник: B1 → черга → слабка FSM у наступному проході */
static void test_weak_hooks_through_super_loop(void)
{
  AppTasks_Init();
  TEST_ASSERT_TRUE(SmartLock_PostEvent(SL_EV_LOCK_REQUEST, 0U));
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT(0U, SmartLock_EventsPending());
  TEST_ASSERT_EQUAL_UINT32(1U, Sched_Task(0)->exec.count);   /* fsm мала роботу */
  TEST_ASSERT_EQUAL_UINT32(0U, fake_platform.dht_calls);     /* env — лише коли настане період */
  fake_platform_advance_ms(TBD_PERIOD_ENV_US / 1000U);
  Sched_RunOnce();
  TEST_ASSERT_EQUAL_UINT32(1U, fake_platform.dht_calls);     /* env (слабкий) читав DHT11 */
}

/* I10: без модулів sl_* STATS повідомляє «DIAG name=modules present=0» */
static void test_weak_diag(void)
{
  static char buf[128];
  FILE *tmp = tmpfile();
  int saved;
  size_t n;

  TEST_ASSERT_NOT_NULL(tmp);
  fflush(stdout);
  saved = dup(1);
  dup2(fileno(tmp), 1);
  SmartLock_PrintDiag();
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  rewind(tmp);
  n = fread(buf, 1U, sizeof(buf) - 1U, tmp);
  buf[n] = '\0';
  fclose(tmp);
  TEST_ASSERT_EQUAL_STRING("DIAG name=modules present=0\r\n", buf);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_weak_hooks);
  RUN_TEST(test_weak_hooks_through_super_loop);
  RUN_TEST(test_weak_diag);
  return UNITY_END();
}
