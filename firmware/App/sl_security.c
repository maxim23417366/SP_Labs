/**
  ******************************************************************************
  * @file    sl_security.c
  * @brief   ЛР3 (K05): PIR, MPU-6050, геркон (див. sl_security.h).
  *
  *          Сильна SmartLock_SecurityPoll() викликається раз на
  *          TBD_PERIOD_SECURITY_US (20 мс). За один виклик: читання входів
  *          (Platform_ReadInputs), не більше ОДНІЄЇ I2C-транзакції MPU-6050
  *          (~1,7 мс, лог 13), до трьох подій у чергу. Очікувань і циклів із
  *          залежністю від зовнішнього світу немає; час лише з now_ms і лише як
  *          різниця (uint32_t), тож переповнення Platform_Millis() безпечне.
  *
  *          Метрика TAMPER: сума |Δ| по трьох осях між сусідніми зразками
  *          акселерометра (L1-норма різниці), переведена в мg. Чому так:
  *            - без sqrt і float: три віднімання, три модулі, одне множення і
  *              зсув (1 g = 16384 LSB, тож мg = LSB · 125 / 2048);
  *            - різниця з попереднім зразком — це фільтр верхніх частот: стале
  *              положення і повільний нахил (макет повертають руками, лог 11)
  *              дають малі кроки, а удар — різкий стрибок; не потрібні
  *              калібрування і нуль осей (у цього датчика X зсунутий на
  *              +63 мg, Z = 1,10 g, лог 08, тож поріг по відхиленню |a| від 1 g
  *              з малим запасом хибно спрацьовував би в спокої);
  *            - L2 ≤ L1 ≤ √3 · L2: для порога із запасом ~6 × шуму відмінність
  *              від евклідової норми несуттєва.
  *          Слабке місце: удар коротший за період опитування (20 мс) може не
  *          потрапити в зразок; тоді потрібне переривання MPU (картка I04).
  ******************************************************************************
  */
#include "sl_security.h"

#include <string.h>

#include "app.h"
#include "event_queue.h"
#include "platform.h"
#include "smartlock_hooks.h"

#define MG_NUM  125U   /* мg = LSB · 125 / 2048 (1000 мg = 16384 LSB) */
#define MG_SHIFT 11U

typedef struct
{
    /* геркон */
    bool            door_known;      /* початковий стан уже зафіксовано */
    bool            door_open;       /* підтверджений (стабільний) стан: true = відчинено */
    bool            raw_valid;
    bool            raw_open;        /* останній сирий рівень */
    uint32_t        raw_since_ms;    /* від якого моменту рівень не змінюється */
    sl_event_type_t door_pending;    /* DOOR_*, яку не вдалося поставити в чергу */
    uint32_t        door_retries;

    /* PIR */
    bool            pir_valid;
    bool            pir_prev;
    bool            motion_sent;
    uint32_t        motion_ms;

    /* MPU-6050 */
    bool            have_prev;
    int16_t         prev_ax, prev_ay, prev_az;
    uint32_t        base_cnt;        /* зразків, поглинутих базовою лінією (0 після старту / відновлення) */
    uint32_t        err_streak;      /* помилок читання поспіль (до TBD_MPU_ERR_LIMIT) */
    uint32_t        zero_streak;     /* нульових зразків поспіль (до TBD_MPU_ZERO_LIMIT) */
    uint32_t        errors;
    uint32_t        zero_samples;
    bool            mpu_tried;
    uint32_t        mpu_try_ms;
    bool            tamper_sent;
    uint32_t        tamper_ms;

    uint32_t        lost;
} sec_state_t;

/* Нульовий стан = стан після старту, тож окрема ініціалізація на платі не потрібна */
static sec_state_t s;

void SmartLock_SecurityInit(void)
{
    (void)memset(&s, 0, sizeof(s));
}

bool SlSecurity_MpuHealthy(void)
{
    return (app_devices.mpu_status == DEV_OK) && (s.err_streak < TBD_MPU_ERR_LIMIT) &&
           (s.zero_streak < TBD_MPU_ZERO_LIMIT);
}

