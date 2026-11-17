/**
  ******************************************************************************
  * @file    flash_log.c
  * @brief   Журнал подій у W25Q64 (див. flash_log.h, docs/lab_3/flash-layout.md).
  ******************************************************************************
  */
#include "flash_log.h"

#include <string.h>

#include "crc32.h"

#define RECORD_MARKER   0xA5U
#define RECORD_VERSION  1U
#define RECORDS_PER_PAGE (W25Q64_PAGE_SIZE / FLASH_LOG_RECORD_SIZE)
#define PAGES_PER_SECTOR (W25Q64_SECTOR_SIZE / W25Q64_PAGE_SIZE)

/* Буфер сторінки для сканування під час монтування (лише з головного циклу) */
static uint8_t page_buf[W25Q64_PAGE_SIZE];

/* ---- little-endian поля ----------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p)
{
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool all_ff(const uint8_t *p, size_t len)
{
  for (size_t i = 0U; i < len; i++)
  {
    if (p[i] != 0xFFU)
    {
      return false;
    }
  }
  return true;
}

/* ---- запис журналу ------------------------------------------------------------ */

void flash_log_pack(const flash_log_entry_t *e, uint8_t out[FLASH_LOG_RECORD_SIZE])
{
  out[0] = RECORD_MARKER;
  out[1] = RECORD_VERSION;
  out[2] = e->type;
  out[3] = 0xFFU;                 /* flags: резерв */
  put32(&out[4], e->seq);
  put16(&out[8], e->boot);
  put16(&out[10], 0xFFFFU);       /* резерв */
  put32(&out[12], e->t_ms);
  put32(&out[16], e->arg);
  put32(&out[20], e->arg2);
  put32(&out[24], e->frame_id);
  put32(&out[28], crc32_compute(out, 28U));
}

flash_log_slot_t flash_log_unpack(const uint8_t in[FLASH_LOG_RECORD_SIZE], flash_log_entry_t *e)
{
  if (all_ff(in, FLASH_LOG_RECORD_SIZE))
  {
    return FLASH_LOG_SLOT_EMPTY;
  }
  if (in[0] != RECORD_MARKER || in[1] != RECORD_VERSION ||
      get32(&in[28]) != crc32_compute(in, 28U))
  {
    return FLASH_LOG_SLOT_DAMAGED;
  }
  e->type = in[2];
  e->seq = get32(&in[4]);
  e->boot = get16(&in[8]);
  e->t_ms = get32(&in[12]);
  e->arg = get32(&in[16]);
  e->arg2 = get32(&in[20]);
  e->frame_id = get32(&in[24]);
  return FLASH_LOG_SLOT_VALID;
}

uint32_t flash_log_slot_addr(uint32_t index)
{
  return FLASH_LOG_BASE + ((index % FLASH_LOG_CAPACITY) * FLASH_LOG_RECORD_SIZE);
}

const char *flash_log_type_name(uint8_t type)
{
  switch (type)
  {
    case FLASH_LOG_BOOT:           return "BOOT";
    case FLASH_LOG_LOCK_ACTIVATED: return "LOCK_ACTIVATED";
    case FLASH_LOG_LOCKED:         return "LOCKED";
    case FLASH_LOG_UNLOCKED:       return "UNLOCKED";
    case FLASH_LOG_ACCESS_GRANTED: return "ACCESS_GRANTED";
    case FLASH_LOG_WRONG_CODE:     return "WRONG_CODE";
    case FLASH_LOG_MOTION:         return "MOTION";
    case FLASH_LOG_TAMPER:         return "TAMPER";
    case FLASH_LOG_FORCED_OPEN:    return "FORCED_OPEN";
    case FLASH_LOG_DOOR_OPEN:      return "DOOR_OPEN";
    case FLASH_LOG_DOOR_CLOSED:    return "DOOR_CLOSED";
    case FLASH_LOG_ENV_WARNING:    return "ENV_WARNING";
    case FLASH_LOG_PHOTO_SAVED:    return "PHOTO_SAVED";
    case FLASH_LOG_PHOTO_FAILED:   return "PHOTO_FAILED";
    case FLASH_LOG_TEST:           return "TEST";
    default:                       return "UNKNOWN";
  }
}

