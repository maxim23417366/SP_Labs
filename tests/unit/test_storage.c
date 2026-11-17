/**
  * @file    test_storage.c
  * @brief   Журнал W25Q64 як бекенд sl_journal.c (storage.c): монтування, BOOT,
  *          нумерація після рестарту, перетворення записів, стирання і помилки,
  *          стендові команди d, e, z і рядкова LOG ERASE.
  *
  *          Флешка — fake_w25q64 (модель W25Q64 у RAM) через справжній драйвер.
  *          Ланцюжок той самий, що на платі: SmartLock_LogEvent() → черга
  *          sl_journal → SmartLock_StoragePoll() → SmartLock_JournalOps()->append().
  *          «Перезапуск» — SmartLock_JournalInit() (RAM після скидання) і повторний
  *          Storage_Init() з тим самим вмістом Flash. Вивід перехоплюється, як у
  *          test_app.c.
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "app.h"
#include "bench.h"
#include "cmd.h"
#include "fake_delay.h"
#include "fake_platform.h"
#include "fake_w25q64.h"
#include "flash_layout.h"
#include "sl_journal.h"
#include "smartlock_hooks.h"
#include "storage.h"

/* ---- перехоплення stdout --------------------------------------------------------- */
static char out[1 << 20];

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

static const char *cmd_chars;

static void run_cmds(void)
{
  for (const char *p = cmd_chars; *p != '\0'; p++)
  {
    Bench_Command(*p);
  }
}

static const char *cmd(const char *chars)
{
  cmd_chars = chars;
  return capture(run_cmds);
}

static const char *line_cmd;

static void run_line(void)
{
  Cmd_Execute(line_cmd);
}

static const char *line(const char *l)
{
  line_cmd = l;
  return capture(run_line);
}

/* Задача storage до спорожнення черги sl_journal */
static void drain(void)
{
  while (SmartLock_StoragePoll(0U))
  {
  }
}

static const char *poll_all(void)
{
  return capture(drain);
}

static unsigned count(const char *hay, const char *needle)
{
  unsigned n = 0U;

  for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle))
  {
    n++;
  }
  return n;
}

static void flash_present(void)
{
  app_devices.flash_status = w25q64_init(&app_devices.flash, fake_w25q64_bus(), fake_delay_ms);
  TEST_ASSERT_EQUAL(DEV_OK, app_devices.flash_status);
}

/* Скидання МК: RAM журналу застосунку порожня, Flash — як була */
static void restart(void)
{
  SmartLock_JournalInit();
  Storage_Init();
}

static dev_status_t append_rec(uint32_t seq, uint8_t type, uint32_t arg, uint32_t ref)
{
  sl_log_rec_t r = { seq, 1234U, arg, ref, type };

  return SmartLock_JournalOps()->append(&r);
}

/* Дописати записи прямо в журнал, поки head не стане на початок сектора */
static void fill_to_sector_end(void)
{
  flash_log_t *log = Storage_Journal();

  while ((log->head % FLASH_LOG_PER_SECTOR) != 0U || !log->head_needs_erase)
  {
    flash_log_entry_t e = { 0U, 0U, FLASH_LOG_TEST, 0U, 0U, 0U, FLASH_LOG_NO_FRAME };

    TEST_ASSERT_EQUAL(DEV_OK, flash_log_append(log, &e));
  }
  SmartLock_JournalSetLastSeq(log->next_seq - 1U);
}

void setUp(void)
{
  fake_platform_reset();
  fake_w25q64_reset();
  fake_delay_reset();
  memset(&app_devices, 0, sizeof(app_devices));
  app_devices.flash_status = DEV_ERR_STATE;
  SmartLock_JournalInit();
}

void tearDown(void)
{
  TEST_ASSERT_EQUAL_UINT(0U, fake_w25q64_protocol_errors());
}

/* ---- без флешки -------------------------------------------------------------------- */

static void test_without_flash_everything_is_off(void)
{
  const char *o = capture(Storage_Init);

  TEST_ASSERT_EQUAL_STRING("LOG,MOUNT,err=state,note=flash_absent\r\n", o);
  TEST_ASSERT_FALSE(Storage_Ready());

  /* Подія все одно отримує ID і друкується, але у Flash не потрапляє */
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_LogEvent(SL_LOG_MOTION, 0U));
  o = poll_all();
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT,id=1,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "name=MOTION,arg=0,flash=err\r\n"));
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_JournalUnsaved());

  o = cmd("dez");
  TEST_ASSERT_EQUAL_STRING("FLOG,cmd=d,err=state\r\nFLOG,cmd=e,err=state\r\nFLOG,cmd=z,err=state\r\n", o);
  TEST_ASSERT_FALSE(Storage_BenchCommand('q'));
  TEST_ASSERT_EQUAL_STRING("FLOG,cmd=LOG ERASE,err=state\r\n", line("LOG ERASE"));
  TEST_ASSERT_FALSE(Storage_LineCommand("STATS"));
  TEST_ASSERT_FALSE(Storage_LineCommand(NULL));
  TEST_ASSERT_EQUAL_STRING("LOG,SUMMARY,err=state\r\n", capture(Storage_Dump));
}

