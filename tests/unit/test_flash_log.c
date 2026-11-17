/**
  * @file    test_flash_log.c
  * @brief   Журнал подій у W25Q64 на моделі мікросхеми (fake_w25q64) через
  *          справжній драйвер w25q64.
  *
  *          Перевіряє критерії FR-08 для журналу (docs/lab_3/flash-layout.md, розділ 5):
  *          100 послідовних подій і їх порядок після «перезапуску» (повторне
  *          монтування з тим самим вмістом Flash), виявлення пошкодженого запису
  *          за CRC, обрив живлення під час запису і стирання, перехід по колу,
  *          незмінність областей поза журналом.
  */
#include <string.h>

#include "unity.h"
#include "crc32.h"
#include "fake_delay.h"
#include "fake_w25q64.h"
#include "flash_layout.h"
#include "flash_log.h"
#include "w25q64.h"

static w25q64_t    dev;
static flash_log_t log_;

void setUp(void)
{
  fake_w25q64_reset();
  fake_delay_reset();
  TEST_ASSERT_EQUAL(DEV_OK, w25q64_init(&dev, fake_w25q64_bus(), fake_delay_ms));
}

void tearDown(void)
{
  TEST_ASSERT_EQUAL_UINT(0U, fake_w25q64_protocol_errors());
}

/* «Перезапуск»: стан модуля втрачено, вміст Flash лишився */
static void reboot(void)
{
  fake_w25q64_power_on();
  memset(&log_, 0x5A, sizeof(log_));
  TEST_ASSERT_EQUAL(DEV_OK, flash_log_mount(&log_, &dev));
}

static void mount_fresh(void)
{
  TEST_ASSERT_EQUAL(DEV_OK, flash_log_mount(&log_, &dev));
}

static dev_status_t append_test(uint32_t arg)
{
  flash_log_entry_t e = { 0U, 0U, FLASH_LOG_TEST, 1000U + arg, arg, ~arg, FLASH_LOG_NO_FRAME };

  return flash_log_append(&log_, &e);
}

static void append_n(uint32_t first_arg, uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++)
  {
    TEST_ASSERT_EQUAL(DEV_OK, append_test(first_arg + i));
  }
}

/* Пройти журнал: повертає кількість дійсних записів, перевіряє зростання seq */
static uint32_t walk(uint32_t *first_seq, uint32_t *last_seq, uint32_t *damaged)
{
  flash_log_iter_t  it;
  flash_log_entry_t e;
  flash_log_slot_t  st;
  uint32_t valid = 0U;
  uint32_t prev = 0U;

  *damaged = 0U;
  flash_log_iter_begin(&log_, &it);
  for (;;)
  {
    TEST_ASSERT_EQUAL(DEV_OK, flash_log_next(&log_, &it, &e, &st, NULL));
    if (st == FLASH_LOG_SLOT_EMPTY)
    {
      break;
    }
    if (st == FLASH_LOG_SLOT_DAMAGED)
    {
      (*damaged)++;
      continue;
    }
    if (valid == 0U)
    {
      *first_seq = e.seq;
    }
    else
    {
      TEST_ASSERT_TRUE_MESSAGE(e.seq > prev, "порядок записів порушено");
    }
    prev = e.seq;
    valid++;
  }
  *last_seq = prev;
  return valid;
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

/* ---- формат запису ------------------------------------------------------------ */

static void test_pack_layout_and_roundtrip(void)
{
  flash_log_entry_t in = { 0x01020304UL, 0x0506U, FLASH_LOG_MOTION, 0x0A0B0C0DUL,
                           0x11121314UL, 0x21222324UL, 7U };
  flash_log_entry_t out;
  uint8_t rec[FLASH_LOG_RECORD_SIZE];

  flash_log_pack(&in, rec);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rec[0]);
  TEST_ASSERT_EQUAL_HEX8(0x01, rec[1]);
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_MOTION, rec[2]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, rec[3]);
  TEST_ASSERT_EQUAL_HEX8(0x04, rec[4]);         /* little-endian */
  TEST_ASSERT_EQUAL_HEX8(0x01, rec[7]);
  TEST_ASSERT_EQUAL_HEX8(0x06, rec[8]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, rec[10]);
  TEST_ASSERT_EQUAL_HEX8(0x07, rec[24]);
  TEST_ASSERT_EQUAL_HEX32(crc32_compute(rec, 28U),
                          (uint32_t)rec[28] | ((uint32_t)rec[29] << 8) |
                          ((uint32_t)rec[30] << 16) | ((uint32_t)rec[31] << 24));

  TEST_ASSERT_EQUAL(FLASH_LOG_SLOT_VALID, flash_log_unpack(rec, &out));
  TEST_ASSERT_EQUAL_HEX32(in.seq, out.seq);
  TEST_ASSERT_EQUAL_HEX16(in.boot, out.boot);
  TEST_ASSERT_EQUAL_HEX8(in.type, out.type);
  TEST_ASSERT_EQUAL_HEX32(in.t_ms, out.t_ms);
  TEST_ASSERT_EQUAL_HEX32(in.arg, out.arg);
  TEST_ASSERT_EQUAL_HEX32(in.arg2, out.arg2);
  TEST_ASSERT_EQUAL_HEX32(in.frame_id, out.frame_id);
}

