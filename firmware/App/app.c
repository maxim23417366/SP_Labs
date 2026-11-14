
/**
  ******************************************************************************
  * @file    app.c
  * @brief   Ініціалізація пристроїв SmartLock в одній прошивці.
  *
  *          ЛР3 (I02): App_Loop() — один прохід Super Loop (sched.c) по задачах з
  *          app_tasks.c. Стенд ЛР2 (self-test, однолітерні команди) збережено.
  ******************************************************************************
  */

#include "app.h"

#include "app_config.h"
#include "app_tasks.h"
#include "bench.h"
#include "platform.h"
#include "sched.h"
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

/* Перший рядок UART-логу: яка саме збірка працює (методичка §5.2) */
static void print_banner(void)
{
    printf("BUILD,fw=SmartLock,lab=LR3,git=%s,cpu_hz=%lu,loop=superloop\r\n", GIT_HASH,
           (unsigned long)Platform_CpuHz());
}

/* Запуск прикладної логіки */
void App_Init(void)
{
    print_banner();
    Platform_BringUp();
    devices_init();
    devices_self_test();
    Bench_Help();
    AppTasks_Init();   /* час, черга подій, таблиця задач (без виводу) */
    AppTasks_PrintMem();   /* I06: пам'ять після self-test (найглибші виклики старту) */
}

/* Один прохід Super Loop. Якщо App_Init() не викликали (хостові тести ЛР2),
 * таблиця задач створюється при першому проході — без виводу. */
void App_Loop(void)
{
    if (!Sched_Started())
    {
        AppTasks_Init();
    }
    Platform_Poll();     /* трафік ЛР1 (команда l), за замовчуванням вимкнено */
    Sched_RunOnce();
}
