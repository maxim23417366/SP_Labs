/**
  ******************************************************************************
  * @file    evt.c
  * @brief   Кільцевий буфер EVT-подій і їх вивід порціями.
  ******************************************************************************
  */
#include "evt.h"

#include <stdio.h>

#include "app_config.h"
#include "perf.h"

typedef struct
{
    uint64_t    t_us;
    const char *name;
    uint32_t    arg;
} evt_rec_t;

static evt_rec_t s_ring[EVT_RING_LEN];
static unsigned  s_head;
static unsigned  s_tail;
static unsigned  s_count;
static uint32_t  s_dropped;

void Evt_Init(void)
{
    s_head    = 0U;
    s_tail    = 0U;
    s_count   = 0U;
    s_dropped = 0U;
}

void Evt_LogAt(uint64_t t_us, const char *name, uint32_t arg)
{
    if (s_count >= EVT_RING_LEN)
    {
        s_dropped++;   /* старі записи цінніші: нову подію відкидаємо */
        return;
    }
    s_ring[s_head].t_us = t_us;
    s_ring[s_head].name = name;
    s_ring[s_head].arg  = arg;
    s_head = (s_head + 1U) % EVT_RING_LEN;
    s_count++;
}

void Evt_LogArg(const char *name, uint32_t arg)
{
    Evt_LogAt(Perf_TimeUs64(), name, arg);
}

void Evt_Log(const char *name)
{
    Evt_LogArg(name, EVT_NOARG);
}

int Evt_Format(char *buf, size_t size, uint64_t t_us, const char *name, uint32_t arg)
{
    char num[21];

    (void)Perf_FormatU64(num, sizeof(num), t_us);
    if (arg == EVT_NOARG)
    {
        return snprintf(buf, size, "EVT t_us=%s name=%s", num, name);
    }
    /* Аргумент — частина імені (mark_42): рядок лишається в форматі
     * ^EVT t_us=(\d+) name=(\S+)$ з docs/metrics-spec.md §3.3 */
    return snprintf(buf, size, "EVT t_us=%s name=%s_%lu", num, name, (unsigned long)arg);
}

unsigned Evt_Flush(unsigned max_lines)
{
    char     line[96];
    unsigned done = 0U;

    while ((s_count != 0U) && (done < max_lines))
    {
        const evt_rec_t *r = &s_ring[s_tail];

        (void)Evt_Format(line, sizeof(line), r->t_us, r->name, r->arg);
        printf("%s\r\n", line);
        s_tail = (s_tail + 1U) % EVT_RING_LEN;
        s_count--;
        done++;
    }
    return done;
}

unsigned Evt_Pending(void)
{
    return s_count;
}

uint32_t Evt_Dropped(void)
{
    return s_dropped;
}