/* ---- службовий сектор ---------------------------------------------------------- */

static void superblock_pack(uint8_t out[FLASH_SB_SIZE])
{
  put32(&out[0], FLASH_SB_MAGIC);
  put16(&out[4], FLASH_LAYOUT_VERSION);
  put16(&out[6], FLASH_LOG_RECORD_SIZE);
  put32(&out[8], FLASH_LOG_BASE);
  put16(&out[12], FLASH_LOG_SECTORS);
  put16(&out[14], FLASH_FRAME_SLOTS);
  put32(&out[16], FLASH_FRAME_BASE);
  put32(&out[20], FLASH_FRAME_SLOT_SIZE);
  put32(&out[24], 0xFFFFFFFFUL);
  put32(&out[28], crc32_compute(out, 28U));
}

/* ---- стан кільця ------------------------------------------------------------------- */

static void reset_state(flash_log_t *log)
{
  log->head = 0U;
  log->head_needs_erase = false;
  log->next_seq = 1U;
  log->boot = 1U;
  memset(log->valid, 0, sizeof(log->valid));
  memset(log->damaged, 0, sizeof(log->damaged));
}

static uint32_t sector_of(uint32_t index)
{
  return index / FLASH_LOG_PER_SECTOR;
}

static uint32_t sector_addr(uint32_t sector)
{
  return FLASH_LOG_BASE + (sector * W25Q64_SECTOR_SIZE);
}

dev_status_t flash_log_format(flash_log_t *log)
{
  uint8_t sb[FLASH_SB_SIZE];
  dev_status_t st;

  if (log == NULL || log->dev == NULL)
  {
    return DEV_ERR_PARAM;
  }
  log->mounted = false;

  for (uint32_t s = 0U; s < FLASH_LOG_SECTORS; s++)
  {
    st = w25q64_erase_sector(log->dev, sector_addr(s));
    if (st != DEV_OK)
    {
      return st;
    }
  }

  /* Службовий запис — останнім: обрив під час форматування призведе до
   * повторного форматування на наступному старті */
  st = w25q64_erase_sector(log->dev, FLASH_SB_ADDR);
  if (st == DEV_OK)
  {
    superblock_pack(sb);
    st = w25q64_write_page(log->dev, FLASH_SB_ADDR, sb, sizeof(sb));
  }
  if (st == DEV_OK)
  {
    uint8_t back[FLASH_SB_SIZE];

    st = w25q64_read(log->dev, FLASH_SB_ADDR, back, sizeof(back));
    if (st == DEV_OK && memcmp(sb, back, sizeof(sb)) != 0)
    {
      st = DEV_ERR_STATE;
    }
  }
  if (st != DEV_OK)
  {
    return st;
  }

  reset_state(log);
  log->mounted = true;
  return DEV_OK;
}

