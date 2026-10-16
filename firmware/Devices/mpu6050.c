/**
  ******************************************************************************
  * @file    mpu6050.c
  * @brief   Драйвер MPU-6050: ідентифікація, ініціалізація, читання, одиниці.
  ******************************************************************************
  */
#include "mpu6050.h"

static dev_status_t write_reg(mpu6050_t *dev, uint8_t reg, uint8_t value)
{
  const uint8_t buf[2] = { reg, value };

  return dev->bus->write(dev->bus->ctx, dev->addr7, buf, sizeof(buf));
}

dev_status_t mpu6050_init(mpu6050_t *dev, const dev_i2c_bus_t *bus, uint8_t addr7)
{
  /* Після скидання датчик спить (SLEEP = 1), діапазони ±2 g / ±250 °/s */
  static const uint8_t init_seq[][2] =
  {
    { MPU6050_REG_PWR_MGMT_1,   MPU6050_PWR_CLK_PLL_XGYRO },
    { MPU6050_REG_GYRO_CONFIG,  0x00U },   /* FS_SEL = 0: ±250 °/s */
    { MPU6050_REG_ACCEL_CONFIG, 0x00U },   /* AFS_SEL = 0: ±2 g */
  };
  uint8_t who = 0U;
  dev_status_t st;

  if ((dev == NULL) || (bus == NULL))
  {
    return DEV_ERR_PARAM;
  }
  dev->bus   = bus;
  dev->addr7 = addr7;

  st = bus->read_reg(bus->ctx, addr7, MPU6050_REG_WHO_AM_I, &who, 1U);
  if (st != DEV_OK)
  {
    return st;
  }
  if (who != MPU6050_WHO_AM_I_VALUE)
  {
    return DEV_ERR_ID;
  }

  for (size_t i = 0U; i < sizeof(init_seq) / sizeof(init_seq[0]); i++)
  {
    st = write_reg(dev, init_seq[i][0], init_seq[i][1]);
    if (st != DEV_OK)
    {
      return st;
    }
  }
  return DEV_OK;
}

void mpu6050_parse(const uint8_t buf[MPU6050_SAMPLE_LEN], mpu6050_raw_t *raw)
{
  int16_t v[MPU6050_SAMPLE_LEN / 2U];

  for (size_t i = 0U; i < MPU6050_SAMPLE_LEN / 2U; i++)
  {
    v[i] = (int16_t)(((uint16_t)buf[2U * i] << 8) | buf[2U * i + 1U]);
  }
  raw->ax   = v[0];
  raw->ay   = v[1];
  raw->az   = v[2];
  raw->temp = v[3];
  raw->gx   = v[4];
  raw->gy   = v[5];
  raw->gz   = v[6];
}

dev_status_t mpu6050_read_raw(mpu6050_t *dev, mpu6050_raw_t *raw)
{
  uint8_t buf[MPU6050_SAMPLE_LEN];
  dev_status_t st = dev->bus->read_reg(dev->bus->ctx, dev->addr7, MPU6050_REG_ACCEL_XOUT_H,
                                       buf, sizeof(buf));

  if (st == DEV_OK)
  {
    mpu6050_parse(buf, raw);
  }
  return st;
}

void mpu6050_convert(const mpu6050_raw_t *raw, mpu6050_data_t *out)
{
  out->ax_mg = (int32_t)raw->ax * 1000 / MPU6050_ACCEL_LSB_PER_G;
  out->ay_mg = (int32_t)raw->ay * 1000 / MPU6050_ACCEL_LSB_PER_G;
  out->az_mg = (int32_t)raw->az * 1000 / MPU6050_ACCEL_LSB_PER_G;

  /* Register Map, розд. 4.18: T(°C) = raw / 340 + 36.53 */
  out->temp_cdeg = (int32_t)raw->temp * 100 / 340 + 3653;

  out->gx_mdps = (int32_t)raw->gx * 1000 / MPU6050_GYRO_LSB_PER_DPS;
  out->gy_mdps = (int32_t)raw->gy * 1000 / MPU6050_GYRO_LSB_PER_DPS;
  out->gz_mdps = (int32_t)raw->gz * 1000 / MPU6050_GYRO_LSB_PER_DPS;
}

int mpu6050_is_at_rest(const mpu6050_data_t *d, uint32_t tol_mg)
{
  /* Порівнюємо квадрати, щоб обійтися без sqrt: |a|² ∈ [(1000 - tol)², (1000 + tol)²] */
  int64_t mag2 = (int64_t)d->ax_mg * d->ax_mg + (int64_t)d->ay_mg * d->ay_mg +
                 (int64_t)d->az_mg * d->az_mg;
  int64_t lo   = 1000 - (int64_t)tol_mg;
  int64_t hi   = 1000 + (int64_t)tol_mg;

  if (lo < 0)
  {
    lo = 0;
  }
  return (mag2 >= lo * lo) && (mag2 <= hi * hi);
}
