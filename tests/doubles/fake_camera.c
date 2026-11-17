/**
  * @file    fake_camera.c
  * @brief   ЛР3 (I07): Platform_CameraCapture() для тестів App/. Кадр — байти
  *          (uint8_t)i, тож CRC-32 кадру відома заздалегідь.
  */
#include "fake_camera.h"

#include <string.h>

fake_camera_t fake_camera;

void fake_camera_reset(void)
{
  memset(&fake_camera, 0, sizeof(fake_camera));
  fake_camera.status = PLATFORM_CAM_OK;
  fake_camera.cycles = 530000U;
}

platform_cam_status_t Platform_CameraCapture(const platform_cam_cfg_t *cfg, uint8_t *buf,
                                             size_t cap, platform_cam_result_t *res)
{
  size_t need;

  fake_camera.calls++;
  if ((cfg == NULL) || (buf == NULL) || (res == NULL))
  {
    return PLATFORM_CAM_ERR_ARG;
  }
  fake_camera.last_cfg = *cfg;
  fake_camera.last_cap = cap;
  memset(res, 0, sizeof(*res));
  need = (size_t)cfg->width * cfg->height * cfg->bytes_per_pixel;
  if (cap < need)
  {
    return PLATFORM_CAM_ERR_OVERFLOW;
  }
  res->cycles = fake_camera.cycles;
  res->vsync_glitches = fake_camera.vsync_glitches;
  if (fake_camera.status == PLATFORM_CAM_ERR_VSYNC_TIMEOUT)
  {
    return fake_camera.status;           /* кадр не почався: 0 байтів */
  }
  for (size_t i = 0U; i < need; i++)
  {
    buf[i] = (uint8_t)i;
  }
  res->bytes = (uint32_t)need;
  res->lines = cfg->height;
  res->short_lines = fake_camera.short_lines;
  return fake_camera.status;
}
