/**
  * @file    test_lsm303d.c
  * @brief   LSM303D: ідентифікація, ініціалізація, автоінкремент при читанні,
  *          розбір little-endian, перетворення в міліg, помилки шини.
  *
  *          fake_i2c не моделює біт автоінкременту LSM303D (старший біт адреси
  *          регістру), тому тест кладе відповідь у регістри з адреси 0xA8 і окремо
  *          перевіряє, що драйвер справді надсилає на шину 0xA8, а не 0x28.
  */
#include <string.h>

#include "unity.h"
#include "fake_i2c.h"
#include "lsm303d.h"
#include "capture_vectors.h"

static uint8_t  *regs;
static lsm303d_t dev;

void setUp(void)
{
  fake_i2c_reset();
  regs = fake_i2c_add_device(LSM303D_I2C_ADDR);
  regs[LSM303D_REG_WHO_AM_I] = LSM303D_WHO_AM_I_VALUE;
}

void tearDown(void)
{
}

static void test_init_checks_who_am_i_and_enables_accel(void)
{
  TEST_ASSERT_EQUAL(DEV_OK, lsm303d_init(&dev, fake_i2c_bus(), LSM303D_I2C_ADDR));
  TEST_ASSERT_EQUAL_HEX8(0x5FU, regs[LSM303D_REG_CTRL1]);
  TEST_ASSERT_EQUAL_HEX8(0x00U, regs[LSM303D_REG_CTRL2]);

  /* WHO_AM_I на шині: 3A 0F 3B 49 (адреса 0x1D) */
  static const uint8_t wire[] = { 0x3A, 0x0F, 0x3B, 0x49 };
  const fake_i2c_xfer_t *x = fake_i2c_log(0);
  TEST_ASSERT_EQUAL_size_t(sizeof(wire), x->wire_len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(wire, x->wire, sizeof(wire));
  TEST_ASSERT_EQUAL_size_t(3U, fake_i2c_log_count());
}

static void test_init_wrong_who_am_i(void)
{
  regs[LSM303D_REG_WHO_AM_I] = 0x33U;   /* LSM303DLHC — інший чип і інша карта регістрів */
  TEST_ASSERT_EQUAL(DEV_ERR_ID, lsm303d_init(&dev, fake_i2c_bus(), LSM303D_I2C_ADDR));
  TEST_ASSERT_EQUAL_size_t(1U, fake_i2c_log_count());
}

static void test_init_nack_on_other_address(void)
{
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, lsm303d_init(&dev, fake_i2c_bus(), LSM303D_I2C_ADDR_ALT));
}

static void test_read_uses_auto_increment_address(void)
{
  TEST_ASSERT_EQUAL(DEV_OK, lsm303d_init(&dev, fake_i2c_bus(), LSM303D_I2C_ADDR));
  size_t before = fake_i2c_log_count();
  lsm303d_raw_t raw;

  TEST_ASSERT_EQUAL(DEV_OK, lsm303d_read_raw(&dev, &raw));
  const fake_i2c_xfer_t *x = fake_i2c_log(before);
  TEST_ASSERT_EQUAL_HEX8(0x3A, x->wire[0]);
  TEST_ASSERT_EQUAL_HEX8(0xA8, x->wire[1]);   /* 0x28 | 0x80 */
  TEST_ASSERT_EQUAL_HEX8(0x3B, x->wire[2]);
  TEST_ASSERT_EQUAL_size_t(3U + LSM303D_SAMPLE_LEN, x->wire_len);
}

static void test_parse_little_endian_and_convert_1g_on_z(void)
{
  /* Модуль лежить горизонтально: X ≈ 0, Y ≈ 0, Z ≈ +1 g = 16393 LSB (0x4009) */
  const uint8_t buf[6] = { 0x10, 0x00, 0xF0, 0xFF, 0x09, 0x40 };
  lsm303d_raw_t raw;
  lsm303d_data_t d;

  lsm303d_parse(buf, &raw);
  TEST_ASSERT_EQUAL_INT16(16, raw.ax);
  TEST_ASSERT_EQUAL_INT16(-16, raw.ay);
  TEST_ASSERT_EQUAL_INT16(16393, raw.az);

  lsm303d_convert(&raw, &d);
  TEST_ASSERT_INT32_WITHIN(1, 0, d.ax_mg);
  TEST_ASSERT_INT32_WITHIN(1, 0, d.ay_mg);
  TEST_ASSERT_INT32_WITHIN(2, 1000, d.az_mg);
}