static void test_mount_error_reported(void)
{
  const char *o;

  flash_present();
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  o = capture(Storage_Init);
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,MOUNT,err=bus,us="));
  TEST_ASSERT_FALSE(Storage_Ready());
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, append_rec(5U, SL_LOG_MOTION, 0U, 0U));
}

/* ---- старт і рестарт --------------------------------------------------------------- */

static void test_first_boot_formats_and_writes_boot(void)
{
  const char *o;

  flash_present();
  o = capture(Storage_Init);
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,MOUNT,result=format_blank,valid=0,damaged=0,next_seq=1,boot=1,head=0,us="));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=1,boot=1,t_ms=0,type=BOOT,arg=0,arg2=0,frame=none,crc=ok\r\n"));
  TEST_ASSERT_TRUE(Storage_Ready());
  TEST_ASSERT_EQUAL_UINT32(0U, Storage_Errors());

  /* Нумерація подій продовжується після BOOT: ID у UART = seq у Flash */
  TEST_ASSERT_EQUAL_UINT32(1U, SmartLock_JournalLastSeq());
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_LogEvent(SL_LOG_LOCK_ACTIVATED, 15000U));
  o = poll_all();
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT,id=2,"));
  TEST_ASSERT_NOT_NULL(strstr(o, ",flash=ok\r\n"));

  o = cmd("d");
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=2,boot=1,t_ms=0,type=LOCK_ACTIVATED,arg=15000,arg2=0,frame=none,crc=ok\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,SUMMARY,valid=2,damaged=0,empty=1918,first_seq=1,last_seq=2,order=ok,boot=1\r\n"));
}

static void test_restart_continues_seq_and_boot(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  (void)SmartLock_LogEvent(SL_LOG_MOTION, 0U);
  (void)SmartLock_LogEvent(SL_LOG_TAMPER, 300U);
  (void)poll_all();

  o = capture(restart);
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,MOUNT,result=mounted,valid=3,damaged=0,next_seq=4,boot=2,head=3,us="));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=4,boot=2,"));
  TEST_ASSERT_EQUAL_UINT32(5U, SmartLock_LogEvent(SL_LOG_UNLOCKED, 0U));   /* не 1 після рестарту */
  (void)poll_all();

  o = cmd("d");
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=3,boot=1,t_ms=0,type=TAMPER,arg=300,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=5,boot=2,t_ms=0,type=UNLOCKED,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "first_seq=1,last_seq=5,order=ok,boot=2\r\n"));
}

/* ---- перетворення записів ---------------------------------------------------------- */

static void test_flash_type_mapping(void)
{
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_LOCK_ACTIVATED, Storage_FlashType(SL_LOG_LOCK_ACTIVATED));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_LOCKED, Storage_FlashType(SL_LOG_LOCKED));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_UNLOCKED, Storage_FlashType(SL_LOG_UNLOCKED));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_ACCESS_GRANTED, Storage_FlashType(SL_LOG_ACCESS_GRANTED));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_WRONG_CODE, Storage_FlashType(SL_LOG_WRONG_CODE));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_MOTION, Storage_FlashType(SL_LOG_MOTION));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_TAMPER, Storage_FlashType(SL_LOG_TAMPER));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_FORCED_OPEN, Storage_FlashType(SL_LOG_FORCED_OPEN));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_ENV_WARNING, Storage_FlashType(SL_LOG_ENV_WARNING));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_DOOR_OPEN, Storage_FlashType(SL_LOG_DOOR_OPEN));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_DOOR_CLOSED, Storage_FlashType(SL_LOG_DOOR_CLOSED));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_PHOTO_SAVED, Storage_FlashType(SL_LOG_PHOTO));
  TEST_ASSERT_EQUAL_HEX8(FLASH_LOG_PHOTO_FAILED, Storage_FlashType(SL_LOG_PHOTO_ERR));   /* K08 */
  TEST_ASSERT_EQUAL_HEX8(0U, Storage_FlashType(SL_LOG_NONE));
  TEST_ASSERT_EQUAL_HEX8(0U, Storage_FlashType(SL_LOG_COUNT));
}