static void test_unpack_empty_and_damaged(void)
{
  flash_log_entry_t e = { 1U, 1U, FLASH_LOG_TEST, 0U, 0U, 0U, FLASH_LOG_NO_FRAME };
  flash_log_entry_t out;
  uint8_t rec[FLASH_LOG_RECORD_SIZE];

  memset(rec, 0xFF, sizeof(rec));
  TEST_ASSERT_EQUAL(FLASH_LOG_SLOT_EMPTY, flash_log_unpack(rec, &out));

  flash_log_pack(&e, rec);
  rec[16] ^= 0x01U;                              /* один біт даних */
  TEST_ASSERT_EQUAL(FLASH_LOG_SLOT_DAMAGED, flash_log_unpack(rec, &out));

  flash_log_pack(&e, rec);
  rec[0] = 0x00U;                                /* маркер */
  TEST_ASSERT_EQUAL(FLASH_LOG_SLOT_DAMAGED, flash_log_unpack(rec, &out));

  flash_log_pack(&e, rec);
  rec[1] = 0x02U;                                /* версія формату */
  TEST_ASSERT_EQUAL(FLASH_LOG_SLOT_DAMAGED, flash_log_unpack(rec, &out));
}

static void test_type_names(void)
{
  static const struct { uint8_t t; const char *n; } names[] =
  {
    { FLASH_LOG_BOOT, "BOOT" }, { FLASH_LOG_LOCK_ACTIVATED, "LOCK_ACTIVATED" },
    { FLASH_LOG_LOCKED, "LOCKED" }, { FLASH_LOG_UNLOCKED, "UNLOCKED" },
    { FLASH_LOG_ACCESS_GRANTED, "ACCESS_GRANTED" }, { FLASH_LOG_WRONG_CODE, "WRONG_CODE" },
    { FLASH_LOG_MOTION, "MOTION" }, { FLASH_LOG_TAMPER, "TAMPER" },
    { FLASH_LOG_FORCED_OPEN, "FORCED_OPEN" }, { FLASH_LOG_DOOR_OPEN, "DOOR_OPEN" },
    { FLASH_LOG_DOOR_CLOSED, "DOOR_CLOSED" }, { FLASH_LOG_ENV_WARNING, "ENV_WARNING" },
    { FLASH_LOG_PHOTO_SAVED, "PHOTO_SAVED" }, { FLASH_LOG_PHOTO_FAILED, "PHOTO_FAILED" },
    { FLASH_LOG_TEST, "TEST" }, { 0x99U, "UNKNOWN" }
  };

  for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); i++)
  {
    TEST_ASSERT_EQUAL_STRING(names[i].n, flash_log_type_name(names[i].t));
  }
}

