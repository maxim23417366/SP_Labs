/**
  * @file    test_stand_test.c
  * @brief   Перевірки стенда (firmware/App/stand_test.c) і звіт self-test
  *          (firmware/App/self_test.c) без плати.
  *
  *          1. Допоміжне форматування: числа з фіксованою комою, знімок входів,
  *             коди помилок; рядки self_test.c для SKIP/FAIL із полями стенда.
  *          2. Повний StandTest_Run() на fake-стенді (fake_platform + fake_i2c +
  *             mock_spi): усе OK; пристрій не відповідає (NACK) -> FAIL з причиною;
  *             неправильний ID -> FAIL; таймаути; CRC; SKIP; камера без VSYNC.
  *          3. Replay запису uart_selftest.sr: fake-пристрої віддають байти з
  *             декодованого I2C-трейсу, а вивід порівнюється з UART-рядками
  *             того самого прогону (tests/vectors/capture_vectors.h).
  *
  *          Вивід перехоплюється через SelfTest_SetOutput(): функція виводу
  *          отримує по одному рядку без символу нового рядка, тож тест зберігає
  *          рядки в масиві й порівнює їх точно.
  */
#include <string.h>

#include "unity.h"
#include "app.h"
#include "capture_vectors.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "mock_spi.h"
#include "platform.h"
#include "self_test.h"
#include "stand_test.h"

/* ---- перехоплення виводу ---------------------------------------------------- */
#define MAX_LINES 64U
#define LINE_LEN  640U

static char   lines[MAX_LINES][LINE_LEN];
static size_t n_lines;
static int    sink_ctx_ok;

static void sink(const char *line, void *ctx)
{
  sink_ctx_ok = (ctx == &n_lines);
  if (n_lines < MAX_LINES)
  {
    strncpy(lines[n_lines], line, LINE_LEN - 1U);
    lines[n_lines][LINE_LEN - 1U] = '\0';
    n_lines++;
  }
}

/* Рядок з номером idx або "", якщо такого немає */
static const char *line(size_t idx)
{
  return (idx < n_lines) ? lines[idx] : "";
}

/* Перший рядок, що починається з prefix (наприклад "ST,BMP180,") */
static const char *find_line(const char *prefix)
{
  for (size_t i = 0U; i < n_lines; i++)
  {
    if (strncmp(lines[i], prefix, strlen(prefix)) == 0) { return lines[i]; }
  }
  return "";
}

/* Одна перевірка: стан і її рядок "ST,..." (через SelfTest_FormatLine) */
static char one_line[LINE_LEN];

static self_test_state_t check(stand_test_check_fn fn)
{
  self_test_device_t d;
  char fields[STAND_TEST_FIELDS_MAX];

  fn(&d, fields, sizeof(fields));
  TEST_ASSERT_TRUE(SelfTest_FormatLine(one_line, sizeof(one_line), &d));
  return d.state;
}

/* ---- fake-стенд ------------------------------------------------------------- */

/* Калібрування BMP180 — приклад з datasheet (див. test_bmp180.c), big-endian */
static const uint8_t ds_calib_raw[BMP180_CALIB_LEN] =
{
  0x01, 0x98,  0xFF, 0xB8,  0xC7, 0xD1,  0x7F, 0xE5,  0x7F, 0xF5,  0x5A, 0x71,
  0x18, 0x2E,  0x00, 0x04,  0x80, 0x00,  0xDD, 0xF9,  0x0B, 0x34,
};

/* Синтетичний «кімнатний» вимір для калібрування з datasheet:
 * UT = 28706 -> 21.5 °C, UP = 33413 (oss = 0) -> 100002 Па. Не з запису. */
static int32_t bmp_ut = 28706;
static int32_t bmp_up = 33413;

static void bmp180_hook(uint8_t *r, uint8_t reg, uint8_t value)
{
  if (reg != BMP180_REG_CTRL_MEAS) { return; }
  if (value == BMP180_CMD_TEMP)
  {
    r[0xF6] = (uint8_t)(bmp_ut >> 8);
    r[0xF7] = (uint8_t)bmp_ut;
  }
  else
  {
    uint32_t raw = (uint32_t)bmp_up << 8;   /* oss = 0 */
    r[0xF6] = (uint8_t)(raw >> 16);
    r[0xF7] = (uint8_t)(raw >> 8);
    r[0xF8] = (uint8_t)raw;
  }
}

