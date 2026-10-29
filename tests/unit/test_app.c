/**
  * @file    test_app.c
  * @brief   App_Init/App_Loop (app.c) і UART-команди стенда (bench.c) без плати.
  *
  *          bench.c друкує через printf, тому тест на час виконання команди
  *          перенаправляє stdout (дескриптор 1) у тимчасовий файл і потім
  *          порівнює текст. ST-рядки йдуть через вивід self_test.c: App_Init()
  *          налаштовує його на printf("%s\n") (як на платі), а для окремих
  *          команд setUp() ставить такий самий вивід. Тому ST-рядки
  *          закінчуються "\n", а службові рядки команд — "\r\n".
  *          Залізо — fake_platform, fake_i2c і mock_spi, як у test_stand_test.c.
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "app.h"
#include "bench.h"
#include "capture_vectors.h"
#include "fake_i2c.h"
#include "fake_platform.h"
#include "mock_spi.h"
#include "platform.h"
#include "self_test.h"

/* ---- перехоплення stdout ------------------------------------------------------- */
static char out[16384];

/* Виконати fn з stdout у файлі; між початком і кінцем не можна викликати TEST_ASSERT */
static const char *capture(void (*fn)(void))
{
  FILE *tmp = tmpfile();
  int saved;
  size_t n = 0U;

  out[0] = '\0';
  if (tmp == NULL) { return out; }
  fflush(stdout);
  saved = dup(1);
  dup2(fileno(tmp), 1);
  fn();
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  rewind(tmp);
  n = fread(out, 1U, sizeof(out) - 1U, tmp);
  out[n] = '\0';
  fclose(tmp);
  return out;
}

/* Подати рядок символів у «UART» і опитувати Bench_Poll(), поки не вичерпається */
static const char *uart_cmds;

static void poll_all(void)
{
  fake_platform.uart_rx  = uart_cmds;
  fake_platform.uart_pos = 0U;
  while (fake_platform.uart_rx[fake_platform.uart_pos] != '\0')
  {
    Bench_Poll();
  }
}

static const char *run(const char *cmds)
{
  uart_cmds = cmds;
  return capture(poll_all);
}

/* Вивід ST-рядків, як self_test_uart_output() в app.c */
static void stdout_line(const char *line, void *ctx)
{
  (void)ctx;
  printf("%s\n", line);
}

/* ---- fake-стенд: стан запису uart_selftest.sr (без MPU-6050) -------------------- */
static void bench(void)
{
  uint8_t *r = fake_i2c_add_device(LSM303D_I2C_ADDR);
  r[LSM303D_REG_WHO_AM_I] = LSM303D_WHO_AM_I_VALUE;
  memcpy(&r[LSM303D_REG_OUT_X_L_A | LSM303D_AUTO_INC], &cap_st_lsm303d_xyz_wire[CAP_ST_LSM303D_XYZ_OFFSET], 6U);
  fake_i2c_add_device(0x6BU)[0x0FU] = 0xD4U;
  uint8_t *cam = fake_i2c_add_device(0x21U);
  cam[0x0AU] = 0x76U;
  cam[0x0BU] = 0x73U;
  fake_platform.vsync_hz = 30U;
  mock_spi_set_miso(0U, cap_w25q64_jedec_miso, sizeof(cap_w25q64_jedec_miso));
  mock_spi_set_miso(1U, cap_w25q64_jedec_miso, sizeof(cap_w25q64_jedec_miso));
}

static void init_only(void)
{
  App_Devices *d = &app_devices;

  d->flash_status = w25q64_init(&d->flash, Platform_FlashSpi(), Platform_DelayMs);
  d->accel_status = lsm303d_init(&d->accel, Platform_SensorI2c(), LSM303D_I2C_ADDR);
  d->mpu_status   = DEV_ERR_NACK;
  d->baro_status  = DEV_ERR_NACK;
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
  SelfTest_SetOutput(stdout_line, NULL);
  bench();
}

void tearDown(void)
{
}

/* ============================================================================ */

