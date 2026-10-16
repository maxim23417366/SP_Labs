/**
  * @file    test_w25q64.c
  * @brief   W25Q64: JEDEC ID і послідовність команд до SPI flash, перевірена через mock.
  *
  *          Тут важливий не лише результат функції, а й кадри на шині:
  *          Write Enable (06h) має йти окремим кадром перед Page Program (02h)
  *          і Sector Erase (20h), а після запису драйвер має дочекатися BUSY = 0.
  */
#include "unity.h"
#include "fake_delay.h"
#include "mock_spi.h"
#include "w25q64.h"

static w25q64_t dev;

/* Типові відповіді MISO. Перший байт кадру — під час передачі команди (не значущий). */
static const uint8_t miso_jedec[]    = { 0xFF, 0xEF, 0x40, 0x17 };
static const uint8_t miso_sr_wel[]   = { 0xFF, W25Q64_SR1_WEL };
static const uint8_t miso_sr_busy[]  = { 0xFF, W25Q64_SR1_WEL | W25Q64_SR1_BUSY };
static const uint8_t miso_sr_ready[] = { 0xFF, 0x00 };

static void assert_frame(size_t i, const uint8_t *expected, size_t len)
{
  const mock_spi_frame_t *f = mock_spi_frame(i);

  TEST_ASSERT_EQUAL_size_t(len, f->len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, f->mosi, len);
}

/* Ініціалізація займає кадр 0; далі нумерація кадрів з 1 */
static void init_ok(void)
{
  mock_spi_set_miso(0U, miso_jedec, sizeof(miso_jedec));
  TEST_ASSERT_EQUAL(DEV_OK, w25q64_init(&dev, mock_spi_bus(), fake_delay_ms));
}

void setUp(void)
{
  mock_spi_reset();
  fake_delay_reset();
}

void tearDown(void)
{
  /* Після будь-якої операції CS# піднятий і протокол не порушено */
  TEST_ASSERT_FALSE(mock_spi_cs_active());
  TEST_ASSERT_EQUAL_UINT(0U, mock_spi_protocol_errors());
}

static void test_init_reads_jedec_id(void)
{
  static const uint8_t cmd[] = { 0x9F, 0xFF, 0xFF, 0xFF };

  init_ok();

  TEST_ASSERT_EQUAL_size_t(1U, mock_spi_frame_count());
  assert_frame(0U, cmd, sizeof(cmd));
  TEST_ASSERT_EQUAL_HEX8(0xEF, dev.jedec_id[0]);
  TEST_ASSERT_EQUAL_HEX8(0x40, dev.jedec_id[1]);
  TEST_ASSERT_EQUAL_HEX8(0x17, dev.jedec_id[2]);
}

static void test_init_no_chip_reads_ff(void)
{
  /* MISO без мікросхеми «висить» у 1: mock за замовчуванням віддає 0xFF */
  TEST_ASSERT_EQUAL(DEV_ERR_ID, w25q64_init(&dev, mock_spi_bus(), fake_delay_ms));
}

static void test_init_other_chip_rejected(void)
{
  static const uint8_t miso_w25q32[] = { 0xFF, 0xEF, 0x40, 0x16 };   /* W25Q32: 32 Мбіт */

  mock_spi_set_miso(0U, miso_w25q32, sizeof(miso_w25q32));
  TEST_ASSERT_EQUAL(DEV_ERR_ID, w25q64_init(&dev, mock_spi_bus(), fake_delay_ms));
}

static void test_init_spi_error_releases_cs(void)
{
  mock_spi_fail_in_frame(0U, DEV_ERR_TIMEOUT);

  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, w25q64_init(&dev, mock_spi_bus(), fake_delay_ms));
  /* tearDown перевіряє, що CS# піднятий навіть після збою */
}

static void test_init_null_params(void)
{
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_init(&dev, NULL, fake_delay_ms));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_init(&dev, mock_spi_bus(), NULL));
  TEST_ASSERT_EQUAL_size_t(0U, mock_spi_frame_count());
}

static void test_write_page_command_sequence(void)
{
  static const uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
  static const uint8_t wren[] = { 0x06 };
  static const uint8_t rdsr[] = { 0x05, 0xFF };
  static const uint8_t pp[]   = { 0x02, 0x12, 0x34, 0x00, 0xDE, 0xAD, 0xBE, 0xEF };

  init_ok();
  mock_spi_set_miso(2U, miso_sr_wel, sizeof(miso_sr_wel));      /* WEL після 06h */
  mock_spi_set_miso(4U, miso_sr_busy, sizeof(miso_sr_busy));    /* іде програмування */
  mock_spi_set_miso(5U, miso_sr_ready, sizeof(miso_sr_ready));  /* готово */

  TEST_ASSERT_EQUAL(DEV_OK, w25q64_write_page(&dev, 0x123400UL, data, sizeof(data)));

  TEST_ASSERT_EQUAL_size_t(6U, mock_spi_frame_count());
  assert_frame(1U, wren, sizeof(wren));   /* Write Enable — окремим кадром */
  assert_frame(2U, rdsr, sizeof(rdsr));   /* перевірка WEL */
  assert_frame(3U, pp, sizeof(pp));       /* команда + 24-біт адреса + дані */
  assert_frame(4U, rdsr, sizeof(rdsr));   /* BUSY = 1 */
  assert_frame(5U, rdsr, sizeof(rdsr));   /* BUSY = 0 */
  TEST_ASSERT_EQUAL_UINT32(1U, fake_delay_total_ms);
}

