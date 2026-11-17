/**
  ******************************************************************************
  * @file    sl_camera_frames.h
  * @brief   ЛР3 (K08, Коваль): кадр охоронної події → W25Q64 (frames.c, H10)
  *          → запис PHOTO / PHOTO_ERR у журналі (sl_camera_link.c).
  *
  *          Точка підключення модуля камери (I09). Камера отримала від
  *          SmartLock_CameraRequest(event_id) запит і захопила кадр у буфер RAM:
  *
  *            SmartLock_CameraStoreFrame(event_id, FRAME_FORMAT_RGB565, 160, 120,
  *                                       buf, 38400);
  *
  *          Функція сама:
  *            1) перевіряє, що на event_id ще чекають (не тайм-аут, не дубль) —
  *               інакше кадр не пишеться (у Flash не з'явиться кадр без посилання);
  *            2) пише кадр через FrameStore_GetOps()->write_frame() із
  *               заголовком event_seq = event_id (flash-layout.md, розділ 4.1);
  *            3) закриває запит SmartLock_OnCameraResult(): SL_CAM_OK і frame_id
  *               (у журналі PHOTO, у Flash PHOTO_SAVED: arg = event_id, frame_id)
  *               або SL_CAM_ERR_STORE (PHOTO_ERR → PHOTO_FAILED, arg2 = 3).
  *          Зв'язок перевіряється в обидва боки: заголовок кадру → seq події, запис
  *          PHOTO_SAVED → seq події і frame_id.
  *
  *          Не вдалося захопити кадр (SCCB, VSYNC, тайм-аут) — камера викликає
  *          SmartLock_OnCameraResult(event_id, 0, SL_CAM_ERR_CAPTURE) без цієї функції.
  *          Логувати PHOTO самостійно (SmartLock_LogEventRef) камера НЕ повинна:
  *          тоді на одну подію з'явилось би два записи.
  *
  *          Запис кадру блокувальний: QQVGA RGB565 38 400 Б ≈ 0,74–0,94 с (стирання
  *          10 секторів + 150 сторінок, flash-layout.md, розділ 4.2). Де саме його
  *          викликати (задача camera, порціями чи цілком) — рішення I09/I10.
  *          Лише з головного циклу.
  ******************************************************************************
  */
#ifndef SL_CAMERA_FRAMES_H
#define SL_CAMERA_FRAMES_H

#include <stdint.h>

#include "dev_bus.h"

/* DEV_OK — кадр збережено і PHOTO поставлено в журнал;
 * DEV_ERR_STATE — на event_id ніхто не чекає (кадр не записано, лічильник unknown);
 * DEV_ERR_PARAM — data == NULL або len == 0 (запит закрито як SL_CAM_ERR_CAPTURE);
 * інший код — помилка frames.c / W25Q64 (запит закрито як SL_CAM_ERR_STORE). */
dev_status_t SmartLock_CameraStoreFrame(uint32_t event_id, uint8_t format, uint16_t width,
                                        uint16_t height, const uint8_t *data, uint32_t len);

#endif /* SL_CAMERA_FRAMES_H */
