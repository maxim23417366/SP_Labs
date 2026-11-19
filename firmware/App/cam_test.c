/**
  ******************************************************************************
  * @file    cam_test.c
  * @brief   ЛР3 (I07): CAM TEST — ініціалізація OV7670 і захоплення одного кадру.
  ******************************************************************************
  */
#include "cam_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "crc32.h"
#include "evt.h"
#include "frame_export.h"
#include "frames.h"
#include "ov7670.h"
#include "platform.h"

/* Один буфер кадру на всю прошивку (docs/camera.md §2: 38 400 Б, ≈ 29 % RAM) */
static uint8_t     s_frame[CAMERA_FRAME_BYTES];
static size_t      s_frame_bytes;
static uint32_t    s_frame_event;   /* I09: ID події кадру (0 — стендовий кадр) для CAM EXPORT */
static ov7670_t    s_cam;
static bool        s_ready;
static perf_stat_t s_stat = PERF_STAT_INIT("camera");
static perf_stat_t s_store_stat = PERF_STAT_INIT("flash_frame");   /* I09: запис кадру (camera.c) */

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

/* I09: захоплення в спільний буфер — і для CAM TEST, і для задачі camera.
 * Камера ініціалізується при першому виклику (рядок CAM,init). */
bool CamTest_Capture(platform_cam_status_t *status, platform_cam_result_t *res)
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
    platform_cam_status_t st;

    if ((status == NULL) || (res == NULL))
    {
        return false;
    }
    memset(res, 0, sizeof(*res));
    *status = PLATFORM_CAM_ERR_ARG;
    if (!s_ready && !cam_init())
    {
        return false;               /* камера не відповідає по SCCB — кадру немає */
    }
    Evt_Log("capture_start");
    st = Platform_CameraCapture(&cfg, s_frame, sizeof(s_frame), res);
    Evt_Log("capture_done");

    s_frame_bytes = res->bytes;
    s_frame_event = 0U;
    *status = st;
    if (st == PLATFORM_CAM_OK)
    {
        Perf_StatAdd(&s_stat, res->cycles);
    }
    return st == PLATFORM_CAM_OK;
}

void CamTest_SetFrameEvent(uint32_t event_id)
{
    s_frame_event = event_id;
}

const char *CamTest_StatusName(platform_cam_status_t s)
{
    return cam_name(s);
}

static void cam_capture(void)
{
    platform_cam_result_t r;
    platform_cam_status_t st;

    if (!CamTest_Capture(&st, &r) && (st == PLATFORM_CAM_ERR_ARG) && !s_ready)
    {
        return;                     /* init не вдався: причина вже в CAM,init */
    }
    printf("CAM,result,status=%s,bytes=%lu,lines=%u,short=%u,vglitch=%u,capture_us=%lu,"
           "crc=0x%08lX\r\n",
           cam_name(st), (unsigned long)r.bytes, (unsigned)r.lines, (unsigned)r.short_lines,
           (unsigned)r.vsync_glitches, (unsigned long)Perf_CyclesToUs(r.cycles),
           (unsigned long)crc32_compute(s_frame, s_frame_bytes));
}

/* I08: синтетичний кадр — 8 кольорових смуг зверху, градієнт сірого знизу.
 * Той самий малюнок генерує tools/frame_decode.py::test_pattern(). */
void CamTest_FillTestPattern(uint8_t *buf, uint16_t width, uint16_t height)
{
    static const uint8_t bars[8][3] = {
        { 255U, 255U, 255U }, { 255U, 255U, 0U }, { 0U, 255U, 255U }, { 0U, 255U, 0U },
        { 255U, 0U, 255U },   { 255U, 0U, 0U },   { 0U, 0U, 255U },   { 0U, 0U, 0U },
    };
    size_t i = 0U;

    if ((buf == NULL) || (width < 2U))
    {
        return;
    }
    for (uint16_t y = 0U; y < height; y++)
    {
        for (uint16_t x = 0U; x < width; x++)
        {
            uint32_t r;
            uint32_t g;
            uint32_t b;
            uint16_t v;

            if (y < (height / 2U))
            {
                const uint8_t *c = bars[((uint32_t)x * 8U) / width];

                r = c[0];
                g = c[1];
                b = c[2];
            }
            else
            {
                r = ((uint32_t)x * 255U) / (uint32_t)(width - 1U);
                g = r;
                b = r;
            }
            v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            buf[i++] = (uint8_t)(v >> 8);       /* RGB565, старший байт першим, як OV7670 */
            buf[i++] = (uint8_t)(v & 0xFFU);
        }
    }
}

