/**
  ******************************************************************************
  * @file    frames.c
  * @brief   Кадри у W25Q64 для прикладної логіки і стенда (див. frames.h).
  *
  *          Рядки UART:
  *            FRM,MOUNT,frames=...,next_id=...,us=...
  *            FRM,write=PASS,id=...,event_seq=...,bytes=...,erase_us=...,write_us=...,total_us=...,crc=...
  *            FRM,verify=PASS,id=...,event_seq=...,bytes=...,mismatch=...,crc=ok,us=...,uptime_ms=...
  ******************************************************************************
  */
#include "frames.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "platform.h"
#include "stand_test.h"
#include "storage.h"

/* Тестовий кадр стенда: QQVGA RGB565 (docs/metrics/theoretical-estimates.md).
 * Формат робочого кадру — TBD, для перевірки Flash важливий лише розмір. */
#define TEST_W      160U
#define TEST_H      120U
#define TEST_BYTES  (TEST_W * TEST_H * 2U)

static frame_store_t s_fs;
static bool          s_ready;

static uint32_t elapsed_us(uint32_t start)
{
  return Platform_CyclesToUs(Platform_Cycles() - start);
}

uint8_t Frames_TestByte(uint32_t frame_id, uint32_t offset)
{
  return (uint8_t)((offset * 7U) ^ (offset >> 8) ^ (frame_id * 37U));
}

void Frames_Init(void)
{
  uint32_t t0;
  dev_status_t st;

  s_ready = false;
  if (app_devices.flash_status != DEV_OK)
  {
    printf("FRM,MOUNT,err=%s,note=flash_absent\r\n", StandTest_ErrStr(app_devices.flash_status));
    return;
  }

  t0 = Platform_Cycles();
  st = frame_store_mount(&s_fs, &app_devices.flash);
  if (st != DEV_OK)
  {
    printf("FRM,MOUNT,err=%s\r\n", StandTest_ErrStr(st));
    return;
  }
  s_ready = true;
  printf("FRM,MOUNT,frames=%lu,next_id=%lu,us=%lu\r\n", (unsigned long)s_fs.valid_frames,
         (unsigned long)s_fs.next_id, (unsigned long)elapsed_us(t0));
}

bool Frames_Ready(void)
{
  return s_ready;
}

frame_store_t *Frames_Store(void)
{
  return &s_fs;
}

/* ---- FrameStore_GetOps() ----------------------------------------------------------- */

static dev_status_t ops_write(const frame_header_t *hdr, const uint8_t *data, uint32_t *frame_id)
{
  return s_ready ? frame_store_write(&s_fs, hdr, data, frame_id) : DEV_ERR_STATE;
}

static dev_status_t ops_read(uint32_t frame_id, frame_header_t *hdr, uint8_t *buf, size_t size)
{
  return s_ready ? frame_store_read(&s_fs, frame_id, hdr, buf, size) : DEV_ERR_STATE;
}

static const frame_store_ops_t s_ops = { ops_write, ops_read };

const frame_store_ops_t *FrameStore_GetOps(void)
{
  return &s_ops;
}

/* ---- стендові команди ---------------------------------------------------------------- */

/* p: подія TEST у журналі → тестовий кадр 38 400 Б у Flash → PHOTO_SAVED.
 * Кадр пишеться сторінками, без буфера кадру в RAM. */
