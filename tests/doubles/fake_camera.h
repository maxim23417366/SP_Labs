/**
  * @file    fake_camera.h
  * @brief   ЛР3 (I07): заміна Platform_CameraCapture() для тестів App/ (CAM TEST).
  *
  *          Справжній алгоритм захоплення перевіряє test_camera_dvp.c на моделі
  *          сигналів (fake_dvp); тут лише задається, що «повернула платформа».
  *          Окремий файл, бо бінарник test_camera_dvp лінкує справжній camera_dvp.c.
  */
#ifndef FAKE_CAMERA_H
#define FAKE_CAMERA_H

#include "platform.h"

typedef struct
{
  platform_cam_status_t status;      /* що повернути */
  uint32_t              cycles;      /* r.cycles (1 такт = 1 мкс у fake_platform) */
  uint16_t              short_lines;
  uint16_t              vsync_glitches;
  uint32_t              calls;
  platform_cam_cfg_t    last_cfg;
  size_t                last_cap;
} fake_camera_t;

extern fake_camera_t fake_camera;

/* status = OK, cycles = 530000 (≈ 0,53 с при CLKRC = 0x0F), лічильники = 0 */
void fake_camera_reset(void);

#endif /* FAKE_CAMERA_H */
