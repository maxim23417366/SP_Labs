/**
  ******************************************************************************
  * @file    storage.c
  * @brief   Журнал подій у W25Q64 як бекенд sl_journal.c (див. storage.h).
  *
  *          Рядки UART (docs/lab_3/flash-layout.md, розділ 3.5):
  *            LOG,MOUNT,result=...,valid=...,damaged=...,next_seq=...,boot=...,head=...,us=...
  *            LOG,seq=17,boot=3,t_ms=48213,type=MOTION,arg=0,arg2=0,frame=none,crc=ok
  *            LOG,addr=0x0012A0,crc=bad
  *            LOG,SUMMARY,valid=...,damaged=...,empty=...,first_seq=...,last_seq=...,order=ok|bad,boot=...
  *            LOG,ERR,op=erase|append|disable|read,...
  *          Успішний запис окремого рядка LOG не дає: його видно в рядку EVT
  *          (sl_journal.c) як flash=ok.
  *          Стендові команди:
  *            FLOG,test=100,ok=...,first_seq=...,last_seq=...,erases=...,min_us=...,avg_us=...,max_us=...,last=...
  *            FLOG,readback=PASS|FAIL,match=...,of=100
  *            FLOG,corrupt=DONE,addr=...,seq=...
  *            FLOG,format=PASS|FAIL,us=...,err=...
  ******************************************************************************
  */
#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "app_config.h"
#include "platform.h"
#include "stand_test.h"

#define BENCH_EVENTS 100U   /* FR-08: 100 послідовних подій (картка журналу, PRD) */

static flash_log_t s_log;
static bool        s_ready;
static uint32_t    s_errors;
static unsigned    s_erase_fails;

/* Збірка без sl_journal.c (тест слабких хуків, tests/Makefile) не має цієї
 * функції; у прошивці її замінює сильна реалізація з sl_journal.c */
__attribute__((weak)) void SmartLock_JournalSetLastSeq(uint32_t last_seq)
{
  (void)last_seq;
}

static const char *mount_name(flash_log_mount_t m)
{
  switch (m)
  {
    case FLASH_LOG_FORMAT_BLANK:  return "format_blank";
    case FLASH_LOG_FORMAT_LAYOUT: return "format_layout";
    default:                      return "mounted";
  }
}

static uint32_t elapsed_us(uint32_t start)
{
  return Platform_CyclesToUs(Platform_Cycles() - start);
}

static void print_entry(const flash_log_entry_t *e)
{
  printf("LOG,seq=%lu,boot=%u,t_ms=%lu,type=%s,arg=%lu,arg2=%lu,frame=",
         (unsigned long)e->seq, (unsigned)e->boot, (unsigned long)e->t_ms,
         flash_log_type_name(e->type), (unsigned long)e->arg, (unsigned long)e->arg2);
  if (e->frame_id == FLASH_LOG_NO_FRAME)
  {
    printf("none");
  }
  else
  {
    printf("%lu", (unsigned long)e->frame_id);
  }
  printf(",crc=ok\r\n");
}

/* Після записів поза sl_journal (BOOT, тестові) нумерація має йти далі */
static void sync_journal_seq(void)
{
  SmartLock_JournalSetLastSeq(s_log.next_seq - 1U);
}

uint32_t Storage_HashWord(const char *git_hash)
{
  uint32_t v = 0U;

  for (unsigned i = 0U; git_hash != NULL && i < 8U; i++)
  {
    char ch = git_hash[i];
    uint32_t d;

    if (ch >= '0' && ch <= '9')
    {
      d = (uint32_t)(ch - '0');
    }
    else if (ch >= 'a' && ch <= 'f')
    {
      d = (uint32_t)(ch - 'a' + 10);
    }
    else if (ch >= 'A' && ch <= 'F')
    {
      d = (uint32_t)(ch - 'A' + 10);
    }
    else
    {
      break;
    }
    v = (v << 4) | d;
  }
  return v;
}

uint8_t Storage_FlashType(sl_log_type_t type)
{
  switch (type)
  {
    case SL_LOG_LOCK_ACTIVATED: return FLASH_LOG_LOCK_ACTIVATED;
    case SL_LOG_LOCKED:         return FLASH_LOG_LOCKED;
    case SL_LOG_UNLOCKED:       return FLASH_LOG_UNLOCKED;
    case SL_LOG_ACCESS_GRANTED: return FLASH_LOG_ACCESS_GRANTED;
    case SL_LOG_WRONG_CODE:     return FLASH_LOG_WRONG_CODE;
    case SL_LOG_MOTION:         return FLASH_LOG_MOTION;
    case SL_LOG_TAMPER:         return FLASH_LOG_TAMPER;
    case SL_LOG_FORCED_OPEN:    return FLASH_LOG_FORCED_OPEN;
    case SL_LOG_ENV_WARNING:    return FLASH_LOG_ENV_WARNING;
    case SL_LOG_DOOR_OPEN:      return FLASH_LOG_DOOR_OPEN;
    case SL_LOG_DOOR_CLOSED:    return FLASH_LOG_DOOR_CLOSED;
    case SL_LOG_PHOTO:          return FLASH_LOG_PHOTO_SAVED;
    default:                    return 0U;
  }
}