static void set_dht_frame(const uint8_t f[5])
{
  for (unsigned i = 0U; i < 40U; i++)
  {
    fake_platform.dht_high_us[i] = (f[i / 8U] & (0x80U >> (i % 8U))) ? 72U : 24U;
  }
  fake_platform.dht_status = DEV_OK;
}

static void set_jedec(const uint8_t miso[4])
{
  for (size_t i = 0U; i < MOCK_SPI_MAX_FRAMES; i++)
  {
    mock_spi_set_miso(i, miso, 4U);
  }
}

static uint8_t *add_bmp180(void)
{
  uint8_t *r = fake_i2c_add_device(BMP180_I2C_ADDR);
  r[BMP180_REG_ID] = BMP180_CHIP_ID;
  memcpy(&r[BMP180_REG_CALIB], ds_calib_raw, sizeof(ds_calib_raw));
  fake_i2c_set_write_hook(BMP180_I2C_ADDR, bmp180_hook);
  return r;
}

static uint8_t *add_lsm303d(const uint8_t xyz[6])
{
  uint8_t *r = fake_i2c_add_device(LSM303D_I2C_ADDR);
  r[LSM303D_REG_WHO_AM_I] = LSM303D_WHO_AM_I_VALUE;
  memcpy(&r[LSM303D_REG_OUT_X_L_A | LSM303D_AUTO_INC], xyz, LSM303D_SAMPLE_LEN);
  return r;
}

/* Справний стенд у спокої без пристрою за адресою missing (0 — усі на місці):
 * MPU-6050 і LSM303D показують 1 g по Z, BMP180 — 21.5 °C / 1000.02 гПа,
 * L3GD20 і OV7670 з правильними ID, JEDEC і кадр DHT11 — із записів аналізатора. */
static void bench_without(uint8_t missing)
{
  static const uint8_t accel_1g_z[6] = { 0x10, 0x00, 0xF0, 0xFF, 0x09, 0x40 };   /* 0, 0, +1 g */

  if (missing != MPU6050_I2C_ADDR)
  {
    uint8_t *m = fake_i2c_add_device(MPU6050_I2C_ADDR);
    m[MPU6050_REG_WHO_AM_I] = MPU6050_WHO_AM_I_VALUE;
    m[MPU6050_REG_ACCEL_XOUT_H + 4] = 0x40;   /* AZ = 16384 = 1 g; T raw = 0 -> 36.5 °C */
  }
  if (missing != BMP180_I2C_ADDR)  { add_bmp180(); }
  if (missing != 0x6BU)            { fake_i2c_add_device(0x6BU)[0x0FU] = 0xD4U; }   /* L3GD20 */
  if (missing != LSM303D_I2C_ADDR) { add_lsm303d(accel_1g_z); }
  if (missing != 0x21U)                                                             /* OV7670 */
  {
    uint8_t *cam = fake_i2c_add_device(0x21U);
    cam[0x0AU] = 0x76U;
    cam[0x0BU] = 0x73U;
  }
  fake_platform.vsync_hz = 30U;
  set_jedec(cap_w25q64_jedec_miso);
  set_dht_frame(cap_dht11_read_frame);
}

static void good_bench(void)
{
  bench_without(0U);
}

/* Стан після devices_init() у App_Init() на справжній платі */
static void init_devices(void)
{
  App_Devices *d = &app_devices;

  d->mpu_status   = mpu6050_init(&d->mpu, Platform_SensorI2c(), MPU6050_I2C_ADDR);
  d->baro_status  = bmp180_init(&d->baro, Platform_SensorI2c(), Platform_DelayMs, 0U);
  d->flash_status = w25q64_init(&d->flash, Platform_FlashSpi(), Platform_DelayMs);
  d->accel_status = lsm303d_init(&d->accel, Platform_SensorI2c(), LSM303D_I2C_ADDR);
}

void setUp(void)
{
  fake_i2c_reset();
  mock_spi_reset();
  fake_platform_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  /* «ще не ініціалізовано»: інакше нульова структура означала б DEV_OK */
  app_devices.mpu_status   = DEV_ERR_STATE;
  app_devices.baro_status  = DEV_ERR_STATE;
  app_devices.flash_status = DEV_ERR_STATE;
  app_devices.accel_status = DEV_ERR_STATE;
  bmp_ut  = 28706;
  bmp_up  = 33413;
  n_lines = 0U;
  sink_ctx_ok = 0;
  SelfTest_SetOutput(sink, &n_lines);
}

