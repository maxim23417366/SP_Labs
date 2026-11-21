/**
  ******************************************************************************
  * @file    camera.c
  * @brief   ЛР3 (I09): запит кадру від FSM → захоплення → запис у W25Q64 з ID події.
  *          Сильна SmartLock_CameraRequest() замінює слабку заглушку sl_camera_link.c.
  ******************************************************************************
  */
#include "camera.h"

#include <stdio.h>

#include "app_config.h"
#include "cam_test.h"
#include "evt.h"
#include "frame_store.h"
#include "sl_camera_frames.h"
#include "sl_camera_link.h"
#include "smartlock_hooks.h"

static uint32_t       s_req;                 /* ID події, для якої чекає кадр; 0 — немає */
static camera_stats_t s_stats;

void Camera_Init(void)
{
    s_req = 0U;
    s_stats = (camera_stats_t){ 0U };
}

bool SmartLock_CameraRequest(uint32_t event_id)
{
    if ((event_id == 0U) || (s_req != 0U) || CamTest_FrameLocked())
    {
        s_stats.rejected++;
        return false;
    }
    s_req = event_id;
    s_stats.accepted++;
    return true;
}

bool Camera_Pending(void)
{
    return s_req != 0U;
}

bool SmartLock_CameraPoll(uint32_t now_ms)
{
    platform_cam_result_t r;
    platform_cam_status_t cap;
    const uint8_t *frame;
    size_t bytes = 0U;
    uint32_t id = s_req;
    uint32_t t0;
    uint32_t store_cyc = 0U;
    const char *result;
    bool ok;
    bool aec_ok;

    (void)now_ms;
    if (id == 0U)
    {
        return false;
    }
    s_req = 0U;

    PROBE_ON(PLATFORM_PROBE_CAMERA);       /* PB2: захоплення кадру */
    ok = CamTest_Capture(&cap, &r);
    PROBE_OFF(PLATFORM_PROBE_CAMERA);
    /* Без очікування сходження AEC (cam_test.c): камеру налаштовано при старті, і до
     * LOCKED минає ≥ 15 с; якщо ж її щойно (пере)ініціалізовано — кадр позначено */
    aec_ok = CamTest_AecSettled();
    if (!ok)
    {
        s_stats.capture_err++;
        SmartLock_OnCameraResult(id, 0U, SL_CAM_ERR_CAPTURE);
        result = "capture_err";
    }
    else
    {
        frame = CamTest_Frame(&bytes);
        CamTest_SetFrameEvent(id);
        Evt_Log("flash_wr_start");
        PROBE_ON(PLATFORM_PROBE_STORAGE);  /* PB12: запис кадру у W25Q64 */
        t0 = Platform_Cycles();
        ok = (SmartLock_CameraStoreFrame(id, FRAME_FORMAT_RGB565, (uint16_t)CAMERA_WIDTH,
                                         (uint16_t)CAMERA_HEIGHT, frame, (uint32_t)bytes) == DEV_OK);
        store_cyc = Platform_Cycles() - t0;
        PROBE_OFF(PLATFORM_PROBE_STORAGE);
        if (ok)
        {
            Perf_StatAdd(CamTest_StoreStat(), store_cyc);
            s_stats.stored++;
            result = "ok";
        }
        else
        {
            s_stats.store_err++;
            result = "store_err";
        }
        Evt_Log("flash_wr_done");
    }
    printf("CAM,photo,event=%lu,status=%s,cap=%s,capture_us=%lu,store_us=%lu,aec=%s\r\n",
           (unsigned long)id, result, CamTest_StatusName(cap),
           (unsigned long)Perf_CyclesToUs(r.cycles), (unsigned long)Perf_CyclesToUs(store_cyc),
           aec_ok ? "ok" : "settling");
    return true;
}

const camera_stats_t *Camera_Stats(void)
{
    return &s_stats;
}

void Camera_PrintDiag(void)
{
    printf("DIAG name=camera accepted=%lu rejected=%lu stored=%lu capture_err=%lu store_err=%lu "
           "pending=%u\r\n",
           (unsigned long)s_stats.accepted, (unsigned long)s_stats.rejected,
           (unsigned long)s_stats.stored, (unsigned long)s_stats.capture_err,
           (unsigned long)s_stats.store_err, Camera_Pending() ? 1U : 0U);
}
