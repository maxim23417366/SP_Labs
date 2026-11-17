/**
  ******************************************************************************
  * @file    sl_camera_frames.c
  * @brief   ЛР3 (K08): збереження кадру охоронної події через frames.c (H10)
  *          і закриття запиту sl_camera_link (див. sl_camera_frames.h).
  *
  *          Окремим файлом від sl_camera_link.c: логіка «рівно один PHOTO /
  *          PHOTO_ERR на подію» не залежить від Flash і тестується з моком
  *          журналу, а тут — лише з'єднання з FrameStore_GetOps().
  ******************************************************************************
  */
#include "sl_camera_frames.h"

#include <string.h>

#include "frames.h"
#include "sl_camera_link.h"

dev_status_t SmartLock_CameraStoreFrame(uint32_t event_id, uint8_t format, uint16_t width,
                                        uint16_t height, const uint8_t *data, uint32_t len)
{
    const frame_store_ops_t *ops = FrameStore_GetOps();
    frame_header_t h;
    uint32_t frame_id = 0U;
    dev_status_t st;

    if (!SmartLock_CameraLinkAwaiting(event_id))
    {
        /* тайм-аут уже записав PHOTO_ERR, або ID чужий / дубль: кадр без посилання не пишемо */
        SmartLock_OnCameraResult(event_id, 0U, SL_CAM_ERR_CAPTURE);   /* лише рахує unknown */
        return DEV_ERR_STATE;
    }
    if ((data == NULL) || (len == 0U))
    {
        SmartLock_OnCameraResult(event_id, 0U, SL_CAM_ERR_CAPTURE);
        return DEV_ERR_PARAM;
    }

    memset(&h, 0, sizeof(h));
    h.format = format;
    h.width = width;
    h.height = height;
    h.event_seq = event_id;   /* той самий seq, що в журналі (sl_journal) і у Flash */
    h.data_len = len;
    st = ops->write_frame(&h, data, &frame_id);

    if (st == DEV_OK)
    {
        SmartLock_OnCameraResult(event_id, frame_id, SL_CAM_OK);
    }
    else
    {
        SmartLock_OnCameraResult(event_id, 0U, SL_CAM_ERR_STORE);
    }
    return st;
}