/* ---- ініціалізація ------------------------------------------------------------- */

void Storage_Init(void)
{
  flash_log_entry_t boot;
  uint32_t t0;
  dev_status_t st;

  memset(&s_log, 0, sizeof(s_log));
  s_ready = false;
  s_errors = 0U;
  s_erase_fails = 0U;

  if (app_devices.flash_status != DEV_OK)
  {
    printf("LOG,MOUNT,err=%s,note=flash_absent\r\n", StandTest_ErrStr(app_devices.flash_status));
    return;
  }

  t0 = Platform_Cycles();
  st = flash_log_mount(&s_log, &app_devices.flash);
  if (st != DEV_OK)
  {
    printf("LOG,MOUNT,err=%s,us=%lu\r\n", StandTest_ErrStr(st), (unsigned long)elapsed_us(t0));
    return;
  }

  s_ready = true;
  printf("LOG,MOUNT,result=%s,valid=%lu,damaged=%lu,next_seq=%lu,boot=%u,head=%lu,us=%lu\r\n",
         mount_name(s_log.mount_result), (unsigned long)flash_log_valid_count(&s_log),
         (unsigned long)flash_log_damaged_count(&s_log), (unsigned long)s_log.next_seq,
         (unsigned)s_log.boot, (unsigned long)s_log.head, (unsigned long)elapsed_us(t0));

  /* Запис про старт — до запуску задач, тож блокувальне стирання тут не заважає */
  memset(&boot, 0, sizeof(boot));
  boot.type = FLASH_LOG_BOOT;
  boot.t_ms = Platform_Millis();
  boot.arg = Storage_HashWord(GIT_HASH);
  boot.frame_id = FLASH_LOG_NO_FRAME;
  st = flash_log_append(&s_log, &boot);
  if (st == DEV_OK)
  {
    print_entry(&boot);
  }
  else
  {
    s_errors++;
    printf("LOG,ERR,op=append,seq=%lu,err=%s\r\n", (unsigned long)boot.seq, StandTest_ErrStr(st));
  }
  sync_journal_seq();
}

bool Storage_Ready(void)
{
  return s_ready;
}

flash_log_t *Storage_Journal(void)
{
  return &s_log;
}

uint32_t Storage_Errors(void)
{
  return s_errors;
}

/* ---- бекенд sl_journal ----------------------------------------------------------- */

/* Стерти сектор наступного запису. Після кількох невдач поспіль журнал
 * вимикається, щоб кожен запис не блокувався на тайм-ауті стирання. */
static dev_status_t erase_if_needed(void)
{
  dev_status_t st;

  if (!flash_log_needs_erase(&s_log))
  {
    return DEV_OK;
  }
  st = flash_log_prepare(&s_log);
  if (st == DEV_OK)
  {
    s_erase_fails = 0U;
    return DEV_OK;
  }

  s_errors++;
  s_erase_fails++;
  printf("LOG,ERR,op=erase,err=%s,attempt=%u\r\n", StandTest_ErrStr(st), s_erase_fails);
  if (s_erase_fails >= TBD_STORAGE_ERASE_RETRIES)
  {
    s_ready = false;
    printf("LOG,ERR,op=disable\r\n");
  }
  return st;
}

dev_status_t Storage_Append(const sl_log_rec_t *rec)
{
  flash_log_entry_t e;
  dev_status_t st;
  uint8_t type;

  if (!s_ready || rec == NULL)
  {
    return DEV_ERR_STATE;
  }
  type = Storage_FlashType((sl_log_type_t)rec->type);
  if (type == 0U)
  {
    return DEV_ERR_PARAM;
  }

  st = erase_if_needed();
  if (st != DEV_OK)
  {
    return st;   /* запис не збережено: sl_journal друкує flash=err */
  }

  memset(&e, 0, sizeof(e));
  e.seq = rec->seq;          /* той самий ID, що в рядку EVT */
  e.type = type;
  e.t_ms = rec->t_ms;
  e.frame_id = FLASH_LOG_NO_FRAME;
  if (rec->type == (uint8_t)SL_LOG_PHOTO)
  {
    /* sl_journal: arg = ID кадру, ref = seq події; flash-layout.md, розділ 3.2:
     * arg = seq події MOTION / TAMPER, frame_id = номер кадру */
    e.arg = rec->ref;
    e.frame_id = rec->arg;
  }
  else
  {
    e.arg = rec->arg;
    e.arg2 = rec->ref;
  }

  st = flash_log_append(&s_log, &e);
  if (st != DEV_OK)
  {
    s_errors++;
    printf("LOG,ERR,op=append,seq=%lu,err=%s\r\n", (unsigned long)rec->seq, StandTest_ErrStr(st));
  }
  return st;
}