void tearDown(void)
{
  SelfTest_SetOutput(NULL, NULL);
}

/* ============================================================================
 * 1. Форматування
 * ========================================================================== */

static void test_error_reason_strings(void)
{
  TEST_ASSERT_EQUAL_STRING("ok", StandTest_ErrStr(DEV_OK));
  TEST_ASSERT_EQUAL_STRING("nack", StandTest_ErrStr(DEV_ERR_NACK));
  TEST_ASSERT_EQUAL_STRING("timeout", StandTest_ErrStr(DEV_ERR_TIMEOUT));
  TEST_ASSERT_EQUAL_STRING("bus", StandTest_ErrStr(DEV_ERR_BUS));
  TEST_ASSERT_EQUAL_STRING("bad_id", StandTest_ErrStr(DEV_ERR_ID));
  TEST_ASSERT_EQUAL_STRING("state", StandTest_ErrStr(DEV_ERR_STATE));
  TEST_ASSERT_EQUAL_STRING("param", StandTest_ErrStr(DEV_ERR_PARAM));
  TEST_ASSERT_EQUAL_STRING("unknown", SelfTest_ErrorText((dev_status_t)99));
}

static void test_fixed_point_matches_uart(void)
{
  char b[24];

  /* Значення з UART-рядків uart_selftest.sr */
  StandTest_FormatFixed(b, sizeof(b), CAP_ST_BMP180_T_DC, 1);  TEST_ASSERT_EQUAL_STRING("20.7", b);
  StandTest_FormatFixed(b, sizeof(b), CAP_ST_BMP180_P_PA, 2);  TEST_ASSERT_EQUAL_STRING("1002.73", b);
  StandTest_FormatFixed(b, sizeof(b), 2938, 3);                TEST_ASSERT_EQUAL_STRING("2.938", b);
  StandTest_FormatFixed(b, sizeof(b), 808, 3);                 TEST_ASSERT_EQUAL_STRING("0.808", b);
  /* Знак і нулі попереду дробової частини */
  StandTest_FormatFixed(b, sizeof(b), -5, 3);                  TEST_ASSERT_EQUAL_STRING("-0.005", b);
  StandTest_FormatFixed(b, sizeof(b), -999, 3);                TEST_ASSERT_EQUAL_STRING("-0.999", b);
  StandTest_FormatFixed(b, sizeof(b), 0, 1);                   TEST_ASSERT_EQUAL_STRING("0.0", b);
  StandTest_FormatFixed(b, sizeof(b), -42, 0);                 TEST_ASSERT_EQUAL_STRING("-42", b);
}

static void test_inputs_snapshot(void)
{
  char b[64];

  /* Як у записі: кнопки відпущені (1), LOCK відпущена, двері закриті */
  StandTest_FormatInputs(b, sizeof(b), PLATFORM_IN_BTN1 | PLATFORM_IN_BTN2 | PLATFORM_IN_BTN3 |
                                       PLATFORM_IN_BTN4 | PLATFORM_IN_LOCK | PLATFORM_IN_DOOR);
  TEST_ASSERT_EQUAL_STRING("btn=1111,lock_b1=1,door=1,pir=0,mpu_int=0", b);
  /* Натиснуті червона і синя кнопки, B1 натиснута, рух PIR, INT від MPU */
  StandTest_FormatInputs(b, sizeof(b), PLATFORM_IN_BTN2 | PLATFORM_IN_BTN4 | PLATFORM_IN_DOOR |
                                       PLATFORM_IN_PIR | PLATFORM_IN_MPUINT);
  TEST_ASSERT_EQUAL_STRING("btn=0101,lock_b1=0,door=1,pir=1,mpu_int=1", b);
}

static void test_fail_reason_comes_from_error_not_fields(void)
{
  /* FAIL: err= додає SelfTest_FormatLine() з поля error, поля йдуть після нього */
  const self_test_device_t dht = { "DHT11", SELF_TEST_FAIL, DEV_ERR_STATE, "hint=crc" };
  /* SKIP: err= автоматично не додається — він частина полів */
  const self_test_device_t mpu = { "MPU6050", SELF_TEST_SKIP, DEV_ERR_NACK,
                                   "err=nack,note=GY-521_faulty_FR06_on_LSM303D" };
  char b[192];

  TEST_ASSERT_TRUE(SelfTest_FormatLine(b, sizeof(b), &dht));
  TEST_ASSERT_EQUAL_STRING("ST,DHT11,FAIL,err=state,hint=crc", b);
  TEST_ASSERT_TRUE(SelfTest_FormatLine(b, sizeof(b), &mpu));
  TEST_ASSERT_EQUAL_STRING("ST,MPU6050,SKIP,err=nack,note=GY-521_faulty_FR06_on_LSM303D", b);
}

