/**
  ******************************************************************************
  * @file    bench.c
  * @brief   Стенд ЛР2: команди з UART (див. bench.h).
  *
  *          Без HAL: залізо доступне лише через platform.h і драйвери Devices/.
  *          Рядки перевірок ("ST,...") ідуть через вивід self_test.c,
  *          решта виводу команд — printf.
  ******************************************************************************
  */

#include "bench.h"

#include <stdio.h>

#include "app.h"
#include "platform.h"
#include "self_test.h"
#include "stand_test.h"

/* ---- адреси й константи команд (docs/pinout.md) -------------------------- */
#define ADDR_OV7670      0x21U
#define ADDR_NACK_DEMO   0x50U   /* на шині нікого: свідома аномалія для аналізатора */

#define OV_REG_CLKRC     0x11U
#define OV_CLKRC_SLOW    0x1FU   /* XCLK / 32 */
#define OV_CLKRC_RESET   0x80U   /* скидне значення, без дільника */

#define SERVO_MIN_US     1000U
#define SERVO_MID_US     1500U
#define SERVO_MAX_US     2000U

#define GPIO_DEBOUNCE_MS 20U

#define LINE_SIZE        192U

static int s_gpio_watch;
static uint32_t s_gpio_last;

/* Знімок входів одним рядком */
static void print_gpio(const char *prefix)
{
    char fields[STAND_TEST_FIELDS_MAX];

    (void)StandTest_FormatInputs(
        fields, sizeof(fields), Platform_ReadInputs()
    );
    printf("%s%s\r\n", prefix, fields);
}

/* Діагностика LSM303D: сирі значення одним рядком з міткою tag */
static void accel_raw_line(const char *tag)
{
    lsm303d_raw_t r;
    lsm303d_data_t a;
    char x[16];
    char y[16];
    char z[16];
    dev_status_t s = lsm303d_read_raw(&app_devices.accel, &r);

    if (s != DEV_OK)
    {
        printf("ACC,%s,err=%s\r\n", tag, StandTest_ErrStr(s));
        return;
    }

    lsm303d_convert(&r, &a);
    (void)StandTest_FormatFixed(x, sizeof(x), a.ax_mg, 3);
    (void)StandTest_FormatFixed(y, sizeof(y), a.ay_mg, 3);
    (void)StandTest_FormatFixed(z, sizeof(z), a.az_mg, 3);
    printf("ACC,%s,raw=%04X,%04X,%04X,x_g=%s,y_g=%s,z_g=%s\r\n",
           tag, (uint16_t)r.ax, (uint16_t)r.ay, (uint16_t)r.az, x, y, z);
}

/* Діагностика LSM303D: регістри, перезавантаження калібрування (BOOT),
 * вбудований self-test. Для випадку «осі X/Y стоять на 0x7FFF». */
static void accel_diag(void)
{
    const dev_i2c_bus_t *bus = Platform_SensorI2c();
    uint8_t addr = app_devices.accel.addr7
                       ? app_devices.accel.addr7 : LSM303D_I2C_ADDR;
    uint8_t boot[2] = { LSM303D_REG_CTRL0, LSM303D_CTRL0_BOOT };
    uint8_t st_on[2] = { LSM303D_REG_CTRL2, LSM303D_CTRL2_SELFTEST };
    uint8_t st_off[2] = { LSM303D_REG_CTRL2, LSM303D_CTRL2_2G };
    dev_status_t s;
    uint8_t v;
    uint8_t r;
    unsigned i;

    printf("ACC,dump");

    for (r = 0x1FU; r <= 0x2DU; ++r)
    {
        v = 0U;
        (void)bus->read_reg(bus->ctx, addr, r, &v, 1U);
        printf(",%02X=%02X", r, v);
    }

    printf("\r\n");
    accel_raw_line("before");

    s = bus->write(bus->ctx, addr, boot, 2U);
    Platform_DelayMs(20U);
    app_devices.accel_status = lsm303d_init(&app_devices.accel, bus, addr);
    Platform_DelayMs(100U);
    printf("ACC,boot,err=%s,init=%s\r\n",
           StandTest_ErrStr(s), StandTest_ErrStr(app_devices.accel_status));

    for (i = 0U; i < 3U; ++i)
    {
        accel_raw_line("after_boot");
        Platform_DelayMs(40U);
    }

    (void)bus->write(bus->ctx, addr, st_on, 2U);
    Platform_DelayMs(100U);

    for (i = 0U; i < 3U; ++i)
    {
        accel_raw_line("selftest_on");
        Platform_DelayMs(40U);
    }

    (void)bus->write(bus->ctx, addr, st_off, 2U);
    Platform_DelayMs(100U);
    accel_raw_line("selftest_off");
    printf("# self-test: each axis must shift ~0.07..1.7 g vs before; "
           "X/Y unchanged = axis stuck\r\n");
}