static void test_write_page_without_wel_is_not_programmed(void)
{
  static const uint8_t data[] = { 0x55 };

  init_ok();
  mock_spi_set_miso(2U, miso_sr_ready, sizeof(miso_sr_ready));  /* WEL = 0 (/WP активний) */

  TEST_ASSERT_EQUAL(DEV_ERR_STATE, w25q64_write_page(&dev, 0U, data, sizeof(data)));
  /* 02h так і не відправлено */
  TEST_ASSERT_EQUAL_size_t(3U, mock_spi_frame_count());
}

static void test_write_page_busy_timeout(void)
{
  static const uint8_t data[] = { 0x55 };

  init_ok();
  mock_spi_set_miso(2U, miso_sr_wel, sizeof(miso_sr_wel));
  for (size_t f = 4U; f < MOCK_SPI_MAX_FRAMES; f++)
  {
    mock_spi_set_miso(f, miso_sr_busy, sizeof(miso_sr_busy));   /* BUSY не знімається */
  }

  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, w25q64_write_page(&dev, 0U, data, sizeof(data)));
  /* tPP max = 3 мс: опитування на 0, 1, 2, 3 мс */
  TEST_ASSERT_EQUAL_UINT32(W25Q64_PROGRAM_TIMEOUT_MS, fake_delay_total_ms);
  TEST_ASSERT_EQUAL_size_t(4U + W25Q64_PROGRAM_TIMEOUT_MS + 1U, mock_spi_frame_count());
}

static void test_write_page_rejects_page_crossing(void)
{
  static uint8_t data[W25Q64_PAGE_SIZE + 1U];

  init_ok();

  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_write_page(&dev, 0x0000F0UL, data, 0x20U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_write_page(&dev, 0U, data, sizeof(data)));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_write_page(&dev, 0U, data, 0U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_write_page(&dev, W25Q64_SIZE_BYTES, data, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_write_page(&dev, 0U, NULL, 1U));
  TEST_ASSERT_EQUAL_size_t(1U, mock_spi_frame_count());   /* лише кадр init */
}

static void test_write_page_full_page_allowed(void)
{
  static uint8_t data[W25Q64_PAGE_SIZE];

  init_ok();
  mock_spi_set_miso(2U, miso_sr_wel, sizeof(miso_sr_wel));
  mock_spi_set_miso(4U, miso_sr_ready, sizeof(miso_sr_ready));

  TEST_ASSERT_EQUAL(DEV_OK, w25q64_write_page(&dev, 0x000100UL, data, sizeof(data)));
  TEST_ASSERT_EQUAL_size_t(4U + W25Q64_PAGE_SIZE, mock_spi_frame(3U)->len);
}

static void test_write_page_spi_error_during_program(void)
{
  static const uint8_t data[] = { 0x55 };

  init_ok();
  mock_spi_set_miso(2U, miso_sr_wel, sizeof(miso_sr_wel));
  mock_spi_fail_in_frame(3U, DEV_ERR_BUS);

  TEST_ASSERT_EQUAL(DEV_ERR_BUS, w25q64_write_page(&dev, 0U, data, sizeof(data)));
}

static void test_write_page_spi_error_while_polling_busy(void)
{
  static const uint8_t data[] = { 0x55 };

  init_ok();
  mock_spi_set_miso(2U, miso_sr_wel, sizeof(miso_sr_wel));
  mock_spi_set_miso(4U, miso_sr_busy, sizeof(miso_sr_busy));
  mock_spi_fail_in_frame(5U, DEV_ERR_TIMEOUT);   /* друге читання 05h не вдалося */

  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, w25q64_write_page(&dev, 0U, data, sizeof(data)));
  TEST_ASSERT_EQUAL_size_t(6U, mock_spi_frame_count());
}

static void test_read_data_returns_miso_bytes(void)
{
  static const uint8_t cmd[]  = { 0x03, 0x7F, 0xFF, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF };
  static const uint8_t miso[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD, 0xBE, 0xEF };
  uint8_t buf[4] = { 0 };

  init_ok();
  mock_spi_set_miso(1U, miso, sizeof(miso));

  /* Останні 4 байти флешки: адреса 0x7FFFFC */
  TEST_ASSERT_EQUAL(DEV_OK, w25q64_read(&dev, W25Q64_SIZE_BYTES - 4U, buf, sizeof(buf)));
  assert_frame(1U, cmd, sizeof(cmd));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(&miso[4], buf, sizeof(buf));
}

