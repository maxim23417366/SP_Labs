/**
  * @file    test_frame_store.c
  * @brief   Збереження кадрів у W25Q64 на моделі мікросхеми (fake_w25q64) через
  *          справжній драйвер w25q64.
  *
  *          Критерії: тестовий буфер після запису й читання побайтно збігається,
  *          у тому числі після «перезапуску»; адреси не виходять за слот і не
  *          перекривають журнал; кадр прив'язаний до seq події; недописаний кадр
  *          (обрив живлення) не вважається дійсним; пошкоджені дані виявляє CRC.
  */
#include <string.h>

#include "unity.h"
#include "crc32.h"
#include "fake_delay.h"
#include "fake_w25q64.h"
#include "flash_layout.h"
#include "frame_store.h"
#include "w25q64.h"

#define QQVGA_RGB565 38400U
#define QQVGA_Y8     19200U

static w25q64_t      dev;
static frame_store_t fs;
static uint8_t       img[FLASH_FRAME_MAX_DATA];
static uint8_t       rd[FLASH_FRAME_MAX_DATA];

/* Тестовий кадр: детермінований шаблон, різний для кожного seed */
static void fill(uint8_t *p, uint32_t len, uint32_t seed)
{
  for (uint32_t i = 0U; i < len; i++)
  {
    p[i] = (uint8_t)((i * 7U) ^ (i >> 8) ^ seed);
  }
}

static frame_header_t hdr_for(uint32_t len, uint32_t event_seq)
{
  frame_header_t h;

  memset(&h, 0, sizeof(h));
  h.format = FRAME_FORMAT_RGB565;
  h.width = 160U;
  h.height = 120U;
  h.event_seq = event_seq;
  h.data_len = len;
  return h;
}

static int region_is_ff(uint32_t from, uint32_t to)
{
  const uint8_t *m = fake_w25q64_mem();

  for (uint32_t a = from; a < to; a++)
  {
    if (m[a] != 0xFFU)
    {
      return 0;
    }
  }
  return 1;
}

static void reboot(void)
{
  fake_w25q64_power_on();
  memset(&fs, 0x5A, sizeof(fs));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_mount(&fs, &dev));
}

void setUp(void)
{
  fake_w25q64_reset();
  fake_delay_reset();
  TEST_ASSERT_EQUAL(DEV_OK, w25q64_init(&dev, fake_w25q64_bus(), fake_delay_ms));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_mount(&fs, &dev));
}

void tearDown(void)
{
  TEST_ASSERT_EQUAL_UINT(0U, fake_w25q64_protocol_errors());
}

/* ---- заголовок ------------------------------------------------------------------ */

static void test_header_roundtrip_and_layout(void)
{
  frame_header_t in = hdr_for(QQVGA_RGB565, 0x01020304UL);
  frame_header_t out;
  uint8_t raw[FRAME_HEADER_SIZE];

  in.frame_id = 128U;
  in.slot = 1U;                                   /* (128 − 1) mod 126 */
  in.data_crc = 0xA1B2C3D4UL;
  frame_store_pack_header(&in, raw);

  TEST_ASSERT_EQUAL_HEX8('S', raw[0]);
  TEST_ASSERT_EQUAL_HEX8('R', raw[3]);
  TEST_ASSERT_EQUAL_HEX8(1U, raw[4]);
  TEST_ASSERT_EQUAL_HEX8(FRAME_FORMAT_RGB565, raw[5]);
  TEST_ASSERT_EQUAL_HEX8(160U, raw[6]);
  TEST_ASSERT_EQUAL_HEX8(120U, raw[8]);
  TEST_ASSERT_EQUAL_HEX8(0x04U, raw[16]);
  TEST_ASSERT_TRUE(frame_store_unpack_header(raw, &out));
  TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof(in));
}