/* Переглянути весь журнал і відновити head, seq, boot і лічильники */
static dev_status_t scan(flash_log_t *log)
{
  int32_t  last_nonempty[FLASH_LOG_SECTORS];
  bool     blank[FLASH_LOG_SECTORS];
  bool     have_valid = false;
  uint32_t max_seq = 0U;
  uint32_t max_index = 0U;
  uint16_t max_boot = 0U;

  reset_state(log);

  for (uint32_t s = 0U; s < FLASH_LOG_SECTORS; s++)
  {
    last_nonempty[s] = -1;
    blank[s] = true;

    for (uint32_t p = 0U; p < PAGES_PER_SECTOR; p++)
    {
      uint32_t addr = sector_addr(s) + (p * W25Q64_PAGE_SIZE);
      dev_status_t st = w25q64_read(log->dev, addr, page_buf, sizeof(page_buf));

      if (st != DEV_OK)
      {
        return st;
      }

      for (uint32_t r = 0U; r < RECORDS_PER_PAGE; r++)
      {
        uint32_t index = (s * FLASH_LOG_PER_SECTOR) + (p * RECORDS_PER_PAGE) + r;
        flash_log_entry_t e;
        flash_log_slot_t kind = flash_log_unpack(&page_buf[r * FLASH_LOG_RECORD_SIZE], &e);

        if (kind == FLASH_LOG_SLOT_EMPTY)
        {
          continue;
        }
        blank[s] = false;
        last_nonempty[s] = (int32_t)(index % FLASH_LOG_PER_SECTOR);

        if (kind == FLASH_LOG_SLOT_DAMAGED)
        {
          log->damaged[s]++;
          continue;
        }
        log->valid[s]++;
        if (!have_valid || e.seq > max_seq)
        {
          have_valid = true;
          max_seq = e.seq;
          max_index = index;
        }
        if (e.boot > max_boot)
        {
          max_boot = e.boot;
        }
      }
    }
  }

  if (!have_valid)
  {
    /* Порожній журнал або лише пошкоджені записи: починаємо з першого сектора */
    log->head = 0U;
    log->head_needs_erase = !blank[0];
    return DEV_OK;
  }

  log->next_seq = max_seq + 1U;
  log->boot = (uint16_t)(max_boot + 1U);

  {
    uint32_t s = sector_of(max_index);
    uint32_t next = (s * FLASH_LOG_PER_SECTOR) + (uint32_t)(last_nonempty[s] + 1);

    if ((next % FLASH_LOG_PER_SECTOR) == 0U)
    {
      /* Сектор заповнений до кінця: далі — наступний по колу, після стирання */
      uint32_t ns = (s + 1U) % FLASH_LOG_SECTORS;

      log->head = ns * FLASH_LOG_PER_SECTOR;
      log->head_needs_erase = !blank[ns];
    }
    else
    {
      log->head = next;
      log->head_needs_erase = false;
    }
  }
  return DEV_OK;
}

dev_status_t flash_log_mount(flash_log_t *log, w25q64_t *dev)
{
  uint8_t expected[FLASH_SB_SIZE];
  uint8_t sb[FLASH_SB_SIZE];
  dev_status_t st;

  if (log == NULL || dev == NULL)
  {
    return DEV_ERR_PARAM;
  }
  memset(log, 0, sizeof(*log));
  log->dev = dev;

  st = w25q64_read(dev, FLASH_SB_ADDR, sb, sizeof(sb));
  if (st != DEV_OK)
  {
    return st;
  }

  superblock_pack(expected);
  if (memcmp(sb, expected, sizeof(sb)) != 0)
  {
    log->mount_result = all_ff(sb, sizeof(sb)) ? FLASH_LOG_FORMAT_BLANK : FLASH_LOG_FORMAT_LAYOUT;
    return flash_log_format(log);
  }

  st = scan(log);
  if (st != DEV_OK)
  {
    return st;
  }
  log->mount_result = FLASH_LOG_MOUNTED;
  log->mounted = true;
  return DEV_OK;
}

bool flash_log_needs_erase(const flash_log_t *log)
{
  return (log != NULL) && log->mounted && log->head_needs_erase;
}

dev_status_t flash_log_prepare(flash_log_t *log)
{
  uint32_t s;
  dev_status_t st;

  if (log == NULL || !log->mounted)
  {
    return DEV_ERR_STATE;
  }
  if (!log->head_needs_erase)
  {
    return DEV_OK;
  }

  s = sector_of(log->head);
  st = w25q64_erase_sector(log->dev, sector_addr(s));
  if (st != DEV_OK)
  {
    return st;
  }
  log->valid[s] = 0U;
  log->damaged[s] = 0U;
  log->head_needs_erase = false;
  return DEV_OK;
}

