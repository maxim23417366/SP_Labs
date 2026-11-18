/**
  ******************************************************************************
  * @file    frame_export.c
  * @brief   ЛР3 (I08): експорт кадру порціями — один рядок за прохід задачі uart.
  ******************************************************************************
  */
#include "frame_export.h"

#include <stdio.h>

#include "app_config.h"
#include "crc32.h"
#include "evt.h"

static const uint8_t *s_data;
static uint32_t       s_len;
static uint32_t       s_off;
static uint32_t       s_event;
static uint32_t       s_crc;
static uint32_t       s_lines;
static bool           s_busy;

int FrameExport_FormatChunk(char *buf, size_t size, uint32_t off, const uint8_t *p, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    int len;

    if ((buf == NULL) || (p == NULL))
    {
        return -1;
    }
    len = snprintf(buf, size, "FRM off=%lu d=", (unsigned long)off);
    if ((len < 0) || (((size_t)len + (2U * n)) >= size))
    {
        return -1;
    }
    for (size_t i = 0U; i < n; i++)
    {
        buf[len++] = hex[p[i] >> 4];
        buf[len++] = hex[p[i] & 0x0FU];
    }
    buf[len] = '\0';
    return len;
}

bool FrameExport_Start(const uint8_t *pixels, uint32_t len, uint32_t event_id)
{
    if (s_busy || (pixels == NULL) || (len == 0U))
    {
        return false;
    }
    s_data  = pixels;
    s_len   = len;
    s_off   = 0U;
    s_event = event_id;
    s_lines = 0U;
    s_crc   = crc32_compute(pixels, len);
    s_busy  = true;
    Evt_LogArg("export_start", event_id);
    printf("FRAME_BEGIN id=%lu seq=0 w=%u h=%u fmt=RGB565 len=%lu crc32=%08lX chunk=%u\r\n",
           (unsigned long)event_id, (unsigned)CAMERA_WIDTH, (unsigned)CAMERA_HEIGHT,
           (unsigned long)len, (unsigned long)s_crc, (unsigned)FRAME_EXPORT_CHUNK);
    return true;
}

bool FrameExport_Step(void)
{
    char line[24U + (2U * FRAME_EXPORT_CHUNK)];

    if (!s_busy)
    {
        return false;
    }
    if (s_off < s_len)
    {
        uint32_t n = s_len - s_off;

        if (n > FRAME_EXPORT_CHUNK)
        {
            n = FRAME_EXPORT_CHUNK;
        }
        (void)FrameExport_FormatChunk(line, sizeof(line), s_off, &s_data[s_off], n);
        printf("%s\r\n", line);
        s_off += n;
        s_lines++;
        return true;
    }

    printf("FRAME_END id=%lu len=%lu crc32=%08lX lines=%lu\r\n", (unsigned long)s_event,
           (unsigned long)s_len, (unsigned long)s_crc, (unsigned long)s_lines);
    Evt_LogArg("export_done", s_event);
    s_busy = false;
    return true;
}

bool FrameExport_Busy(void)
{
    return s_busy;
}

void FrameExport_Abort(void)
{
    s_busy = false;
}
