/**
  * @file    test_fakes_lr3.c
  * @brief   ЛР3: самоперевірка test doubles для модулів K02–K06 — журнал серво,
  *          сценарій DHT11, помічники входів / часу (fake_platform) і датчики
  *          MPU-6050 / BMP180 через app_devices (fake_sensors).
  *
  *          Заодно це приклади використання для тестів test_sl_*.c.
  */
#include <string.h>

#include "unity.h"

#include "app.h"
#include "dht11.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "fake_sensors.h"
#include "platform.h"

void setUp(void)
{
  fake_platform_reset();
  fake_i2c_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.mpu_status  = DEV_ERR_STATE;
  app_devices.baro_status = DEV_ERR_STATE;
}

void tearDown(void)
{
}

static void test_inputs_and_time_helpers(void)
{
  /* після reset: кнопки, B1 і геркон HIGH (відпущено / двері відчинені), PIR 0 */
  TEST_ASSERT_EQUAL_HEX32(0U, Platform_ReadInputs() & PLATFORM_IN_PIR);
  fake_platform_set_inputs(PLATFORM_IN_DOOR | PLATFORM_IN_BTN2, 0);   /* магніт поруч, зелена натиснута */
  fake_platform_set_inputs(PLATFORM_IN_PIR, 1);
  TEST_ASSERT_EQUAL_HEX32(0U, Platform_ReadInputs() & (PLATFORM_IN_DOOR | PLATFORM_IN_BTN2));
  TEST_ASSERT_EQUAL_HEX32(PLATFORM_IN_PIR, Platform_ReadInputs() & PLATFORM_IN_PIR);

  fake_platform.now_ms = 0xFFFFFFF0U;
  fake_platform_advance_ms(0x20U);   /* переповнення Platform_Millis() */
  TEST_ASSERT_EQUAL_UINT32(0x10U, Platform_Millis());
  TEST_ASSERT_EQUAL_UINT32(0U, fake_platform.delay_total_ms);
}

static void test_servo_log_keeps_every_command(void)
{
  fake_platform.now_ms = 100U;
  Platform_ServoSetPulseUs(1000U);
  fake_platform.now_ms = 15100U;
  Platform_ServoSetPulseUs(2000U);
  TEST_ASSERT_EQUAL_UINT32(2U, fake_platform.servo_set_calls);
  TEST_ASSERT_EQUAL_UINT32(1000U, fake_platform.servo_log[0]);
  TEST_ASSERT_EQUAL_UINT32(2000U, fake_platform.servo_log[1]);
  TEST_ASSERT_EQUAL_UINT32(15100U, fake_platform.servo_log_ms[1]);
  TEST_ASSERT_EQUAL_UINT32(2000U, Platform_ServoPulseUs());

  for (unsigned i = 0U; i < FAKE_SERVO_LOG_LEN; i++)
  {
    Platform_ServoSetPulseUs(1500U);   /* понад журнал: лічильник росте, журнал не виходить за межі */
  }
  TEST_ASSERT_EQUAL_UINT32(FAKE_SERVO_LOG_LEN + 2U, fake_platform.servo_set_calls);
}

static void test_dht_frame_and_script(void)
{
  static const fake_dht_step_t script[] =
  {
    { DEV_OK,          { 40U, 0U, 25U, 0U, 65U } },
    { DEV_ERR_TIMEOUT, { 0U } },
    { DEV_OK,          { 90U, 0U, 45U, 0U, 135U } },
  };
  uint16_t us[40];
  uint8_t  f[DHT11_FRAME_LEN];
  dht11_data_t d;

  fake_platform_dht_values(35U, 21U);
  TEST_ASSERT_EQUAL(DEV_OK, Platform_Dht11Capture(us));
  dht11_bits_to_bytes(us, 48U, f);
  TEST_ASSERT_EQUAL(DEV_OK, dht11_decode(f, &d));
  TEST_ASSERT_EQUAL_UINT8(35U, d.rh_pct);
  TEST_ASSERT_EQUAL_INT8(21, d.temp_c);

  fake_platform_dht_script(script, 3U);
  TEST_ASSERT_EQUAL(DEV_OK, Platform_Dht11Capture(us));
  dht11_bits_to_bytes(us, 48U, f);
  TEST_ASSERT_EQUAL_UINT8(25U, f[2]);
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, Platform_Dht11Capture(us));
  TEST_ASSERT_EQUAL(DEV_OK, Platform_Dht11Capture(us));
  TEST_ASSERT_EQUAL(DEV_OK, Platform_Dht11Capture(us));   /* останній крок повторюється */
  dht11_bits_to_bytes(us, 48U, f);
  TEST_ASSERT_EQUAL_UINT8(45U, f[2]);
  TEST_ASSERT_EQUAL_UINT32(5U, fake_platform.dht_calls);

  fake_platform_dht_script(NULL, 0U);   /* назад до статичних полів */
  fake_platform.dht_status = DEV_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, Platform_Dht11Capture(us));
}

static void test_mpu_attach_and_accel(void)
{
  mpu6050_raw_t raw;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_mpu_attach());
  TEST_ASSERT_EQUAL(DEV_OK, mpu6050_read_raw(&app_devices.mpu, &raw));
  TEST_ASSERT_EQUAL_INT16(MPU6050_ACCEL_LSB_PER_G, raw.az);
  fake_sensors_mpu_set_accel(-16384, 8192, 32767);
  TEST_ASSERT_EQUAL(DEV_OK, mpu6050_read_raw(&app_devices.mpu, &raw));
  TEST_ASSERT_EQUAL_INT16(-16384, raw.ax);
  TEST_ASSERT_EQUAL_INT16(8192, raw.ay);
  TEST_ASSERT_EQUAL_INT16(32767, raw.az);

  fake_i2c_reset();   /* модуль «зник» після init */
  fake_sensors_mpu_set_accel(0, 0, 0);   /* без пристрою — нічого не робить */
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, mpu6050_read_raw(&app_devices.mpu, &raw));
}

static void test_bmp_attach_datasheet_pressure(void)
{
  int32_t t_dc = 0;
  int32_t p_pa = 0;

  TEST_ASSERT_EQUAL(DEV_OK, fake_sensors_bmp_attach());
  TEST_ASSERT_EQUAL(DEV_OK, bmp180_read(&app_devices.baro, &t_dc, &p_pa));
  TEST_ASSERT_EQUAL_INT32(150, t_dc);
  TEST_ASSERT_EQUAL_INT32(FAKE_BMP180_DS_P_PA, p_pa);
  TEST_ASSERT_TRUE(fake_platform.now_ms > 0U);   /* перетворення «тривали» через Platform_DelayMs */

  fake_sensors_bmp_set_raw(FAKE_BMP180_DS_UT, FAKE_BMP180_DS_UP + 1000);
  TEST_ASSERT_EQUAL(DEV_OK, bmp180_read(&app_devices.baro, &t_dc, &p_pa));
  TEST_ASSERT_TRUE(p_pa > FAKE_BMP180_DS_P_PA);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_inputs_and_time_helpers);
  RUN_TEST(test_servo_log_keeps_every_command);
  RUN_TEST(test_dht_frame_and_script);
  RUN_TEST(test_mpu_attach_and_accel);
  RUN_TEST(test_bmp_attach_datasheet_pressure);
  return UNITY_END();
}
