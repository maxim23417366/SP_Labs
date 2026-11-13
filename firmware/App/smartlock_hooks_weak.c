/**
  ******************************************************************************
  * @file    smartlock_hooks_weak.c
  * @brief   Слабкі реалізації точок підключення (smartlock_hooks.h).
  *
  *          Вони не реалізують логіку SmartLock (це картки K02–K08, H09/H10), а
  *          лише створюють реальне навантаження на шини, щоб S1–S3 вимірювали
  *          справжній цикл ще до злиття гілок: опитування входів, читання
  *          MPU-6050, DHT11 і BMP180 з виводом у UART (FR-14/FR-15: тиск без
  *          окремої температури BMP180). Коли власник картки визначає функцію з
  *          тим самим ім'ям, лінкер бере його версію, а ця відкидається.
  ******************************************************************************
  */
#include "smartlock_hooks.h"

#include <stdio.h>

#include "app.h"
#include "dht11.h"
#include "event_queue.h"
#include "frame_store.h"
#include "platform.h"

#define DHT_THRESHOLD_US 48U   /* як у stand_test.c: HIGH > 48 мкс = 1 */

/* Без FSM (K02) події лише вибираються з черги, щоб вона не переповнювалась;
 * момент вибирання видно в EVT name=fsm_event, очікування — STAT evq_wait. */
__attribute__((weak)) bool SmartLock_FsmStep(uint32_t now_ms)
{
    sl_event_t ev;
    bool did = false;

    (void)now_ms;
    while (SmartLock_TakeEvent(&ev))
    {
        did = true;
    }
    return did;
}

__attribute__((weak)) bool SmartLock_ButtonsPoll(uint32_t now_ms, bool b1_edge)
{
    (void)now_ms;
    (void)Platform_ReadInputs();
    if (b1_edge)
    {
        (void)SmartLock_PostEvent(SL_EV_LOCK_REQUEST, 0U);   /* антидребезг — K03 */
    }
    return true;
}

__attribute__((weak)) bool SmartLock_SecurityPoll(uint32_t now_ms, bool pir_edge, bool mpu_edge)
{
    mpu6050_raw_t raw;

    (void)now_ms;
    (void)mpu_edge;   /* поріг TAMPER (FR-06) не затверджено — K05 */
    (void)Platform_ReadInputs();   /* геркон */
    if (pir_edge)
    {
        (void)SmartLock_PostEvent(SL_EV_MOTION, 0U);   /* FSM вирішить: лише в LOCKED */
    }
    if (app_devices.mpu_status == DEV_OK)
    {
        (void)mpu6050_read_raw(&app_devices.mpu, &raw);   /* 14 байт по I2C ≈ 1,7 мс при 100 кГц */
    }
    return true;
}

__attribute__((weak)) bool SmartLock_EnvPoll(uint32_t now_ms)
{
    uint16_t     us[40];
    uint8_t      frame[DHT11_FRAME_LEN];
    dht11_data_t d = { 0U, 0U, 0, 0U };
    dev_status_t ds;
    dev_status_t bs = DEV_ERR_STATE;
    int32_t      t_dc = 0;
    int32_t      p_pa = 0;

    (void)now_ms;
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
        printf(",p_pa=%ld", (long)p_pa);
    }
    printf("\r\n");
    return true;
}

__attribute__((weak)) bool SmartLock_StoragePoll(uint32_t now_ms)
{
    (void)now_ms;
    return false;
}

__attribute__((weak)) void SmartLock_OnCameraResult(const camera_result_t *r)
{
    printf("CAM,result,id=%lu,seq=%lu,status=%s,cap=%s,bytes=%lu,lines=%u,short=%u,vglitch=%u,"
           "crc32=%08lX,capture_us=%lu,save_us=%lu,slot=%lu,store_rc=%d\r\n",
           (unsigned long)r->event_id, (unsigned long)r->seq, Camera_StatusStr(r->status),
           Camera_CapStatusStr(r->cap_status), (unsigned long)r->bytes, (unsigned)r->lines,
           (unsigned)r->short_lines, (unsigned)r->vsync_glitches, (unsigned long)r->crc32, (unsigned long)r->capture_us,
           (unsigned long)r->save_us, (unsigned long)r->slot, r->store_status);
}

__attribute__((weak)) const frame_store_ops_t *FrameStore_GetOps(void)
{
    return NULL;
}