dev_status_t flash_log_append(flash_log_t *log, flash_log_entry_t *e)
{
  uint8_t rec[FLASH_LOG_RECORD_SIZE];
  uint8_t back[FLASH_LOG_RECORD_SIZE];
  uint32_t addr;
  uint32_t s;
  dev_status_t st;

  if (log == NULL || e == NULL)
  {
    return DEV_ERR_PARAM;
  }
  if (!log->mounted)
  {
    return DEV_ERR_STATE;
  }
  /* Номер, менший за вже використаний, зламав би порядок журналу */
  if (e->seq != 0U && e->seq < log->next_seq)
  {
    return DEV_ERR_PARAM;
  }

  st = flash_log_prepare(log);
  if (st != DEV_OK)
  {
    return st;
  }

  if (e->seq == 0U)
  {
    e->seq = log->next_seq;
  }
  e->boot = log->boot;
  flash_log_pack(e, rec);

  addr = flash_log_slot_addr(log->head);
  s = sector_of(log->head);

  /* Місце і номер використано за будь-якого результату: запис міг частково
   * потрапити у Flash, а повторний номер зламав би порядок */
  log->next_seq = e->seq + 1U;
  log->head++;
  if ((log->head % FLASH_LOG_PER_SECTOR) == 0U)
  {
    log->head %= FLASH_LOG_CAPACITY;
    log->head_needs_erase = true;
  }

  st = w25q64_write_page(log->dev, addr, rec, sizeof(rec));
  if (st == DEV_OK)
  {
    st = w25q64_read(log->dev, addr, back, sizeof(back));
  }
  if (st == DEV_OK && memcmp(rec, back, sizeof(rec)) != 0)
  {
    st = DEV_ERR_STATE;
  }

  if (st == DEV_OK)
  {
    log->valid[s]++;
  }
  else
  {
    log->damaged[s]++;
  }
  return st;
}

void flash_log_iter_begin(const flash_log_t *log, flash_log_iter_t *it)
{
  uint32_t start;

  if (log == NULL || it == NULL || !log->mounted)
  {
    if (it != NULL)
    {
      it->index = 0U;
      it->left = 0U;
    }
    return;
  }

  if (log->head_needs_erase)
  {
    /* head на початку ще не стертого сектора: у ньому найстаріші записи
     * (їх знищить наступний запис), далі — решта кільця по порядку */
    it->index = log->head;
    it->left = FLASH_LOG_CAPACITY;
    return;
  }

  /* Найстаріші записи — у секторі, наступному після сектора head; останнім
   * переглядається сектор head до самого head (далі в ньому порожньо) */
  start = ((sector_of(log->head) + 1U) % FLASH_LOG_SECTORS) * FLASH_LOG_PER_SECTOR;
  it->index = start;
  it->left = (log->head + FLASH_LOG_CAPACITY - start) % FLASH_LOG_CAPACITY;
}

dev_status_t flash_log_next(flash_log_t *log, flash_log_iter_t *it,
                            flash_log_entry_t *e, flash_log_slot_t *state, uint32_t *addr)
{
  uint8_t rec[FLASH_LOG_RECORD_SIZE];

  if (log == NULL || it == NULL || e == NULL || state == NULL)
  {
    return DEV_ERR_PARAM;
  }

  while (it->left > 0U)
  {
    uint32_t a = flash_log_slot_addr(it->index);
    dev_status_t st = w25q64_read(log->dev, a, rec, sizeof(rec));

    it->index = (it->index + 1U) % FLASH_LOG_CAPACITY;
    it->left--;

    if (st != DEV_OK)
    {
      return st;
    }

    *state = flash_log_unpack(rec, e);
    if (*state != FLASH_LOG_SLOT_EMPTY)
    {
      if (addr != NULL)
      {
        *addr = a;
      }
      return DEV_OK;
    }
  }

  *state = FLASH_LOG_SLOT_EMPTY;
  return DEV_OK;
}

uint32_t flash_log_valid_count(const flash_log_t *log)
{
  uint32_t n = 0U;

  for (uint32_t s = 0U; s < FLASH_LOG_SECTORS; s++)
  {
    n += log->valid[s];
  }
  return n;
}

uint32_t flash_log_damaged_count(const flash_log_t *log)
{
  uint32_t n = 0U;

  for (uint32_t s = 0U; s < FLASH_LOG_SECTORS; s++)
  {
    n += log->damaged[s];
  }
  return n;
}