static void test_fields_truncate_safely(void)
{
  self_test_device_t d;
  char small[12];

  good_bench();
  StandTest_CheckServo(&d, small, sizeof(small));
  TEST_ASSERT_EQUAL_STRING("freq_hz=50,", small);   /* обрізано, але з '\0' */
  TEST_ASSERT_EQUAL_PTR(small, d.fields);
}

static void test_output_errors(void)
{
  const self_test_device_t ok  = { "UART", SELF_TEST_OK, DEV_OK, NULL };
  const self_test_device_t bad = { "", SELF_TEST_OK, DEV_OK, NULL };

  /* Пристрій без назви: рядок не формується, звіт перериває ST,REPORT,FAIL */
  TEST_ASSERT_FALSE(StandTest_PrintOne(&bad));
  TEST_ASSERT_FALSE(SelfTest_Report(&bad, 1U));
  TEST_ASSERT_EQUAL_STRING("ST,START", line(0));
  TEST_ASSERT_EQUAL_STRING("ST,REPORT,FAIL,err=format", line(1));
  TEST_ASSERT_TRUE(sink_ctx_ok);   /* контекст SelfTest_SetOutput() доходить до виводу */

  /* Вивід не налаштовано: нічого не друкується */
  SelfTest_SetOutput(NULL, NULL);
  TEST_ASSERT_FALSE(SelfTest_Emit("x"));
  TEST_ASSERT_FALSE(StandTest_PrintOne(&ok));
  TEST_ASSERT_EQUAL_size_t(2U, n_lines);
}

/* ============================================================================
 * 2. Повний прогін на fake-стенді
 * ========================================================================== */

static void test_run_all_ok(void)
{
  good_bench();
  init_devices();
  TEST_ASSERT_TRUE(StandTest_Run());

  TEST_ASSERT_EQUAL_size_t(15U, n_lines);
  TEST_ASSERT_EQUAL_STRING("", line(0));
  TEST_ASSERT_EQUAL_STRING("# SmartLock selftest v2 (LR2)", line(1));
  TEST_ASSERT_EQUAL_STRING("I2C,scan,0x1D,0x21,0x68,0x6B,0x77", line(2));
  TEST_ASSERT_EQUAL_STRING("ST,START", line(3));
  TEST_ASSERT_EQUAL_STRING("ST,UART,OK,baud=115200", line(4));
  TEST_ASSERT_EQUAL_STRING("ST,MPU6050,OK,who=0x68,a_g=1.000,ax_g=0.000,ay_g=0.000,az_g=1.000,"
                           "gyro_max_dps=0.0,t_c=36.5", line(5));
  TEST_ASSERT_EQUAL_STRING("ST,BMP180,OK,id=0x55,t_c=21.5,p_hpa=1000.02", line(6));
  TEST_ASSERT_EQUAL_STRING("ST,L3GD20,OK,addr=0x6B,who=0xD4", line(7));
  TEST_ASSERT_EQUAL_STRING("ST,LSM303D,OK,addr=0x1D,who=0x49,role=FR06_accel,raw=0010,FFF0,4009,"
                           "a_g=0.999,ax_g=0.000,ay_g=0.000,az_g=0.999", line(8));   /* 16393 × 0,061 = 999,97 → до нуля */
  TEST_ASSERT_EQUAL_STRING("ST,W25Q64,OK,jedec=EF4017,size_mbit=64", line(9));
  TEST_ASSERT_EQUAL_STRING("ST,OV7670,OK,pid=0x76,ver=0x73,vsync_hz=30", line(10));
  TEST_ASSERT_EQUAL_STRING("ST,DHT11,OK,t_c=21.4,rh=35.2,raw=230215043E,crc=OK", line(11));
  TEST_ASSERT_EQUAL_STRING("ST,SERVO_PWM,OK,freq_hz=50,pulse_us=1500,note=no_feedback", line(12));
  TEST_ASSERT_EQUAL_STRING("ST,GPIO,OK,btn=1111,lock_b1=1,door=1,pir=0,mpu_int=0", line(13));
  TEST_ASSERT_EQUAL_STRING("ST,SUMMARY,OK,ok=10,warn=0,fail=0,skip=0", line(14));

  /* Тривалість: 1 с VSYNC + 200 мс перед DHT11 + 2 × 5 мс перетворення BMP180 */
  TEST_ASSERT_EQUAL_UINT32(1210U, fake_platform.delay_total_ms);
}

