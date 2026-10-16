/**
  * @file    test_mpu6050.c
  * @brief   MPU-6050: ідентифікація, ініціалізація, розбір і перетворення
  *          даних, помилки шини, вектор із запису логічного аналізатора.
  */
#include <string.h>

#include "unity.h"
#include "capture_vectors.h"
#include "fake_i2c.h"
#include "mpu6050.h"

static uint8_t  *regs;
static mpu6050_t dev;

void setUp(void)
{
  fake_i2c_reset();
  regs = fake_i2c_add_device(MPU6050_I2C_ADDR);
  regs[MPU6050_REG_WHO_AM_I]  = MPU6050_WHO_AM_I_VALUE;
  regs[MPU6050_REG_PWR_MGMT_1] = 0x40U;  /* значення після скидання: SLEEP = 1 */
}

void tearDown(void)
{
}

/* ---- Ідентифікація та ініціалізація ------------------------------------- */

static void test_init_checks_who_am_i_and_configures(void)
{
  TEST_ASSERT_EQUAL(DEV_OK, mpu6050_init(&dev, fake_i2c_bus(), MPU6050_I2C_ADDR));

  /* SLEEP знято, тактування від PLL, діапазони ±250 °/s і ±2 g */
  TEST_ASSERT_EQUAL_HEX8(0x01U, regs[MPU6050_REG_PWR_MGMT_1]);
  TEST_ASSERT_EQUAL_HEX8(0x00U, regs[MPU6050_REG_GYRO_CONFIG]);
  TEST_ASSERT_EQUAL_HEX8(0x00U, regs[MPU6050_REG_ACCEL_CONFIG]);

  /* Першою транзакцією має бути саме читання WHO_AM_I: D0 75 D1 68 */
  const fake_i2c_xfer_t *x = fake_i2c_log(0);
  static const uint8_t who_am_i_wire[] = { 0xD0, 0x75, 0xD1, 0x68 };
  TEST_ASSERT_EQUAL_size_t(sizeof(who_am_i_wire), x->wire_len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(who_am_i_wire, x->wire, sizeof(who_am_i_wire));
  TEST_ASSERT_EQUAL_size_t(4U, fake_i2c_log_count());
}

static void test_init_wrong_who_am_i(void)
{
  regs[MPU6050_REG_WHO_AM_I] = 0x70U;  /* MPU-6500 на модулі, підписаному як MPU-6050 */

  TEST_ASSERT_EQUAL(DEV_ERR_ID, mpu6050_init(&dev, fake_i2c_bus(), MPU6050_I2C_ADDR));
  /* Конфігурацію в чужий чип не пишемо */
  TEST_ASSERT_EQUAL_size_t(1U, fake_i2c_log_count());
}

static void test_init_wrong_address_nack(void)
{
  /* AD0 = 1 на модулі, а прошивка звертається до 0x69 (або навпаки) */
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, mpu6050_init(&dev, fake_i2c_bus(), 0x69U));
}

static void test_init_config_write_timeout(void)
{
  fake_i2c_fail_at(2U, DEV_ERR_TIMEOUT);  /* збій на записі GYRO_CONFIG */

  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, mpu6050_init(&dev, fake_i2c_bus(), MPU6050_I2C_ADDR));
  TEST_ASSERT_EQUAL_size_t(3U, fake_i2c_log_count());
}

static void test_init_null_params(void)
{
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, mpu6050_init(NULL, fake_i2c_bus(), MPU6050_I2C_ADDR));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, mpu6050_init(&dev, NULL, MPU6050_I2C_ADDR));
}

/* Вектор із реального запису ЛР1: модуль не підключений, на шині лише
 * адресний байт 0xD0 і NAK. Драйвер має відправити той самий байт і
 * повернути DEV_ERR_NACK, а не зависнути чи прочитати сміття. */
static void test_capture_lr1_probe_without_module(void)
{
  fake_i2c_reset();

  TEST_ASSERT_EQUAL(DEV_ERR_NACK, mpu6050_init(&dev, fake_i2c_bus(), MPU6050_I2C_ADDR));

  const fake_i2c_xfer_t *x = fake_i2c_log(0);
  TEST_ASSERT_EQUAL_size_t(sizeof(cap_lr1_mpu6050_probe_wire), x->wire_len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(cap_lr1_mpu6050_probe_wire, x->wire, x->wire_len);
  TEST_ASSERT_EQUAL(CAP_LR1_MPU6050_PROBE_ACKED, x->result == DEV_OK);
}

/* ---- Читання і перетворення -------------------------------------------- */

static void test_read_raw_is_single_14_byte_burst(void)
{
  static const uint8_t sample[MPU6050_SAMPLE_LEN] =
  {
    0x00, 0x10,  0xFF, 0xF0,  0x40, 0x00,   /* ax = 16, ay = -16, az = 16384 */
    0xFD, 0xF7,                             /* temp = -521 */
    0x00, 0x83,  0xFF, 0x7D,  0x00, 0x00,   /* gx = 131, gy = -131, gz = 0 */
  };
  mpu6050_raw_t raw;

  memcpy(&regs[MPU6050_REG_ACCEL_XOUT_H], sample, sizeof(sample));
  TEST_ASSERT_EQUAL(DEV_OK, mpu6050_init(&dev, fake_i2c_bus(), MPU6050_I2C_ADDR));
  TEST_ASSERT_EQUAL(DEV_OK, mpu6050_read_raw(&dev, &raw));

  TEST_ASSERT_EQUAL_INT16(16, raw.ax);
  TEST_ASSERT_EQUAL_INT16(-16, raw.ay);
  TEST_ASSERT_EQUAL_INT16(16384, raw.az);
  TEST_ASSERT_EQUAL_INT16(-521, raw.temp);
  TEST_ASSERT_EQUAL_INT16(131, raw.gx);
  TEST_ASSERT_EQUAL_INT16(-131, raw.gy);
  TEST_ASSERT_EQUAL_INT16(0, raw.gz);

  /* Одна транзакція: D0 3B D1 + 14 байт */
  const fake_i2c_xfer_t *x = fake_i2c_log(4);
  TEST_ASSERT_EQUAL_HEX8(MPU6050_REG_ACCEL_XOUT_H, x->wire[1]);
  TEST_ASSERT_EQUAL_size_t(3U + MPU6050_SAMPLE_LEN, x->wire_len);
}

static void test_read_raw_timeout_leaves_output_untouched(void)
{
  mpu6050_raw_t raw;

  memset(&raw, 0x5A, sizeof(raw));
  TEST_ASSERT_EQUAL(DEV_OK, mpu6050_init(&dev, fake_i2c_bus(), MPU6050_I2C_ADDR));
  fake_i2c_fail_at(4U, DEV_ERR_TIMEOUT);

  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, mpu6050_read_raw(&dev, &raw));
  TEST_ASSERT_EQUAL_HEX16(0x5A5A, (uint16_t)raw.ax);
}

