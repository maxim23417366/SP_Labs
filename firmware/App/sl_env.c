/**
  ******************************************************************************
  * @file    sl_env.c
  * @brief   ЛР3 (K06): DHT11 і BMP180, ENV_WARNING (правила — у sl_env.h).
  *
  *          Сильна SmartLock_EnvPoll() замінює слабку зі smartlock_hooks_weak.c.
  *          Датчики читаються так само, як у слабкому хуку; драйверні
  *          блокувальні затримки (DHT11 ~25 мс, BMP180 ~12 мс) не змінювались.
  ******************************************************************************
  */
#include "sl_env.h"

#include <stdio.h>

#include "app.h"
#include "dht11.h"
#include "event_queue.h"
#include "platform.h"
#include "smartlock_hooks.h"

#define DHT_THRESHOLD_US  48U   /* як у stand_test.c: HIGH > 48 мкс = 1 */
#define ENV_PARAMS        2U    /* індекс 0 — T, 1 — RH; біт параметра = 1 << індекс */

/* Різниця часу беззнакова: працює, поки тривалість < 2^31 мс */
_Static_assert(TBD_ENV_WARN_DURATION_MS < 0x80000000U, "TBD_ENV_WARN_DURATION_MS занадто велика");
_Static_assert(TBD_ENV_T_MAX_C - TBD_ENV_T_MIN_C > 2 * TBD_ENV_T_HYST_C, "гістерезис T перекриває зону норми");
_Static_assert(TBD_ENV_RH_MAX_PCT - TBD_ENV_RH_MIN_PCT > 2 * TBD_ENV_RH_HYST_PCT, "гістерезис RH перекриває зону норми");
_Static_assert(TBD_ENV_DHT_ERR_RESET_COUNT > 0U, "TBD_ENV_DHT_ERR_RESET_COUNT має бути > 0");

typedef struct
{
    uint32_t out_mask;                 /* параметри зараз поза межами (з гістерезисом) */
    uint32_t reported_mask;            /* про які вже покладено ENV_WARNING */
    uint32_t since_ms[ENV_PARAMS];     /* now_ms початку безперервного виходу */

    bool     dht_valid;
    int16_t  last_t_dc;
    uint8_t  last_rh_pct;
    bool     baro_valid;
    int32_t  last_p_pa;

    uint32_t dht_errors;
    uint32_t dht_err_streak;
    uint32_t bmp_errors;
    uint32_t warnings_posted;
    uint32_t post_failed;
} env_state_t;

static env_state_t env;   /* усе нулі = початковий стан */

void SmartLock_EnvInit(void)
{
    env = (env_state_t){ 0 };
}

/* Тригер Шмітта: поза межею — так; у смузі гістерезису — як було; глибше — ні */
static bool param_out(bool was_out, int32_t v, int32_t lo, int32_t hi, int32_t hyst)
{
    if ((v > hi) || (v < lo))
    {
        return true;
    }
    return was_out && ((v > (hi - hyst)) || (v < (lo + hyst)));
}

/* Один валідний зразок DHT11: оновити стан параметрів і, за потреби, подати подію */
static void env_evaluate(uint32_t now_ms, int32_t t_dc, int32_t rh_pct)
{
    uint32_t mature = 0U;
    uint32_t fresh;

    for (uint32_t i = 0U; i < ENV_PARAMS; i++)
    {
        uint32_t bit     = 1UL << i;
        bool     was_out = (env.out_mask & bit) != 0U;
        bool     is_out  = (i == 0U)
            ? param_out(was_out, t_dc, TBD_ENV_T_MIN_C * 10, TBD_ENV_T_MAX_C * 10, TBD_ENV_T_HYST_C * 10)
            : param_out(was_out, rh_pct, TBD_ENV_RH_MIN_PCT, TBD_ENV_RH_MAX_PCT, TBD_ENV_RH_HYST_PCT);

        if (is_out && !was_out)
        {
            env.out_mask |= bit;
            env.since_ms[i] = now_ms;
        }
        else if (!is_out && was_out)
        {
            env.out_mask &= ~bit;
            env.reported_mask &= ~bit;   /* нове перевищення знову дасть подію */
        }
        else
        {
            /* стан не змінився */
        }

        if (is_out && (((env.reported_mask & bit) != 0U) ||
                       ((uint32_t)(now_ms - env.since_ms[i]) >= TBD_ENV_WARN_DURATION_MS)))
        {
            mature |= bit;
        }
    }

    fresh = mature & ~env.reported_mask;
    if (fresh != 0U)
    {
        if (SmartLock_PostEvent(SL_EV_ENV_WARNING, mature))
        {
            env.reported_mask |= fresh;
            env.warnings_posted++;
        }
        else
        {
            env.post_failed++;   /* спроба повториться при наступному виклику */
        }
    }
}