static void bench_write(void)
{
  uint8_t chunk[W25Q64_PAGE_SIZE];
  frame_header_t h;
  uint32_t seq = 0U;
  uint32_t id = s_fs.next_id;
  uint32_t t0;
  uint32_t t1;
  uint32_t erase_us;
  dev_status_t st;

  /* Подія, до якої належить кадр, — у журнал першою (якщо журнал є) */
  (void)Storage_WriteRecord(FLASH_LOG_TEST, id, 0U, FLASH_LOG_NO_FRAME, &seq);

  memset(&h, 0, sizeof(h));
  h.format = FRAME_FORMAT_RGB565;
  h.width = TEST_W;
  h.height = TEST_H;
  h.event_seq = seq;
  h.data_len = TEST_BYTES;

  t0 = Platform_Cycles();
  st = frame_store_begin(&s_fs, &h);
  erase_us = elapsed_us(t0);
  t1 = Platform_Cycles();

  for (uint32_t off = 0U; st == DEV_OK && off < TEST_BYTES; off += sizeof(chunk))
  {
    for (uint32_t i = 0U; i < sizeof(chunk); i++)
    {
      chunk[i] = Frames_TestByte(id, off + i);
    }
    st = frame_store_append(&s_fs, chunk, sizeof(chunk));
  }
  if (st == DEV_OK)
  {
    st = frame_store_commit(&s_fs, &id);
  }

  (void)Storage_WriteRecord((st == DEV_OK) ? FLASH_LOG_PHOTO_SAVED : FLASH_LOG_PHOTO_FAILED, seq,
                            (uint32_t)st, (st == DEV_OK) ? id : FLASH_LOG_NO_FRAME, NULL);

  if (st != DEV_OK)
  {
    printf("FRM,write=FAIL,id=%lu,event_seq=%lu,err=%s\r\n", (unsigned long)id,
           (unsigned long)seq, StandTest_ErrStr(st));
    return;
  }
  printf("FRM,write=PASS,id=%lu,event_seq=%lu,bytes=%u,erase_us=%lu,write_us=%lu,"
         "total_us=%lu,crc=%08lX\r\n",
         (unsigned long)id, (unsigned long)seq, (unsigned)TEST_BYTES, (unsigned long)erase_us,
         (unsigned long)elapsed_us(t1), (unsigned long)(erase_us + elapsed_us(t1)),
         (unsigned long)s_fs.wr.data_crc);
}

/* o: прочитати останній кадр, звірити побайтно з тестовим шаблоном і CRC */
static void bench_verify(void)
{
  uint8_t chunk[W25Q64_PAGE_SIZE];
  frame_header_t h;
  uint32_t id = s_fs.next_id - 1U;
  uint32_t mismatch = 0U;
  uint32_t t0;
  dev_status_t st;
  dev_status_t crc_st;

  if (id == 0U)
  {
    printf("FRM,verify=SKIP,note=no_frames\r\n");
    return;
  }

  t0 = Platform_Cycles();
  st = frame_store_read_header(&s_fs, id, &h);
  for (uint32_t off = 0U; st == DEV_OK && off < h.data_len; off += sizeof(chunk))
  {
    uint32_t n = h.data_len - off;

    n = (n > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : n;
    st = frame_store_read_data(&s_fs, &h, off, chunk, n);
    for (uint32_t i = 0U; st == DEV_OK && i < n; i++)
    {
      mismatch += (chunk[i] != Frames_TestByte(id, off + i)) ? 1U : 0U;
    }
  }
  if (st != DEV_OK)
  {
    printf("FRM,verify=FAIL,id=%lu,err=%s\r\n", (unsigned long)id, StandTest_ErrStr(st));
    return;
  }

  crc_st = frame_store_verify(&s_fs, id, &h);
  printf("FRM,verify=%s,id=%lu,event_seq=%lu,bytes=%lu,mismatch=%lu,crc=%s,us=%lu,uptime_ms=%lu\r\n",
         (mismatch == 0U && crc_st == DEV_OK) ? "PASS" : "FAIL", (unsigned long)id,
         (unsigned long)h.event_seq, (unsigned long)h.data_len, (unsigned long)mismatch,
         (crc_st == DEV_OK) ? "ok" : "bad", (unsigned long)elapsed_us(t0),
         (unsigned long)Platform_Millis());
}

bool Frames_BenchCommand(int c)
{
  if (c != 'p' && c != 'o')
  {
    return false;
  }
  if (!s_ready)
  {
    printf("FRM,cmd=%c,err=state\r\n", c);
    return true;
  }
  if (c == 'p')
  {
    bench_write();
  }
  else
  {
    bench_verify();
  }
  return true;
}

void Frames_BenchHelp(void)
{
  printf("#           p frame write 38400 B (+journal) | o frame verify last\r\n");
}
