/**
  * @file    fake_w25q64.c
  * @brief   Модель W25Q64 за SPI для хостових тестів (див. fake_w25q64.h).
  */
#include "fake_w25q64.h"

#include <string.h>

#define SECTOR 4096U
#define PAGE   256U

static uint8_t  mem[FAKE_W25Q64_SIZE];
static uint8_t  page_buf[PAGE];
static uint16_t erases[FAKE_W25Q64_SIZE / SECTOR];

static int      cs;
static uint8_t  cmd;
static size_t   pos;        /* номер байта в поточному кадрі */
static uint32_t addr;
static size_t   prog_len;   /* скільки байтів даних прийшло для 02h */
static int      wel;

static int      powered;
static int      cut_armed;
static uint32_t cut_budget;

static unsigned     fail_skip;
static unsigned     fail_left;
static dev_status_t fail_status;

static unsigned erase_count;
static unsigned program_count;
static unsigned protocol_errors;

void fake_w25q64_reset(void)
{
  memset(mem, 0xFF, sizeof(mem));
  memset(erases, 0, sizeof(erases));
  cs = 0;
  pos = 0U;
  wel = 0;
  powered = 1;
  cut_armed = 0;
  fail_skip = 0U;
  fail_left = 0U;
  erase_count = 0U;
  program_count = 0U;
  protocol_errors = 0U;
}

uint8_t *fake_w25q64_mem(void)
{
  return mem;
}

void fake_w25q64_cut_after(uint32_t bytes)
{
  cut_armed = 1;
  cut_budget = bytes;
}

void fake_w25q64_power_on(void)
{
  powered = 1;
  cut_armed = 0;
  wel = 0;
}

int fake_w25q64_powered(void)
{
  return powered;
}

void fake_w25q64_fail_transfers(unsigned count, dev_status_t status)
{
  fake_w25q64_fail_from(0U, count, status);
}

void fake_w25q64_fail_from(unsigned skip, unsigned count, dev_status_t status)
{
  fail_skip = skip;
  fail_left = count;
  fail_status = status;
}

unsigned fake_w25q64_erase_count(void)
{
  return erase_count;
}

unsigned fake_w25q64_program_count(void)
{
  return program_count;
}

unsigned fake_w25q64_sector_erases(uint32_t sector)
{
  return (sector < (FAKE_W25Q64_SIZE / SECTOR)) ? erases[sector] : 0U;
}

unsigned fake_w25q64_protocol_errors(void)
{
  return protocol_errors;
}

/* Скільки з n байтів ще встигне змінитися до обриву живлення */
static uint32_t allowed(uint32_t n)
{
  if (!powered)
  {
    return 0U;
  }
  if (!cut_armed)
  {
    return n;
  }
  if (cut_budget >= n)
  {
    cut_budget -= n;
    return n;
  }
  n = cut_budget;
  cut_budget = 0U;
  powered = 0;
  return n;
}

static void apply_program(void)
{
  uint32_t base = addr & ~(uint32_t)(PAGE - 1U);
  uint32_t n = allowed((uint32_t)((prog_len > PAGE) ? PAGE : prog_len));

  /* Запис іде по колу в межах сторінки, як у мікросхемі */
  for (uint32_t i = 0U; i < n; i++)
  {
    mem[base + (((addr % PAGE) + i) % PAGE)] &= page_buf[i];
  }
  program_count++;
}

static void apply_erase(void)
{
  uint32_t base = addr & ~(uint32_t)(SECTOR - 1U);
  uint32_t n = allowed(SECTOR);

  memset(&mem[base], 0xFF, n);
  erases[base / SECTOR]++;
  erase_count++;
}

static void fake_select(void *ctx)
{
  (void)ctx;
  if (cs)
  {
    protocol_errors++;
  }
  cs = 1;
  pos = 0U;
  cmd = 0U;
  addr = 0U;
  prog_len = 0U;
}

static void fake_deselect(void *ctx)
{
  (void)ctx;
  if (!cs)
  {
    protocol_errors++;
  }
  cs = 0;

  if (pos == 0U)
  {
    return;
  }

  switch (cmd)
  {
    case 0x06U:
      wel = powered;
      break;

    case 0x02U:
      if (wel && pos >= 4U)
      {
        apply_program();
      }
      wel = 0;
      break;

    case 0x20U:
      if (wel && pos >= 4U)
      {
        apply_erase();
      }
      wel = 0;
      break;

    default:
      break;
  }
}

/* Один байт кадру: tx — з MOSI, повертає байт для MISO */
static uint8_t clock_byte(uint8_t tx)
{
  static const uint8_t jedec[3] = { 0xEFU, 0x40U, 0x17U };
  uint8_t rx = 0xFFU;

  if (pos == 0U)
  {
    cmd = tx;
  }
  else if (pos <= 3U && (cmd == 0x02U || cmd == 0x03U || cmd == 0x20U))
  {
    addr = (addr << 8) | tx;
  }
  else
  {
    switch (cmd)
    {
      case 0x9FU:
        rx = (pos <= 3U) ? jedec[pos - 1U] : 0xFFU;
        break;

      case 0x05U:
        rx = wel ? 0x02U : 0x00U;
        break;

      case 0x03U:
        rx = mem[(addr + (uint32_t)(pos - 4U)) % FAKE_W25Q64_SIZE];
        break;

      case 0x02U:
        page_buf[prog_len % PAGE] = tx;
        prog_len++;
        break;

      default:
        break;
    }
  }
  pos++;
  return rx;
}

static dev_status_t fake_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
  (void)ctx;
  if (!cs)
  {
    protocol_errors++;
  }
  if (fail_skip > 0U)
  {
    fail_skip--;
  }
  else if (fail_left > 0U)
  {
    fail_left--;
    return fail_status;
  }
  for (size_t i = 0U; i < len; i++)
  {
    uint8_t r = clock_byte((tx != NULL) ? tx[i] : 0xFFU);

    if (rx != NULL)
    {
      rx[i] = r;
    }
  }
  return DEV_OK;
}

static const dev_spi_bus_t bus = { NULL, fake_select, fake_deselect, fake_transfer };

const dev_spi_bus_t *fake_w25q64_bus(void)
{
  return &bus;
}