static void test_header_rejects_invalid(void)
{
  frame_header_t h = hdr_for(100U, 1U);
  frame_header_t out;
  uint8_t raw[FRAME_HEADER_SIZE];

  h.frame_id = 5U;
  h.slot = 4U;

  frame_store_pack_header(&h, raw);
  raw[0] ^= 1U;                                   /* magic */
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));

  frame_store_pack_header(&h, raw);
  raw[4] = 2U;                                    /* версія */
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));

  frame_store_pack_header(&h, raw);
  raw[20] ^= 1U;                                  /* CRC заголовка */
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));

  memset(raw, 0xFF, sizeof(raw));                 /* порожній слот */
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));

  h.slot = 3U;                                    /* чужий слот */
  frame_store_pack_header(&h, raw);
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));

  h.slot = FLASH_FRAME_SLOTS;
  frame_store_pack_header(&h, raw);
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));

  h.slot = 0U;
  h.frame_id = 0U;
  frame_store_pack_header(&h, raw);
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));

  h.frame_id = 1U;
  h.data_len = 0U;
  frame_store_pack_header(&h, raw);
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));

  h.data_len = FLASH_FRAME_MAX_DATA + 1U;
  frame_store_pack_header(&h, raw);
  TEST_ASSERT_FALSE(frame_store_unpack_header(raw, &out));
}

static void test_slot_addresses(void)
{
  TEST_ASSERT_EQUAL_HEX32(0x010000UL, frame_store_slot_addr(1U));
  TEST_ASSERT_EQUAL_HEX32(0x020000UL, frame_store_slot_addr(2U));
  TEST_ASSERT_EQUAL_HEX32(0x7E0000UL, frame_store_slot_addr(FLASH_FRAME_SLOTS));
  TEST_ASSERT_EQUAL_HEX32(0x010000UL, frame_store_slot_addr(FLASH_FRAME_SLOTS + 1U));
}

/* ---- запис і читання -------------------------------------------------------------- */

static void test_blank_mount(void)
{
  TEST_ASSERT_TRUE(fs.mounted);
  TEST_ASSERT_EQUAL_UINT32(1U, fs.next_id);
  TEST_ASSERT_EQUAL_UINT32(0U, fs.valid_frames);
}

static void test_qqvga_rgb565_roundtrip_bytewise(void)
{
  frame_header_t h = hdr_for(QQVGA_RGB565, 17U);
  frame_header_t got;
  uint32_t id = 0U;

  fill(img, QQVGA_RGB565, 0x11U);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
  TEST_ASSERT_EQUAL_UINT32(1U, id);

  /* 256 Б заголовкової сторінки + 38 400 Б = 10 секторів, 150 сторінок + заголовок */
  TEST_ASSERT_EQUAL_UINT(10U, fake_w25q64_erase_count());
  TEST_ASSERT_EQUAL_UINT(151U, fake_w25q64_program_count());

  memset(rd, 0, sizeof(rd));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read(&fs, id, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_MEMORY(img, rd, QQVGA_RGB565);
  TEST_ASSERT_EQUAL_UINT32(17U, got.event_seq);
  TEST_ASSERT_EQUAL_UINT32(QQVGA_RGB565, got.data_len);
  TEST_ASSERT_EQUAL_HEX32(crc32_compute(img, QQVGA_RGB565), got.data_crc);
  TEST_ASSERT_EQUAL_UINT8(FRAME_FORMAT_RGB565, got.format);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_verify(&fs, id, &got));

  /* Поза слотом нічого не змінилося: журнал, інші слоти */
  TEST_ASSERT_TRUE(region_is_ff(0U, FLASH_FRAME_BASE));
  TEST_ASSERT_TRUE(region_is_ff(FLASH_FRAME_BASE + FLASH_FRAME_SLOT_SIZE, W25Q64_SIZE_BYTES));
  /* Решта сторінки заголовка порожня */
  TEST_ASSERT_TRUE(region_is_ff(FLASH_FRAME_BASE + FRAME_HEADER_SIZE, FLASH_FRAME_BASE + FLASH_FRAME_DATA_OFFSET));
}