static void test_photo_and_ref_fields(void)
{
  uint32_t ev;
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  ev = SmartLock_LogEvent(SL_LOG_MOTION, 0U);
  (void)SmartLock_LogEventRef(SL_LOG_PHOTO, 7U, ev);          /* кадр 7 до події ev */
  (void)SmartLock_LogEventRef(SL_LOG_WRONG_CODE, 4U, 99U);    /* ref іде в arg2 */
  (void)SmartLock_LogEventRef(SL_LOG_PHOTO_ERR, 4U, ev);      /* K08: причина 4 до події ev */
  (void)poll_all();

  o = cmd("d");
  TEST_ASSERT_NOT_NULL(strstr(o, "type=PHOTO_SAVED,arg=2,arg2=0,frame=7,crc=ok\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "type=WRONG_CODE,arg=4,arg2=99,frame=none,crc=ok\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "type=PHOTO_FAILED,arg=2,arg2=4,frame=none,crc=ok\r\n"));
}

static void test_seq_gap_allowed_and_stale_seq_rejected(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);   /* BOOT = seq 1 */

  /* Подію відкинуто з повної черги sl_journal: її ID пропущено — пропуск дозволений */
  SmartLock_JournalSetLastSeq(5U);
  TEST_ASSERT_EQUAL_UINT32(6U, SmartLock_LogEvent(SL_LOG_MOTION, 0U));
  o = poll_all();
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT,id=6,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "flash=ok"));

  /* Номер, менший за вже записаний, порушив би порядок */
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, append_rec(3U, SL_LOG_MOTION, 0U, 0U));
  TEST_ASSERT_EQUAL_UINT32(1U, Storage_Errors());

  /* Невідомий тип у Flash не пишеться */
  TEST_ASSERT_EQUAL(DEV_ERR_PARAM, append_rec(7U, SL_LOG_NONE, 0U, 0U));
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, SmartLock_JournalOps()->append(NULL));

  o = cmd("d");
  TEST_ASSERT_NOT_NULL(strstr(o, "valid=2,damaged=0,empty=1918,first_seq=1,last_seq=6,order=ok"));
}

/* ---- стирання і помилки ------------------------------------------------------------ */

static void test_append_erases_next_sector_once(void)
{
  unsigned erases;

  flash_present();
  (void)capture(Storage_Init);
  fill_to_sector_end();

  erases = fake_w25q64_erase_count();
  TEST_ASSERT_EQUAL(DEV_OK, append_rec(SmartLock_JournalLastSeq() + 1U, SL_LOG_MOTION, 0U, 0U));
  TEST_ASSERT_EQUAL_UINT(erases + 1U, fake_w25q64_erase_count());
  TEST_ASSERT_EQUAL(DEV_OK, append_rec(SmartLock_JournalLastSeq() + 2U, SL_LOG_MOTION, 0U, 0U));
  TEST_ASSERT_EQUAL_UINT(erases + 1U, fake_w25q64_erase_count());
}

static dev_status_t s_last;

static void append_next(void)
{
  s_last = append_rec(Storage_Journal()->next_seq, SL_LOG_MOTION, 0U, 0U);
}

static void test_erase_failures_disable_journal(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  fill_to_sector_end();

  for (unsigned i = 1U; i < TBD_STORAGE_ERASE_RETRIES; i++)
  {
    fake_w25q64_fail_transfers(1U, DEV_ERR_TIMEOUT);
    o = capture(append_next);
    TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, s_last);
    TEST_ASSERT_NOT_NULL(strstr(o, "LOG,ERR,op=erase,err=timeout,attempt="));
    TEST_ASSERT_TRUE(Storage_Ready());
  }
  fake_w25q64_fail_transfers(1U, DEV_ERR_TIMEOUT);
  o = capture(append_next);
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,ERR,op=disable\r\n"));
  TEST_ASSERT_FALSE(Storage_Ready());
  TEST_ASSERT_EQUAL_UINT32(TBD_STORAGE_ERASE_RETRIES, Storage_Errors());
}

static void test_erase_recovers_after_single_failure(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  fill_to_sector_end();

  fake_w25q64_fail_transfers(1U, DEV_ERR_TIMEOUT);
  o = capture(append_next);
  TEST_ASSERT_NOT_NULL(strstr(o, "attempt=1"));
  o = capture(append_next);
  TEST_ASSERT_EQUAL(DEV_OK, s_last);
  TEST_ASSERT_EQUAL_STRING("", o);
  TEST_ASSERT_TRUE(Storage_Ready());
}