/* ---- монтування і форматування ------------------------------------------------ */

static void test_mount_blank_formats(void)
{
  const uint8_t *m = fake_w25q64_mem();

  mount_fresh();

  TEST_ASSERT_EQUAL(FLASH_LOG_FORMAT_BLANK, log_.mount_result);
  TEST_ASSERT_TRUE(log_.mounted);
  TEST_ASSERT_EQUAL_UINT32(1U, log_.next_seq);
  TEST_ASSERT_EQUAL_UINT16(1U, log_.boot);
  TEST_ASSERT_EQUAL_UINT32(0U, log_.head);
  TEST_ASSERT_FALSE(flash_log_needs_erase(&log_));
  TEST_ASSERT_EQUAL_UINT(FLASH_LOG_SECTORS + 1U, fake_w25q64_erase_count());
  TEST_ASSERT_EQUAL_HEX8(0x53, m[0]);            /* "SLMP" */
  TEST_ASSERT_EQUAL_HEX8(0x50, m[3]);
  TEST_ASSERT_EQUAL_UINT32(0U, flash_log_valid_count(&log_));
}

static void test_mount_formatted_keeps_data(void)
{
  mount_fresh();
  append_n(0U, 3U);
  reboot();

  TEST_ASSERT_EQUAL(FLASH_LOG_MOUNTED, log_.mount_result);
  TEST_ASSERT_EQUAL_UINT32(3U, flash_log_valid_count(&log_));
  TEST_ASSERT_EQUAL_UINT32(4U, log_.next_seq);
  TEST_ASSERT_EQUAL_UINT16(2U, log_.boot);
  TEST_ASSERT_EQUAL_UINT32(3U, log_.head);
}

static void test_mount_other_layout_reformats(void)
{
  uint8_t *m = fake_w25q64_mem();

  mount_fresh();
  append_n(0U, 5U);
  m[4] = 0x00U;                                  /* інша версія розмітки */
  reboot();

  TEST_ASSERT_EQUAL(FLASH_LOG_FORMAT_LAYOUT, log_.mount_result);
  TEST_ASSERT_EQUAL_UINT32(0U, flash_log_valid_count(&log_));
  TEST_ASSERT_EQUAL_UINT32(1U, log_.next_seq);
  TEST_ASSERT_TRUE(region_is_ff(FLASH_LOG_BASE, FLASH_LOG_END));
}

static void test_mount_errors(void)
{
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_mount(NULL, &dev));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_mount(&log_, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_format(NULL));

  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);   /* читання службового сектора */
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, flash_log_mount(&log_, &dev));
  TEST_ASSERT_FALSE(log_.mounted);

  /* Помилка під час форматування: службовий сектор прочитано (порожній),
   * перше стирання сектора журналу не вдалося */
  fake_w25q64_fail_from(2U, 1U, DEV_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, flash_log_mount(&log_, &dev));
  TEST_ASSERT_FALSE(log_.mounted);
}

static void test_format_superblock_write_error(void)
{
  mount_fresh();

  /* Живлення зникає після стирань (15 секторів журналу + службовий), тобто
   * на записі опису розмітки: формат не завершено */
  fake_w25q64_cut_after((FLASH_LOG_SECTORS + 1U) * W25Q64_SECTOR_SIZE);
  TEST_ASSERT_NOT_EQUAL(DEV_OK, flash_log_format(&log_));
  TEST_ASSERT_FALSE(log_.mounted);

  /* Наступний старт бачить порожній службовий сектор і форматує заново */
  reboot();
  TEST_ASSERT_EQUAL(FLASH_LOG_FORMAT_BLANK, log_.mount_result);
}

