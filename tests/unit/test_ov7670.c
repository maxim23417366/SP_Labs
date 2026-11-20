/**
  * @file    test_ov7670.c
  * @brief   Драйвер OV7670 (Devices/ov7670.c): PID/VER, порядок запису регістрів,
  *          обробка помилок SCCB. Шина SCCB — локальна модель: 256 регістрів і
  *          журнал записів (адреса, регістр, значення).
  */
#include <string.h>

#include "unity.h"
#include "ov7670.h"

#define LOG_MAX 256U

static uint8_t      regs[256];
static int          present;
static unsigned     xfers;           /* лічильник транзакцій (читання і записи) */
static int          fail_at;         /* номер транзакції з помилкою, -1 — без помилок */
static dev_status_t fail_status;
static struct { uint8_t addr, reg, val; } wlog[LOG_MAX];
static unsigned     wlog_n;
static unsigned     delay_calls;
static uint32_t     delay_last;

static dev_status_t check_xfer(uint8_t addr7)
{
  unsigned n = xfers++;

  if (!present || (addr7 != OV7670_I2C_ADDR))
  {
    return DEV_ERR_NACK;
  }
  if ((fail_at >= 0) && (n == (unsigned)fail_at))
  {
    return fail_status;
  }
  return DEV_OK;
}

static dev_status_t sccb_read(uint8_t addr7, uint8_t reg, uint8_t *val)
{
  dev_status_t st = check_xfer(addr7);

  if (st == DEV_OK)
  {
    *val = regs[reg];
  }
  return st;
}

static dev_status_t sccb_write(uint8_t addr7, uint8_t reg, uint8_t val)
{
  dev_status_t st = check_xfer(addr7);

  if (st == DEV_OK)
  {
    regs[reg] = val;
    if (wlog_n < LOG_MAX)
    {
      wlog[wlog_n].addr = addr7;
      wlog[wlog_n].reg = reg;
      wlog[wlog_n].val = val;
      wlog_n++;
    }
  }
  return st;
}

static void delay_ms(uint32_t ms)
{
  delay_calls++;
  delay_last = ms;
}

void setUp(void)
{
  memset(regs, 0, sizeof(regs));
  regs[OV7670_REG_PID] = OV7670_PID_VALUE;
  regs[OV7670_REG_VER] = OV7670_VER_VALUE;
  present = 1;
  xfers = 0U;
  fail_at = -1;
  fail_status = DEV_ERR_TIMEOUT;
  wlog_n = 0U;
  delay_calls = 0U;
  delay_last = 0U;
}

void tearDown(void)
{
}

static void test_init_reads_pid_ver(void)
{
  ov7670_t cam;

  TEST_ASSERT_EQUAL(DEV_OK, ov7670_init(&cam, sccb_read, sccb_write, delay_ms));
  TEST_ASSERT_EQUAL_HEX8(0x76U, cam.pid);
  TEST_ASSERT_EQUAL_HEX8(0x73U, cam.ver);
  TEST_ASSERT_EQUAL_HEX8(0x21U, cam.addr7);
  TEST_ASSERT_EQUAL_UINT(2U, xfers);
  TEST_ASSERT_EQUAL_UINT(0U, wlog_n);   /* ідентифікація нічого не пише */
}

static void test_init_wrong_id_and_missing(void)
{
  ov7670_t cam;

  regs[OV7670_REG_VER] = 0x70U;   /* інша ревізія — не наша камера */
  TEST_ASSERT_EQUAL(DEV_ERR_ID, ov7670_init(&cam, sccb_read, sccb_write, delay_ms));
  present = 0;
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, ov7670_init(&cam, sccb_read, sccb_write, delay_ms));
  setUp();
  fail_at = 1;   /* PID прочитано, VER — ні */
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, ov7670_init(&cam, sccb_read, sccb_write, delay_ms));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_init(NULL, sccb_read, sccb_write, delay_ms));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_init(&cam, NULL, sccb_write, delay_ms));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_init(&cam, sccb_read, NULL, delay_ms));
}