dev_status_t Storage_WriteRecord(uint8_t type, uint32_t arg, uint32_t arg2, uint32_t frame_id,
                                 uint32_t *seq)
{
  flash_log_entry_t e;
  dev_status_t st;

  if (seq != NULL)
  {
    *seq = 0U;
  }
  if (!s_ready)
  {
    return DEV_ERR_STATE;
  }
  st = erase_if_needed();
  if (st != DEV_OK)
  {
    return st;
  }

  memset(&e, 0, sizeof(e));
  e.type = type;
  e.t_ms = Platform_Millis();
  e.arg = arg;
  e.arg2 = arg2;
  e.frame_id = frame_id;
  st = flash_log_append(&s_log, &e);
  if (seq != NULL)
  {
    *seq = e.seq;   /* номер використано за будь-якого результату */
  }
  sync_journal_seq();

  if (st == DEV_OK)
  {
    print_entry(&e);
  }
  else
  {
    s_errors++;
    printf("LOG,ERR,op=append,seq=%lu,err=%s\r\n", (unsigned long)e.seq, StandTest_ErrStr(st));
  }
  return st;
}

/* ---- читання ------------------------------------------------------------------- */

void Storage_Dump(void)
{
  flash_log_iter_t it;
  flash_log_entry_t e;
  flash_log_slot_t state;
  uint32_t addr = 0U;
  uint32_t valid = 0U;
  uint32_t damaged = 0U;
  uint32_t first = 0U;
  uint32_t last = 0U;
  bool ordered = true;
  dev_status_t st = DEV_OK;

  if (!s_ready)
  {
    printf("LOG,SUMMARY,err=state\r\n");
    return;
  }

  flash_log_iter_begin(&s_log, &it);
  for (;;)
  {
    st = flash_log_next(&s_log, &it, &e, &state, &addr);
    if (st != DEV_OK || state == FLASH_LOG_SLOT_EMPTY)
    {
      break;
    }
    if (state == FLASH_LOG_SLOT_DAMAGED)
    {
      damaged++;
      printf("LOG,addr=0x%06lX,crc=bad\r\n", (unsigned long)addr);
      continue;
    }
    if (valid == 0U)
    {
      first = e.seq;
    }
    else if (e.seq <= last)
    {
      ordered = false;   /* від найстарішого до найновішого seq лише зростає */
    }
    last = e.seq;
    valid++;
    print_entry(&e);
  }

  if (st != DEV_OK)
  {
    printf("LOG,ERR,op=read,err=%s\r\n", StandTest_ErrStr(st));
  }
  printf("LOG,SUMMARY,valid=%lu,damaged=%lu,empty=%lu,first_seq=%lu,last_seq=%lu,order=%s,boot=%u\r\n",
         (unsigned long)valid, (unsigned long)damaged,
         (unsigned long)(FLASH_LOG_CAPACITY - valid - damaged),
         (unsigned long)first, (unsigned long)last, ordered ? "ok" : "bad", (unsigned)s_log.boot);
}

/* ---- стендові команди ---------------------------------------------------------- */

/* Прочитати журнал і порахувати тестові записи first..last з очікуваним вмістом */
static unsigned bench_readback(uint32_t first, uint32_t last)
{
  flash_log_iter_t it;
  flash_log_entry_t e;
  flash_log_slot_t state;
  unsigned match = 0U;

  flash_log_iter_begin(&s_log, &it);
  while (flash_log_next(&s_log, &it, &e, &state, NULL) == DEV_OK && state != FLASH_LOG_SLOT_EMPTY)
  {
    uint32_t n;

    if (state != FLASH_LOG_SLOT_VALID || e.type != FLASH_LOG_TEST || e.seq < first || e.seq > last)
    {
      continue;
    }
    n = e.seq - first + 1U;   /* порядковий номер тестової події */
    if (e.arg == n && e.arg2 == ~n && e.frame_id == FLASH_LOG_NO_FRAME)
    {
      match++;
    }
  }
  return match;
}

