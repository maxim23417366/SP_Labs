/**
  ******************************************************************************
  * @file    frame_store.h
  * @brief   Збереження кадрів камери у W25Q64 (FR-07, FR-08).
  *
  *          Формат і правила — docs/lab_3/flash-layout.md, розділ 4:
  *            - 126 слотів по 64 КБ від 0x010000; кадр frame_id лежить у слоті
  *              (frame_id − 1) mod 126, тож після 126 кадрів найстаріший
  *              перезаписується;
  *            - у слоті: заголовок 32 Б (сторінка 0) і дані з 0x100;
  *            - заголовок записується ОСТАННІМ: недописаний кадр (обрив живлення)
  *              не має дійсного заголовка і вважається відсутнім;
  *            - заголовок зберігає seq події журналу (MOTION / TAMPER), до якої
  *              належить кадр, і CRC-32 даних.
  *
  *          Дані можна записувати частинами (begin → append … → commit), щоб не
  *          тримати в RAM другий буфер кадру: append() пише у Flash кожну
  *          заповнену сторінку 256 Б. Для буфера, що вже весь у RAM, —
  *          frame_store_write().
  *
  *          Коди результату:
  *            DEV_ERR_PARAM — розмір 0 або більший за FLASH_FRAME_MAX_DATA,
  *                            frame_id = 0, замалий буфер, зайві байти;
  *            DEV_ERR_ID    — кадру з таким frame_id немає (не записаний,
  *                            перезаписаний новішим або заголовок пошкоджений);
  *            DEV_ERR_STATE — CRC даних не збіглася, або порушено порядок
  *                            викликів (append без begin, commit до кінця даних).
  *
  *          Операції блокувальні. Кадр 38 400 Б: стирання 10 секторів
  *          (≈ 0,64 с за виміряними 64 мс) + 150 сторінок (≤ 0,3 с).
  ******************************************************************************
  */
#ifndef FRAME_STORE_H
#define FRAME_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_layout.h"
#include "w25q64.h"

#define FRAME_FORMAT_RGB565  1U
#define FRAME_FORMAT_Y8      2U      /* лише яскравість, 1 Б на піксель */
#define FRAME_FORMAT_UNSET   0xFFU   /* формат кадру ще не затверджено */

#define FRAME_HEADER_SIZE    32U

typedef struct
{
  uint8_t  format;      /* FRAME_FORMAT_* */
  uint16_t width;
  uint16_t height;
  uint16_t slot;        /* заповнює модуль */
  uint32_t frame_id;    /* заповнює модуль */
  uint32_t event_seq;   /* seq події журналу, до якої належить кадр */
  uint32_t data_len;
  uint32_t data_crc;    /* заповнює модуль */
} frame_header_t;

typedef struct
{
  w25q64_t *dev;
  bool      mounted;
  uint32_t  next_id;          /* frame_id наступного кадру */
  uint32_t  valid_frames;     /* дійсних заголовків під час монтування */

  /* Поточний запис частинами */
  bool           writing;
  frame_header_t wr;
  uint32_t       wr_done;     /* байтів даних прийнято */
  uint32_t       wr_crc;      /* проміжне значення CRC-32 */
  uint16_t       page_fill;   /* байтів у page[] */
  uint8_t        page[W25Q64_PAGE_SIZE];
} frame_store_t;

/* Прочитати заголовки всіх слотів і визначити наступний frame_id (≈ 12 мс). */
dev_status_t frame_store_mount(frame_store_t *fs, w25q64_t *dev);

/* Почати кадр: перевірити розмір, стерти сектори слота, які займе кадр.
 * У hdr задаються format, width, height, event_seq, data_len. */
dev_status_t frame_store_begin(frame_store_t *fs, const frame_header_t *hdr);

/* Дописати len байтів даних (сумарно не більше data_len) */
dev_status_t frame_store_append(frame_store_t *fs, const uint8_t *data, size_t len);

/* Завершити: дописати останню сторінку, записати заголовок і перевірити його.
 * frame_id (може бути NULL) — номер збереженого кадру. */
dev_status_t frame_store_commit(frame_store_t *fs, uint32_t *frame_id);

/* Скасувати незавершений кадр (слот лишається без заголовка) */
void frame_store_abort(frame_store_t *fs);

/* Увесь кадр одним викликом: begin + append + commit */
dev_status_t frame_store_write(frame_store_t *fs, const frame_header_t *hdr,
                               const uint8_t *data, uint32_t *frame_id);

/* Прочитати і перевірити заголовок кадру frame_id */
dev_status_t frame_store_read_header(frame_store_t *fs, uint32_t frame_id, frame_header_t *hdr);

/* Прочитати частину даних кадру: offset + len у межах data_len */
dev_status_t frame_store_read_data(frame_store_t *fs, const frame_header_t *hdr,
                                   uint32_t offset, uint8_t *buf, size_t len);

/* Прочитати кадр цілком у buf (size ≥ data_len) і перевірити CRC даних */
dev_status_t frame_store_read(frame_store_t *fs, uint32_t frame_id, frame_header_t *hdr,
                              uint8_t *buf, size_t size);

/* Перевірити CRC даних без буфера кадру (читає сторінками) */
dev_status_t frame_store_verify(frame_store_t *fs, uint32_t frame_id, frame_header_t *hdr);

/* Адреса слота кадру frame_id (frame_id ≥ 1) */
uint32_t frame_store_slot_addr(uint32_t frame_id);

/* Формування і розбір заголовка (для тестів) */
void frame_store_pack_header(const frame_header_t *hdr, uint8_t out[FRAME_HEADER_SIZE]);
bool frame_store_unpack_header(const uint8_t in[FRAME_HEADER_SIZE], frame_header_t *hdr);

#endif /* FRAME_STORE_H */
