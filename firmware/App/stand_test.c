/**
  ******************************************************************************
  * @file    stand_test.c
  * @brief   Перевірки пристроїв стенда ЛР2 (див. stand_test.h).
  *
  *          Без HAL: залізо доступне лише через platform.h і драйвери Devices/.
  *          Рядки формує і виводить self_test.c.
  ******************************************************************************
  */

#include "stand_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "dht11.h"
#include "platform.h"

/* ---- адреси й ідентифікатори (docs/pinout.md) ---------------------------- */
#define ADDR_OV7670      0x21U
#define ADDR_L3GD20_A    0x6BU
#define ADDR_L3GD20_B    0x6AU

#define REG_WHO_AM_I     0x0FU   /* L3GD20 */
#define L3GD20_WHO_A     0xD4U
#define L3GD20_WHO_B     0xD7U   /* L3GD20H */
#define OV_REG_PID       0x0AU
#define OV_REG_VER       0x0BU
#define OV_PID           0x76U
#define OV_VER           0x73U

#define SERVO_FREQ_HZ    50U
#define SERVO_MIN_US     1000U
#define SERVO_MAX_US     2000U

#define DHT_THRESHOLD_US 48U

#define I2C_ADDR_FIRST   0x08U
#define I2C_ADDR_LAST    0x77U

/* "I2C,scan" + до 112 адрес по ",0xNN": при SDA, притиснутій до землі,
 * «відповідають» усі адреси, і рядок має бути повним */
#define SCAN_LINE_SIZE   (8U + 5U * (I2C_ADDR_LAST - I2C_ADDR_FIRST + 1U) + 1U)

#define LINE_SIZE        192U

/* ============================================================================
 * Допоміжне форматування
 * ========================================================================== */

/* Буфер полів, який заповнюється частинами */
typedef struct
{
    char *s;
    size_t size;
    size_t n;
} fields_t;

static void fl_init(fields_t *f, char *buffer, size_t size)
{
    f->s = buffer;
    f->size = size;
    f->n = 0U;

    if (buffer != NULL && size > 0U)
    {
        buffer[0] = '\0';
    }
}

