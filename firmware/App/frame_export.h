/**
  ******************************************************************************
  * @file    frame_export.h
  * @brief   ЛР3 (I08): формування кадру і передача на ПК через UART текстом.
  *
  *          Чому текст, а не двійковий потік: той самий UART несе EVT-логи,
  *          STATS і відповіді на команди. Двійковий потік розірвала б будь-яка
  *          вставка між порціями, а рядок у Super Loop друкується цілком (одна
  *          задача в один момент), тож рядки кадру і логів не змішуються.
  *
  *            FRAME_BEGIN id=<event> seq=0 w=160 h=120 fmt=RGB565 len=38400 crc32=<hex8> chunk=32
  *            FRM off=<зсув> d=<2·chunk hex-символів>
  *            ...
  *            FRAME_END id=<event> len=38400 crc32=<hex8> lines=<кількість FRM>
  *
  *          off дає змогу знайти загублений рядок, crc32 (CRC-32/zlib, той самий
  *          Devices/crc32.c, що в CAM TEST і frame_store) — перевірити кадр цілком
  *          скриптом tools/frame_decode.py. Ціна: hex удвічі довший за двійкові
  *          дані — ≈ 8,5 с на кадр при 115200 замість ≈ 3,3 с (docs/camera.md §5).
  *          Один рядок FRM за прохід задачі uart (≈ 7 мс TX), тож цикл не стоїть.
  ******************************************************************************
  */
#ifndef FRAME_EXPORT_H
#define FRAME_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Почати експорт; друкує FRAME_BEGIN. Буфер не можна змінювати до FRAME_END
 * (CAM TEST / CAM PATTERN перевіряють FrameExport_Busy()).
 * false — немає даних або експорт уже йде. */
bool FrameExport_Start(const uint8_t *pixels, uint32_t len, uint32_t event_id);

/* Один рядок FRM (або FRAME_END) за виклик. true — щось надруковано. */
bool FrameExport_Step(void);
bool FrameExport_Busy(void);

/* Перервати експорт (FRAME_END не друкується — скрипт покаже "no FRAME_END") */
void FrameExport_Abort(void);

/* "FRM off=<off> d=<hex>" без "\r\n"; довжина рядка або -1, якщо не вміщається */
int  FrameExport_FormatChunk(char *buf, size_t size, uint32_t off, const uint8_t *p, size_t n);

#endif /* FRAME_EXPORT_H */
