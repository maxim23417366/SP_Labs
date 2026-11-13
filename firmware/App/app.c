
/**
  ******************************************************************************
  * @file    app.c
  * @brief   Ініціалізація пристроїв SmartLock в одній прошивці.
  *
  *          ЛР3: App_Loop() — один прохід Super Loop (sched.c) по задачах з
  *          app_tasks.c. Стенд ЛР2 (self-test, однолітерні команди) збережено.
  ******************************************************************************
  */

#include "app.h"

#include "app_config.h"
#include "app_tasks.h"
#include "bench.h"
#include "camera_service.h"
#include "cmd.h"
#include "platform.h"
#include "sched.h"
#include "self_test.h"
#include "stand_test.h"
#include "stats.h"

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

/* OV7670: PID/VER, потім QQVGA RGB565. Після self-test ЛР2, бо він міряє
 * частоту VSYNC у заводському режимі камери (30 Гц). */
static void camera_init(void)
{
    camera_cfg_t cfg;
    size_t n = 0U;
    const ov7670_reg_t *table = ov7670_qqvga_rgb565_table(&n);
    uint8_t failed = 0U;
    App_Devices *d = &app_devices;

    d->cam_status = ov7670_init(&d->cam, Platform_SccbRead, Platform_SccbWrite, Platform_DelayMs);
    if (d->cam_status == DEV_OK)
    {
        d->cam_status = ov7670_configure(&d->cam, table, n, TBD_CAMERA_CLKRC, &failed);
    }
    printf("CAM,init,status=%s,pid=0x%02X,ver=0x%02X,clkrc=0x%02X,failed_reg=0x%02X,"
           "fmt=RGB565,w=%u,h=%u\r\n",
           StandTest_ErrStr(d->cam_status), d->cam.pid, d->cam.ver, TBD_CAMERA_CLKRC, failed,
           (unsigned)CAMERA_WIDTH, (unsigned)CAMERA_HEIGHT);

    cfg.capture.width             = CAMERA_WIDTH;
    cfg.capture.height            = CAMERA_HEIGHT;
    cfg.capture.bytes_per_pixel   = CAMERA_BPP;
    cfg.capture.irq_lock_per_line = CAMERA_IRQ_LOCK_PER_LINE;
    cfg.capture.vsync_timeout_us  = TBD_CAMERA_VSYNC_TIMEOUT_US;
    cfg.capture.vsync_min_us      = TBD_CAMERA_VSYNC_MIN_US;
    cfg.capture.href_timeout_us   = TBD_CAMERA_HREF_TIMEOUT_US;
    cfg.capture.pclk_spin_max     = TBD_CAMERA_PCLK_SPIN_MAX;
    cfg.hw_ready = (d->cam_status == DEV_OK);
    Camera_Init(&cfg);
}

/* Запуск прикладної логіки */
void App_Init(void)
{
    print_banner();
    Platform_BringUp();
    devices_init();
    devices_self_test();
    Bench_Help();
    AppTasks_Init();   /* час, EVT, таблиця задач (без виводу) */
    camera_init();
    Cmd_Execute("HELP");
    Stats_PrintMem();
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