static void test_frame_survives_restart(void)
{
  frame_header_t h = hdr_for(QQVGA_Y8, 5U);
  frame_header_t got;
  uint32_t id = 0U;

  h.format = FRAME_FORMAT_Y8;
  fill(img, QQVGA_Y8, 0x22U);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
  TEST_ASSERT_EQUAL_UINT(5U, fake_w25q64_erase_count());

  reboot();
  TEST_ASSERT_EQUAL_UINT32(1U, fs.valid_frames);
  TEST_ASSERT_EQUAL_UINT32(2U, fs.next_id);

  memset(rd, 0, sizeof(rd));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read(&fs, id, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_MEMORY(img, rd, QQVGA_Y8);
  TEST_ASSERT_EQUAL_UINT8(FRAME_FORMAT_Y8, got.format);
}

static void test_chunked_write_equals_whole(void)
{
  static const size_t chunks[] = { 1U, 255U, 256U, 1000U, 3U, 4096U };
  frame_header_t h = hdr_for(QQVGA_RGB565, 9U);
  frame_header_t got;
  uint32_t off = 0U;
  uint32_t id = 0U;
  size_t k = 0U;

  fill(img, QQVGA_RGB565, 0x33U);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_begin(&fs, &h));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_append(&fs, img, 0U));
  while (off < QQVGA_RGB565)
  {
    size_t n = chunks[k++ % (sizeof(chunks) / sizeof(chunks[0]))];

    if (n > QQVGA_RGB565 - off)
    {
      n = QQVGA_RGB565 - off;
    }
    TEST_ASSERT_EQUAL(DEV_OK, frame_store_append(&fs, &img[off], n));
    off += (uint32_t)n;
  }
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_commit(&fs, &id));

  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read(&fs, id, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_MEMORY(img, rd, QQVGA_RGB565);
  TEST_ASSERT_EQUAL_HEX32(crc32_compute(img, QQVGA_RGB565), got.data_crc);
}