/* Ємність флеш-пам'яті за JEDEC ID (типово — W25Q64) */
static uint32_t flash_size_bytes(void)
{
    uint8_t id[3] = { 0U, 0U, 0U };

    if (w25q64_read_jedec_id(&app_devices.flash, id) == DEV_OK &&
        id[2] >= 0x11U && id[2] <= 0x1AU)
    {
        return 1UL << id[2];
    }

    return W25Q64_SIZE_BYTES;
}

/* Стирання, запис і читання останнього сектора флеш-пам'яті */
static void flash_rw_test(void)
{
    static uint8_t wr[W25Q64_PAGE_SIZE];
    static uint8_t rd[W25Q64_PAGE_SIZE];
    w25q64_t *f = &app_devices.flash;
    uint32_t addr;
    uint32_t seed = Platform_Millis();
    dev_status_t s;
    unsigned bad = 0U;
    unsigned i;

    f->bus = Platform_FlashSpi();
    f->delay_ms = Platform_DelayMs;
    addr = flash_size_bytes() - W25Q64_SECTOR_SIZE;   /* останній сектор */

    for (i = 0U; i < W25Q64_PAGE_SIZE; ++i)
    {
        wr[i] = (uint8_t)(i ^ (seed >> 3) ^ 0xA5U);
    }

    s = w25q64_erase_sector(f, addr);

    if (s == DEV_OK)
    {
        s = w25q64_write_page(f, addr, wr, sizeof(wr));
    }

    if (s == DEV_OK)
    {
        s = w25q64_read(f, addr, rd, sizeof(rd));
    }

    if (s != DEV_OK)
    {
        printf("FLASH,rw=FAIL,addr=0x%06lX,err=%s\r\n",
               (unsigned long)addr, StandTest_ErrStr(s));
        return;
    }

    for (i = 0U; i < sizeof(rd); ++i)
    {
        if (rd[i] != wr[i])
        {
            ++bad;
        }
    }

    printf("FLASH,rw=%s,addr=0x%06lX,bytes=%u,mismatch=%u,"
           "first=%02X%02X%02X%02X\r\n",
           (bad == 0U) ? "PASS" : "FAIL", (unsigned long)addr,
           (unsigned)sizeof(rd), bad, rd[0], rd[1], rd[2], rd[3]);
}

/* Команда h: рядок DHT11 з коротким префіксом "DHT,..." замість "ST,DHT11,..." */
static void dht11_line(void)
{
    static const char prefix[] = "ST,DHT11";
    self_test_device_t device;
    char fields[STAND_TEST_FIELDS_MAX];
    char line[LINE_SIZE];

    StandTest_CheckDht11(&device, fields, sizeof(fields));

    if (SelfTest_FormatLine(line, sizeof(line), &device))
    {
        printf("DHT%s\r\n", &line[sizeof(prefix) - 1U]);
    }
}

/* ---- self-test ----------------------------------------------------------- */

/* Повний self-test і довідка */
void Bench_SelfTest(void)
{
    (void)StandTest_Run();
    Bench_Help();
}

/* Довідка з командами */
void Bench_Help(void)
{
    printf("# commands: s selftest | i i2c scan | g gpio watch on/off | "
           "m accel x10 (LSM303D, FR-06) | b bmp180\r\n");
    printf("#           h dht11 x5 | j jedec | f flash r/w | "
           "1/2/3 servo 1000/1500/2000us\r\n");
    printf("#           a accel diag (LSM303D) | c camera id | "
           "k camera slow PCLK on/off | x NACK demo | l LR1 loop traffic | "
           "? help\r\n");
}

/* ---- команди ------------------------------------------------------------- */