static void test_read_rejects_out_of_range(void)
{
  uint8_t buf[8];

  init_ok();

  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_read(&dev, W25Q64_SIZE_BYTES - 4U, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_read(&dev, W25Q64_SIZE_BYTES, buf, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_read(&dev, 0U, NULL, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_read(&dev, 0U, buf, 0U));
  TEST_ASSERT_EQUAL_size_t(1U, mock_spi_frame_count());
}

static void test_erase_sector_busy_timeout(void)
{
  static const uint8_t wren[] = { 0x06 };
  static const uint8_t se[]   = { 0x20, 0x00, 0x30, 0x00 };

  init_ok();
  mock_spi_set_miso(2U, miso_sr_wel, sizeof(miso_sr_wel));
  for (size_t f = 4U; f < MOCK_SPI_MAX_FRAMES; f++)
  {
    mock_spi_set_miso(f, miso_sr_busy, sizeof(miso_sr_busy));   /* BUSY не знімається */
  }

  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, w25q64_erase_sector(&dev, 0x003000UL));
  assert_frame(1U, wren, sizeof(wren));
  assert_frame(3U, se, sizeof(se));
  /* tSE max = 400 мс: 401 опитування з кроком 1 мс */
  TEST_ASSERT_EQUAL_UINT32(W25Q64_ERASE_TIMEOUT_MS, fake_delay_total_ms);
  TEST_ASSERT_EQUAL_size_t(4U + W25Q64_ERASE_TIMEOUT_MS + 1U, mock_spi_frame_count());
}

static void test_erase_sector_completes(void)
{
  static const uint8_t se[] = { 0x20, 0x7F, 0xF0, 0x00 };

  init_ok();
  mock_spi_set_miso(2U, miso_sr_wel, sizeof(miso_sr_wel));
  mock_spi_set_miso(4U, miso_sr_busy, sizeof(miso_sr_busy));
  mock_spi_set_miso(5U, miso_sr_busy, sizeof(miso_sr_busy));
  mock_spi_set_miso(6U, miso_sr_ready, sizeof(miso_sr_ready));

  TEST_ASSERT_EQUAL(DEV_OK, w25q64_erase_sector(&dev, W25Q64_SIZE_BYTES - W25Q64_SECTOR_SIZE));
  assert_frame(3U, se, sizeof(se));
  TEST_ASSERT_EQUAL_size_t(7U, mock_spi_frame_count());
  TEST_ASSERT_EQUAL_UINT32(2U, fake_delay_total_ms);
}

static void test_erase_sector_spi_error_on_command(void)
{
  init_ok();
  mock_spi_set_miso(2U, miso_sr_wel, sizeof(miso_sr_wel));
  mock_spi_fail_in_frame(3U, DEV_ERR_BUS);

  TEST_ASSERT_EQUAL(DEV_ERR_BUS, w25q64_erase_sector(&dev, 0U));
  /* Після збою команди 20h стан BUSY не опитується */
  TEST_ASSERT_EQUAL_size_t(4U, mock_spi_frame_count());
}

static void test_erase_sector_rejects_unaligned(void)
{
  init_ok();

  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_erase_sector(&dev, 0x001001UL));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, w25q64_erase_sector(&dev, W25Q64_SIZE_BYTES));
  TEST_ASSERT_EQUAL_size_t(1U, mock_spi_frame_count());
}

static void test_erase_sector_without_wel(void)
{
  init_ok();
  mock_spi_set_miso(2U, miso_sr_ready, sizeof(miso_sr_ready));

  TEST_ASSERT_EQUAL(DEV_ERR_STATE, w25q64_erase_sector(&dev, 0U));
  TEST_ASSERT_EQUAL_size_t(3U, mock_spi_frame_count());
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_init_reads_jedec_id);
  RUN_TEST(test_init_no_chip_reads_ff);
  RUN_TEST(test_init_other_chip_rejected);
  RUN_TEST(test_init_spi_error_releases_cs);
  RUN_TEST(test_init_null_params);
  RUN_TEST(test_write_page_command_sequence);
  RUN_TEST(test_write_page_without_wel_is_not_programmed);
  RUN_TEST(test_write_page_busy_timeout);
  RUN_TEST(test_write_page_rejects_page_crossing);
  RUN_TEST(test_write_page_full_page_allowed);
  RUN_TEST(test_write_page_spi_error_during_program);
  RUN_TEST(test_write_page_spi_error_while_polling_busy);
  RUN_TEST(test_read_data_returns_miso_bytes);
  RUN_TEST(test_read_rejects_out_of_range);
  RUN_TEST(test_erase_sector_busy_timeout);
  RUN_TEST(test_erase_sector_completes);
  RUN_TEST(test_erase_sector_spi_error_on_command);
  RUN_TEST(test_erase_sector_rejects_unaligned);
  RUN_TEST(test_erase_sector_without_wel);
  return UNITY_END();
}