static void test_configure_order_reset_clkrc_table(void)
{
  ov7670_t cam;
  size_t n = 0U;
  const ov7670_reg_t *t = ov7670_qqvga_rgb565_table(&n);
  uint8_t failed = 0xEEU;

  TEST_ASSERT_EQUAL(DEV_OK, ov7670_init(&cam, sccb_read, sccb_write, delay_ms));
  TEST_ASSERT_TRUE(n >= 10U);
  TEST_ASSERT_EQUAL(DEV_OK, ov7670_configure(&cam, t, n, 0xCFU, &failed));
  TEST_ASSERT_EQUAL_HEX8(0x00U, failed);

  /* 1) скидання, 2) пауза, 3) CLKRC з маскою 0x3F, 4) таблиця по порядку */
  TEST_ASSERT_EQUAL_UINT(2U + n, wlog_n);
  TEST_ASSERT_EQUAL_HEX8(OV7670_REG_COM7, wlog[0].reg);
  TEST_ASSERT_EQUAL_HEX8(OV7670_COM7_RESET, wlog[0].val);
  TEST_ASSERT_EQUAL_UINT(1U, delay_calls);
  TEST_ASSERT_EQUAL_UINT(OV7670_RESET_DELAY_MS, delay_last);
  TEST_ASSERT_EQUAL_HEX8(OV7670_REG_CLKRC, wlog[1].reg);
  TEST_ASSERT_EQUAL_HEX8(0x0FU, wlog[1].val);
  for (size_t i = 0U; i < n; i++)
  {
    TEST_ASSERT_EQUAL_HEX8(0x21U, wlog[2U + i].addr);
    TEST_ASSERT_EQUAL_HEX8(t[i].reg, wlog[2U + i].reg);
    TEST_ASSERT_EQUAL_HEX8(t[i].val, wlog[2U + i].val);
  }
  /* RGB565 і DCW-зменшення в 4 рази — серцевина формату QQVGA RGB565 */
  TEST_ASSERT_EQUAL_HEX8(0x04U, regs[OV7670_REG_COM7]);
  TEST_ASSERT_EQUAL_HEX8(0xD0U, regs[0x40U]);
  TEST_ASSERT_EQUAL_HEX8(0x22U, regs[0x72U]);
}

static void test_configure_stops_on_failed_register(void)
{
  ov7670_t cam;
  size_t n = 0U;
  const ov7670_reg_t *t = ov7670_qqvga_rgb565_table(&n);
  uint8_t failed = 0U;

  TEST_ASSERT_EQUAL(DEV_OK, ov7670_init(&cam, sccb_read, sccb_write, delay_ms));

  fail_at = (int)xfers;   /* сам reset */
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, ov7670_configure(&cam, t, n, 0x0FU, &failed));
  TEST_ASSERT_EQUAL_HEX8(OV7670_REG_COM7, failed);

  fail_at = (int)xfers + 1;   /* CLKRC після reset */
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, ov7670_configure(&cam, t, n, 0x0FU, &failed));
  TEST_ASSERT_EQUAL_HEX8(OV7670_REG_CLKRC, failed);

  fail_at = (int)xfers + 2 + 3;   /* четвертий запис таблиці */
  wlog_n = 0U;
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, ov7670_configure(&cam, t, n, 0x0FU, &failed));
  TEST_ASSERT_EQUAL_HEX8(t[3].reg, failed);
  TEST_ASSERT_EQUAL_UINT(2U + 3U, wlog_n);   /* після помилки нічого не пишемо */

  /* без delay_ms (NULL) — теж працює; NULL-аргументи — DEV_ERR_PARAM */
  cam.delay_ms = NULL;
  fail_at = -1;
  TEST_ASSERT_EQUAL(DEV_OK, ov7670_configure(&cam, t, n, 0x0FU, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_configure(NULL, t, n, 0x0FU, &failed));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_configure(&cam, NULL, 3U, 0x0FU, &failed));
  TEST_ASSERT_EQUAL(DEV_OK, ov7670_configure(&cam, NULL, 0U, 0x0FU, &failed));
  present = 0;
  TEST_ASSERT_EQUAL(DEV_ERR_NACK, ov7670_configure(&cam, t, n, 0x0FU, NULL));
  present = 1;
  fail_at = (int)xfers + 1;
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, ov7670_configure(&cam, t, n, 0x0FU, NULL));
  fail_at = (int)xfers + 4;
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, ov7670_configure(&cam, t, n, 0x0FU, NULL));
}

static void test_reg_access_params(void)
{
  ov7670_t cam;
  uint8_t v = 0U;

  memset(&cam, 0, sizeof(cam));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_read_reg(&cam, 0x0AU, &v));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_write_reg(&cam, 0x11U, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_read_reg(NULL, 0x0AU, &v));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_write_reg(NULL, 0x11U, 1U));
  TEST_ASSERT_EQUAL(DEV_OK, ov7670_init(&cam, sccb_read, sccb_write, delay_ms));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, ov7670_read_reg(&cam, 0x0AU, NULL));
  TEST_ASSERT_EQUAL(DEV_OK, ov7670_read_reg(&cam, 0x0AU, &v));
  TEST_ASSERT_EQUAL_HEX8(0x76U, v);
  TEST_ASSERT_NOT_NULL(ov7670_qqvga_rgb565_table(NULL));
}

/* ЛР3 (лог 23, кадр пересвічений): після таблиці увімкнено AEC, AGC і AWB, задано
 * гамму і кольорову матрицю RGB; при цьому не зачеплено регістри, що міняють такти
 * (DBLV — PLL), вікно, частоту кадрів (NT_CTRL) і порядок байтів (TSLB, COM13[0]) */