static void test_app_init_brings_up_and_runs_selftest(void)
{
  SelfTest_SetOutput(NULL, NULL);   /* вивід налаштовує сам App_Init() */
  const char *o = capture(App_Init);

  TEST_ASSERT_EQUAL_UINT(1U, fake_platform.bringup_calls);
  TEST_ASSERT_EQUAL(DEV_OK, app_devices.flash_status);
  TEST_ASSERT_EQUAL(DEV_OK, app_devices.accel_status);
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, app_devices.mpu_status);
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, app_devices.baro_status);
  /* Self-test, підсумок і довідка — в одному потоці, у порядку плати */
  TEST_ASSERT_EQUAL_STRING_LEN("\n# SmartLock selftest v2 (LR2)\nI2C,scan,0x1D,0x21,0x6B\n"
                               "ST,START\nST,UART,OK,baud=115200\n", o, 79);
  TEST_ASSERT_NOT_NULL(strstr(o, "ST,MPU6050,SKIP,err=nack,note=GY-521_faulty_FR06_on_LSM303D\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "ST,BMP180,FAIL,err=nack\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "ST,SUMMARY,FAIL,ok=6,warn=1,fail=2,skip=1\n# commands: s selftest"));
}

static void app_loop_once(void)
{
  App_Loop();
}

static void test_app_loop_polls_platform_and_uart(void)
{
  fake_platform.uart_rx = "x";
  const char *o = capture(app_loop_once);

  TEST_ASSERT_EQUAL_UINT(1U, fake_platform.poll_calls);
  TEST_ASSERT_EQUAL_STRING("I2C,nack_demo,addr=0x50,result=nack\r\n", o);
}

static void test_help_and_unknown(void)
{
  const char *o = run("?z\x01\r\n ");

  TEST_ASSERT_EQUAL_STRING(
    "# commands: s selftest | i i2c scan | g gpio watch on/off | m accel x10 (LSM303D, FR-06) | b bmp180\r\n"
    "#           h dht11 x5 | j jedec | f flash r/w | 1/2/3 servo 1000/1500/2000us\r\n"
    "#           a accel diag (LSM303D) | c camera id | k camera slow PCLK on/off | x NACK demo | l LR1 loop traffic | ? help\r\n"
    "# unknown 'z', ? for help\r\n"
    "# unknown '.', ? for help\r\n", o);
}

static void test_scan_jedec_camera(void)
{
  init_only();
  const char *o = run("ijc");

  TEST_ASSERT_EQUAL_STRING("I2C,scan,0x1D,0x21,0x6B\n"
                           "SPI,jedec=EF4017,err=ok\r\n"
                           "ST,OV7670,OK,pid=0x76,ver=0x73,vsync_hz=30\n", o);
}

static void test_servo_commands_set_pulse(void)
{
  const char *o = run("13");

  TEST_ASSERT_EQUAL_STRING("SERVO,pulse_us=1000,freq_hz=50\r\nSERVO,pulse_us=2000,freq_hz=50\r\n", o);
  TEST_ASSERT_EQUAL_UINT32(2000U, fake_platform.servo_pulse_us);
  run("2");
  TEST_ASSERT_EQUAL_UINT32(1500U, fake_platform.servo_pulse_us);
}

static void test_camera_clkrc_toggle(void)
{
  const char *o = run("k");

  TEST_ASSERT_EQUAL_STRING("CAM,slow_pclk=1,clkrc=0x1F,err=ok\r\n", o);
  TEST_ASSERT_EQUAL_HEX8(0x11U, fake_platform.sccb_last_reg);
  TEST_ASSERT_EQUAL_HEX8(0x1FU, fake_platform.sccb_last_val);
  o = run("k");
  TEST_ASSERT_EQUAL_STRING("CAM,slow_pclk=0,clkrc=0x80,err=ok\r\n", o);
  TEST_ASSERT_EQUAL_HEX8(0x80U, fake_platform.sccb_last_val);
}

static void test_nack_demo_and_loop_traffic(void)
{
  const char *o = run("xl");

  TEST_ASSERT_EQUAL_STRING("I2C,nack_demo,addr=0x50,result=nack\r\n"
                           "# LR1 loop traffic on (MPU probe + JEDEC every 500 ms)\r\n", o);
  TEST_ASSERT_EQUAL_INT(1, fake_platform.loop_traffic);
  /* на шині — лише адресний байт A0 і NACK, як на i2c_nack_0x50.sr */
  const fake_i2c_xfer_t *x = fake_i2c_log(fake_i2c_log_count() - 1U);
  TEST_ASSERT_EQUAL_HEX8(0xA0, x->wire[0]);
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, x->result);
  run("l");
  TEST_ASSERT_EQUAL_INT(0, fake_platform.loop_traffic);
}

static void poll_twice(void)
{
  Bench_Poll();
  Bench_Poll();
}