/* Виконання однієї команди */
static void cmd(int c)
{
    static int slow_pclk = 0;
    static int loop_on = 0;
    unsigned i;

    switch (c)
    {
        case 's':
            Bench_SelfTest();
            break;

        case '?':
            Bench_Help();
            break;

        case 'i':
            StandTest_I2cScan();
            break;

        case 'g':
            s_gpio_watch = !s_gpio_watch;
            printf("# gpio watch %s\r\n",
                   s_gpio_watch ? "on (prints on change)" : "off");
            s_gpio_last = ~0U;
            break;

        case 'm':
            /* FR-06: LSM303D (GY-89); MPU-6050 — лише якщо модуль відповідає */
            for (i = 0U; i < 10U; ++i)
            {
                (void)StandTest_RunOne(StandTest_CheckAccel);
                Platform_DelayMs(100U);
            }

            if (app_devices.mpu_status == DEV_OK)
            {
                (void)StandTest_RunOne(StandTest_CheckMpu);
            }
            break;

        case 'b':
            (void)StandTest_RunOne(StandTest_CheckBmp);
            break;

        case 'h':
            for (i = 0U; i < 5U; ++i)
            {
                dht11_line();

                if (i < 4U)
                {
                    Platform_DelayMs(2000U);
                }
            }
            break;

        case 'j':
        {
            uint8_t id[3] = { 0U, 0U, 0U };
            dev_status_t s;

            app_devices.flash.bus = Platform_FlashSpi();
            s = w25q64_read_jedec_id(&app_devices.flash, id);
            printf("SPI,jedec=%02X%02X%02X,err=%s\r\n",
                   id[0], id[1], id[2], StandTest_ErrStr(s));
            break;
        }

        case 'f':
            flash_rw_test();
            break;

        case '1':
        case '2':
        case '3':
        {
            uint32_t us = (c == '1') ? SERVO_MIN_US
                        : (c == '2') ? SERVO_MID_US : SERVO_MAX_US;

            Platform_ServoSetPulseUs(us);
            printf("SERVO,pulse_us=%lu,freq_hz=%lu\r\n",
                   (unsigned long)Platform_ServoPulseUs(),
                   (unsigned long)Platform_ServoFreqHz());
            break;
        }

        case 'c':
            (void)StandTest_RunOne(StandTest_CheckCamera);
            break;

        case 'a':
            accel_diag();
            break;

        case 'k':
        {
            /* CLKRC[5:0]: внутрішній такт = XCLK / (prescale + 1). 0x1F -> 24 МГц / 32
             * = 750 кГц; на трейсі cam_vsync_href_pclk.sr PCLK = 375 кГц — ще /2,
             * причина не з'ясована (docs/captures/analysis.md). Вимкнення повертає
             * скидне значення 0x80 (без дільника), яке прошивка не змінює при ініціалізації */
            uint8_t clkrc;
            dev_status_t s;

            slow_pclk = !slow_pclk;
            clkrc = slow_pclk ? OV_CLKRC_SLOW : OV_CLKRC_RESET;
            s = Platform_SccbWrite(ADDR_OV7670, OV_REG_CLKRC, clkrc);
            printf("CAM,slow_pclk=%d,clkrc=0x%02X,err=%s\r\n",
                   slow_pclk, clkrc, StandTest_ErrStr(s));
            break;
        }

        case 'x':
        {
            uint8_t v = 0U;
            const dev_i2c_bus_t *bus = Platform_SensorI2c();
            dev_status_t s = bus->read_reg(bus->ctx, ADDR_NACK_DEMO,
                                           0x00U, &v, 1U);

            printf("I2C,nack_demo,addr=0x%02X,result=%s\r\n",
                   ADDR_NACK_DEMO, StandTest_ErrStr(s));
            break;
        }

        case 'l':
            loop_on = !loop_on;
            Platform_SetLoopTraffic(loop_on);
            printf("# LR1 loop traffic %s (MPU probe + JEDEC every 500 ms)\r\n",
                   loop_on ? "on" : "off");
            break;

        case '\r':
        case '\n':
        case ' ':
            break;

        default:
            printf("# unknown '%c', ? for help\r\n",
                   (c >= 32 && c < 127) ? c : '.');
            break;
    }
}

/* Опитування UART і стеження за входами */
void Bench_Poll(void)
{
    static uint32_t last_ms;
    int c = Platform_UartGetChar();

    if (c >= 0)
    {
        cmd(c);
    }

    if (s_gpio_watch)
    {
        uint32_t now = Platform_Millis();
        uint32_t v = Platform_ReadInputs() & ~PLATFORM_IN_MPUINT;

        /* антидребезг 20 мс: друк лише стабільної зміни */
        if (v != s_gpio_last)
        {
            if ((now - last_ms) >= GPIO_DEBOUNCE_MS)
            {
                s_gpio_last = v;
                printf("[%lu] ", (unsigned long)now);
                print_gpio("GPIO,");
            }
        }
        else
        {
            last_ms = now;
        }
    }
}
