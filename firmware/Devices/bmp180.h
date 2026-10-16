/**
  ******************************************************************************
  * @file    bmp180.h
  * @brief   Драйвер барометра BMP180 (модуль GY-89), I2C.
  *
  *          Джерело: Bosch BMP180 Data sheet BST-BMP180-DS000-09 — розділи
  *          «Calibration coefficients», «Calculating pressure and temperature»
  *          (з числовим прикладом) і «Global memory map and register description».
  ******************************************************************************
  */
#ifndef BMP180_H
#define BMP180_H

#include "dev_bus.h"

#define BMP180_I2C_ADDR       0x77U
#define BMP180_REG_ID         0xD0U
#define BMP180_CHIP_ID        0x55U
#define BMP180_REG_CALIB      0xAAU   /* 0xAA..0xBF: 11 слів, старший байт першим */
#define BMP180_CALIB_LEN      22U
#define BMP180_REG_CTRL_MEAS  0xF4U
#define BMP180_REG_OUT_MSB    0xF6U   /* 0xF6..0xF8: MSB, LSB, XLSB */
#define BMP180_CMD_TEMP       0x2EU
#define BMP180_CMD_PRESSURE   0x34U   /* + (oss << 6) */
#define BMP180_OSS_MAX        3U

typedef struct
{
  int16_t  ac1;
  int16_t  ac2;
  int16_t  ac3;
  uint16_t ac4;
  uint16_t ac5;
  uint16_t ac6;
  int16_t  b1;
  int16_t  b2;
  int16_t  mb;
  int16_t  mc;
  int16_t  md;
} bmp180_calib_t;

typedef struct
{
  const dev_i2c_bus_t *bus;
  dev_delay_ms_fn      delay_ms;
  uint8_t              oss;       /* oversampling_setting 0..3 */
  bmp180_calib_t       calib;
} bmp180_t;

/* Перевіряє chip ID і зчитує калібрувальні коефіцієнти з EEPROM. */
dev_status_t bmp180_init(bmp180_t *dev, const dev_i2c_bus_t *bus,
                         dev_delay_ms_fn delay_ms, uint8_t oss);

/* Розбирає 22 байти з 0xAA..0xBF. Повертає 0, якщо якесь слово дорівнює
 * 0x0000 або 0xFFFF (за datasheet — ознака збою зв'язку). */
int bmp180_parse_calib(const uint8_t raw[BMP180_CALIB_LEN], bmp180_calib_t *calib);

/* Компенсація за алгоритмом із datasheet. Температура — у 0,1 °C, тиск — у Па.
 * b5 — проміжне значення з розрахунку температури, потрібне для тиску. */
int32_t bmp180_compensate_temp(const bmp180_calib_t *c, int32_t ut, int32_t *b5);
int32_t bmp180_compensate_pressure(const bmp180_calib_t *c, int32_t up, int32_t b5, uint8_t oss);

/* Один цикл вимірювання: сирі UT і UP. */
dev_status_t bmp180_read_raw(bmp180_t *dev, int32_t *ut, int32_t *up);

/* Вимірювання з компенсацією: температура в 0,1 °C, тиск у Па. */
dev_status_t bmp180_read(bmp180_t *dev, int32_t *temp_dc, int32_t *pressure_pa);

#endif /* BMP180_H */
