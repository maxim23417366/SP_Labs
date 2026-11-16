
/**
  ******************************************************************************
  * @file    app.c
  * @brief   Ініціалізація пристроїв SmartLock в одній прошивці.
  *
  *          ЛР3 (I02): App_Loop() — один прохід Super Loop (sched.c) по задачах з
  *          app_tasks.c. Стенд ЛР2 (self-test, однолітерні команди) збережено.
  *          ЛР3: App_SensorService() — повторна ініціалізація MPU-6050 не лише
  *          під час старту, а й після несправності (опис — app.h).
  ******************************************************************************
  */

#include "app.h"

#include "app_config.h"
#include "app_tasks.h"
#include "bench.h"
#include "platform.h"
#include "sched.h"
#include "self_test.h"
#include "sl_security.h"   /* TBD_MPU_RETRY_MS — той самий період, що й у задачі security */
#include "stand_test.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

/* ---- Нагляд за MPU-6050 і шиною I2C (опис — app.h) --------------------------- */

static struct
{
    bool                 started;
    uint32_t             last_ms;
    dev_status_t         last_err;     /* остання надрукована помилка відновлення */
    uint32_t             recoveries;   /* успішних повторних ініціалізацій MPU */
    platform_i2c_stats_t i2c;          /* лічильники шини на момент останнього рядка */
    bool                 i2c_down;     /* про невдале відновлення шини вже повідомлено */
} sensor_sv;

void App_SensorServiceReset(void)
{
    (void)memset(&sensor_sv, 0, sizeof(sensor_sv));
}

/* Рядок I2C,RECOVER — на кожне вдале відновлення шини і один раз на період,
 * коли лінії не піднімаються (без живлення модулів лог не засмічується) */
static void i2c_report(void)
{
    platform_i2c_stats_t now;
    bool recovered;
    bool failed;

    Platform_I2cStats(&now);
    recovered = (now.recovered != sensor_sv.i2c.recovered);
    failed    = (now.failed != sensor_sv.i2c.failed);

    if (recovered)
    {
        printf("I2C,RECOVER,ok,n=%lu,fail=%lu\r\n",
               (unsigned long)now.recovered, (unsigned long)now.failed);
    }
    else if (failed && !sensor_sv.i2c_down)
    {
        printf("I2C,RECOVER,err=bus,note=lines_low,n=%lu,fail=%lu\r\n",
               (unsigned long)now.recovered, (unsigned long)now.failed);
    }
    sensor_sv.i2c_down = failed && !recovered;
    sensor_sv.i2c      = now;
}

void App_SensorService(uint32_t now_ms)
{
    App_Devices *d = &app_devices;
    dev_status_t st;

    if (sensor_sv.started && ((uint32_t)(now_ms - sensor_sv.last_ms) < TBD_MPU_RETRY_MS))
    {
        return;
    }
    sensor_sv.started = true;
    sensor_sv.last_ms = now_ms;

    i2c_report();

    if (d->mpu.bus == NULL)
    {
        return;   /* devices_init() ще не викликали (хостові тести без App_Init) */
    }

    if (d->mpu_status == DEV_OK)
    {
        st = mpu6050_check(&d->mpu);
        if (st != DEV_OK)
        {
            /* Відновлення — у наступному виклику: так задача security хоч раз
             * побачить mpu_status != DEV_OK і збере базову лінію заново */
            d->mpu_status      = st;
            sensor_sv.last_err = st;
            printf("MPU,FAULT,err=%s%s\r\n", SelfTest_ErrorText(st),
                   (st == DEV_ERR_STATE) ? ",note=sensor_reset" : "");
        }
        return;
    }

    st = mpu6050_recover(&d->mpu);
    d->mpu_status = st;
    if (st == DEV_OK)
    {
        sensor_sv.recoveries++;
        sensor_sv.last_err = DEV_OK;
        printf("MPU,RECOVER,ok,n=%lu\r\n", (unsigned long)sensor_sv.recoveries);
    }
    else if (st != sensor_sv.last_err)
    {
        sensor_sv.last_err = st;
        printf("MPU,RECOVER,err=%s\r\n", SelfTest_ErrorText(st));
    }
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
    App_SensorService(Platform_Millis());   /* MPU-6050 і I2C після збою — без скидання МК */
}