static void test_run_device_nack_fails_with_reason(void)
{
  bench_without(0x6BU);   /* L3GD20 не відповідає ні на 0x6B, ні на 0x6A */
  init_devices();
  TEST_ASSERT_FALSE(StandTest_Run());

  TEST_ASSERT_EQUAL_STRING("ST,L3GD20,FAIL,err=nack,addr=0x6B|0x6A", find_line("ST,L3GD20,"));
  TEST_ASSERT_EQUAL_STRING("ST,SUMMARY,FAIL,ok=9,warn=0,fail=1,skip=0", find_line("ST,SUMMARY,"));
}

static void test_run_bmp180_absent_nack(void)
{
  bench_without(BMP180_I2C_ADDR);
  init_devices();
  TEST_ASSERT_FALSE(StandTest_Run());

  TEST_ASSERT_EQUAL_STRING("ST,BMP180,FAIL,err=nack", find_line("ST,BMP180,"));
  TEST_ASSERT_EQUAL_STRING("I2C,scan,0x1D,0x21,0x68,0x6B", find_line("I2C,scan"));
}

static void test_run_wrong_id_fails(void)
{
  static const uint8_t miso_no_chip[4] = { 0xFF, 0xFF, 0xFF, 0xFF };   /* MISO не підключено */

  good_bench();
  fake_i2c_device_regs(BMP180_I2C_ADDR)[BMP180_REG_ID] = 0x58U;   /* BMP280 замість BMP180 */
  set_jedec(miso_no_chip);
  init_devices();
  TEST_ASSERT_FALSE(StandTest_Run());

  TEST_ASSERT_EQUAL_STRING("ST,BMP180,FAIL,err=bad_id", find_line("ST,BMP180,"));
  TEST_ASSERT_EQUAL_STRING("ST,W25Q64,FAIL,err=bad_id,jedec=FFFFFF", find_line("ST,W25Q64,"));
  TEST_ASSERT_EQUAL_STRING("ST,SUMMARY,FAIL,ok=8,warn=0,fail=2,skip=0", find_line("ST,SUMMARY,"));
}

static void test_run_unexpected_id_answering_is_warn(void)
{
  static const uint8_t miso_w25q32[4] = { 0xFF, 0xEF, 0x40, 0x16 };

  good_bench();
  fake_i2c_device_regs(0x6BU)[0x0FU] = 0x11U;    /* відповів, але не L3GD20 */
  fake_i2c_device_regs(0x21U)[0x0AU] = 0x77U;    /* камера з іншим PID */
  set_jedec(miso_w25q32);                         /* Winbond, але 32 Мбіт */
  init_devices();
  TEST_ASSERT_FALSE(StandTest_Run());

  TEST_ASSERT_EQUAL_STRING("ST,L3GD20,WARN,addr=0x6B,who=0x11", find_line("ST,L3GD20,"));
  TEST_ASSERT_EQUAL_STRING("ST,OV7670,WARN,pid=0x77,ver=0x73,vsync_hz=30", find_line("ST,OV7670,"));
  TEST_ASSERT_EQUAL_STRING("ST,W25Q64,WARN,jedec=EF4016,size_mbit=32,note=not_q64", find_line("ST,W25Q64,"));
  TEST_ASSERT_EQUAL_STRING("ST,SUMMARY,WARN,ok=7,warn=3,fail=0,skip=0", find_line("ST,SUMMARY,"));
}

