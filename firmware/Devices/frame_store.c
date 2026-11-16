/**
  ******************************************************************************
  * @file    frame_store.c
  * @brief   Кадри камери у W25Q64 (див. frame_store.h, docs/lab_3/flash-layout.md).
  ******************************************************************************
  */
#include "frame_store.h"

#include <string.h>

#include "crc32.h"

#define FRAME_MAGIC    0x52464C53UL   /* "SLFR" */
#define FRAME_VERSION  1U

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

static uint16_t slot_of(uint32_t frame_id)
{
  return (uint16_t)((frame_id - 1U) % FLASH_FRAME_SLOTS);
}

uint32_t frame_store_slot_addr(uint32_t frame_id)
{
  return FLASH_FRAME_BASE + ((uint32_t)slot_of(frame_id) * FLASH_FRAME_SLOT_SIZE);
}

void frame_store_pack_header(const frame_header_t *hdr, uint8_t out[FRAME_HEADER_SIZE])
{
  put32(&out[0], FRAME_MAGIC);
  out[4] = FRAME_VERSION;
  out[5] = hdr->format;
  put16(&out[6], hdr->width);
  put16(&out[8], hdr->height);
  put16(&out[10], hdr->slot);
  put32(&out[12], hdr->frame_id);
  put32(&out[16], hdr->event_seq);
  put32(&out[20], hdr->data_len);
  put32(&out[24], hdr->data_crc);
  put32(&out[28], crc32_compute(out, 28U));
}

bool frame_store_unpack_header(const uint8_t in[FRAME_HEADER_SIZE], frame_header_t *hdr)
{
  frame_header_t h;

  if (get32(&in[0]) != FRAME_MAGIC || in[4] != FRAME_VERSION ||
      get32(&in[28]) != crc32_compute(in, 28U))
  {
    return false;
  }

  h.format = in[5];
  h.width = get16(&in[6]);
  h.height = get16(&in[8]);
  h.slot = get16(&in[10]);
  h.frame_id = get32(&in[12]);
  h.event_seq = get32(&in[16]);
  h.data_len = get32(&in[20]);
  h.data_crc = get32(&in[24]);

  /* Заголовок має належати саме цьому слоту і описувати можливий кадр */
  if (h.frame_id == 0U || h.slot >= FLASH_FRAME_SLOTS || slot_of(h.frame_id) != h.slot ||
      h.data_len == 0U || h.data_len > FLASH_FRAME_MAX_DATA)
  {
    return false;
  }
  *hdr = h;
  return true;
}

dev_status_t frame_store_mount(frame_store_t *fs, w25q64_t *dev)
{
  uint32_t max_id = 0U;

  if (fs == NULL || dev == NULL)
  {
    return DEV_ERR_PARAM;
  }
  memset(fs, 0, sizeof(*fs));
  fs->dev = dev;

  for (uint32_t k = 0U; k < FLASH_FRAME_SLOTS; k++)
  {
    uint8_t raw[FRAME_HEADER_SIZE];
    frame_header_t h;
    dev_status_t st = w25q64_read(dev, FLASH_FRAME_BASE + (k * FLASH_FRAME_SLOT_SIZE), raw, sizeof(raw));

    if (st != DEV_OK)
    {
      return st;
    }
    if (frame_store_unpack_header(raw, &h) && h.slot == k)
    {
      fs->valid_frames++;
      if (h.frame_id > max_id)
      {
        max_id = h.frame_id;
      }
    }
  }

  fs->next_id = max_id + 1U;
  fs->mounted = true;
  return DEV_OK;
}

void frame_store_abort(frame_store_t *fs)
{
  if (fs != NULL)
  {
    fs->writing = false;
    fs->page_fill = 0U;
  }
}