static void test_scan_read_error(void)
{
  flash_log_t l2;

  mount_fresh();
  append_n(0U, 2U);

  /* Службовий сектор — один кадр із двох transfer (команда, дані);
   * збій — на першій сторінці журналу */
  fake_w25q64_fail_from(2U, 1U, DEV_ERR_BUS);
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, flash_log_mount(&l2, &dev));
  TEST_ASSERT_FALSE(l2.mounted);
}

/* ---- критерій FR-08: 100 подій, перезапуск, порядок ------------------------------ */

static void test_100_events_survive_restart_in_order(void)
{
  flash_log_iter_t  it;
  flash_log_entry_t e;
  flash_log_slot_t  st;
  uint32_t n = 0U;

  mount_fresh();
  append_n(0U, 100U);
  reboot();

  TEST_ASSERT_EQUAL(FLASH_LOG_MOUNTED, log_.mount_result);
  TEST_ASSERT_EQUAL_UINT32(100U, flash_log_valid_count(&log_));
  TEST_ASSERT_EQUAL_UINT32(0U, flash_log_damaged_count(&log_));

  flash_log_iter_begin(&log_, &it);
  for (;;)
  {
    TEST_ASSERT_EQUAL(DEV_OK, flash_log_next(&log_, &it, &e, &st, NULL));
    if (st == FLASH_LOG_SLOT_EMPTY)
    {
      break;
    }
    TEST_ASSERT_EQUAL(FLASH_LOG_SLOT_VALID, st);
    TEST_ASSERT_EQUAL_UINT32(n + 1U, e.seq);
    TEST_ASSERT_EQUAL_UINT32(n, e.arg);
    TEST_ASSERT_EQUAL_UINT32(~n, e.arg2);
    TEST_ASSERT_EQUAL_UINT32(1000U + n, e.t_ms);
    TEST_ASSERT_EQUAL_UINT16(1U, e.boot);
    TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_TEST, e.type);
    TEST_ASSERT_EQUAL_HEX32(FLASH_LOG_NO_FRAME, e.frame_id);
    n++;
  }
  TEST_ASSERT_EQUAL_UINT32(100U, n);

  /* Після перезапуску нумерація продовжується, запуск — наступний */
  append_n(100U, 1U);
  {
    uint32_t first, last, dmg;

    TEST_ASSERT_EQUAL_UINT32(101U, walk(&first, &last, &dmg));
    TEST_ASSERT_EQUAL_UINT32(1U, first);
    TEST_ASSERT_EQUAL_UINT32(101U, last);
  }
}

static void test_damaged_record_detected_by_crc(void)
{
  flash_log_iter_t  it;
  flash_log_entry_t e;
  flash_log_slot_t  st;
  uint32_t addr = 0U;
  uint32_t pos = 0U;
  uint32_t damaged_pos = 0U;
  uint32_t first, last, dmg;

  mount_fresh();
  append_n(0U, 100U);

  /* NOR дозволяє скинути біти без стирання: так само зіпсуємо запис на стенді */
  fake_w25q64_mem()[flash_log_slot_addr(41U) + 21U] &= 0x7FU;   /* arg2 = ~41: старший біт байта 0xFF → 0 */
  reboot();

  TEST_ASSERT_EQUAL_UINT32(99U, flash_log_valid_count(&log_));
  TEST_ASSERT_EQUAL_UINT32(1U, flash_log_damaged_count(&log_));

  flash_log_iter_begin(&log_, &it);
  for (;;)
  {
    TEST_ASSERT_EQUAL(DEV_OK, flash_log_next(&log_, &it, &e, &st, &addr));
    if (st == FLASH_LOG_SLOT_EMPTY)
    {
      break;
    }
    if (st == FLASH_LOG_SLOT_DAMAGED)
    {
      damaged_pos = pos;
      TEST_ASSERT_EQUAL_HEX32(flash_log_slot_addr(41U), addr);
    }
    pos++;
  }
  TEST_ASSERT_EQUAL_UINT32(41U, damaged_pos);

  /* Решта записів — на місці і в порядку */
  TEST_ASSERT_EQUAL_UINT32(99U, walk(&first, &last, &dmg));
  TEST_ASSERT_EQUAL_UINT32(1U, dmg);
  TEST_ASSERT_EQUAL_UINT32(100U, last);
}

