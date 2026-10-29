
/**
  ******************************************************************************
  * @file    app.c
  * @brief   Ініціалізація пристроїв SmartLock в одній прошивці.
  ******************************************************************************
  */

#include "app.h"

#include "bench.h"
#include "platform.h"
#include "self_test.h"
#include "stand_test.h"

#include <stdio.h>

App_Devices app_devices;

/* Виведення повідомлень self-test через UART */
static void self_test_uart_output(const char *line, void *context)
{
    (void)context;

    if (line != NULL)
    {
        printf("%s\n", line);
    }
}

/* Ініціалізація пристроїв SmartLock */
static void devices_init(void)
{
    App_Devices *d = &app_devices;

    d->mpu_status = mpu6050_init(
        &d->mpu,
        Platform_SensorI2c(),
        MPU6050_I2C_ADDR
    );

    d->baro_status = bmp180_init(
        &d->baro,
        Platform_SensorI2c(),
        Platform_DelayMs,
        0U
    );

    d->flash_status = w25q64_init(
        &d->flash,
        Platform_FlashSpi(),
        Platform_DelayMs
    );

    d->accel_status = lsm303d_init(
        &d->accel,
        Platform_SensorI2c(),
        LSM303D_I2C_ADDR
    );
}

/* Формування структурованого звіту самоперевірки:
 * перевірки всіх пристроїв стенда (stand_test.c) і звіт (self_test.c) */
static void devices_self_test(void)
{
    SelfTest_SetOutput(self_test_uart_output, NULL);

    (void)StandTest_Run();
}

/* Запуск прикладної логіки */
void App_Init(void)
{
    Platform_BringUp();
    devices_init();
    devices_self_test();
    Bench_Help();
}

/* Основний цикл програми */
void App_Loop(void)
{
    Platform_Poll();
    Bench_Poll();
}
