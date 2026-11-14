/**
  ******************************************************************************
  * @file    sl_camera_link.c
  * @brief   ЛР3 (K08): зв'язок події з фото і слабка заглушка камери
  *          (див. sl_camera_link.h).
  *
  *          Список очікуваних запитів: слот живе від CameraLinkStart() до
  *          запису PHOTO / PHOTO_ERR у журнал. Слот має два стани: «чекає
  *          результату» (resolved = false) і «результат є, чекає місця в
  *          журналі» (resolved = true).
  ******************************************************************************
  */
#include "sl_camera_link.h"

#include "sl_journal.h"

typedef struct
{
    bool     used;
    bool     resolved;     /* result_type / result_arg заповнено, лишилось записати */
    uint32_t event_id;
    uint32_t t_req_ms;
    uint8_t  result_type;  /* sl_log_type_t: PHOTO або PHOTO_ERR */
    uint32_t result_arg;   /* ID кадру або sl_cam_status_t */
} camlink_slot_t;

static camlink_slot_t     s_slot[TBD_CAMLINK_PENDING_MAX];
static sl_camlink_stats_t s_stats;

/* ---- слабка заглушка камери -------------------------------------------------- */
static bool     s_stub_accept;
static unsigned s_stub_calls;
static uint32_t s_stub_last_id;

__attribute__((weak)) bool SmartLock_CameraRequest(uint32_t event_id)
{
    s_stub_calls++;
    s_stub_last_id = event_id;
    return s_stub_accept;
}

void SmartLock_CameraStubSetAccept(bool accept)
{
    s_stub_accept = accept;
}

unsigned SmartLock_CameraStubCalls(void)
{
    return s_stub_calls;
}

uint32_t SmartLock_CameraStubLastId(void)
{
    return s_stub_last_id;
}

void SmartLock_CameraStubReset(void)
{
    s_stub_accept = false;
    s_stub_calls = 0U;
    s_stub_last_id = 0U;
}

/* ---- список очікуваних запитів -------------------------------------------------- */
void SmartLock_CameraLinkInit(void)
{
    for (unsigned i = 0U; i < TBD_CAMLINK_PENDING_MAX; i++)
    {
        s_slot[i].used = false;
        s_slot[i].resolved = false;
    }
    s_stats = (sl_camlink_stats_t){ 0U };
}

static camlink_slot_t *find_slot(uint32_t event_id)
{
    for (unsigned i = 0U; i < TBD_CAMLINK_PENDING_MAX; i++)
    {
        if (s_slot[i].used && (s_slot[i].event_id == event_id))
        {
            return &s_slot[i];
        }
    }
    return NULL;
}

static camlink_slot_t *free_slot(void)
{
    for (unsigned i = 0U; i < TBD_CAMLINK_PENDING_MAX; i++)
    {
        if (!s_slot[i].used)
        {
            return &s_slot[i];
        }
    }
    return NULL;
}

static void resolve(camlink_slot_t *s, sl_log_type_t type, uint32_t arg)
{
    s->resolved = true;
    s->result_type = (uint8_t)type;
    s->result_arg = arg;
}

/* Записати результат у журнал; слот звільняється лише коли запис прийнято */
static bool flush(camlink_slot_t *s)
{
    if (SmartLock_LogEventRef((sl_log_type_t)s->result_type, s->result_arg, s->event_id) == SL_LOG_ID_NONE)
    {
        return false;   /* FIFO повна навіть із резервом: спробуємо в наступному проході */
    }
    s->used = false;
    s->resolved = false;
    return true;
}

bool SmartLock_CameraLinkStart(uint32_t event_id, uint32_t now_ms)
{
    camlink_slot_t *s;
    bool accepted;

    if (event_id == SL_LOG_ID_NONE)
    {
        s_stats.skipped_no_id++;   /* кадр нікуди прив'язати — не знімаємо */
        return false;
    }
    s = free_slot();
    if (s == NULL)
    {
        /* список повний: ставимо запис-помилку без слота, а камеру не турбуємо */
        s_stats.rejected++;
        (void)SmartLock_LogEventRef(SL_LOG_PHOTO_ERR, SL_CAM_ERR_REJECTED, event_id);
        return false;
    }
    /* слот займаємо ДО виклику камери: результат може прийти синхронно */
    s->used = true;
    s->resolved = false;
    s->event_id = event_id;
    s->t_req_ms = now_ms;
    s_stats.requested++;
    accepted = SmartLock_CameraRequest(event_id);
    if (!accepted && !s->resolved)
    {
        s_stats.rejected++;
        resolve(s, SL_LOG_PHOTO_ERR, SL_CAM_ERR_REJECTED);
        (void)flush(s);
    }
    return accepted;
}

void SmartLock_OnCameraResult(uint32_t event_id, uint32_t frame_id, sl_cam_status_t status)
{
    camlink_slot_t *s = find_slot(event_id);

    if ((s == NULL) || s->resolved)
    {
        /* не чекали: дубль, ID = 0 або результат уже після тайм-ауту (слот звільнено) */
        s_stats.unknown++;
        return;
    }
    if (status == SL_CAM_OK)
    {
        s_stats.ok++;
        resolve(s, SL_LOG_PHOTO, frame_id);
    }
    else
    {
        s_stats.failed++;
        resolve(s, SL_LOG_PHOTO_ERR, ((unsigned)status < (unsigned)SL_CAM_STATUS_COUNT) ? (uint32_t)status
                                                                                         : (uint32_t)SL_CAM_ERR_CAPTURE);
    }
    (void)flush(s);
}

bool SmartLock_CameraLinkPoll(uint32_t now_ms)
{
    bool changed = false;

    for (unsigned i = 0U; i < TBD_CAMLINK_PENDING_MAX; i++)
    {
        camlink_slot_t *s = &s_slot[i];

        if (!s->used)
        {
            continue;
        }
        if (!s->resolved)
        {
            if ((uint32_t)(now_ms - s->t_req_ms) < TBD_CAM_RESULT_TIMEOUT_MS)
            {
                continue;
            }
            s_stats.timeouts++;
            resolve(s, SL_LOG_PHOTO_ERR, SL_CAM_ERR_TIMEOUT);
        }
        if (flush(s))
        {
            changed = true;
        }
    }
    return changed;
}

unsigned SmartLock_CameraLinkPending(void)
{
    unsigned n = 0U;

    for (unsigned i = 0U; i < TBD_CAMLINK_PENDING_MAX; i++)
    {
        if (s_slot[i].used)
        {
            n++;
        }
    }
    return n;
}

const sl_camlink_stats_t *SmartLock_CameraLinkStats(void)
{
    return &s_stats;
}

const char *SmartLock_CameraStatusName(sl_cam_status_t status)
{
    static const char *const names[SL_CAM_STATUS_COUNT] =
    {
        "OK", "REJECTED", "CAPTURE", "STORE", "TIMEOUT",
    };

    return ((unsigned)status < (unsigned)SL_CAM_STATUS_COUNT) ? names[status] : "?";
}
