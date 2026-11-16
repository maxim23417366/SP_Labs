/**
  * @file    fake_sensors.h
  * @brief   ЛР3 (K05/K06): датчики MPU-6050 і BMP180 на fake_i2c, підключені
  *          до app_devices так само, як це робить App_Init() на платі.
  *
  *          Для тестів security (TAMPER, FR-06) і env (тиск, FR-15): тест не
  *          збирає регістри вручну, а викликає attach і задає «фізичні» сирі
  *          значення. Шина — справжня модель fake_i2c, тому помилки шини
  *          імітуються як і раніше: fake_i2c_fail_at() або fake_i2c_reset()
  *          («модуль зник» після init).
  *
  *          BMP180: коефіцієнти і UT/UP з числового прикладу datasheet
  *          (BST-BMP180-DS000-09): oss = 0, UT = 27898, UP = 23843 →
  *          T = 15,0 °C, p = 69964 Па. Затримки драйвера — Platform_DelayMs(),
  *          тобто bmp180_read() просуває fake_platform.now_ms (≈ 10 мс).
  */
#ifndef FAKE_SENSORS_H
#define FAKE_SENSORS_H

#include <stdint.h>

#include "dev_bus.h"

#define FAKE_BMP180_DS_UT      27898
#define FAKE_BMP180_DS_UP      23843
#define FAKE_BMP180_DS_P_PA    69964   /* результат bmp180_read() для DS_UT / DS_UP */

/* Додає MPU-6050 (0x68) на fake_i2c і виконує mpu6050_init(&app_devices.mpu).
 * Акселерометр у спокої: (0, 0, +1 g). Повертає app_devices.mpu_status. */
dev_status_t fake_sensors_mpu_attach(void);

/* Сирі значення акселерометра, LSB (±2 g: 16384 LSB = 1 g) */
void fake_sensors_mpu_set_accel(int16_t ax, int16_t ay, int16_t az);

/* Додає BMP180 (0x77) з коефіцієнтами datasheet і виконує bmp180_init(&app_devices.baro).
 * Повертає app_devices.baro_status. */
dev_status_t fake_sensors_bmp_attach(void);

/* Сирі UT / UP, які «чип» покладе в 0xF6..0xF8 після наступної команди вимірювання */
void fake_sensors_bmp_set_raw(int32_t ut, int32_t up);

#endif /* FAKE_SENSORS_H */