static void test_convert_full_scale_negative(void)
{
  const lsm303d_raw_t raw = { -32768, 0, 32767 };
  lsm303d_data_t d;

  lsm303d_convert(&raw, &d);
  TEST_ASSERT_EQUAL_INT32(-1998, d.ax_mg);   /* −32768 × 0,061 = −1998,8 → до нуля */
  TEST_ASSERT_EQUAL_INT32(1998, d.az_mg);
}

static void test_read_bus_error_propagates(void)
{
  TEST_ASSERT_EQUAL(DEV_OK, lsm303d_init(&dev, fake_i2c_bus(), LSM303D_I2C_ADDR));
  lsm303d_raw_t raw = { 1, 2, 3 };

  fake_i2c_fail_at(fake_i2c_log_count(), DEV_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, lsm303d_read_raw(&dev, &raw));
  TEST_ASSERT_EQUAL_INT16(1, raw.ax);   /* при помилці вихід не змінюється */
}

static void test_init_bus_error_on_config(void)
{
  fake_i2c_fail_at(1U, DEV_ERR_NACK);
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, lsm303d_init(&dev, fake_i2c_bus(), LSM303D_I2C_ADDR));
}

/* Вектор з uart_selftest.sr (546.987 мс): 3A A8 3B F8 7F F8 7F CD 33.
 * UART того самого прогону: raw=7FF8,7FF8,33CD, ax_g=1.998, ay_g=1.998, az_g=0.808
 * (осі X/Y «залипли» майже на +full scale — тому self-test дав WARN). */
static void test_capture_selftest_sample(void)
{
  lsm303d_raw_t raw;
  lsm303d_data_t d;

  TEST_ASSERT_EQUAL(DEV_OK, lsm303d_init(&dev, fake_i2c_bus(), LSM303D_I2C_ADDR));
  memcpy(&regs[LSM303D_REG_OUT_X_L_A | LSM303D_AUTO_INC], &cap_st_lsm303d_xyz_wire[CAP_ST_LSM303D_XYZ_OFFSET],
         LSM303D_SAMPLE_LEN);
  size_t before = fake_i2c_log_count();

  TEST_ASSERT_EQUAL(DEV_OK, lsm303d_read_raw(&dev, &raw));
  const fake_i2c_xfer_t *x = fake_i2c_log(before);
  TEST_ASSERT_EQUAL_size_t(sizeof(cap_st_lsm303d_xyz_wire), x->wire_len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(cap_st_lsm303d_xyz_wire, x->wire, sizeof(cap_st_lsm303d_xyz_wire));

  TEST_ASSERT_EQUAL_HEX16(0x7FF8, (uint16_t)raw.ax);
  TEST_ASSERT_EQUAL_HEX16(0x7FF8, (uint16_t)raw.ay);
  TEST_ASSERT_EQUAL_HEX16(0x33CD, (uint16_t)raw.az);
  lsm303d_convert(&raw, &d);
  TEST_ASSERT_EQUAL_INT32(1998, d.ax_mg);   /* 32760 × 0,061 = 1998,4 */
  TEST_ASSERT_EQUAL_INT32(1998, d.ay_mg);
  TEST_ASSERT_EQUAL_INT32(808, d.az_mg);    /* 13261 × 0,061 = 808,9 → до нуля */
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_checks_who_am_i_and_enables_accel);
  RUN_TEST(test_init_wrong_who_am_i);
  RUN_TEST(test_init_nack_on_other_address);
  RUN_TEST(test_read_uses_auto_increment_address);
  RUN_TEST(test_parse_little_endian_and_convert_1g_on_z);
  RUN_TEST(test_convert_full_scale_negative);
  RUN_TEST(test_read_bus_error_propagates);
  RUN_TEST(test_init_bus_error_on_config);
  RUN_TEST(test_capture_selftest_sample);
  return UNITY_END();
}
