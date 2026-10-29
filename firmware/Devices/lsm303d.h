/**
  ******************************************************************************
  * @file    lsm303d.h
  * @brief   LSM303D (модуль GY-89): акселерометр для FR-06 (удар/вібрація).
  *
  *          У ЛР2 замінює MPU-6050: модуль GY-521 на стенді виявився
  *          несправним (не відповідає на I²C ні на 0x68, ні на 0x69). Драйвер
  *          MPU-6050 лишається в Devices/ — після заміни модуля він потрібен знову.
  *
  *          Магнітометр LSM303D тут не використовується.
  ******************************************************************************
  */
#ifndef LSM303D_H
#define LSM303D_H

#include <stdint.h>

#include "dev_bus.h"

/* SA0 на GY-89 підтягнутий до VDD: 0x1D (при SA0 = 0 — 0x1E) */
#define LSM303D_I2C_ADDR        0x1DU
#define LSM303D_I2C_ADDR_ALT    0x1EU

/* Datasheet LSM303D (DocID023312), розд. 9 «Register mapping» */
#define LSM303D_REG_WHO_AM_I    0x0FU
#define LSM303D_REG_CTRL0       0x1FU   /* біт 7 BOOT: перезавантажити калібрування з внутрішньої пам'яті */
#define LSM303D_CTRL0_BOOT      0x80U
#define LSM303D_CTRL2_SELFTEST  0x02U   /* AST = 1: вбудований self-test акселерометра */
#define LSM303D_WHO_AM_I_VALUE  0x49U
#define LSM303D_REG_CTRL1       0x20U
#define LSM303D_REG_CTRL2       0x21U
#define LSM303D_REG_OUT_X_L_A   0x28U   /* 0x28..0x2D: X, Y, Z, молодший байт першим */

/* Для багатобайтового читання по I²C старший біт адреси регістру вмикає
 * автоінкремент (datasheet, розд. 6.1.1). Без нього чип 6 разів поверне OUT_X_L_A. */
#define LSM303D_AUTO_INC        0x80U

/* CTRL1: AODR = 0101 (50 Гц), BDU = 1 (старший і молодший байт з одного виміру),
 * AZEN = AYEN = AXEN = 1 */
#define LSM303D_CTRL1_50HZ_XYZ  0x5FU
/* CTRL2: AFS = 000 → ±2 g, смуга фільтра 773 Гц, без self-test */
#define LSM303D_CTRL2_2G        0x00U

#define LSM303D_SAMPLE_LEN      6U
/* Чутливість при ±2 g: 0,061 mg/LSB (datasheet, табл. 3) */
#define LSM303D_UG_PER_LSB_2G   61

typedef struct
{
  int16_t ax, ay, az;
} lsm303d_raw_t;

typedef struct
{
  int32_t ax_mg, ay_mg, az_mg;
} lsm303d_data_t;

typedef struct
{
  const dev_i2c_bus_t *bus;
  uint8_t              addr7;
} lsm303d_t;

/* Перевіряє WHO_AM_I, вмикає акселерометр 50 Гц, ±2 g. */
dev_status_t lsm303d_init(lsm303d_t *dev, const dev_i2c_bus_t *bus, uint8_t addr7);

/* Одне багатобайтове читання 0x28..0x2D з автоінкрементом (адреса 0xA8 на шині). */
dev_status_t lsm303d_read_raw(lsm303d_t *dev, lsm303d_raw_t *raw);

/* 6 байт у порядку регістрів: X_L, X_H, Y_L, Y_H, Z_L, Z_H (little-endian). */
void lsm303d_parse(const uint8_t buf[LSM303D_SAMPLE_LEN], lsm303d_raw_t *raw);

/* Перетворення в міліg для ±2 g. */
void lsm303d_convert(const lsm303d_raw_t *raw, lsm303d_data_t *out);

#endif /* LSM303D_H */