static void test_run_timeouts(void)
{
  good_bench();
  init_devices();
  /* Транзакції прогону: MPU 1, BMP180 4, L3GD20 1, LSM303D 1 -> таймаут на LSM303D.
   * SPI: кадр 0 — w25q64_init, кадр 1 — JEDEC у self-test. DHT11 мовчить. */
  fake_i2c_fail_at(fake_i2c_log_count() + 6U, DEV_ERR_TIMEOUT);
  mock_spi_fail_in_frame(1U, DEV_ERR_TIMEOUT);
  fake_platform.dht_status = DEV_ERR_TIMEOUT;
  TEST_ASSERT_FALSE(StandTest_Run());

  TEST_ASSERT_EQUAL_STRING("ST,LSM303D,FAIL,err=timeout", find_line("ST,LSM303D,"));
  TEST_ASSERT_EQUAL_STRING("ST,W25Q64,FAIL,err=timeout,jedec=000000", find_line("ST,W25Q64,"));
  TEST_ASSERT_EQUAL_STRING("ST,DHT11,FAIL,err=timeout", find_line("ST,DHT11,"));
  TEST_ASSERT_EQUAL_STRING("ST,SUMMARY,FAIL,ok=7,warn=0,fail=3,skip=0", find_line("ST,SUMMARY,"));
  TEST_ASSERT_FALSE(mock_spi_cs_active());
}

static void test_run_dht11_bad_checksum(void)
{
  static const uint8_t bad_crc[5] = { 0x22, 0x09, 0x15, 0x05, 0x46 };

  good_bench();
  set_dht_frame(bad_crc);
  init_devices();
  TEST_ASSERT_FALSE(StandTest_Run());
  TEST_ASSERT_EQUAL_STRING("ST,DHT11,FAIL,err=state,hint=crc", find_line("ST,DHT11,"));
}

static void test_run_mpu_absent_is_skip_not_fail(void)
{
  bench_without(MPU6050_I2C_ADDR);
  init_devices();
  TEST_ASSERT_FALSE(StandTest_Run());   /* true — лише якщо все OK */

  TEST_ASSERT_EQUAL_STRING("ST,MPU6050,SKIP,err=nack,note=GY-521_faulty_FR06_on_LSM303D", find_line("ST,MPU6050,"));
  /* Правило self_test.c: SKIP без FAIL дає загальний WARN, не FAIL */
  TEST_ASSERT_EQUAL_STRING("ST,SUMMARY,WARN,ok=9,warn=0,fail=0,skip=1", find_line("ST,SUMMARY,"));
}

static void test_run_camera_and_servo_faults(void)
{
  bench_without(0x21U);
  fake_platform.servo_freq_hz = 0U;   /* ШІМ не запущено */
  init_devices();
  TEST_ASSERT_FALSE(StandTest_Run());

  TEST_ASSERT_EQUAL_STRING("ST,OV7670,FAIL,err=nack,vsync_hz=30,hint=check_MCLK_D7_PWDN0_RST1", find_line("ST,OV7670,"));
  TEST_ASSERT_EQUAL_STRING("ST,SERVO_PWM,FAIL,err=state,freq_hz=0,pulse_us=1500,note=no_feedback", find_line("ST,SERVO_PWM,"));
}

static void test_camera_without_vsync_is_warn(void)
{
  good_bench();
  fake_platform.vsync_hz = 0U;   /* ID читається, але кадрів немає (немає XCLK / PWDN) */
  TEST_ASSERT_EQUAL(SELF_TEST_WARN, StandTest_RunOne(StandTest_CheckCamera));
  TEST_ASSERT_EQUAL_size_t(1U, n_lines);
  TEST_ASSERT_EQUAL_STRING("ST,OV7670,WARN,pid=0x76,ver=0x73,vsync_hz=0", line(0));
}

static void test_accel_falls_back_to_alt_address(void)
{
  static const uint8_t minus_1g_z[6] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0xC0 };
  uint8_t *r = fake_i2c_add_device(LSM303D_I2C_ADDR_ALT);   /* SA0 = 0 */

  r[LSM303D_REG_WHO_AM_I] = LSM303D_WHO_AM_I_VALUE;
  memcpy(&r[LSM303D_REG_OUT_X_L_A | LSM303D_AUTO_INC], minus_1g_z, sizeof(minus_1g_z));
  TEST_ASSERT_EQUAL(SELF_TEST_OK, check(StandTest_CheckAccel));
  TEST_ASSERT_EQUAL_STRING("ST,LSM303D,OK,addr=0x1E,who=0x49,role=FR06_accel,raw=0001,0000,C000,"
                           "a_g=0.999,ax_g=0.000,ay_g=0.000,az_g=-0.999", one_line);
}

