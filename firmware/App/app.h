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

/* Викликається з main.c (USER CODE 3) у нескінченному циклі: один прохід
 * Super Loop по всіх задачах (docs/tasks.md). */
void App_Loop(void);

/* ЛР3: нагляд за MPU-6050 і шиною I2C після старту (docs/lab_3/driver-audit.md,
 * дефекти 1 і 2). Не частіше ніж раз на TBD_MPU_RETRY_MS (sl_security.h):
 *   - датчик справний (mpu_status == DEV_OK) — mpu6050_check(), одне читання
 *     PWR_MGMT_1 (~0,3 мс). Датчик перезапустився (спить, віддає нулі) або
 *     помилка шини — mpu_status стає кодом помилки, рядок MPU,FAULT. Поки
 *     mpu_status != DEV_OK, sl_security.c не читає датчик і скидає базову
 *     лінію TAMPER, тож відновлений датчик не дасть хибного удару;
 *   - датчик несправний — mpu6050_recover() (повторна ініціалізація з виходом
 *     зі сну), рядок MPU,RECOVER. Відсутній модуль коштує одне звернення з NACK
 *     (~0,1 мс) на період, а помилка друкується один раз, а не щосекунди;
 *   - відновлення шини I2C (platform_bus.c) — рядок I2C,RECOVER.
 * Без затримок і циклів очікування. Зараз викликається з App_Loop(); у таблиці
 * задач Super Loop (app_tasks.c) її місце — окрема задача з тим самим періодом. */
void App_SensorService(uint32_t now_ms);

/* Скидання стану App_SensorService() (тести) */
void App_SensorServiceReset(void);

#endif /* APP_H */
