/**
  ******************************************************************************
  * @file    cam_test.c
  * @brief   ЛР3 (I07): CAM TEST — ініціалізація OV7670 і захоплення одного кадру.
  ******************************************************************************
  */
#include "cam_test.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "crc32.h"
#include "evt.h"
#include "ov7670.h"
#include "platform.h"

/* Один буфер кадру на всю прошивку (docs/camera.md §2: 38 400 Б, ≈ 29 % RAM) */
static uint8_t     s_frame[CAMERA_FRAME_BYTES];
static size_t      s_frame_bytes;
static ov7670_t    s_cam;
static bool        s_ready;
static perf_stat_t s_stat = PERF_STAT_INIT("camera");

static const char *dev_name(dev_status_t s)
{
    switch (s)
    {
        case DEV_OK:          return "ok";
        case DEV_ERR_NACK:    return "nack";
        case DEV_ERR_TIMEOUT: return "timeout";
        case DEV_ERR_ID:      return "wrong_id";
        default:              return "err";
    }
}

static const char *cam_name(platform_cam_status_t s)
{
    switch (s)
    {
        case PLATFORM_CAM_OK:                return "ok";
        case PLATFORM_CAM_ERR_OVERFLOW:      return "overflow";
        case PLATFORM_CAM_ERR_VSYNC_TIMEOUT: return "vsync_timeout";
        case PLATFORM_CAM_ERR_HREF_TIMEOUT:  return "href_timeout";
        case PLATFORM_CAM_ERR_PCLK_TIMEOUT:  return "pclk_timeout";
        case PLATFORM_CAM_ERR_SHORT_FRAME:   return "short_frame";
        default:                             return "arg";
    }
}

/* PID/VER, скидання, CLKRC, таблиця QQVGA RGB565. Друкує CAM,init,... */
static bool cam_init(void)
{
    size_t       n = 0U;
    uint8_t      failed = 0U;
    dev_status_t s = ov7670_init(&s_cam, Platform_SccbRead, Platform_SccbWrite, Platform_DelayMs);

    if (s == DEV_OK)
    {
        const ov7670_reg_t *table = ov7670_qqvga_rgb565_table(&n);

        s = ov7670_configure(&s_cam, table, n, (uint8_t)TBD_CAMERA_CLKRC, &failed);
    }
    printf("CAM,init,status=%s,pid=0x%02X,ver=0x%02X,clkrc=0x%02X,failed_reg=0x%02X\r\n",
           dev_name(s), (unsigned)s_cam.pid, (unsigned)s_cam.ver,
           (unsigned)(TBD_CAMERA_CLKRC & OV7670_CLKRC_MASK), (unsigned)failed);
    s_ready = (s == DEV_OK);
    return s_ready;
}

static void cam_capture(void)
{
    const platform_cam_cfg_t cfg = {
        .width             = (uint16_t)CAMERA_WIDTH,
        .height            = (uint16_t)CAMERA_HEIGHT,
        .bytes_per_pixel   = (uint8_t)CAMERA_BPP,
        .irq_lock_per_line = (uint8_t)CAMERA_IRQ_LOCK_PER_LINE,
        .vsync_timeout_us  = TBD_CAMERA_VSYNC_TIMEOUT_US,
        .vsync_min_us      = TBD_CAMERA_VSYNC_MIN_US,
        .href_timeout_us   = TBD_CAMERA_HREF_TIMEOUT_US,
        .pclk_spin_max     = TBD_CAMERA_PCLK_SPIN_MAX,
    };
    platform_cam_result_t r;
    platform_cam_status_t st;

    memset(&r, 0, sizeof(r));
    Evt_Log("capture_start");
    st = Platform_CameraCapture(&cfg, s_frame, sizeof(s_frame), &r);
    Evt_Log("capture_done");

    s_frame_bytes = r.bytes;
    if (st == PLATFORM_CAM_OK)
    {
        Perf_StatAdd(&s_stat, r.cycles);
    }
    printf("CAM,result,status=%s,bytes=%lu,lines=%u,short=%u,vglitch=%u,capture_us=%lu,"
           "crc=0x%08lX\r\n",
           cam_name(st), (unsigned long)r.bytes, (unsigned)r.lines, (unsigned)r.short_lines,
           (unsigned)r.vsync_glitches, (unsigned long)Perf_CyclesToUs(r.cycles),
           (unsigned long)crc32_compute(s_frame, s_frame_bytes));
}

bool CamTest_LineCommand(const char *line)
{
    if ((line == NULL) || (strcmp(line, "CAM TEST") != 0))
    {
        return false;
    }
    if (s_ready || cam_init())
    {
        cam_capture();
    }
    return true;
}

const perf_stat_t *CamTest_Stat(void)
{
    return &s_stat;
}

void CamTest_ResetStats(void)
{
    Perf_StatReset(&s_stat);
}

const uint8_t *CamTest_Frame(size_t *bytes)
{
    if (bytes != NULL)
    {
        *bytes = s_frame_bytes;
    }
    return s_frame;
}

void CamTest_Reset(void)
{
    s_ready = false;
    s_frame_bytes = 0U;
    memset(&s_cam, 0, sizeof(s_cam));
}