static void test_partial_last_page_and_read_data(void)
{
  frame_header_t h = hdr_for(1000U, 1U);
  frame_header_t got;
  uint8_t part[10];
  uint32_t id = 0U;

  fill(img, 1000U, 0x44U);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read_header(&fs, id, &got));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read_data(&fs, &got, 990U, part, sizeof(part)));
  TEST_ASSERT_EQUAL_MEMORY(&img[990], part, sizeof(part));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_data(&fs, &got, 991U, part, sizeof(part)));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_data(&fs, &got, 1001U, part, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_data(&fs, &got, 0U, part, 0U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_data(&fs, &got, 0U, NULL, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_data(&fs, NULL, 0U, part, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_data(NULL, &got, 0U, part, 1U));
}

static void test_max_size_stays_in_slot(void)
{
  uint8_t *m = fake_w25q64_mem();
  frame_header_t h = hdr_for(FLASH_FRAME_MAX_DATA, 1U);
  uint32_t id = 0U;

  memset(&m[FLASH_FRAME_BASE + FLASH_FRAME_SLOT_SIZE], 0x5A, 256U);   /* слот 2 */
  memset(&m[FLASH_LOG_END - 256U], 0x6B, 256U);                        /* кінець журналу */
  fill(img, FLASH_FRAME_MAX_DATA, 0x55U);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
  TEST_ASSERT_EQUAL_UINT(16U, fake_w25q64_erase_count());
  TEST_ASSERT_EQUAL_HEX8(0x5A, m[FLASH_FRAME_BASE + FLASH_FRAME_SLOT_SIZE]);
  TEST_ASSERT_EQUAL_HEX8(0x6B, m[FLASH_LOG_END - 1U]);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read(&fs, id, &h, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_MEMORY(img, rd, FLASH_FRAME_MAX_DATA);
}

static void test_ring_overwrites_oldest(void)
{
  frame_header_t h = hdr_for(100U, 0U);
  frame_header_t got;
  uint32_t id = 0U;

  for (uint32_t i = 1U; i <= FLASH_FRAME_SLOTS + 1U; i++)
  {
    h.event_seq = i;
    fill(img, 100U, i);
    TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
    TEST_ASSERT_EQUAL_UINT32(i, id);
  }

  /* Кадр 127 зайняв слот кадру 1 */
  TEST_ASSERT_EQUAL(DEV_ERR_ID, frame_store_read_header(&fs, 1U, &got));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read(&fs, FLASH_FRAME_SLOTS + 1U, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_UINT32(FLASH_FRAME_SLOTS + 1U, got.event_seq);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read_header(&fs, 2U, &got));

  reboot();
  TEST_ASSERT_EQUAL_UINT32(FLASH_FRAME_SLOTS, fs.valid_frames);
  TEST_ASSERT_EQUAL_UINT32(FLASH_FRAME_SLOTS + 2U, fs.next_id);
}

static void test_unknown_frame_id(void)
{
  frame_header_t got;

  TEST_ASSERT_EQUAL(DEV_ERR_ID, frame_store_read_header(&fs, 1U, &got));
  TEST_ASSERT_EQUAL(DEV_ERR_ID, frame_store_read(&fs, 1U, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL(DEV_ERR_ID, frame_store_verify(&fs, 1U, &got));
}

/* ---- цілісність ---------------------------------------------------------------------- */

static void test_corrupted_data_detected(void)
{
  frame_header_t h = hdr_for(QQVGA_Y8, 1U);
  frame_header_t got;
  uint32_t id = 0U;

  fill(img, QQVGA_Y8, 0x66U);
  img[12345] = 0xFFU;                             /* щоб біт точно можна було скинути */
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
  fake_w25q64_mem()[frame_store_slot_addr(id) + FLASH_FRAME_DATA_OFFSET + 12345U] = 0x7FU;

  TEST_ASSERT_EQUAL(DEV_ERR_STATE, frame_store_read(&fs, id, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, frame_store_verify(&fs, id, &got));
}

static void test_power_cut_during_data_leaves_no_frame(void)
{
  frame_header_t h = hdr_for(QQVGA_RGB565, 1U);
  frame_header_t got;
  uint32_t id = 0U;

  fill(img, QQVGA_RGB565, 0x77U);
  /* 10 стирань пройшли, із 38 400 Б даних записано 20 000 */
  fake_w25q64_cut_after((10U * W25Q64_SECTOR_SIZE) + 20000U);
  TEST_ASSERT_NOT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));

  reboot();
  TEST_ASSERT_EQUAL_UINT32(0U, fs.valid_frames);
  TEST_ASSERT_EQUAL_UINT32(1U, fs.next_id);       /* слот буде використано знову */
  TEST_ASSERT_EQUAL(DEV_ERR_ID, frame_store_read_header(&fs, 1U, &got));

  TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read(&fs, id, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_MEMORY(img, rd, QQVGA_RGB565);
}

static void test_power_cut_during_header_leaves_no_frame(void)
{
  frame_header_t h = hdr_for(QQVGA_Y8, 1U);
  frame_header_t got;
  uint32_t id = 0U;

  fill(img, QQVGA_Y8, 0x78U);
  fake_w25q64_cut_after((5U * W25Q64_SECTOR_SIZE) + QQVGA_Y8 + 10U);   /* із заголовка — 10 Б */
  TEST_ASSERT_NOT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));

  reboot();
  TEST_ASSERT_EQUAL_UINT32(0U, fs.valid_frames);
  TEST_ASSERT_EQUAL(DEV_ERR_ID, frame_store_read_header(&fs, 1U, &got));
}

static void test_previous_frame_kept_when_new_one_fails_elsewhere(void)
{
  frame_header_t h = hdr_for(QQVGA_Y8, 1U);
  frame_header_t got;
  uint32_t id = 0U;

  fill(img, QQVGA_Y8, 0x79U);
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
  fake_w25q64_cut_after(1000U);                   /* обрив на стиранні слота 2 */
  TEST_ASSERT_NOT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));

  reboot();
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read(&fs, 1U, &got, rd, sizeof(rd)));
  TEST_ASSERT_EQUAL_MEMORY(img, rd, QQVGA_Y8);
  TEST_ASSERT_EQUAL_UINT32(2U, fs.next_id);
}

/* ---- помилки виклику і шини ------------------------------------------------------------ */

