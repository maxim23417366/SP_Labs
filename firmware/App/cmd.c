/**
  ******************************************************************************
  * @file    cmd.c
  * @brief   Накопичення і розбір рядкових команд ЛР3 (I03).
  ******************************************************************************
  */
#include "cmd.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_tasks.h"
#include "cam_test.h"
#include "evt.h"
#include "perf.h"
#include "stats.h"
#include "storage.h"

static char     s_line[CMD_LINE_MAX];
static size_t   s_len;
static bool     s_active;
static bool     s_overflow;
static uint32_t s_overflows;
static bool     s_skip;        /* після втрати байтів — відкинути все до кінця рядка */
static uint32_t s_dropped;

void Cmd_Reset(void)
{
    s_len = 0U;
    s_active = false;
    s_overflow = false;
    s_overflows = 0U;
    s_skip = false;
    s_dropped = 0U;
}

uint32_t Cmd_LineOverflows(void)
{
    return s_overflows;
}

/* Байти загублено (PLATFORM_UART_LOST): рядок, що зараз набирається, і будь-який
 * рядок, у який могли входити загублені байти, не виконується — інакше «PING 12»
 * без «2» стала б «PING 1», а «MARK 2» без '\r' злиплася б з наступною командою */
void Cmd_RxLost(void)
{
    s_active = false;
    s_len = 0U;
    s_skip = true;
}

uint32_t Cmd_Dropped(void)
{
    return s_dropped;
}

static void help(void)
{
    printf("# LR3: STATS [RAW|RESET] | MEM | PING <n> | MARK <n> | HELP\r\n");
    printf("#      LOG ERASE (journal W25Q64)\r\n");
    printf("#      CAM TEST (OV7670: init + 1 frame QQVGA RGB565)\r\n");
    printf("#      CAM PATTERN (test frame) | CAM EXPORT (last frame -> PC, tools/frame_decode.py)\r\n");
}

void Cmd_Execute(const char *line)
{
    if (strcmp(line, "STATS") == 0)
    {
        Stats_Print(false);
    }
    else if (strcmp(line, "STATS RAW") == 0)
    {
        Stats_Print(true);
    }
    else if (strcmp(line, "STATS RESET") == 0)
    {
        Stats_Reset();
        Evt_Log("stats_reset");
        printf("STATS,reset\r\n");
    }
    else if (strcmp(line, "MEM") == 0)
    {
        AppTasks_PrintMem();
    }
    else if (strncmp(line, "PING ", 5U) == 0)
    {
        char t_us[21];
        unsigned long n = strtoul(&line[5], NULL, 10);

        (void)Perf_FormatU64(t_us, sizeof(t_us), Perf_TimeUs64());
        printf("PONG seq=%lu t_us=%s\r\n", n, t_us);
    }
    else if (strncmp(line, "MARK ", 5U) == 0)
    {
        Evt_LogArg("mark", (uint32_t)strtoul(&line[5], NULL, 10));
    }
    else if (strcmp(line, "HELP") == 0)
    {
        help();
    }
    else if (CamTest_LineCommand(line))
    {
        /* I07: камера OV7670 — init і один кадр (cam_test.c) */
    }
    else if (Storage_LineCommand(line))
    {
        /* журнал W25Q64: LOG ERASE (storage.c) */
    }
    else
    {
        printf("# unknown command '%s', HELP for list\r\n", line);
    }
}

int Cmd_Feed(int c)
{
    if (s_skip)
    {
        if ((c == '\r') || (c == '\n'))
        {
            s_skip = false;
            s_dropped++;
            printf("ERR,cmd=dropped,reason=rx_lost\r\n");   /* ПК бачить відповідь і може повторити */
        }
        return 1;
    }
    if (!s_active)
    {
        if ((c < 'A') || (c > 'Z'))
        {
            return 0;
        }
        s_active = true;
        s_overflow = false;
        s_len = 0U;
    }

    if ((c == '\r') || (c == '\n'))
    {
        s_line[s_len] = '\0';
        s_active = false;
        if (s_overflow)
        {
            printf("# command too long (>%u)\r\n", (unsigned)(CMD_LINE_MAX - 1U));
        }
        else
        {
            Evt_Log("uart_cmd_rx");
            Cmd_Execute(s_line);
            Evt_Log("uart_cmd_tx");
        }
        return 1;
    }

    if (s_len < CMD_LINE_MAX - 1U)
    {
        s_line[s_len++] = (char)c;
    }
    else if (!s_overflow)
    {
        s_overflow = true;
        s_overflows++;
    }
    return 1;
}