dev_status_t frame_store_begin(frame_store_t *fs, const frame_header_t *hdr)
{
  uint32_t base;
  uint32_t sectors;

  if (fs == NULL || hdr == NULL)
  {
    return DEV_ERR_PARAM;
  }
  if (!fs->mounted)
  {
    return DEV_ERR_STATE;
  }
  if (hdr->data_len == 0U || hdr->data_len > FLASH_FRAME_MAX_DATA)
  {
    return DEV_ERR_PARAM;
  }

  frame_store_abort(fs);
  fs->wr = *hdr;
  fs->wr.frame_id = fs->next_id;
  fs->wr.slot = slot_of(fs->next_id);
  fs->wr.data_crc = 0U;
  base = frame_store_slot_addr(fs->next_id);

  /* Сектор 0 (із заголовком) стирається першим: старий кадр у слоті зникає
   * до того, як почнуть змінюватися його дані */
  sectors = (FLASH_FRAME_DATA_OFFSET + hdr->data_len + W25Q64_SECTOR_SIZE - 1U) / W25Q64_SECTOR_SIZE;
  for (uint32_t s = 0U; s < sectors; s++)
  {
    dev_status_t st = w25q64_erase_sector(fs->dev, base + (s * W25Q64_SECTOR_SIZE));

    if (st != DEV_OK)
    {
      return st;
    }
  }

  fs->wr_done = 0U;
  fs->wr_crc = CRC32_INIT;
  fs->page_fill = 0U;
  fs->writing = true;
  return DEV_OK;
}

/* Записати накопичену сторінку даних (повну або останню неповну) */
static dev_status_t flush_page(frame_store_t *fs)
{
  uint32_t programmed = fs->wr_done - fs->page_fill;
  uint32_t addr = frame_store_slot_addr(fs->wr.frame_id) + FLASH_FRAME_DATA_OFFSET + programmed;
  dev_status_t st = w25q64_write_page(fs->dev, addr, fs->page, fs->page_fill);

  fs->page_fill = 0U;
  return st;
}

dev_status_t frame_store_append(frame_store_t *fs, const uint8_t *data, size_t len)
{
  if (fs == NULL || (data == NULL && len > 0U))
  {
    return DEV_ERR_PARAM;
  }
  if (!fs->writing)
  {
    return DEV_ERR_STATE;
  }
  if (len > (size_t)(fs->wr.data_len - fs->wr_done))
  {
    return DEV_ERR_PARAM;
  }

  fs->wr_crc = crc32_update(fs->wr_crc, data, len);
  while (len > 0U)
  {
    size_t n = W25Q64_PAGE_SIZE - fs->page_fill;

    if (n > len)
    {
      n = len;
    }
    memcpy(&fs->page[fs->page_fill], data, n);
    fs->page_fill = (uint16_t)(fs->page_fill + n);
    fs->wr_done += (uint32_t)n;
    data += n;
    len -= n;

    if (fs->page_fill == W25Q64_PAGE_SIZE)
    {
      dev_status_t st = flush_page(fs);

      if (st != DEV_OK)
      {
        frame_store_abort(fs);
        return st;
      }
    }
  }
  return DEV_OK;
}

dev_status_t frame_store_commit(frame_store_t *fs, uint32_t *frame_id)
{
  uint8_t raw[FRAME_HEADER_SIZE];
  uint8_t back[FRAME_HEADER_SIZE];
  uint32_t base;
  dev_status_t st = DEV_OK;

  if (fs == NULL)
  {
    return DEV_ERR_PARAM;
  }
  if (!fs->writing || fs->wr_done != fs->wr.data_len)
  {
    return DEV_ERR_STATE;
  }

  if (fs->page_fill > 0U)
  {
    st = flush_page(fs);
  }

  base = frame_store_slot_addr(fs->wr.frame_id);
  fs->wr.data_crc = crc32_final(fs->wr_crc);
  frame_store_pack_header(&fs->wr, raw);

  /* Заголовок — останнім: тепер кадр вважається збереженим */
  if (st == DEV_OK)
  {
    st = w25q64_write_page(fs->dev, base, raw, sizeof(raw));
  }
  if (st == DEV_OK)
  {
    st = w25q64_read(fs->dev, base, back, sizeof(back));
  }
  if (st == DEV_OK && memcmp(raw, back, sizeof(raw)) != 0)
  {
    st = DEV_ERR_STATE;
  }

  frame_store_abort(fs);
  if (st != DEV_OK)
  {
    return st;
  }

  if (frame_id != NULL)
  {
    *frame_id = fs->wr.frame_id;
  }
  fs->next_id++;
  return DEV_OK;
}