/* e: 100 тестових подій поспіль з виміром часу кожного запису, потім читання назад */
static void bench_events(void)
{
  uint32_t min = 0U;
  uint32_t max = 0U;
  uint32_t sum = 0U;
  uint32_t first = 0U;
  uint32_t last = 0U;
  unsigned ok = 0U;
  unsigned erases = 0U;
  unsigned match;
  dev_status_t st = DEV_OK;

  for (uint32_t i = 1U; i <= BENCH_EVENTS; i++)
  {
    flash_log_entry_t e = { 0U, 0U, FLASH_LOG_TEST, Platform_Millis(), i, ~i, FLASH_LOG_NO_FRAME };
    uint32_t t0;
    uint32_t us;

    if (flash_log_needs_erase(&s_log))
    {
      erases++;
    }
    t0 = Platform_Cycles();
    st = flash_log_append(&s_log, &e);
    us = elapsed_us(t0);

    if (st == DEV_OK)
    {
      ok++;
    }
    if (i == 1U)
    {
      first = e.seq;
      min = us;
    }
    last = e.seq;
    min = (us < min) ? us : min;
    max = (us > max) ? us : max;
    sum += us;
  }
  sync_journal_seq();

  printf("FLOG,test=%u,ok=%u,first_seq=%lu,last_seq=%lu,erases=%u,"
         "min_us=%lu,avg_us=%lu,max_us=%lu,last=%s\r\n",
         (unsigned)BENCH_EVENTS, ok, (unsigned long)first, (unsigned long)last, erases,
         (unsigned long)min, (unsigned long)(sum / BENCH_EVENTS), (unsigned long)max,
         StandTest_ErrStr(st));

  match = bench_readback(first, last);
  printf("FLOG,readback=%s,match=%u,of=%u\r\n",
         (match == BENCH_EVENTS && last - first + 1U == BENCH_EVENTS) ? "PASS" : "FAIL",
         match, (unsigned)BENCH_EVENTS);
}

/* z: зіпсувати найновіший запис, скинувши біти байта flags без стирання (NOR
 * дозволяє 1 → 0). CRC запису після цього не збігається. */
static void bench_corrupt(void)
{
  static const uint8_t zero = 0x00U;
  uint8_t rec[FLASH_LOG_RECORD_SIZE];
  flash_log_entry_t e;
  uint32_t addr;
  dev_status_t st;

  addr = flash_log_slot_addr(s_log.head + FLASH_LOG_CAPACITY - 1U);
  st = w25q64_read(s_log.dev, addr, rec, sizeof(rec));
  if (st == DEV_OK && flash_log_unpack(rec, &e) != FLASH_LOG_SLOT_VALID)
  {
    printf("FLOG,corrupt=SKIP,addr=0x%06lX,note=no_valid_record\r\n", (unsigned long)addr);
    return;
  }
  if (st == DEV_OK)
  {
    st = w25q64_write_page(s_log.dev, addr + 3U, &zero, 1U);
  }
  if (st != DEV_OK)
  {
    printf("FLOG,corrupt=FAIL,addr=0x%06lX,err=%s\r\n", (unsigned long)addr, StandTest_ErrStr(st));
    return;
  }
  printf("FLOG,corrupt=DONE,addr=0x%06lX,seq=%lu\r\n", (unsigned long)addr, (unsigned long)e.seq);
}

/* LOG ERASE: стерти журнал повністю (≈ 1 с) */
static void bench_format(void)
{
  uint32_t t0 = Platform_Cycles();
  dev_status_t st;

  s_log.dev = &app_devices.flash;
  st = flash_log_format(&s_log);
  s_erase_fails = 0U;
  s_ready = (st == DEV_OK);
  printf("FLOG,format=%s,us=%lu,err=%s\r\n", (st == DEV_OK) ? "PASS" : "FAIL",
         (unsigned long)elapsed_us(t0), StandTest_ErrStr(st));
}

bool Storage_BenchCommand(int c)
{
  if (c != 'd' && c != 'e' && c != 'z')
  {
    return false;
  }
  if (!s_ready)
  {
    printf("FLOG,cmd=%c,err=state\r\n", c);
    return true;
  }

  switch (c)
  {
    case 'd':
      Storage_Dump();
      break;
    case 'e':
      bench_events();
      break;
    default:
      bench_corrupt();
      break;
  }
  return true;
}

bool Storage_LineCommand(const char *line)
{
  if (line == NULL || strcmp(line, "LOG ERASE") != 0)
  {
    return false;
  }
  /* Можна і без змонтованого журналу (наприклад, після вимкнення через помилки) */
  if (app_devices.flash_status != DEV_OK)
  {
    printf("FLOG,cmd=LOG ERASE,err=state\r\n");
    return true;
  }
  bench_format();
  return true;
}

void Storage_BenchHelp(void)
{
  printf("#           d journal dump | e journal +100 test events | "
         "z corrupt newest record | LOG ERASE erase journal\r\n");
}