static const char *load_name(dev_status_t s)
{
    switch (s)
    {
        case DEV_OK:        return "ok";
        case DEV_ERR_ID:    return "no_frame";
        case DEV_ERR_STATE: return "crc";
        case DEV_ERR_PARAM: return "param";
        default:            return "flash_err";
    }
}

static void cam_load(const char *arg)
{
    char *end = NULL;
    unsigned long id = strtoul(arg, &end, 10);
    frame_header_t h;
    dev_status_t st;

    memset(&h, 0, sizeof(h));
    if ((end == arg) || (*end != '\0'))
    {
        st = DEV_ERR_PARAM;
    }
    else
    {
        st = FrameStore_GetOps()->read_frame((uint32_t)id, &h, s_frame, sizeof(s_frame));
    }
    if (st == DEV_OK)
    {
        s_frame_bytes = h.data_len;
        s_frame_event = h.event_seq;
    }
    else
    {
        s_frame_bytes = 0U;         /* буфер міг бути частково перезаписаний */
        s_frame_event = 0U;
    }
    printf("CAM,load,frame=%lu,status=%s,event=%lu,w=%u,h=%u,len=%lu,crc=0x%08lX\r\n", id,
           load_name(st), (unsigned long)h.event_seq, (unsigned)h.width, (unsigned)h.height,
           (unsigned long)s_frame_bytes, (unsigned long)crc32_compute(s_frame, s_frame_bytes));
}

/* Буфер кадру не змінюється, поки йде експорт (I08) */
static bool frame_locked(const char *op)
{
    if (FrameExport_Busy())
    {
        printf("CAM,busy,op=%s,reason=export\r\n", op);
        return true;
    }
    return false;
}

bool CamTest_LineCommand(const char *line)
{
    if (line == NULL)
    {
        return false;
    }
    if (strcmp(line, "CAM TEST") == 0)
    {
        if (!frame_locked("test"))
        {
            cam_capture();
        }
        return true;
    }
    if (strcmp(line, "CAM PATTERN") == 0)
    {
        /* I08: відомий кадр без камери — перевірка передачі й відновлення на ПК */
        if (!frame_locked("pattern"))
        {
            CamTest_FillTestPattern(s_frame, (uint16_t)CAMERA_WIDTH, (uint16_t)CAMERA_HEIGHT);
            s_frame_bytes = CAMERA_FRAME_BYTES;
            s_frame_event = 0U;
            printf("CAM,pattern,bytes=%lu,crc=0x%08lX\r\n", (unsigned long)s_frame_bytes,
                   (unsigned long)crc32_compute(s_frame, s_frame_bytes));
        }
        return true;
    }
    if (strncmp(line, "CAM LOAD ", 9U) == 0)
    {
        /* I09: кадр із W25Q64 за frame_id назад у буфер (після рестарту теж) —
         * далі CAM EXPORT передає його на ПК з id = event_seq із заголовка */
        if (!frame_locked("load"))
        {
            cam_load(line + 9);
        }
        return true;
    }
    if (strcmp(line, "CAM EXPORT") == 0)
    {
        /* I08: останній кадр (CAM TEST або CAM PATTERN) на ПК — FRAME_BEGIN/FRM/FRAME_END */
        if (s_frame_bytes != CAMERA_FRAME_BYTES)
        {
            printf("CAM,export,status=no_frame\r\n");
        }
        else if (!FrameExport_Start(s_frame, (uint32_t)s_frame_bytes, s_frame_event))
        {
            printf("CAM,busy,op=export,reason=export\r\n");
        }
        return true;
    }
    return false;
}

const perf_stat_t *CamTest_Stat(void)
{
    return &s_stat;
}

void CamTest_ResetStats(void)
{
    Perf_StatReset(&s_stat);
    Perf_StatReset(&s_store_stat);
}

perf_stat_t *CamTest_StoreStat(void)
{
    return &s_store_stat;
}

const uint8_t *CamTest_Frame(size_t *bytes)
{
    if (bytes != NULL)
    {
        *bytes = s_frame_bytes;
    }
    return s_frame;
}

bool CamTest_FrameLocked(void)
{
    return FrameExport_Busy();
}

void CamTest_Reset(void)
{
    s_ready = false;
    s_frame_bytes = 0U;
    s_frame_event = 0U;
    memset(&s_cam, 0, sizeof(s_cam));
}