/* ---- обрив живлення ----------------------------------------------------------------- */

static void test_power_cut_during_record_write(void)
{
  uint32_t first, last, dmg;

  mount_fresh();
  append_n(0U, 10U);

  fake_w25q64_cut_after(12U);                    /* із 32 Б встигли 12 */
  (void)append_test(10U);
  TEST_ASSERT_FALSE(fake_w25q64_powered());

  reboot();
  TEST_ASSERT_EQUAL_UINT32(10U, flash_log_valid_count(&log_));
  TEST_ASSERT_EQUAL_UINT32(1U, flash_log_damaged_count(&log_));
  TEST_ASSERT_EQUAL_UINT32(11U, log_.head);      /* після пошкодженого */
  TEST_ASSERT_EQUAL_UINT32(11U, log_.next_seq);

  append_n(10U, 5U);
  TEST_ASSERT_EQUAL_UINT32(15U, walk(&first, &last, &dmg));
  TEST_ASSERT_EQUAL_UINT32(1U, dmg);
  TEST_ASSERT_EQUAL_UINT32(1U, first);
  TEST_ASSERT_EQUAL_UINT32(15U, last);
}

static void test_power_cut_during_first_record(void)
{
  mount_fresh();
  fake_w25q64_cut_after(3U);
  (void)append_test(0U);

  reboot();
  TEST_ASSERT_EQUAL_UINT32(0U, flash_log_valid_count(&log_));
  TEST_ASSERT_EQUAL_UINT32(1U, flash_log_damaged_count(&log_));
  /* Дійсних записів немає: почати з першого сектора, стерши його */
  TEST_ASSERT_EQUAL_UINT32(0U, log_.head);
  TEST_ASSERT_TRUE(flash_log_needs_erase(&log_));
  TEST_ASSERT_EQUAL_UINT32(1U, log_.next_seq);

  append_n(0U, 1U);
  TEST_ASSERT_EQUAL_UINT32(1U, flash_log_valid_count(&log_));
  TEST_ASSERT_EQUAL_UINT32(0U, flash_log_damaged_count(&log_));
}

static void test_power_cut_during_sector_erase(void)
{
  uint32_t first, last, dmg;

  mount_fresh();
  append_n(0U, FLASH_LOG_CAPACITY);              /* кільце заповнене, head знову на 0 */
  TEST_ASSERT_EQUAL_UINT32(0U, log_.head);
  TEST_ASSERT_TRUE(flash_log_needs_erase(&log_));

  fake_w25q64_cut_after(1000U);                  /* стирання сектора обірвалося */
  TEST_ASSERT_NOT_EQUAL(DEV_OK, append_test(FLASH_LOG_CAPACITY));

  reboot();
  /* Найбільший seq — в останньому секторі, тож дописування починається з
   * частково стертого сектора, який перед записом стирається заново */
  TEST_ASSERT_EQUAL_UINT32(0U, log_.head);
  TEST_ASSERT_TRUE(flash_log_needs_erase(&log_));
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_CAPACITY + 1U, log_.next_seq);

  append_n(FLASH_LOG_CAPACITY, 1U);
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_CAPACITY - FLASH_LOG_PER_SECTOR + 1U,
                           walk(&first, &last, &dmg));
  TEST_ASSERT_EQUAL_UINT32(0U, dmg);
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_PER_SECTOR + 1U, first);
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_CAPACITY + 1U, last);
}

/* ---- кільце і стирання ---------------------------------------------------------------- */