static void test_append_error_reported(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  (void)SmartLock_LogEvent(SL_LOG_FORCED_OPEN, 0U);
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  o = poll_all();
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,ERR,op=append,seq=2,err=bus\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "name=FORCED_OPEN,arg=0,flash=err\r\n"));
  TEST_ASSERT_EQUAL_UINT32(1U, Storage_Errors());
}

static uint32_t s_rec_seq;
static dev_status_t s_rec_st;

static void write_test_record(void)
{
  s_rec_st = Storage_WriteRecord(FLASH_LOG_TEST, 5U, 6U, 7U, &s_rec_seq);
}

/* Прямий запис поза чергою (стендова команда кадрів p) */
static void test_write_record_direct(void)
{
  const char *o;

  (void)capture(Storage_Init);                    /* без флешки: журнал вимкнений */
  TEST_ASSERT_EQUAL(DEV_ERR_STATE, Storage_WriteRecord(FLASH_LOG_TEST, 0U, 0U, 0U, &s_rec_seq));
  TEST_ASSERT_EQUAL_UINT32(0U, s_rec_seq);

  flash_present();
  (void)capture(Storage_Init);                    /* BOOT = 1 */
  o = capture(write_test_record);
  TEST_ASSERT_EQUAL(DEV_OK, s_rec_st);
  TEST_ASSERT_EQUAL_UINT32(2U, s_rec_seq);
  TEST_ASSERT_EQUAL_STRING("LOG,seq=2,boot=1,t_ms=0,type=TEST,arg=5,arg2=6,frame=7,crc=ok\r\n", o);
  TEST_ASSERT_EQUAL_UINT32(2U, SmartLock_JournalLastSeq());   /* події далі — з 3 */
  (void)capture(write_test_record);               /* seq 3; seq може бути NULL — нижче */
  TEST_ASSERT_EQUAL_UINT32(3U, s_rec_seq);

  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  o = capture(write_test_record);
  TEST_ASSERT_EQUAL(DEV_ERR_BUS, s_rec_st);
  TEST_ASSERT_EQUAL_UINT32(4U, s_rec_seq);        /* номер використано */
  TEST_ASSERT_EQUAL_STRING("LOG,ERR,op=append,seq=4,err=bus\r\n", o);

  fill_to_sector_end();
  fake_w25q64_fail_transfers(1U, DEV_ERR_TIMEOUT);
  o = capture(write_test_record);
  TEST_ASSERT_EQUAL(DEV_ERR_TIMEOUT, s_rec_st);
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,ERR,op=erase,err=timeout,attempt=1"));
}

static void test_hash_word(void)
{
  TEST_ASSERT_EQUAL_HEX32(0x7BAF32E0U, Storage_HashWord("7baf32e0-dirty"));
  TEST_ASSERT_EQUAL_HEX32(0x7BAF32EU, Storage_HashWord("7BAF32E"));
  TEST_ASSERT_EQUAL_HEX32(0x0U, Storage_HashWord("nogit"));
  TEST_ASSERT_EQUAL_HEX32(0x0U, Storage_HashWord(NULL));
}

/* ---- стенд ------------------------------------------------------------------------- */

/* Картка журналу: 100 послідовних подій записано й прочитано; після рестарту
 * порядок зберігся; пошкоджений запис визначається CRC */
static void test_bench_100_events_restart_corrupt(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);                    /* BOOT = 1 */

  o = cmd("e");
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,test=100,ok=100,first_seq=2,last_seq=101,erases=0,"));
  TEST_ASSERT_NOT_NULL(strstr(o, ",last=ok\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,readback=PASS,match=100,of=100\r\n"));
  TEST_ASSERT_EQUAL_UINT32(101U, SmartLock_JournalLastSeq());   /* події далі — з 102 */

  o = capture(restart);
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,MOUNT,result=mounted,valid=101,damaged=0,next_seq=102,boot=2,"));
  o = cmd("d");
  TEST_ASSERT_EQUAL_UINT(100U, count(o, "type=TEST"));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=2,boot=1,t_ms=0,type=TEST,arg=1,arg2=4294967294,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=101,boot=1,t_ms=0,type=TEST,arg=100,arg2=4294967195,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "valid=102,damaged=0,empty=1818,first_seq=1,last_seq=102,order=ok,boot=2\r\n"));

  o = cmd("z");
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,corrupt=DONE,addr=0x001CA0,seq=102\r\n"));
  o = cmd("d");
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,addr=0x001CA0,crc=bad\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "valid=101,damaged=1,"));

  /* Пошкоджено саме BOOT другого запуску: його seq і boot невідомі, тож новий
   * запуск бере їх знову — серед дійсних записів повторів немає */
  o = capture(restart);
  TEST_ASSERT_NOT_NULL(strstr(o, "result=mounted,valid=101,damaged=1,next_seq=102,boot=2,head=102,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,seq=102,boot=2,t_ms=0,type=BOOT,"));
}