/* Відмова DHT11: короткі серії стан не змінюють, довга скидає відлік (див. sl_env.h, п. 6) */
static void env_dht_failed(void)
{
    env.dht_errors++;
    env.dht_err_streak++;
    if (env.dht_err_streak >= TBD_ENV_DHT_ERR_RESET_COUNT)
    {
        env.out_mask = 0U;
        env.reported_mask = 0U;
    }
}

bool SmartLock_EnvPoll(uint32_t now_ms)
{
    uint16_t     us[40];
    uint8_t      frame[DHT11_FRAME_LEN];
    dht11_data_t d = { 0U, 0U, 0, 0U };
    dev_status_t ds;
    dev_status_t bs = DEV_ERR_STATE;
    int32_t      t_dc = 0;
    int32_t      p_pa = 0;

    ds = Platform_Dht11Capture(us);   /* 20 мс старт + ~4 мс кадр з вимкненими перериваннями */
    if (ds == DEV_OK)
    {
        dht11_bits_to_bytes(us, DHT_THRESHOLD_US, frame);
        ds = dht11_decode(frame, &d);
    }
    if (app_devices.baro_status == DEV_OK)
    {
        bs = bmp180_read(&app_devices.baro, &t_dc, &p_pa);   /* ~10 мс: два перетворення з HAL_Delay */
    }

    printf("ENV,dht=%s", (ds == DEV_OK) ? "ok" : "err");
    if (ds == DEV_OK)
    {
        printf(",t_c=%d.%u,rh=%u", (int)d.temp_c, (unsigned)d.temp_tenth, (unsigned)d.rh_pct);
    }
    printf(",bmp=%s", (bs == DEV_OK) ? "ok" : "err");
    if (bs == DEV_OK)
    {
        printf(",p_pa=%ld", (long)p_pa);   /* температуру BMP180 (t_dc) не виводимо — FR-15 */
    }
    printf("\r\n");

    if (ds == DEV_OK)
    {
        int32_t dc = ((int32_t)d.temp_c * 10) +
                     ((d.temp_c < 0) ? -(int32_t)d.temp_tenth : (int32_t)d.temp_tenth);

        env.dht_err_streak = 0U;
        env.dht_valid = true;
        env.last_t_dc = (int16_t)dc;
        env.last_rh_pct = d.rh_pct;
        env_evaluate(now_ms, dc, (int32_t)d.rh_pct);
    }
    else
    {
        env_dht_failed();
    }

    if (bs == DEV_OK)
    {
        env.baro_valid = true;
        env.last_p_pa = p_pa;
    }
    else
    {
        env.bmp_errors++;   /* BMP180 подій і тривог не створює */
    }
    return true;
}

bool SmartLock_EnvLastDht(int16_t *t_dc, uint8_t *rh_pct)
{
    if (t_dc != NULL)
    {
        *t_dc = env.last_t_dc;
    }
    if (rh_pct != NULL)
    {
        *rh_pct = env.last_rh_pct;
    }
    return env.dht_valid;
}

bool SmartLock_EnvLastPressure(int32_t *p_pa)
{
    if (p_pa != NULL)
    {
        *p_pa = env.last_p_pa;
    }
    return env.baro_valid;
}

uint32_t SmartLock_EnvDhtErrors(void)      { return env.dht_errors; }
uint32_t SmartLock_EnvDhtErrStreak(void)   { return env.dht_err_streak; }
uint32_t SmartLock_EnvBmpErrors(void)      { return env.bmp_errors; }
uint32_t SmartLock_EnvWarningsPosted(void) { return env.warnings_posted; }
uint32_t SmartLock_EnvPostFailed(void)     { return env.post_failed; }
uint32_t SmartLock_EnvActiveMask(void)     { return env.reported_mask; }