dev_status_t frame_store_write(frame_store_t *fs, const frame_header_t *hdr,
                               const uint8_t *data, uint32_t *frame_id)
{
  dev_status_t st;

  if (data == NULL)
  {
    return DEV_ERR_PARAM;
  }
  st = frame_store_begin(fs, hdr);
  if (st == DEV_OK)
  {
    st = frame_store_append(fs, data, hdr->data_len);
  }
  if (st == DEV_OK)
  {
    st = frame_store_commit(fs, frame_id);
  }
  return st;
}

dev_status_t frame_store_read_header(frame_store_t *fs, uint32_t frame_id, frame_header_t *hdr)
{
  uint8_t raw[FRAME_HEADER_SIZE];
  frame_header_t h;
  dev_status_t st;

  if (fs == NULL || hdr == NULL || frame_id == 0U)
  {
    return DEV_ERR_PARAM;
  }
  if (!fs->mounted)
  {
    return DEV_ERR_STATE;
  }

  st = w25q64_read(fs->dev, frame_store_slot_addr(frame_id), raw, sizeof(raw));
  if (st != DEV_OK)
  {
    return st;
  }
  if (!frame_store_unpack_header(raw, &h) || h.frame_id != frame_id)
  {
    return DEV_ERR_ID;
  }
  *hdr = h;
  return DEV_OK;
}

dev_status_t frame_store_read_data(frame_store_t *fs, const frame_header_t *hdr,
                                   uint32_t offset, uint8_t *buf, size_t len)
{
  if (fs == NULL || hdr == NULL || buf == NULL || len == 0U ||
      offset > hdr->data_len || len > (size_t)(hdr->data_len - offset))
  {
    return DEV_ERR_PARAM;
  }
  return w25q64_read(fs->dev, frame_store_slot_addr(hdr->frame_id) + FLASH_FRAME_DATA_OFFSET + offset,
                     buf, len);
}

dev_status_t frame_store_read(frame_store_t *fs, uint32_t frame_id, frame_header_t *hdr,
                              uint8_t *buf, size_t size)
{
  dev_status_t st;

  if (buf == NULL)
  {
    return DEV_ERR_PARAM;
  }
  st = frame_store_read_header(fs, frame_id, hdr);
  if (st != DEV_OK)
  {
    return st;
  }
  if (size < hdr->data_len)
  {
    return DEV_ERR_PARAM;
  }
  st = frame_store_read_data(fs, hdr, 0U, buf, hdr->data_len);
  if (st == DEV_OK && crc32_compute(buf, hdr->data_len) != hdr->data_crc)
  {
    st = DEV_ERR_STATE;
  }
  return st;
}

dev_status_t frame_store_verify(frame_store_t *fs, uint32_t frame_id, frame_header_t *hdr)
{
  uint8_t chunk[W25Q64_PAGE_SIZE];
  uint32_t crc = CRC32_INIT;
  dev_status_t st = frame_store_read_header(fs, frame_id, hdr);

  for (uint32_t off = 0U; st == DEV_OK && off < hdr->data_len; off += sizeof(chunk))
  {
    uint32_t n = hdr->data_len - off;

    if (n > sizeof(chunk))
    {
      n = sizeof(chunk);
    }
    st = frame_store_read_data(fs, hdr, off, chunk, n);
    crc = crc32_update(crc, chunk, n);
  }
  if (st == DEV_OK && crc32_final(crc) != hdr->data_crc)
  {
    st = DEV_ERR_STATE;
  }
  return st;
}
