/**
  ******************************************************************************
  * @file    mpu6050.h
  * @brief   Драйвер акселерометра/гіроскопа MPU-6050, I2C.
  *
  *          Джерела: InvenSense MPU-6000/MPU-6050 Product Specification
  *          PS-MPU-6000A-00 rev 3.4 (розд. 6.1–6.2: чутливість), Register Map
  *          RM-MPU-6000A-00 rev 4.2 (регістри 0x1B, 0x1C, 0x3B–0x48, 0x6B, 0x75).
  ******************************************************************************
  */
#ifndef MPU6050_H
#define MPU6050_H

#include "dev_bus.h"

#define MPU6050_I2C_ADDR          0x68U   /* AD0 = 0; 0x69 при AD0 = 1 */
#define MPU6050_REG_GYRO_CONFIG   0x1BU
#define MPU6050_REG_ACCEL_CONFIG  0x1CU
#define MPU6050_REG_ACCEL_XOUT_H  0x3BU   /* 0x3B..0x48: accel XYZ, temp, gyro XYZ */
#define MPU6050_REG_PWR_MGMT_1    0x6BU
#define MPU6050_REG_WHO_AM_I      0x75U
#define MPU6050_WHO_AM_I_VALUE    0x68U   /* не залежить від AD0 */
#define MPU6050_PWR_CLK_PLL_XGYRO 0x01U   /* SLEEP = 0, тактування від PLL гіроскопа X */
#define MPU6050_SAMPLE_LEN        14U

/* Діапазони, які налаштовує mpu6050_init(): ±2 g і ±250 °/s */
#define MPU6050_ACCEL_LSB_PER_G   16384
#define MPU6050_GYRO_LSB_PER_DPS  131

typedef struct
{
  int16_t ax, ay, az;
  int16_t temp;
  int16_t gx, gy, gz;
} mpu6050_raw_t;

typedef struct
{
  int32_t ax_mg, ay_mg, az_mg;        /* міліg */
  int32_t temp_cdeg;                  /* 0,01 °C */
  int32_t gx_mdps, gy_mdps, gz_mdps;  /* міліградуси за секунду */
} mpu6050_data_t;

typedef struct
{
  const dev_i2c_bus_t *bus;
  uint8_t              addr7;
} mpu6050_t;

/* Перевіряє WHO_AM_I, будить датчик і задає діапазони ±2 g / ±250 °/s. */
dev_status_t mpu6050_init(mpu6050_t *dev, const dev_i2c_bus_t *bus, uint8_t addr7);

/* Одне багатобайтове читання 0x3B..0x48 (14 байт). */
dev_status_t mpu6050_read_raw(mpu6050_t *dev, mpu6050_raw_t *raw);

/* Розбір 14 байт у порядку регістрів (старший байт першим). */
void mpu6050_parse(const uint8_t buf[MPU6050_SAMPLE_LEN], mpu6050_raw_t *raw);

/* Перетворення у фізичні одиниці для діапазонів ±2 g / ±250 °/s. */
void mpu6050_convert(const mpu6050_raw_t *raw, mpu6050_data_t *out);

/* Перевірка правдоподібності в спокої: |a| = 1 g ± tol_mg. */
int mpu6050_is_at_rest(const mpu6050_data_t *d, uint32_t tol_mg);

#endif /* MPU6050_H */
