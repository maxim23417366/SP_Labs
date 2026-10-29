/**
  ******************************************************************************
  * @file    app.h
  * @brief   Прикладний шар SmartLock: точка входу з main.c.
  *
  *          App/ не включає HAL і заголовки CubeMX (перевіряє make check-deps):
  *          шини отримує з Platform/ через platform.h, пристроями керує через
  *          драйвери з Devices/.
  ******************************************************************************
  */
#ifndef APP_H
#define APP_H

#include "bmp180.h"
#include "dev_bus.h"
#include "lsm303d.h"
#include "mpu6050.h"
#include "w25q64.h"

/* Стан пристроїв після App_Init(): хендли драйверів і результат ініціалізації.
 * Звідси дані бере UART self-test. */
typedef struct
{
  mpu6050_t    mpu;
  dev_status_t mpu_status;

  bmp180_t     baro;
  dev_status_t baro_status;

  w25q64_t     flash;
  dev_status_t flash_status;

  /* FR-06 (удар/вібрація): акселерометр LSM303D на GY-89 замість MPU-6050,
   * поки модуль GY-521 несправний (журнал проблем ЛР2) */
  lsm303d_t    accel;
  dev_status_t accel_status;
} App_Devices;

extern App_Devices app_devices;

/* Викликається з main.c (USER CODE 2) після всіх MX_*_Init(). */
void App_Init(void);

/* Викликається з main.c (USER CODE 3) у нескінченному циклі. */
void App_Loop(void);

#endif /* APP_H */
