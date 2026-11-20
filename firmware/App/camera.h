/**
  ******************************************************************************
  * @file    camera.h
  * @brief   ЛР3 (I09): камера в Super Loop — кадр охоронної події у W25Q64.
  *
  *          Шлях (docs/lab_3/journal-camera-integration.md §4, docs/camera.md §6):
  *
  *            FSM (LOCKED, MOTION / TAMPER) ── SmartLock_CameraLinkStart(id)
  *              └► SmartLock_CameraRequest(id)    [тут, сильна; не чекає — лише запит]
  *            задача camera (наступний прохід):
  *              CamTest_Capture()                  кадр 38 400 Б у спільний буфер (I07)
  *              SmartLock_CameraStoreFrame(id,…)   write_frame (H10) з event_seq = id,
  *                                                 PHOTO / PHOTO_ERR STORE у журналі (K08)
  *              або SmartLock_OnCameraResult(id, 0, SL_CAM_ERR_CAPTURE)
  *
  *          Рівно один результат на прийнятий запит. Модуль сам PHOTO не логує.
  *          Один запит за раз: поки попередній не оброблено або буфер кадру
  *          передається на ПК (CAM EXPORT, I08) — запит відхиляється
  *          (журнал запише PHOTO_ERR REJECTED).
  *
  *          Рядок UART після кожного запиту:
  *            CAM,photo,event=<id>,status=ok|capture_err|store_err,cap=<статус>,
  *                      capture_us=<мкс>,store_us=<мкс>
  *          EVT (metrics-spec §3.3): capture_start / capture_done (cam_test.c),
  *          flash_wr_start / flash_wr_done навколо запису кадру.
  *          STATS: STAT name=camera (захоплення), STAT name=flash_frame (запис кадру).
  ******************************************************************************
  */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t accepted;       /* запитів прийнято */
    uint32_t rejected;       /* відхилено: зайнята / буфер експортується / id = 0 */
    uint32_t stored;         /* кадр записано у Flash */
    uint32_t capture_err;    /* кадр не захоплено */
    uint32_t store_err;      /* кадр не записано (Flash / тайм-аут запиту) */
} camera_stats_t;

/* Задача camera (app_tasks.c через SmartLock_CameraPoll): обробляє запит, якщо є */
bool SmartLock_CameraPoll(uint32_t now_ms);

/* true — є запит, який ще не оброблено */
bool Camera_Pending(void);

const camera_stats_t *Camera_Stats(void);
void                  Camera_Init(void);

/* I10: рядок STATS «DIAG name=camera accepted= rejected= stored= capture_err=
 * store_err= pending=». Викликає sl_diag.c; там же слабка версія на випадок
 * збірки без camera.c («DIAG name=camera present=0»). */
void Camera_PrintDiag(void);

#endif /* CAMERA_H */