static void test_gpio_watch_prints_stable_change_only(void)
{
  const char *o = run("g");

  TEST_ASSERT_EQUAL_STRING("# gpio watch on (prints on change)\r\n", o);
  /* Перший виклик після ввімкнення друкує поточний стан (gpio_last = ~0) */
  fake_platform.now_ms = 1000U;
  o = capture(poll_twice);
  TEST_ASSERT_EQUAL_STRING("[1000] GPIO,btn=1111,lock_b1=1,door=1,pir=0,mpu_int=0\r\n", o);

  /* Натиснута червона кнопка: друк лише після 20 мс стабільного стану */
  fake_platform.inputs &= ~PLATFORM_IN_BTN1;
  fake_platform.now_ms = 1005U;
  o = capture(poll_twice);
  TEST_ASSERT_EQUAL_STRING("", o);
  fake_platform.now_ms = 1020U;
  o = capture(poll_twice);
  TEST_ASSERT_EQUAL_STRING("[1020] GPIO,btn=0111,lock_b1=1,door=1,pir=0,mpu_int=0\r\n", o);
  run("g");
}

static void test_accel_repeat_and_diag(void)
{
  init_only();
  const char *o = run("m");
  const char *l = "ST,LSM303D,WARN,addr=0x1D,who=0x49,role=FR06_accel,raw=7FF8,7FF8,33CD,"
                  "a_g=2.938,ax_g=1.998,ay_g=1.998,az_g=0.808\n";
  TEST_ASSERT_EQUAL_size_t(10U * strlen(l), strlen(o));
  TEST_ASSERT_EQUAL_STRING_LEN(l, o, strlen(l));

  o = run("a");
  TEST_ASSERT_EQUAL_STRING_LEN("ACC,dump,1F=00,20=5F,21=00,", o, 27);
  TEST_ASSERT_NOT_NULL(strstr(o, "ACC,before,raw=7FF8,7FF8,33CD,x_g=1.998,y_g=1.998,z_g=0.808\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "ACC,boot,err=ok,init=ok\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "ACC,selftest_off,raw=7FF8,7FF8,33CD,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "# self-test: each axis must shift"));
  /* self-test LSM303D вимкнено назад: CTRL2 = ±2 g */
  TEST_ASSERT_EQUAL_HEX8(LSM303D_CTRL2_2G, fake_i2c_device_regs(LSM303D_I2C_ADDR)[LSM303D_REG_CTRL2]);
}

static void test_accel_diag_read_error(void)
{
  init_only();
  fake_i2c_reset();   /* LSM303D зник після init */
  const char *o = run("a");
  TEST_ASSERT_NOT_NULL(strstr(o, "ACC,before,err=nack\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "ACC,boot,err=nack,init=nack\r\n"));
}

static void test_bmp_dht_mpu_commands_report_failures(void)
{
  init_only();
  const char *o = run("bh");

  TEST_ASSERT_EQUAL_STRING("ST,BMP180,FAIL,err=nack\n"
                           "DHT,FAIL,err=timeout\r\nDHT,FAIL,err=timeout\r\nDHT,FAIL,err=timeout\r\n"
                           "DHT,FAIL,err=timeout\r\nDHT,FAIL,err=timeout\r\n", o);
  /* 5 читань з інтервалом 2 с (після останнього паузи немає) */
  TEST_ASSERT_EQUAL_UINT32(4U * 2000U, fake_platform.delay_total_ms);
}

static void test_accel_repeat_includes_mpu_when_present(void)
{
  uint8_t *m = fake_i2c_add_device(MPU6050_I2C_ADDR);

  m[MPU6050_REG_WHO_AM_I] = MPU6050_WHO_AM_I_VALUE;
  m[MPU6050_REG_ACCEL_XOUT_H + 4] = 0x40;
  init_only();
  app_devices.mpu_status = mpu6050_init(&app_devices.mpu, Platform_SensorI2c(), MPU6050_I2C_ADDR);
  const char *o = run("m");
  TEST_ASSERT_NOT_NULL(strstr(o, "az_g=0.808\nST,MPU6050,OK,who=0x68,a_g=1.000,"));
}

static void test_dht_command_ok_line(void)
{
  static const uint8_t f[5] = { 0x23, 0x02, 0x15, 0x04, 0x3E };

  for (unsigned i = 0U; i < 40U; i++)
  {
    fake_platform.dht_high_us[i] = (f[i / 8U] & (0x80U >> (i % 8U))) ? 72U : 24U;
  }
  fake_platform.dht_status = DEV_OK;
  const char *o = run("h");
  const char *l = "DHT,OK,t_c=21.4,rh=35.2,raw=230215043E,crc=OK\r\n";
  TEST_ASSERT_EQUAL_size_t(5U * strlen(l), strlen(o));
  TEST_ASSERT_EQUAL_STRING_LEN(l, o, strlen(l));
}

static void test_flash_rw_pass(void)
{
  static const uint8_t sr_wel[]   = { 0xFF, W25Q64_SR1_WEL };
  static const uint8_t sr_ready[] = { 0xFF, 0x00 };
  static uint8_t read_frame[4U + W25Q64_PAGE_SIZE];

  init_only();
  /* Кадри: 1 JEDEC; стирання 2 WREN, 3 RDSR, 4 SE, 5 RDSR; запис 6 WREN, 7 RDSR,
   * 8 PP, 9 RDSR; 10 READ. Шаблон запису: i ^ (Millis() >> 3) ^ 0xA5, Millis() = 0 */
  memset(read_frame, 0xFF, 4U);
  for (unsigned i = 0U; i < W25Q64_PAGE_SIZE; i++) { read_frame[4U + i] = (uint8_t)(i ^ 0xA5U); }
  mock_spi_set_miso(3U, sr_wel, sizeof(sr_wel));
  mock_spi_set_miso(5U, sr_ready, sizeof(sr_ready));
  mock_spi_set_miso(7U, sr_wel, sizeof(sr_wel));
  mock_spi_set_miso(9U, sr_ready, sizeof(sr_ready));
  mock_spi_set_miso(10U, read_frame, sizeof(read_frame));
  fake_platform.now_ms = 0U;

  const char *o = run("f");
  TEST_ASSERT_EQUAL_STRING("FLASH,rw=PASS,addr=0x7FF000,bytes=256,mismatch=0,first=A5A4A7A6\r\n", o);
  /* стирався саме останній сектор: 20 7F F0 00 */
  static const uint8_t se[] = { 0x20, 0x7F, 0xF0, 0x00 };
  TEST_ASSERT_EQUAL_HEX8_ARRAY(se, mock_spi_frame(4U)->mosi, sizeof(se));
}

static void test_flash_rw_mismatch_and_error(void)
{
  static const uint8_t sr_wel[]   = { 0xFF, W25Q64_SR1_WEL };
  static const uint8_t sr_ready[] = { 0xFF, 0x00 };

  init_only();
  mock_spi_set_miso(3U, sr_wel, sizeof(sr_wel));
  mock_spi_set_miso(5U, sr_ready, sizeof(sr_ready));
  mock_spi_set_miso(7U, sr_wel, sizeof(sr_wel));
  mock_spi_set_miso(9U, sr_ready, sizeof(sr_ready));
  /* кадр 10 без сценарію: MISO = FF — «запис не відбувся» */
  const char *o = run("f");
  /* збігся лише байт 0x5A ^ 0xA5 = 0xFF */
  TEST_ASSERT_EQUAL_STRING("FLASH,rw=FAIL,addr=0x7FF000,bytes=256,mismatch=255,first=FFFFFFFF\r\n", o);

  setUp();
  init_only();
  /* кадр 3 (RDSR після WREN) без WEL: /WP активний */
  mock_spi_set_miso(3U, sr_ready, sizeof(sr_ready));
  o = run("f");
  TEST_ASSERT_EQUAL_STRING("FLASH,rw=FAIL,addr=0x7FF000,err=state\r\n", o);
}

static void test_selftest_command(void)
{
  init_only();
  const char *o = run("s");

  TEST_ASSERT_EQUAL_STRING_LEN("\n# SmartLock selftest v2 (LR2)\n", o, 31);
  TEST_ASSERT_NOT_NULL(strstr(o, "ST,LSM303D,WARN,addr=0x1D"));
  TEST_ASSERT_NOT_NULL(strstr(o, "ST,SUMMARY,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "| ? help\r\n"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_app_init_brings_up_and_runs_selftest);
  RUN_TEST(test_app_loop_polls_platform_and_uart);
  RUN_TEST(test_help_and_unknown);
  RUN_TEST(test_scan_jedec_camera);
  RUN_TEST(test_servo_commands_set_pulse);
  RUN_TEST(test_camera_clkrc_toggle);
  RUN_TEST(test_nack_demo_and_loop_traffic);
  RUN_TEST(test_gpio_watch_prints_stable_change_only);
  RUN_TEST(test_accel_repeat_and_diag);
  RUN_TEST(test_accel_diag_read_error);
  RUN_TEST(test_bmp_dht_mpu_commands_report_failures);
  RUN_TEST(test_accel_repeat_includes_mpu_when_present);
  RUN_TEST(test_dht_command_ok_line);
  RUN_TEST(test_flash_rw_pass);
  RUN_TEST(test_flash_rw_mismatch_and_error);
  RUN_TEST(test_selftest_command);
  return UNITY_END();
}