static void test_mpu_moving_is_warn(void)
{
  uint8_t *m = fake_i2c_add_device(MPU6050_I2C_ADDR);

  m[MPU6050_REG_WHO_AM_I] = MPU6050_WHO_AM_I_VALUE;
  m[MPU6050_REG_ACCEL_XOUT_H + 4] = 0x40;   /* 1 g */
  m[0x43] = 0x04;                           /* GX = 1024 LSB = 7.8 °/с > 5 °/с */
  TEST_ASSERT_EQUAL(SELF_TEST_WARN, check(StandTest_CheckMpu));
  TEST_ASSERT_EQUAL_STRING("ST,MPU6050,WARN,who=0x68,a_g=1.000,ax_g=0.000,ay_g=0.000,az_g=1.000,"
                           "gyro_max_dps=7.8,t_c=36.5", one_line);
}

static void test_mpu_gyro_axes_and_bmp_out_of_range(void)
{
  uint8_t *m = fake_i2c_add_device(MPU6050_I2C_ADDR);

  m[MPU6050_REG_WHO_AM_I] = MPU6050_WHO_AM_I_VALUE;
  m[MPU6050_REG_ACCEL_XOUT_H + 4] = 0x40;
  m[0x45] = 0x01;   /* GY = 256 LSB */
  m[0x47] = 0x02;   /* GZ = 512 LSB = 3.9 °/с — найбільший, але < 5 °/с */
  TEST_ASSERT_EQUAL(SELF_TEST_OK, check(StandTest_CheckMpu));
  TEST_ASSERT_NOT_NULL(strstr(one_line, ",gyro_max_dps=3.9,"));

  add_bmp180();
  bmp_ut = 20000;   /* холодно: поза 15–35 °C */
  TEST_ASSERT_EQUAL(SELF_TEST_WARN, check(StandTest_CheckBmp));
  TEST_ASSERT_EQUAL_STRING_LEN("ST,BMP180,WARN,id=0x55,t_c=", one_line, 27);
}

static void test_dht11_out_of_range_is_warn(void)
{
  static const uint8_t hot[5] = { 0x05, 0x00, 0x2D, 0x00, 0x32 };   /* 5 %RH, 45 °C */

  set_dht_frame(hot);
  TEST_ASSERT_EQUAL(SELF_TEST_WARN, check(StandTest_CheckDht11));
  TEST_ASSERT_EQUAL_STRING("ST,DHT11,WARN,t_c=45.0,rh=5.0,raw=05002D0032,crc=OK", one_line);
}

/* ============================================================================
 * 3. Replay uart_selftest.sr
 * ========================================================================== */

/* Fake-пристрої віддають байти з I2C-трейсу запису; результат порівнюється з
 * UART-рядками того самого прогону. Очікувані розбіжності з записом:
 *   - формат self_test.c: після скану I2C іде рядок ST,START, а ST,UART —
 *     після нього (на записі ST,UART стояв перед сканом);
 *   - флеш-пам'ять тепер зветься W25Q64 (на записі — W25QXX), поля ті самі;
 *   - BMP180: калібрування датчика в запис не потрапило (App_Init до запису), тут —
 *     коефіцієнти з datasheet, тому значення інші; байти транзакцій — як на записі;
 *     через це BMP180 тут WARN, і SUMMARY = ok=7,warn=2 замість ok=8,warn=1
 *     (загальний WARN — і за правилом self_test.c, бо є SKIP);
 *   - DHT11: запис зроблено до коміту 79d2c32 (rh без десятих): rh=34 -> rh=34.9. */