static void test_bench_events_cross_sector_and_failure(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  o = cmd("e");                                   /* 2..101 */
  o = cmd("e");                                   /* 102..201: перехід у сектор 1 */
  TEST_ASSERT_NOT_NULL(strstr(o, "first_seq=102,last_seq=201,erases=1,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "readback=PASS"));

  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);    /* перший запис серії не вдався */
  o = cmd("e");
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,test=100,ok=99,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,readback=FAIL,match=99,of=100\r\n"));
}

static void test_bench_corrupt_skips_and_errors(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  o = cmd("z");                                   /* BOOT — валідний, псується */
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,corrupt=DONE,addr=0x001000,seq=1\r\n"));
  o = cmd("z");                                   /* вже пошкоджений */
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,corrupt=SKIP,addr=0x001000,note=no_valid_record\r\n"));

  (void)SmartLock_LogEvent(SL_LOG_MOTION, 0U);
  (void)poll_all();
  fake_w25q64_fail_from(2U, 1U, DEV_ERR_BUS);     /* читання OK, запис нуля — збій */
  o = cmd("z");
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,corrupt=FAIL,addr=0x001020,err=bus\r\n"));
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);    /* збій уже на читанні */
  o = cmd("z");
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,corrupt=FAIL,"));
}

static void test_dump_read_error(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  o = cmd("d");
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,ERR,op=read,err=bus\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,SUMMARY,valid=0,"));
}

static void test_log_erase_line_command(void)
{
  const char *o;

  flash_present();
  (void)capture(Storage_Init);
  (void)cmd("e");

  o = line("LOG ERASE");
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,format=PASS,us="));
  TEST_ASSERT_TRUE(Storage_Ready());
  o = cmd("d");
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG,SUMMARY,valid=0,damaged=0,empty=1920,"));

  /* Після стирання ID sl_journal не повертається назад: пропуск дозволений */
  (void)SmartLock_LogEvent(SL_LOG_LOCKED, 0U);
  o = poll_all();
  TEST_ASSERT_NOT_NULL(strstr(o, "EVT,id=102,"));
  TEST_ASSERT_NOT_NULL(strstr(o, "flash=ok"));

  fake_w25q64_fail_transfers(1U, DEV_ERR_BUS);
  o = line("LOG ERASE");
  TEST_ASSERT_NOT_NULL(strstr(o, "FLOG,format=FAIL,"));
  TEST_ASSERT_NOT_NULL(strstr(o, ",err=bus\r\n"));
  TEST_ASSERT_FALSE(Storage_Ready());

  o = line("HELP");
  TEST_ASSERT_NOT_NULL(strstr(o, "LOG ERASE"));
}

static void test_bench_help_lists_journal_commands(void)
{
  const char *o = capture(Bench_Help);

  TEST_ASSERT_NOT_NULL(strstr(o, "d journal dump | e journal +100 test events | "
                                 "z corrupt newest record | LOG ERASE erase journal\r\n"));
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_without_flash_everything_is_off);
  RUN_TEST(test_mount_error_reported);
  RUN_TEST(test_first_boot_formats_and_writes_boot);
  RUN_TEST(test_restart_continues_seq_and_boot);
  RUN_TEST(test_flash_type_mapping);
  RUN_TEST(test_photo_and_ref_fields);
  RUN_TEST(test_seq_gap_allowed_and_stale_seq_rejected);
  RUN_TEST(test_append_erases_next_sector_once);
  RUN_TEST(test_erase_failures_disable_journal);
  RUN_TEST(test_erase_recovers_after_single_failure);
  RUN_TEST(test_append_error_reported);
  RUN_TEST(test_write_record_direct);
  RUN_TEST(test_hash_word);
  RUN_TEST(test_bench_100_events_restart_corrupt);
  RUN_TEST(test_bench_events_cross_sector_and_failure);
  RUN_TEST(test_bench_corrupt_skips_and_errors);
  RUN_TEST(test_dump_read_error);
  RUN_TEST(test_log_erase_line_command);
  RUN_TEST(test_bench_help_lists_journal_commands);
  return UNITY_END();
}