uint32_t SlSecurity_MpuErrors(void)
{
    return s.errors;
}

uint32_t SlSecurity_MpuZeroSamples(void)
{
    return s.zero_samples;
}

uint32_t SlSecurity_EventsLost(void)
{
    return s.lost;
}

uint32_t SlSecurity_DoorRetries(void)
{
    return s.door_retries;
}

/* Минуло не менше interval мс від last (різниця беззнакова — переповнення безпечне) */
static bool elapsed(uint32_t now_ms, uint32_t last_ms, uint32_t interval_ms)
{
    return (uint32_t)(now_ms - last_ms) >= interval_ms;
}

/* ---- геркон -------------------------------------------------------------------- */

static bool door_poll(uint32_t now_ms, uint32_t inputs)
{
    bool cur_open = ((inputs & PLATFORM_IN_DOOR) != 0U);   /* 1 = відчинено */
    sl_event_type_t ev;

    if (!s.raw_valid || (s.raw_open != cur_open))
    {
        s.raw_valid    = true;
        s.raw_open     = cur_open;
        s.raw_since_ms = now_ms;   /* будь-яка зміна рівня перезапускає антидребезг */
    }

    /* Раніше підтверджена, але недоставлена подія — першою і в тому ж порядку.
     * Нова зміна не підтверджується, поки її немає куди покласти. */
    if (s.door_pending != SL_EV_NONE)
    {
        if (!SmartLock_PostEvent(s.door_pending, 0U))
        {
            s.door_retries++;
            return false;
        }
        s.door_pending = SL_EV_NONE;
        return true;
    }

    if (!elapsed(now_ms, s.raw_since_ms, TBD_DOOR_DEBOUNCE_MS))
    {
        return false;
    }
    if (s.door_known && (s.door_open == s.raw_open))
    {
        return false;   /* стан не змінився */
    }

    /* Стабільна зміна, або перший стабільний стан після старту */
    s.door_known = true;
    s.door_open  = s.raw_open;
    ev = s.door_open ? SL_EV_DOOR_OPEN : SL_EV_DOOR_CLOSED;
    if (!SmartLock_PostEvent(ev, 0U))
    {
        s.door_pending = ev;   /* DOOR_* втрачати не можна: повторимо в наступному виклику */
        s.door_retries++;
        return false;
    }
    return true;
}

/* ---- PIR ----------------------------------------------------------------------- */

static bool pir_poll(uint32_t now_ms, uint32_t inputs, bool pir_edge)
{
    bool level = ((inputs & PLATFORM_IN_PIR) != 0U);
    /* Перший зразок після старту — лише базовий рівень (HC-SR501 після подачі
     * живлення тримає вихід високим під час прогріву). pir_edge з EXTI — теж
     * фронт, але подвійну подію з рівнем гасить інтервал нижче. */
    bool rise  = pir_edge || (s.pir_valid && level && !s.pir_prev);

    s.pir_valid = true;
    s.pir_prev  = level;

    if (!rise)
    {
        return false;
    }
    if (s.motion_sent && !elapsed(now_ms, s.motion_ms, TBD_PIR_MIN_INTERVAL_MS))
    {
        return false;   /* імпульс тієї ж серії HC-SR501 */
    }
    if (!SmartLock_PostEvent(SL_EV_MOTION, 0U))
    {
        s.lost++;       /* мітку часу не оновлюємо: наступний імпульс спробує знову */
        return false;
    }
    s.motion_sent = true;
    s.motion_ms   = now_ms;
    return true;
}

/* ---- MPU-6050 ------------------------------------------------------------------ */

static void mpu_reset_baseline(void)
{
    s.have_prev = false;
    s.base_cnt  = 0U;   /* нульовий стан після старту — теж «база ще не встановлена» */
}

static bool raw_all_zero(const mpu6050_raw_t *r)
{
    return (r->ax == 0) && (r->ay == 0) && (r->az == 0) && (r->gx == 0) && (r->gy == 0) &&
           (r->gz == 0);
}