static void test_replay_uart_selftest_capture(void)
{
  bmp_ut = CAP_ST_BMP180_UT;
  bmp_up = CAP_ST_BMP180_UP;
  add_bmp180();
  add_lsm303d(&cap_st_lsm303d_xyz_wire[CAP_ST_LSM303D_XYZ_OFFSET]);
  fake_i2c_add_device(0x6BU)[0x0FU] = cap_st_l3gd20_who_wire[3];
  uint8_t *cam = fake_i2c_add_device(0x21U);
  cam[0x0AU] = CAP_ST_OV7670_PID;
  cam[0x0BU] = CAP_ST_OV7670_VER;
  fake_platform.vsync_hz = 30U;   /* VSYNC 30.01 Гц на cam_vsync_glitch.sr */
  set_jedec(cap_w25q64_jedec_miso);
  set_dht_frame(cap_st_dht11_frame);
  init_devices();
  size_t first = fake_i2c_log_count();

  (void)StandTest_Run();

  /* I2C-транзакції self-test — ті самі байти й порядок, що на записі */
  static const struct { const uint8_t *wire; size_t len; dev_status_t res; } exp[] = {
    { cap_st_mpu6050_probe_wire,   sizeof(cap_st_mpu6050_probe_wire),   DEV_ERR_NACK },
    { cap_st_bmp180_ut_cmd_wire,   sizeof(cap_st_bmp180_ut_cmd_wire),   DEV_OK },
    { cap_st_bmp180_ut_read_wire,  sizeof(cap_st_bmp180_ut_read_wire),  DEV_OK },
    { cap_st_bmp180_up_cmd_wire,   sizeof(cap_st_bmp180_up_cmd_wire),   DEV_OK },
    { cap_st_bmp180_up_read_wire,  sizeof(cap_st_bmp180_up_read_wire),  DEV_OK },
    { cap_st_l3gd20_who_wire,      sizeof(cap_st_l3gd20_who_wire),      DEV_OK },
    { cap_st_lsm303d_xyz_wire,     sizeof(cap_st_lsm303d_xyz_wire),     DEV_OK },
  };
  TEST_ASSERT_EQUAL_size_t(first + sizeof(exp) / sizeof(exp[0]), fake_i2c_log_count());
  for (size_t i = 0U; i < sizeof(exp) / sizeof(exp[0]); i++)
  {
    const fake_i2c_xfer_t *x = fake_i2c_log(first + i);
    TEST_ASSERT_EQUAL(exp[i].res, x->result);
    TEST_ASSERT_EQUAL_size_t(exp[i].len, x->wire_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp[i].wire, x->wire, exp[i].len);
  }

  /* UART: рядки запису (індекс k) -> рядки цього прогону.
   * Тут: 0 "", 1 заголовок, 2 скан I2C, 3 ST,START, 4 ST,UART, 5.. пристрої */
  static const size_t map[CAP_ST_UART_LINES] = { 1, 4, 2, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
  TEST_ASSERT_EQUAL_size_t(15U, n_lines);
  TEST_ASSERT_EQUAL_STRING("", line(0));
  TEST_ASSERT_EQUAL_STRING("ST,START", line(3));
  for (size_t k = 0U; k < CAP_ST_UART_LINES; k++)
  {
    const char *got = line(map[k]);
    if (k == CAP_ST_UART_IDX_BMP180)
    {
      TEST_ASSERT_EQUAL_STRING_LEN("ST,BMP180,", got, 10);
    }
    else if (k == CAP_ST_UART_IDX_DHT11)
    {
      TEST_ASSERT_EQUAL_STRING("ST,DHT11,OK,t_c=21.5,rh=34.9,raw=2209150545,crc=OK", got);
    }
    else if (k == CAP_ST_UART_LINES - 1U)
    {
      TEST_ASSERT_EQUAL_STRING("ST,SUMMARY,WARN,ok=7,warn=2,fail=0,skip=1", got);
    }
    else if (strncmp(cap_st_uart_lines[k], "ST,W25QXX,", 10) == 0)
    {
      TEST_ASSERT_EQUAL_STRING_LEN("ST,W25Q64,", got, 10);
      TEST_ASSERT_EQUAL_STRING(&cap_st_uart_lines[k][10], &got[10]);
    }
    else
    {
      TEST_ASSERT_EQUAL_STRING(cap_st_uart_lines[k], got);
    }
  }
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_error_reason_strings);
  RUN_TEST(test_fixed_point_matches_uart);
  RUN_TEST(test_inputs_snapshot);
  RUN_TEST(test_fail_reason_comes_from_error_not_fields);
  RUN_TEST(test_fields_truncate_safely);
  RUN_TEST(test_output_errors);
  RUN_TEST(test_run_all_ok);
  RUN_TEST(test_run_device_nack_fails_with_reason);
  RUN_TEST(test_run_bmp180_absent_nack);
  RUN_TEST(test_run_wrong_id_fails);
  RUN_TEST(test_run_unexpected_id_answering_is_warn);
  RUN_TEST(test_run_timeouts);
  RUN_TEST(test_run_dht11_bad_checksum);
  RUN_TEST(test_run_mpu_absent_is_skip_not_fail);
  RUN_TEST(test_run_camera_and_servo_faults);
  RUN_TEST(test_camera_without_vsync_is_warn);
  RUN_TEST(test_accel_falls_back_to_alt_address);
  RUN_TEST(test_mpu_moving_is_warn);
  RUN_TEST(test_mpu_gyro_axes_and_bmp_out_of_range);
  RUN_TEST(test_dht11_out_of_range_is_warn);
  RUN_TEST(test_replay_uart_selftest_capture);
  return UNITY_END();
}