static void test_wraps_and_keeps_newest(void)
{
  uint32_t first, last, dmg;
  const uint32_t total = FLASH_LOG_CAPACITY + 5U;

  mount_fresh();
  append_n(0U, total);

  /* Сектор 0 стерто вдруге: у ньому 5 найновіших, решта — 14 повних секторів */
  TEST_ASSERT_EQUAL_UINT(2U, fake_w25q64_sector_erases(FLASH_LOG_BASE / W25Q64_SECTOR_SIZE));
  TEST_ASSERT_EQUAL_UINT32(5U, log_.head);
  TEST_ASSERT_EQUAL_UINT32((FLASH_LOG_SECTORS - 1U) * FLASH_LOG_PER_SECTOR + 5U,
                           walk(&first, &last, &dmg));
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_PER_SECTOR + 1U, first);
  TEST_ASSERT_EQUAL_UINT32(total, last);

  reboot();
  TEST_ASSERT_EQUAL_UINT32(5U, log_.head);
  TEST_ASSERT_EQUAL_UINT32(total + 1U, log_.next_seq);
  TEST_ASSERT_EQUAL_UINT32((FLASH_LOG_SECTORS - 1U) * FLASH_LOG_PER_SECTOR + 5U,
                           walk(&first, &last, &dmg));
  TEST_ASSERT_EQUAL_UINT32(total, last);
}

static void test_full_ring_before_erase_lists_all(void)
{
  uint32_t first, last, dmg;

  mount_fresh();
  append_n(0U, FLASH_LOG_CAPACITY);
  reboot();

  /* head на початку ще не стертого сектора 0: найстаріші записи там */
  TEST_ASSERT_TRUE(flash_log_needs_erase(&log_));
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_CAPACITY, walk(&first, &last, &dmg));
  TEST_ASSERT_EQUAL_UINT32(1U, first);
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_CAPACITY, last);
}

static void test_prepare_erases_ahead(void)
{
  unsigned before;

  mount_fresh();
  append_n(0U, FLASH_LOG_PER_SECTOR);            /* сектор 0 заповнено */
  TEST_ASSERT_TRUE(flash_log_needs_erase(&log_));
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_PER_SECTOR, log_.head);

  /* Перед першим записом у новий сектор модуль стирає його окремим кроком
   * (64 мс), щоб стирання не потрапило в один прохід циклу із записом */
  before = fake_w25q64_erase_count();
  TEST_ASSERT_EQUAL(DEV_OK, flash_log_prepare(&log_));
  TEST_ASSERT_EQUAL_UINT(before + 1U, fake_w25q64_erase_count());
  TEST_ASSERT_FALSE(flash_log_needs_erase(&log_));

  TEST_ASSERT_EQUAL(DEV_OK, flash_log_prepare(&log_));   /* вже нічого робити */
  append_n(FLASH_LOG_PER_SECTOR, 1U);
  TEST_ASSERT_EQUAL_UINT(before + 1U, fake_w25q64_erase_count());
}

static void test_prepare_and_append_errors(void)
{
  flash_log_t idle;
  flash_log_entry_t e = { 0U, 0U, FLASH_LOG_TEST, 0U, 0U, 0U, FLASH_LOG_NO_FRAME };

  memset(&idle, 0, sizeof(idle));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, flash_log_prepare(NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, flash_log_prepare(&idle));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_append(NULL, &e));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_append(&idle, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, flash_log_append(&idle, &e));
  TEST_ASSERT_FALSE(flash_log_needs_erase(NULL));

  mount_fresh();
  append_n(0U, FLASH_LOG_PER_SECTOR);
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);   /* стирання не вдалося */
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, append_test(0U));
  TEST_ASSERT_TRUE(flash_log_needs_erase(&log_));
  TEST_ASSERT_EQUAL_UINT32(FLASH_LOG_PER_SECTOR + 1U, log_.next_seq);   /* номер не використано */
}

static void test_write_error_consumes_slot_and_seq(void)
{
  mount_fresh();
  append_n(0U, 2U);

  fake_w25q64_fail_transfers(1U, DEV_ERR_TIMEOUT);
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, append_test(2U));
  TEST_ASSERT_EQUAL_UINT32(3U, log_.head);
  TEST_ASSERT_EQUAL_UINT32(4U, log_.next_seq);
  TEST_ASSERT_EQUAL_UINT32(1U, flash_log_damaged_count(&log_));

  append_n(3U, 1U);
  TEST_ASSERT_EQUAL_UINT32(3U, flash_log_valid_count(&log_));
}

