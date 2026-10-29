/* Тести розбору DHT11: біти -> байти, контрольна сума, діапазони. */
#include "unity.h"

#include "capture_vectors.h"
#include "dht11.h"

/* Поріг 0/1, з яким прошивка викликає dht11_bits_to_bytes (App/bench.c, DHT_THRESHOLD_US) */
#define FW_THRESHOLD_US 48U

void setUp(void) {}
void tearDown(void) {}

/* 23 °C, 41 %: 29 00 17 00 40 */
static void fill_bits(uint16_t us[40], const uint8_t frame[5])
{
  for (int i = 0; i < 40; i++)
  {
    us[i] = (frame[i / 8] & (0x80U >> (i % 8))) ? 70U : 27U;
  }
}

static void test_bits_to_bytes_uses_pulse_width(void)
{
  const uint8_t exp[5] = { 0x29, 0x00, 0x17, 0x00, 0x40 };
  uint16_t us[40];
  uint8_t frame[5];

  fill_bits(us, exp);
  dht11_bits_to_bytes(us, 48U, frame);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, frame, 5);
}

static void test_decode_valid_frame(void)
{
  const uint8_t frame[5] = { 41, 0, 23, 4, 68 };
  dht11_data_t d;

  TEST_ASSERT_EQUAL(DEV_OK, dht11_decode(frame, &d));
  TEST_ASSERT_EQUAL_UINT8(41, d.rh_pct);
  TEST_ASSERT_EQUAL_UINT8(0, d.rh_tenth);
  TEST_ASSERT_EQUAL_INT8(23, d.temp_c);
  TEST_ASSERT_EQUAL_UINT8(4, d.temp_tenth);
}

static void test_decode_bad_checksum(void)
{
  const uint8_t frame[5] = { 41, 0, 23, 0, 65 };
  dht11_data_t d;

  TEST_ASSERT_EQUAL(DEV_ERR_STATE, dht11_decode(frame, &d));
}

static void test_decode_floating_line_is_rejected(void)
{
  /* DATA без підтяжки: 40 одиниць -> FF FF FF FF FF; сума FC != FF */
  const uint8_t frame[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  dht11_data_t d;

  TEST_ASSERT_NOT_EQUAL(DEV_OK, dht11_decode(frame, &d));
}

static void test_decode_out_of_range(void)
{
  const uint8_t frame[5] = { 200, 0, 23, 0, 223 };
  dht11_data_t d;

  TEST_ASSERT_EQUAL(DEV_ERR_ID, dht11_decode(frame, &d));
}

static void test_decode_negative_flag(void)
{
  const uint8_t frame[5] = { 50, 0, 5, 0x83, (uint8_t)(50 + 5 + 0x83) };
  dht11_data_t d;

  TEST_ASSERT_EQUAL(DEV_OK, dht11_decode(frame, &d));
  TEST_ASSERT_EQUAL_INT8(-5, d.temp_c);
}

/* Вектор з логічного аналізатора: 40 тривалостей HIGH з dht11_read.sr
 * -> ті самі байти, що на дроті -> ті самі значення, що виводить прошивка. */
static void test_capture_bits_to_frame(void)
{
  uint8_t frame[5];

  dht11_bits_to_bytes(cap_dht11_read_high_us, FW_THRESHOLD_US, frame);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(cap_dht11_read_frame, frame, 5);
}

static void test_capture_frame_decodes_to_35_2_rh_21_4_c(void)
{
  uint8_t frame[5];
  dht11_data_t d;

  dht11_bits_to_bytes(cap_dht11_read_high_us, FW_THRESHOLD_US, frame);
  TEST_ASSERT_EQUAL(DEV_OK, dht11_decode(frame, &d));
  /* 21.4 °C: ціла частина і десяті зберігаються окремо */
  TEST_ASSERT_EQUAL_INT8(21, d.temp_c);
  TEST_ASSERT_EQUAL_UINT8(4, d.temp_tenth);
  /* 35.2 %RH: ціла частина і десяті (байт 1 = 0x02) окремо;
   * UART-рядок прошивки містить rh=35.2 */
  TEST_ASSERT_EQUAL_UINT8(35, d.rh_pct);
  TEST_ASSERT_EQUAL_UINT8(2, d.rh_tenth);
  TEST_ASSERT_EQUAL_HEX8(0x02, frame[1]);
}

/* Запас по порогу: на записі найдовший «0» — 25 мкс, найкоротша «1» — 72 мкс.
 * Будь-який поріг у [25, 71] дає той самий кадр; 48 мкс — посередині. */
static void test_capture_threshold_margin(void)
{
  uint8_t frame[5];

  dht11_bits_to_bytes(cap_dht11_read_high_us, 25U, frame);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(cap_dht11_read_frame, frame, 5);
  dht11_bits_to_bytes(cap_dht11_read_high_us, 71U, frame);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(cap_dht11_read_frame, frame, 5);
  /* поріг 24 мкс: «0» тривалістю 25 мкс (байт 1, біт 7) уже читається як 1 */
  dht11_bits_to_bytes(cap_dht11_read_high_us, 24U, frame);
  TEST_ASSERT_EQUAL_HEX8(0x03, frame[1]);
}

static void test_decode_null_params(void)
{
  const uint8_t frame[5] = { 0 };
  dht11_data_t d;

  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, dht11_decode(NULL, &d));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, dht11_decode(frame, NULL));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_bits_to_bytes_uses_pulse_width);
  RUN_TEST(test_decode_valid_frame);
  RUN_TEST(test_decode_bad_checksum);
  RUN_TEST(test_decode_floating_line_is_rejected);
  RUN_TEST(test_decode_out_of_range);
  RUN_TEST(test_decode_negative_flag);
  RUN_TEST(test_capture_bits_to_frame);
  RUN_TEST(test_capture_frame_decodes_to_35_2_rh_21_4_c);
  RUN_TEST(test_capture_threshold_margin);
  RUN_TEST(test_decode_null_params);
  return UNITY_END();
}
