/**
  ******************************************************************************
  * @file    sl_camera_link.h
  * @brief   ЛР3 (K02 → K08/I10): запит фотофіксації від FSM до камери (FR-07).
  *
  *          FSM у LOCKED на MOTION / TAMPER записує подію в журнал і викликає
  *          SmartLock_CameraRequest(id), де id — ID запису журналу (seq,
  *          sl_journal.h). Камера знімає ОДИН кадр, а після запису у Flash
  *          K08 додає запис SL_LOG_PHOTO (arg = ID кадру, ref = id).
  *
  *          Модуль камери (I07–I09, Непорожній) ще не існує. У docs/tasks.md
  *          функція названа Camera_Request(id): при інтеграції (I10) камера
  *          визначає СИЛЬНУ SmartLock_CameraRequest() (наприклад, як обгортку
  *          над Camera_Request), і слабка заглушка з sl_camera_link.c
  *          відкидається лінкером — без правок FSM.
  *
  *          Виклик не чекає: лише ставить запит у чергу камери, захоплення —
  *          у задачі camera того самого проходу (docs/tasks.md §4).
  ******************************************************************************
  */
#ifndef SL_CAMERA_LINK_H
#define SL_CAMERA_LINK_H

#include <stdbool.h>
#include <stdint.h>

/* true — запит прийнято; false — камери немає / черга зайнята (FSM рахує
 * відмови, SmartLock_FsmCameraRejected()). event_id може бути SL_LOG_ID_NONE,
 * якщо запис журналу втрачено через переповнення FIFO. */
bool SmartLock_CameraRequest(uint32_t event_id);

#endif /* SL_CAMERA_LINK_H */
