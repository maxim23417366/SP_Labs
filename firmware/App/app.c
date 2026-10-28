
/**
  ******************************************************************************
  * @file    app.c
  * @brief   Ініціалізація пристроїв SmartLock в одній прошивці.
  ******************************************************************************
  */

#include "app.h"

#include "platform.h"
#include "self_test.h"

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
}

/* Формування структурованого звіту самоперевірки */
static void devices_self_test(void)
{
    const App_Devices *d = &app_devices;

    const self_test_device_t results[] =
    {
        {
            "MPU6050",
            d->mpu_status == DEV_OK
                ? SELF_TEST_OK : SELF_TEST_FAIL,
            d->mpu_status,
            NULL
        },
        {
            "BMP180",
            d->baro_status == DEV_OK
                ? SELF_TEST_OK : SELF_TEST_FAIL,
            d->baro_status,
            NULL
        },
        {
            "W25Q64",
            d->flash_status == DEV_OK
                ? SELF_TEST_OK : SELF_TEST_FAIL,
            d->flash_status,
            NULL
        }
    };

    SelfTest_SetOutput(self_test_uart_output, NULL);

    (void)SelfTest_Report(
        results,
        sizeof(results) / sizeof(results[0])
    );
}

/* Запуск прикладної логіки */
void App_Init(void)
{
    Platform_BringUp();
    devices_init();
    devices_self_test();
}

/* Основний цикл програми */
void App_Loop(void)
{
    Platform_Poll();
}