static void test_convert_accel_full_scale_2g(void)
{
  mpu6050_raw_t  raw = { .ax = 16384, .ay = -16384, .az = 32767 };
  mpu6050_data_t d;

  mpu6050_convert(&raw, &d);

  TEST_ASSERT_EQUAL_INT32(1000, d.ax_mg);
  TEST_ASSERT_EQUAL_INT32(-1000, d.ay_mg);
  TEST_ASSERT_EQUAL_INT32(1999, d.az_mg);   /* +2 g недосяжне: максимум 32767 LSB */
}

static void test_convert_temperature(void)
{
  mpu6050_raw_t  raw = { .temp = -521 };
  mpu6050_data_t d;

  mpu6050_convert(&raw, &d);
  TEST_ASSERT_EQUAL_INT32(3500, d.temp_cdeg);   /* -521 / 340 + 36.53 = 35.00 °C */

  raw.temp = 0;
  mpu6050_convert(&raw, &d);
  TEST_ASSERT_EQUAL_INT32(3653, d.temp_cdeg);
}

static void test_convert_gyro_250dps(void)
{
  mpu6050_raw_t  raw = { .gx = 131, .gy = -32768, .gz = 32767 };
  mpu6050_data_t d;

  mpu6050_convert(&raw, &d);

  TEST_ASSERT_EQUAL_INT32(1000, d.gx_mdps);
  TEST_ASSERT_EQUAL_INT32(-250137, d.gy_mdps);
  TEST_ASSERT_EQUAL_INT32(250129, d.gz_mdps);
}

/* ---- Правдоподібність: |a| = 1 g у спокої -------------------------------- */

static void test_at_rest_flat_and_tilted(void)
{
  mpu6050_data_t flat   = { .az_mg = 1000 };
  mpu6050_data_t tilted = { .ax_mg = 707, .az_mg = 707 };   /* 45°: |a| = 999.8 mg */
  mpu6050_data_t edge   = { .ax_mg = 1050 };

  TEST_ASSERT_TRUE(mpu6050_is_at_rest(&flat, 50U));
  TEST_ASSERT_TRUE(mpu6050_is_at_rest(&tilted, 50U));
  TEST_ASSERT_TRUE(mpu6050_is_at_rest(&edge, 50U));
}

static void test_at_rest_rejects_free_fall_and_shock(void)
{
  mpu6050_data_t free_fall = { .az_mg = 20 };
  mpu6050_data_t shock     = { .ax_mg = 900, .az_mg = 1000 };
  mpu6050_data_t edge      = { .ax_mg = 1051 };
  mpu6050_data_t zero      = { 0 };

  TEST_ASSERT_FALSE(mpu6050_is_at_rest(&free_fall, 50U));
  TEST_ASSERT_FALSE(mpu6050_is_at_rest(&shock, 50U));
  TEST_ASSERT_FALSE(mpu6050_is_at_rest(&edge, 50U));
  /* Допуск більший за 1 g: нижня межа обмежується нулем */
  TEST_ASSERT_TRUE(mpu6050_is_at_rest(&zero, 1500U));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_checks_who_am_i_and_configures);
  RUN_TEST(test_init_wrong_who_am_i);
  RUN_TEST(test_init_wrong_address_nack);
  RUN_TEST(test_init_config_write_timeout);
  RUN_TEST(test_init_null_params);
  RUN_TEST(test_capture_lr1_probe_without_module);
  RUN_TEST(test_read_raw_is_single_14_byte_burst);
  RUN_TEST(test_read_raw_timeout_leaves_output_untouched);
  RUN_TEST(test_convert_accel_full_scale_2g);
  RUN_TEST(test_convert_temperature);
  RUN_TEST(test_convert_gyro_250dps);
  RUN_TEST(test_at_rest_flat_and_tilted);
  RUN_TEST(test_at_rest_rejects_free_fall_and_shock);
  return UNITY_END();
}
