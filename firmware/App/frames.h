/**
  ******************************************************************************
  * @file    frames.h
  * @brief   Збереження кадрів камери у W25Q64 для прикладної логіки (FR-07,
  *          FR-08): FrameStore_GetOps() із docs/tasks.md і стендові команди.
  *
  *          Задача камери після захоплення кадру для запиту event_seq
  *          (SmartLock_CameraRequest, sl_camera_link.h) викликає
  *
  *            SmartLock_CameraStoreFrame(event_seq, FRAME_FORMAT_RGB565, 160, 120,
  *                                       buf, 38400);   // sl_camera_frames.h (K08)
  *
  *          Вона пише кадр через FrameStore_GetOps()->write_frame() із заголовком
  *          event_seq і закриває запит: у журналі рівно один SL_LOG_PHOTO (arg = id
  *          кадру, ref = event_seq) або SL_LOG_PHOTO_ERR. Окремо логувати PHOTO
  *          камера не повинна — інакше два записи на одну подію.
  *
  *          event_seq — ID події MOTION / TAMPER, який повернув SmartLock_LogEvent()
  *          (sl_journal.h); у Flash це той самий seq. Запис SL_LOG_PHOTO у Flash стає
  *          PHOTO_SAVED з arg = event_seq і frame_id = id (storage.c).
  *          Через вказівники на функції модуль камери можна тестувати на ПК із
  *          власною підміною (без Flash).
  *
  *          Запис кадру блокувальний: QQVGA RGB565 ≈ 0,74–0,94 с за виміряними
  *          часами стирання і запису (docs/lab_3/flash-layout.md, розділ 4.2).
  ******************************************************************************
  */
#ifndef FRAMES_H
#define FRAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame_store.h"

typedef struct
{
  /* Зберегти кадр data (hdr->data_len байтів). frame_id — номер кадру. */
  dev_status_t (*write_frame)(const frame_header_t *hdr, const uint8_t *data, uint32_t *frame_id);

  /* Прочитати кадр frame_id у buf (size ≥ data_len) з перевіркою CRC.
   * DEV_ERR_ID — кадру немає або його перезаписано, DEV_ERR_STATE — CRC. */
  dev_status_t (*read_frame)(uint32_t frame_id, frame_header_t *hdr, uint8_t *buf, size_t size);
} frame_store_ops_t;

/* Прочитати заголовки слотів (≈ 12 мс) і надрукувати FRM,MOUNT,... */
void Frames_Init(void);

bool Frames_Ready(void);

frame_store_t *Frames_Store(void);

/* Операції збереження кадру; до Frames_Init() або без флешки повертають DEV_ERR_STATE */
const frame_store_ops_t *FrameStore_GetOps(void);

/* Стендові команди кадрів (p, o). true — команду оброблено. */
bool Frames_BenchCommand(int c);
void Frames_BenchHelp(void);

/* Байт тестового кадру стенда (детермінований: його можна звірити після перезапуску) */
uint8_t Frames_TestByte(uint32_t frame_id, uint32_t offset);

#endif /* FRAMES_H */