static void test_call_order_and_params(void)
{
  frame_store_t idle;
  frame_header_t h = hdr_for(300U, 1U);
  frame_header_t got;
  uint32_t id = 0U;

  memset(&idle, 0, sizeof(idle));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_mount(NULL, &dev));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_mount(&idle, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_begin(NULL, &h));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_begin(&fs, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, frame_store_begin(&idle, &h));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, frame_store_read_header(&idle, 1U, &got));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_header(&fs, 0U, &got));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_header(NULL, 1U, &got));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read_header(&fs, 1U, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read(&fs, 1U, &got, NULL, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_write(&fs, &h, NULL, &id));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_commit(NULL, &id));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_append(NULL, img, 1U));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_append(&fs, NULL, 1U));
  frame_store_abort(NULL);

  h.data_len = 0U;
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_begin(&fs, &h));
  h.data_len = FLASH_FRAME_MAX_DATA + 1U;
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_begin(&fs, &h));
  TEST_ASSERT_EQUAL_UINT(0U, fake_w25q64_erase_count());   /* без жодної операції з Flash */

  h.data_len = 300U;
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, frame_store_append(&fs, img, 1U));    /* без begin */
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, frame_store_commit(&fs, &id));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_begin(&fs, &h));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_append(&fs, img, 301U));  /* понад data_len */
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_append(&fs, img, 299U));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, frame_store_commit(&fs, &id));        /* бракує 1 Б */
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_append(&fs, img, 1U));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_commit(&fs, NULL));

  TEST_ASSERT_EQUAL(DEV_OK, frame_store_read_header(&fs, 1U, &got));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, frame_store_read(&fs, 1U, &got, rd, 299U));   /* замалий буфер */
}

static void test_bus_errors(void)
{
  frame_header_t h = hdr_for(600U, 1U);
  frame_header_t got;
  frame_store_t other;
  uint32_t id = 0U;

  fill(img, 600U, 0x88U);

  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);                 /* стирання */
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, frame_store_begin(&fs, &h));
  TEST_ASSERT_FALSE(fs.writing);

  TEST_ASSERT_EQUAL(DEV_OK, frame_store_begin(&fs, &h));
  fake_w25q64_fail_transfers(1U, DEV_ERR_TIMEOUT);             /* запис першої сторінки */
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, frame_store_append(&fs, img, 600U));
  TEST_ASSERT_FALSE(fs.writing);

  TEST_ASSERT_EQUAL(DEV_OK, frame_store_begin(&fs, &h));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_append(&fs, img, 600U));
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);                 /* остання неповна сторінка */
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, frame_store_commit(&fs, &id));
  TEST_ASSERT_EQUAL_UINT32(1U, fs.next_id);

  /* Заголовок не збігся при зчитуванні назад */
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_begin(&fs, &h));
  TEST_ASSERT_EQUAL(DEV_OK, frame_store_append(&fs, img, 600U));
  fake_w25q64_mem()[frame_store_slot_addr(1U) + 12U] = 0x00U;  /* молодший байт frame_id */
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, frame_store_commit(&fs, &id));

  TEST_ASSERT_EQUAL(DEV_OK, frame_store_write(&fs, &h, img, &id));
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, frame_store_read_header(&fs, id, &got));
  fake_w25q64_fail_from(2U, 1U, DEV_ERR_BUS);                  /* заголовок OK, збій на даних */
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, frame_store_verify(&fs, id, &got));

  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, frame_store_mount(&other, &dev));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_header_roundtrip_and_layout);
  RUN_TEST(test_header_rejects_invalid);
  RUN_TEST(test_slot_addresses);
  RUN_TEST(test_blank_mount);
  RUN_TEST(test_qqvga_rgb565_roundtrip_bytewise);
  RUN_TEST(test_frame_survives_restart);
  RUN_TEST(test_chunked_write_equals_whole);
  RUN_TEST(test_partial_last_page_and_read_data);
  RUN_TEST(test_max_size_stays_in_slot);
  RUN_TEST(test_ring_overwrites_oldest);
  RUN_TEST(test_unknown_frame_id);
  RUN_TEST(test_corrupted_data_detected);
  RUN_TEST(test_power_cut_during_data_leaves_no_frame);
  RUN_TEST(test_power_cut_during_header_leaves_no_frame);
  RUN_TEST(test_previous_frame_kept_when_new_one_fails_elsewhere);
  RUN_TEST(test_call_order_and_params);
  RUN_TEST(test_bus_errors);
  return UNITY_END();
}