static void fl_add(fields_t *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Дописує фрагмент; при переповненні рядок обрізається, але лишається з '\0' */
static void fl_add(fields_t *f, const char *fmt, ...)
{
    va_list ap;
    int k;

    if (f->s == NULL || f->n + 1U >= f->size)
    {
        return;
    }

    va_start(ap, fmt);
    k = vsnprintf(&f->s[f->n], f->size - f->n, fmt, ap);
    va_end(ap);

    if (k > 0)
    {
        f->n += (size_t)k;

        if (f->n > f->size - 1U)
        {
            f->n = f->size - 1U;
        }
    }
}

static void fl_fixed(fields_t *f, int32_t value, unsigned dec)
{
    char num[24];

    (void)StandTest_FormatFixed(num, sizeof(num), value, dec);
    fl_add(f, "%s", num);
}

/* Цілий квадратний корінь (модуль вектора прискорення) */
static uint32_t isqrt(uint64_t x)
{
    uint64_t r = 0U;
    uint64_t b = 1ULL << 62;

    while (b > x)
    {
        b >>= 2;
    }

    while (b != 0U)
    {
        if (x >= r + b)
        {
            x -= r + b;
            r = (r >> 1) + b;
        }
        else
        {
            r >>= 1;
        }

        b >>= 2;
    }

    return (uint32_t)r;
}

static uint32_t magnitude(int32_t x, int32_t y, int32_t z)
{
    return isqrt((uint64_t)((int64_t)x * x +
                            (int64_t)y * y +
                            (int64_t)z * z));
}

/* Початкове заповнення результату перевірки */
static void device_init(
    self_test_device_t *device,
    const char *name,
    fields_t *f,
    char *fields,
    size_t size
)
{
    fl_init(f, fields, size);
    device->name = name;
    device->state = SELF_TEST_OK;
    device->error = DEV_OK;
    device->fields = fields;
}

static void device_fail(self_test_device_t *device, dev_status_t error)
{
    device->state = SELF_TEST_FAIL;
    device->error = error;
}

/* Повертає код помилки для виводу команд стенда */
const char *StandTest_ErrStr(dev_status_t status)
{
    return (status == DEV_OK) ? "ok" : SelfTest_ErrorText(status);
}

/* Форматує число з фіксованою комою */
int StandTest_FormatFixed(
    char *buffer,
    size_t size,
    int32_t value,
    unsigned dec
)
{
    int32_t div = 1;
    const char *sign = "";
    unsigned i;

    for (i = 0U; i < dec; ++i)
    {
        div *= 10;
    }

    if (value < 0)
    {
        sign = "-";
        value = -value;
    }

    if (dec == 0U)
    {
        return snprintf(buffer, size, "%s%ld", sign, (long)value);
    }

    return snprintf(
        buffer, size,
        "%s%ld.%0*ld",
        sign,
        (long)(value / div),
        (int)dec,
        (long)(value % div)
    );
}

/* Форматує знімок цифрових входів */
int StandTest_FormatInputs(
    char *buffer,
    size_t size,
    uint32_t v
)
{
    /* кнопки: 1 = відпущена, 0 = натиснута. lock = синя USER B1 на Nucleo (FR-02) */
    return snprintf(
        buffer, size,
        "btn=%c%c%c%c,lock_b1=%u,door=%u,pir=%u,mpu_int=%u",
        (v & PLATFORM_IN_BTN1) ? '1' : '0',
        (v & PLATFORM_IN_BTN2) ? '1' : '0',
        (v & PLATFORM_IN_BTN3) ? '1' : '0',
        (v & PLATFORM_IN_BTN4) ? '1' : '0',
        (v & PLATFORM_IN_LOCK) ? 1U : 0U,
        (v & PLATFORM_IN_DOOR) ? 1U : 0U,
        (v & PLATFORM_IN_PIR) ? 1U : 0U,
        (v & PLATFORM_IN_MPUINT) ? 1U : 0U
    );
}

/* ============================================================================
 * Перевірки пристроїв
 * ========================================================================== */

/* UART: якщо рядок дійшов до ПК — канал і швидкість правильні */
void StandTest_CheckUart(self_test_device_t *device, char *fields, size_t size)
{
    fields_t f;

    device_init(device, "UART", &f, fields, size);
    fl_add(&f, "baud=115200");
}

/* MPU-6050 (GY-521): відомий несправний модуль, тому помилка шини — SKIP */
void StandTest_CheckMpu(self_test_device_t *device, char *fields, size_t size)
{
    App_Devices *d = &app_devices;
    mpu6050_raw_t raw;
    mpu6050_data_t m;
    dev_status_t s;
    uint32_t a;
    int32_t gmax;
    fields_t f;

    device_init(device, "MPU6050", &f, fields, size);

    if (d->mpu_status != DEV_OK)
    {
        d->mpu_status = mpu6050_init(
            &d->mpu, Platform_SensorI2c(), MPU6050_I2C_ADDR
        );
    }

    s = (d->mpu_status == DEV_OK)
            ? mpu6050_read_raw(&d->mpu, &raw) : d->mpu_status;

    if (s != DEV_OK)
    {
        /* FR-06 виконує LSM303D, тому це не провал self-test;
         * для SKIP err= не додається автоматично — він у полях */
        device->state = SELF_TEST_SKIP;
        device->error = s;
        fl_add(&f, "err=%s,note=GY-521_faulty_FR06_on_LSM303D",
               SelfTest_ErrorText(s));
        return;
    }

    mpu6050_convert(&raw, &m);
    a = magnitude(m.ax_mg, m.ay_mg, m.az_mg);

    gmax = abs(m.gx_mdps);

    if (abs(m.gy_mdps) > gmax)
    {
        gmax = abs(m.gy_mdps);
    }

    if (abs(m.gz_mdps) > gmax)
    {
        gmax = abs(m.gz_mdps);
    }

    /* у спокої |a| = 1 g ± 5 %, гіроскоп < 5 °/с */
    if (a < 950U || a > 1050U || gmax >= 5000)
    {
        device->state = SELF_TEST_WARN;
    }

    fl_add(&f, "who=0x68,a_g=");    fl_fixed(&f, (int32_t)a, 3);
    fl_add(&f, ",ax_g=");           fl_fixed(&f, m.ax_mg, 3);
    fl_add(&f, ",ay_g=");           fl_fixed(&f, m.ay_mg, 3);
    fl_add(&f, ",az_g=");           fl_fixed(&f, m.az_mg, 3);
    fl_add(&f, ",gyro_max_dps=");   fl_fixed(&f, gmax / 100, 1);
    fl_add(&f, ",t_c=");            fl_fixed(&f, m.temp_cdeg / 10, 1);
}

/* BMP180: температура і тиск у кімнатному діапазоні */
void StandTest_CheckBmp(self_test_device_t *device, char *fields, size_t size)
{
    App_Devices *d = &app_devices;
    int32_t t_dc = 0;
    int32_t p_pa = 0;
    dev_status_t s;
    fields_t f;

    device_init(device, "BMP180", &f, fields, size);

    if (d->baro_status != DEV_OK)
    {
        d->baro_status = bmp180_init(
            &d->baro, Platform_SensorI2c(), Platform_DelayMs, 0U
        );
    }

    s = (d->baro_status == DEV_OK)
            ? bmp180_read(&d->baro, &t_dc, &p_pa) : d->baro_status;

    if (s != DEV_OK)
    {
        device_fail(device, s);
        return;
    }

    /* кімната 15–35 °C, тиск 950–1050 гПа (Київ ~170 м: близько 990–1000) */
    if (t_dc < 150 || t_dc > 350 || p_pa < 95000 || p_pa > 105000)
    {
        device->state = SELF_TEST_WARN;
    }

    fl_add(&f, "id=0x55,t_c=");     fl_fixed(&f, t_dc, 1);
    fl_add(&f, ",p_hpa=");          fl_fixed(&f, p_pa, 2);
}

/* L3GD20: WHO_AM_I на основній, потім на альтернативній адресі */
void StandTest_CheckL3gd20(self_test_device_t *device, char *fields, size_t size)
{
    const dev_i2c_bus_t *bus = Platform_SensorI2c();
    const uint8_t addrs[2] = { ADDR_L3GD20_A, ADDR_L3GD20_B };
    fields_t f;
    unsigned i;

    device_init(device, "L3GD20", &f, fields, size);

    for (i = 0U; i < 2U; ++i)
    {
        uint8_t who = 0U;

        if (bus->read_reg(bus->ctx, addrs[i], REG_WHO_AM_I,
                          &who, 1U) == DEV_OK)
        {
            if (who != L3GD20_WHO_A && who != L3GD20_WHO_B)
            {
                device->state = SELF_TEST_WARN;
            }

            fl_add(&f, "addr=0x%02X,who=0x%02X", addrs[i], who);
            return;
        }
    }

    device_fail(device, DEV_ERR_NACK);
    fl_add(&f, "addr=0x%02X|0x%02X", ADDR_L3GD20_A, ADDR_L3GD20_B);
}

/* FR-06: акселерометр LSM303D на GY-89. У спокої |a| = 1 g ± 5 %. */
void StandTest_CheckAccel(self_test_device_t *device, char *fields, size_t size)
{
    App_Devices *d = &app_devices;
    lsm303d_raw_t raw;
    lsm303d_data_t a;
    dev_status_t s;
    uint32_t mag;
    fields_t f;

    device_init(device, "LSM303D", &f, fields, size);

    if (d->accel_status != DEV_OK)
    {
        d->accel_status = lsm303d_init(
            &d->accel, Platform_SensorI2c(), LSM303D_I2C_ADDR
        );

        if (d->accel_status != DEV_OK)
        {
            d->accel_status = lsm303d_init(
                &d->accel, Platform_SensorI2c(), LSM303D_I2C_ADDR_ALT
            );
        }
    }

    s = (d->accel_status == DEV_OK)
            ? lsm303d_read_raw(&d->accel, &raw) : d->accel_status;

    if (s != DEV_OK)
    {
        device_fail(device, s);
        return;
    }

    lsm303d_convert(&raw, &a);
    mag = magnitude(a.ax_mg, a.ay_mg, a.az_mg);

    if (mag < 950U || mag > 1050U)
    {
        device->state = SELF_TEST_WARN;
    }

    fl_add(&f, "addr=0x%02X,who=0x49,role=FR06_accel,"
               "raw=%04X,%04X,%04X,a_g=",
           d->accel.addr7,
           (uint16_t)raw.ax, (uint16_t)raw.ay, (uint16_t)raw.az);
    fl_fixed(&f, (int32_t)mag, 3);
    fl_add(&f, ",ax_g=");   fl_fixed(&f, a.ax_mg, 3);
    fl_add(&f, ",ay_g=");   fl_fixed(&f, a.ay_mg, 3);
    fl_add(&f, ",az_g=");   fl_fixed(&f, a.az_mg, 3);
}

/* W25Q64: JEDEC ID, ємність з третього байта */
void StandTest_CheckFlash(self_test_device_t *device, char *fields, size_t size)
{
    App_Devices *d = &app_devices;
    uint8_t id[3] = { 0U, 0U, 0U };
    dev_status_t s;
    unsigned mbit;
    fields_t f;

    device_init(device, "W25Q64", &f, fields, size);

    d->flash.bus = Platform_FlashSpi();
    d->flash.delay_ms = Platform_DelayMs;
    s = w25q64_read_jedec_id(&d->flash, id);

    fl_add(&f, "jedec=%02X%02X%02X", id[0], id[1], id[2]);

    if (s != DEV_OK || id[0] != W25Q64_JEDEC_MANUFACTURER)
    {
        /* FF FF FF — MISO не підключено або CS не той; 00 00 00 — немає живлення */
        device_fail(device, (s != DEV_OK) ? s : DEV_ERR_ID);
        return;
    }

    if (id[2] != W25Q64_JEDEC_CAPACITY)
    {
        device->state = SELF_TEST_WARN;
    }

    /* ємність = 2^id[2] байт; 0x17 -> 8 МБ = 64 Мбіт */
    mbit = (id[2] >= 0x11U && id[2] <= 0x1AU)
               ? (1U << (id[2] - 0x11U)) : 0U;

    fl_add(&f, ",size_mbit=%u%s", mbit,
           (device->state == SELF_TEST_WARN) ? ",note=not_q64" : "");
}

/* OV7670: ID по SCCB і наявність кадрів (VSYNC) за 1 с */
void StandTest_CheckCamera(self_test_device_t *device, char *fields, size_t size)
{
    uint8_t pid = 0U;
    uint8_t ver = 0U;
    uint32_t v0;
    uint32_t vs;
    dev_status_t s;
    fields_t f;

    device_init(device, "OV7670", &f, fields, size);

    s = Platform_SccbRead(ADDR_OV7670, OV_REG_PID, &pid);

    if (s == DEV_OK)
    {
        s = Platform_SccbRead(ADDR_OV7670, OV_REG_VER, &ver);
    }

    v0 = Platform_VsyncCount();
    Platform_DelayMs(1000U);
    vs = Platform_VsyncCount() - v0;

    if (s != DEV_OK)
    {
        device_fail(device, s);
        fl_add(&f, "vsync_hz=%lu,hint=check_MCLK_D7_PWDN0_RST1",
               (unsigned long)vs);
        return;
    }

    if (pid != OV_PID || ver != OV_VER || vs == 0U)
    {
        device->state = SELF_TEST_WARN;
    }

    fl_add(&f, "pid=0x%02X,ver=0x%02X,vsync_hz=%lu",
           pid, ver, (unsigned long)vs);
}

/* DHT11: один кадр 40 біт; CRC не зійшлася — FAIL (err=state, hint=crc) */
void StandTest_CheckDht11(self_test_device_t *device, char *fields, size_t size)
{
    uint16_t us[40];
    uint8_t frame[DHT11_FRAME_LEN];
    dht11_data_t d;
    dev_status_t s;
    fields_t f;

    device_init(device, "DHT11", &f, fields, size);

    s = Platform_Dht11Capture(us);

    if (s == DEV_OK)
    {
        dht11_bits_to_bytes(us, DHT_THRESHOLD_US, frame);
        s = dht11_decode(frame, &d);
    }

    if (s != DEV_OK)
    {
        device_fail(device, s);

        if (s == DEV_ERR_STATE)
        {
            fl_add(&f, "hint=crc");
        }

        return;
    }

    if (d.temp_c < 10 || d.temp_c > 40 ||
        d.rh_pct < 10U || d.rh_pct > 95U)
    {
        device->state = SELF_TEST_WARN;
    }

    fl_add(&f, "t_c=%d.%u,rh=%u.%u,raw=%02X%02X%02X%02X%02X,crc=OK",
           d.temp_c, d.temp_tenth, d.rh_pct, d.rh_tenth,
           frame[0], frame[1], frame[2], frame[3], frame[4]);
}

/* ШІМ серво на PA6: частота і ширина імпульсу */
void StandTest_CheckServo(self_test_device_t *device, char *fields, size_t size)
{
    uint32_t fr = Platform_ServoFreqHz();
    uint32_t p = Platform_ServoPulseUs();
    fields_t f;

    device_init(device, "SERVO_PWM", &f, fields, size);

    if (fr != SERVO_FREQ_HZ || p < SERVO_MIN_US || p > SERVO_MAX_US)
    {
        /* ШІМ не запущено або імпульс поза межами */
        device_fail(device, DEV_ERR_STATE);
    }

    /* Серво не має зворотного зв'язку: перевіряється лише ШІМ,
     * сам привід — аналізатором і очима */
    fl_add(&f, "freq_hz=%lu,pulse_us=%lu,note=no_feedback",
           (unsigned long)fr, (unsigned long)p);
}

/* Знімок цифрових входів */
void StandTest_CheckGpio(self_test_device_t *device, char *fields, size_t size)
{
    fields_t f;

    device_init(device, "GPIO", &f, fields, size);
    (void)StandTest_FormatInputs(fields, size, Platform_ReadInputs());
}

/* ============================================================================
 * Вивід
 * ========================================================================== */

/* Скан шини I2C1: адреси, що відповіли ACK */
void StandTest_I2cScan(void)
{
    static char line[SCAN_LINE_SIZE];
    fields_t f;
    uint8_t a;

    fl_init(&f, line, sizeof(line));
    fl_add(&f, "I2C,scan");

    for (a = I2C_ADDR_FIRST; a <= I2C_ADDR_LAST; ++a)
    {
        if (Platform_I2cProbe(a) == DEV_OK)
        {
            fl_add(&f, ",0x%02X", a);
        }
    }

    (void)SelfTest_Emit(line);
}

/* Виводить рядок одного пристрою */
bool StandTest_PrintOne(const self_test_device_t *device)
{
    char line[LINE_SIZE];

    if (!SelfTest_FormatLine(line, sizeof(line), device))
    {
        return false;
    }

    return SelfTest_Emit(line);
}

/* Виконує одну перевірку і виводить її рядок */
self_test_state_t StandTest_RunOne(stand_test_check_fn check)
{
    self_test_device_t device;
    char fields[STAND_TEST_FIELDS_MAX];

    check(&device, fields, sizeof(fields));
    (void)StandTest_PrintOne(&device);

    return device.state;
}

/* ============================================================================
 * Повний прогін
 * ========================================================================== */

/* Перевірка і пауза перед нею */
typedef struct
{
    stand_test_check_fn check;
    uint32_t delay_before_ms;
} stand_test_step_t;

static const stand_test_step_t s_steps[STAND_TEST_DEVICE_COUNT] =
{
    { StandTest_CheckUart,   0U },
    { StandTest_CheckMpu,    0U },
    { StandTest_CheckBmp,    0U },
    { StandTest_CheckL3gd20, 0U },
    { StandTest_CheckAccel,  0U },
    { StandTest_CheckFlash,  0U },
    { StandTest_CheckCamera, 0U },
    /* DHT11 не опитувати частіше, ніж раз на ~1 с після вмикання живлення */
    { StandTest_CheckDht11,  200U },
    { StandTest_CheckServo,  0U },
    { StandTest_CheckGpio,   0U }
};

/* Виконує всі перевірки і формує звіт */
bool StandTest_Run(void)
{
    static self_test_device_t devices[STAND_TEST_DEVICE_COUNT];
    static char fields[STAND_TEST_DEVICE_COUNT][STAND_TEST_FIELDS_MAX];
    size_t i;

    (void)SelfTest_Emit("");
    (void)SelfTest_Emit("# SmartLock selftest v2 (LR2)");
    StandTest_I2cScan();

    for (i = 0U; i < STAND_TEST_DEVICE_COUNT; ++i)
    {
        if (s_steps[i].delay_before_ms > 0U)
        {
            Platform_DelayMs(s_steps[i].delay_before_ms);
        }

        s_steps[i].check(&devices[i], fields[i], sizeof(fields[i]));
    }

    return SelfTest_Report(devices, STAND_TEST_DEVICE_COUNT);
}
