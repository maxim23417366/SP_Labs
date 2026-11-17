/**
  * @file    test_crc32.c
  * @brief   CRC-32 (IEEE 802.3): контрольні значення і обчислення частинами.
  */
#include <string.h>

#include "unity.h"
#include "crc32.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* Контрольне значення CRC-32 для ASCII "123456789" (каталог CRC: CRC-32/ISO-HDLC) */
static void test_check_value(void)
{
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL, crc32_compute("123456789", 9U));
}

static void test_empty_and_single_byte(void)
{
  static const uint8_t zero = 0x00U;

  TEST_ASSERT_EQUAL_HEX32(0x00000000UL, crc32_compute(NULL, 0U));
  TEST_ASSERT_EQUAL_HEX32(0xD202EF8DUL, crc32_compute(&zero, 1U));
}

static void test_split_equals_whole(void)
{
  static uint8_t buf[1000];
  uint32_t crc = CRC32_INIT;

  for (size_t i = 0U; i < sizeof(buf); i++)
  {
    buf[i] = (uint8_t)(i * 31U + 7U);
  }
  crc = crc32_update(crc, buf, 1U);
  crc = crc32_update(crc, &buf[1], 255U);
  crc = crc32_update(crc, &buf[256], sizeof(buf) - 256U);
  TEST_ASSERT_EQUAL_HEX32(crc32_compute(buf, sizeof(buf)), crc32_final(crc));
}

static void test_detects_single_bit_flip(void)
{
  uint8_t a[32];
  uint32_t ref;

  memset(a, 0x5A, sizeof(a));
  ref = crc32_compute(a, sizeof(a));
  for (size_t bit = 0U; bit < sizeof(a) * 8U; bit++)
  {
    a[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
    TEST_ASSERT_NOT_EQUAL(ref, crc32_compute(a, sizeof(a)));
    a[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
  }
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_check_value);
  RUN_TEST(test_empty_and_single_byte);
  RUN_TEST(test_split_equals_whole);
  RUN_TEST(test_detects_single_bit_flip);
  return UNITY_END();
}