static void test_table_enables_auto_exposure_and_white_balance(void)
{
  ov7670_t cam;
  size_t n = 0U;
  const ov7670_reg_t *t = ov7670_qqvga_rgb565_table(&n);
  int last_com8 = -1;
  int first_com8 = -1;
  int aew = -1;

  for (size_t i = 0U; i < n; i++)
  {
    TEST_ASSERT_NOT_EQUAL_HEX8_MESSAGE(0x6BU, t[i].reg, "DBLV: PLL змінив би PCLK");
    TEST_ASSERT_NOT_EQUAL_HEX8_MESSAGE(0xA4U, t[i].reg, "NT_CTRL: змінив би період VSYNC");
    TEST_ASSERT_NOT_EQUAL_HEX8_MESSAGE(OV7670_REG_TSLB, t[i].reg, "TSLB: порядок байтів");
    TEST_ASSERT_NOT_EQUAL_HEX8_MESSAGE(0x1EU, t[i].reg, "MVFP: дзеркалення");
    TEST_ASSERT_NOT_EQUAL_HEX8_MESSAGE(OV7670_REG_CLKRC, t[i].reg, "CLKRC пише configure()");
    if (t[i].reg == OV7670_REG_COM8)
    {
      if (first_com8 < 0)
      {
        first_com8 = (int)i;
      }
      last_com8 = (int)i;
    }
    if (t[i].reg == 0x24U)
    {
      aew = (int)i;
    }
  }
  /* параметри AEC пишуться при вимкненій автоматиці, вмикається вона останньою */
  TEST_ASSERT_TRUE(first_com8 >= 0);
  TEST_ASSERT_EQUAL_HEX8(0x00U, t[first_com8].val & 0x07U);
  TEST_ASSERT_TRUE(aew > first_com8);
  TEST_ASSERT_EQUAL_HEX8(OV7670_COM8_AUTO, t[last_com8].val);

  TEST_ASSERT_EQUAL(DEV_OK, ov7670_init(&cam, sccb_read, sccb_write, delay_ms));
  TEST_ASSERT_EQUAL(DEV_OK, ov7670_configure(&cam, t, n, 0x0FU, NULL));
  TEST_ASSERT_EQUAL_HEX8(0xE7U, regs[OV7670_REG_COM8]);       /* AEC|AGC|AWB + швидкий AEC */
  TEST_ASSERT_EQUAL_HEX8(0x00U, regs[OV7670_REG_COM13] & 0x01U);
  TEST_ASSERT_EQUAL_HEX8(0x80U, regs[OV7670_REG_COM13] & 0x80U);   /* гамма */
  TEST_ASSERT_EQUAL_HEX8(0xB3U, regs[0x4FU]);                  /* матриця RGB */
  TEST_ASSERT_EQUAL_HEX8(0x9EU, regs[0x58U]);
  TEST_ASSERT_EQUAL_HEX8(0x08U, regs[0x41U] & 0x08U);          /* COM16: AWB gain */
  /* формат QQVGA RGB565 після блоку автоматики не змінився */
  TEST_ASSERT_EQUAL_HEX8(0x04U, regs[OV7670_REG_COM7]);
  TEST_ASSERT_EQUAL_HEX8(0xD0U, regs[0x40U]);
  TEST_ASSERT_EQUAL_HEX8(0x22U, regs[0x72U]);
  TEST_ASSERT_EQUAL_HEX8(0x1AU, regs[0x3EU]);
}

/* Порядок байтів RGB565: датчик дає молодший першим, прошивка зберігає старший першим */
static void test_rgb565_swap_bytes(void)
{
  /* червоний F800, зелений 07E0, синій 001F так, як їх віддає датчик */
  uint8_t px[] = { 0x00U, 0xF8U, 0xE0U, 0x07U, 0x1FU, 0x00U, 0xAAU };
  const uint8_t be[] = { 0xF8U, 0x00U, 0x07U, 0xE0U, 0x00U, 0x1FU, 0xAAU };

  ov7670_rgb565_swap_bytes(px, 7U);           /* непарний хвіст не чіпається */
  TEST_ASSERT_EQUAL_HEX8_ARRAY(be, px, sizeof(be));
  ov7670_rgb565_swap_bytes(px, 6U);
  ov7670_rgb565_swap_bytes(px, 6U);           /* двічі — те саме */
  TEST_ASSERT_EQUAL_HEX8_ARRAY(be, px, sizeof(be));
  ov7670_rgb565_swap_bytes(px, 0U);
  ov7670_rgb565_swap_bytes(NULL, 6U);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(be, px, sizeof(be));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_reads_pid_ver);
  RUN_TEST(test_init_wrong_id_and_missing);
  RUN_TEST(test_configure_order_reset_clkrc_table);
  RUN_TEST(test_configure_stops_on_failed_register);
  RUN_TEST(test_reg_access_params);
  RUN_TEST(test_table_enables_auto_exposure_and_white_balance);
  RUN_TEST(test_rgb565_swap_bytes);
  return UNITY_END();
}