static void test_readback_mismatch_reported(void)
{
  mount_fresh();
  /* Місце вже має скинуті біти (наприклад, після збою): запис не збігається */
  fake_w25q64_mem()[flash_log_slot_addr(0U) + 4U] = 0x00U;   /* молодший байт seq = 1 */
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, append_test(0x55U));
  TEST_ASSERT_EQUAL_UINT32(1U, flash_log_damaged_count(&log_));
}

static void test_iterator_guards_and_read_error(void)
{
  flash_log_t idle;
  flash_log_iter_t it;
  flash_log_entry_t e;
  flash_log_slot_t st;

  memset(&idle, 0, sizeof(idle));
  flash_log_iter_begin(&idle, &it);
  TEST_ASSERT_EQUAL_UINT32(0U, it.left);
  flash_log_iter_begin(NULL, NULL);
  flash_log_iter_begin(NULL, &it);
  TEST_ASSERT_EQUAL_UINT32(0U, it.left);
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_next(NULL, &it, &e, &st, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_next(&idle, NULL, &e, &st, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_next(&idle, &it, NULL, &st, NULL));
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, flash_log_next(&idle, &it, &e, NULL, NULL));

  mount_fresh();
  append_n(0U, 1U);
  flash_log_iter_begin(&log_, &it);
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, flash_log_next(&log_, &it, &e, &st, NULL));
}

static void test_other_regions_untouched(void)
{
  uint8_t *m = fake_w25q64_mem();

  memset(&m[FLASH_BENCH_SECTOR], 0x3C, 256U);    /* шаблон стендових команд */
  mount_fresh();
  append_n(0U, FLASH_LOG_CAPACITY + 200U);       /* кільце двічі стирає сектори */
  reboot();

  TEST_ASSERT_TRUE(region_is_ff(FLASH_FRAME_BASE, FLASH_FRAME_END));
  TEST_ASSERT_TRUE(region_is_ff(FLASH_FRAME_END, FLASH_BENCH_SECTOR));
  TEST_ASSERT_EQUAL_HEX8(0x3C, m[FLASH_BENCH_SECTOR]);
  TEST_ASSERT_EQUAL_HEX8(0x3C, m[FLASH_BENCH_SECTOR + 255U]);
  TEST_ASSERT_TRUE(region_is_ff(FLASH_SB_SIZE, W25Q64_SECTOR_SIZE));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_pack_layout_and_roundtrip);
  RUN_TEST(test_unpack_empty_and_damaged);
  RUN_TEST(test_type_names);
  RUN_TEST(test_mount_blank_formats);
  RUN_TEST(test_mount_formatted_keeps_data);
  RUN_TEST(test_mount_other_layout_reformats);
  RUN_TEST(test_mount_errors);
  RUN_TEST(test_format_superblock_write_error);
  RUN_TEST(test_scan_read_error);
  RUN_TEST(test_100_events_survive_restart_in_order);
  RUN_TEST(test_damaged_record_detected_by_crc);
  RUN_TEST(test_power_cut_during_record_write);
  RUN_TEST(test_power_cut_during_first_record);
  RUN_TEST(test_power_cut_during_sector_erase);
  RUN_TEST(test_wraps_and_keeps_newest);
  RUN_TEST(test_full_ring_before_erase_lists_all);
  RUN_TEST(test_prepare_erases_ahead);
  RUN_TEST(test_prepare_and_append_errors);
  RUN_TEST(test_write_error_consumes_slot_and_seq);
  RUN_TEST(test_readback_mismatch_reported);
  RUN_TEST(test_iterator_guards_and_read_error);
  RUN_TEST(test_other_regions_untouched);
  return UNITY_END();
}