static uint32_t abs_diff16(int16_t a, int16_t b)
{
    int32_t d = (int32_t)a - (int32_t)b;

    return (uint32_t)((d < 0) ? -d : d);
}

/* Метрика TAMPER, мg (пояснення — у шапці файлу). Максимум 3 · 65535 · 125 < 2^31. */
static uint32_t tamper_metric_mg(const mpu6050_raw_t *r)
{
    uint32_t sum = abs_diff16(r->ax, s.prev_ax) + abs_diff16(r->ay, s.prev_ay) +
                   abs_diff16(r->az, s.prev_az);

    return (sum * MG_NUM) >> MG_SHIFT;
}

/* Повертає true, якщо виконано транзакцію I2C */
static bool mpu_poll(uint32_t now_ms)
{
    mpu6050_raw_t raw;
    uint32_t      mg;

    if (app_devices.mpu_status != DEV_OK)
    {
        mpu_reset_baseline();   /* після повторної ініціалізації база збирається заново */
        s.err_streak  = 0U;
        s.zero_streak = 0U;
        return false;
    }
    if ((s.err_streak >= TBD_MPU_ERR_LIMIT) && s.mpu_tried &&
        !elapsed(now_ms, s.mpu_try_ms, TBD_MPU_RETRY_MS))
    {
        return false;   /* шина не відповідає: не витрачаємо ~26 мс щодвадцять мс */
    }
    s.mpu_tried  = true;
    s.mpu_try_ms = now_ms;

    if (mpu6050_read_raw(&app_devices.mpu, &raw) != DEV_OK)
    {
        s.errors++;
        if (s.err_streak < TBD_MPU_ERR_LIMIT)
        {
            s.err_streak++;
            if (s.err_streak == TBD_MPU_ERR_LIMIT)
            {
                mpu_reset_baseline();   /* після відновлення положення могло змінитись */
            }
        }
        return true;
    }
    s.err_streak = 0U;

    if (raw_all_zero(&raw))
    {
        /* Модуль без живлення / у режимі сну відповідає нулями без помилки (лог 08, 15).
         * Такий зразок не порівнюємо: «0 → 1 g» не є ударом. */
        s.zero_samples++;
        if (s.zero_streak < TBD_MPU_ZERO_LIMIT)
        {
            s.zero_streak++;
            if (s.zero_streak == TBD_MPU_ZERO_LIMIT)
            {
                mpu_reset_baseline();
            }
        }
        return true;
    }
    s.zero_streak = 0U;

    if (!s.have_prev || (s.base_cnt < TBD_MPU_BASELINE_SAMPLES))
    {
        /* Встановлення базової лінії: події не формуються */
        if (s.base_cnt < TBD_MPU_BASELINE_SAMPLES)
        {
            s.base_cnt++;
        }
    }
    else
    {
        mg = tamper_metric_mg(&raw);
        if ((mg > TBD_TAMPER_THRESHOLD_MG) &&
            (!s.tamper_sent || elapsed(now_ms, s.tamper_ms, TBD_TAMPER_MIN_INTERVAL_MS)))
        {
            if (SmartLock_PostEvent(SL_EV_TAMPER, mg - TBD_TAMPER_THRESHOLD_MG))
            {
                s.tamper_sent = true;
                s.tamper_ms   = now_ms;
            }
            else
            {
                s.lost++;
            }
        }
    }
    s.prev_ax   = raw.ax;
    s.prev_ay   = raw.ay;
    s.prev_az   = raw.az;
    s.have_prev = true;
    return true;
}

/* ---- хук ----------------------------------------------------------------------- */

bool SmartLock_SecurityPoll(uint32_t now_ms, bool pir_edge, bool mpu_edge)
{
    uint32_t inputs = Platform_ReadInputs();
    bool     did;

    (void)mpu_edge;   /* MPU читається в кожному виклику; INT у датчику не ввімкнено */

    /* Порядок подій у черзі: двері, рух, удар. Відмова MPU геркон і PIR не торкається. */
    did  = door_poll(now_ms, inputs);
    did |= pir_poll(now_ms, inputs, pir_edge);
    did |= mpu_poll(now_ms);
    return did;
}
