/**
  ******************************************************************************
  * @file    sl_camera_link.h
  * @brief   ЛР3 (K02 → K08/I10): зв'язок охоронної події з фото (FR-07, FR-08).
  *
  *          FSM у LOCKED на MOTION / TAMPER записує подію в журнал і запитує
  *          ОДИН кадр: SmartLock_CameraLinkStart(id, now). Далі шлях такий:
  *
  *            FSM ── CameraLinkStart(id) ──► SmartLock_CameraRequest(id)  [камера, I09]
  *            камера ── після запису кадру ──► SmartLock_OnCameraResult(id, frame, st)
  *            журнал ◄── PHOTO {ref = id, arg = ID кадру}  або  PHOTO_ERR {ref = id, arg = статус}
  *
  *          Критерій готовності K08: кожна MOTION / TAMPER, для якої запитано
  *          кадр, завершується РІВНО ОДНИМ записом — PHOTO або PHOTO_ERR з
  *          ref = ID події. Для цього модуль веде короткий список очікуваних
  *          результатів (TBD_CAMLINK_PENDING_MAX):
  *            - камера не прийняла запит (зайнята попереднім кадром, чи її немає)
  *              → PHOTO_ERR, статус SL_CAM_ERR_REJECTED, одразу; повторів немає;
  *            - результат не прийшов за TBD_CAM_RESULT_TIMEOUT_MS → PHOTO_ERR,
  *              SL_CAM_ERR_TIMEOUT; пізній результат після цього ігнорується
  *              (лічильник unknown), другого запису немає;
  *            - результат для ID, якого ніхто не чекає (0, дубль, чужий) —
  *              ігнорується, лічильник unknown;
  *            - журнал на мить не прийняв PHOTO / PHOTO_ERR — модуль повторює
  *              спробу в наступних проходах, запис не губиться;
  *            - подія не потрапила в журнал (ID = 0, FIFO повна) — кадр НЕ
  *              запитується: прив'язати його ні до чого (skipped_no_id).
  *
  *          Модуль камери (I07–I09, Непорожній) ще не існує. У docs/tasks.md
  *          запит названо Camera_Request(id), результат — SmartLock_OnCameraResult.
  *          При інтеграції (I10) камера визначає СИЛЬНУ SmartLock_CameraRequest()
  *          (обгортка над Camera_Request); слабка заглушка нижче відкидається
  *          лінкером — без правок FSM. SmartLock_OnCameraResult() і решта цього
  *          модуля НЕ слабкі: їх викликає камера, замінювати їх не треба.
  *
  *          Усе — лише з головного циклу, без очікувань. Документ для власників
  *          блокерів: docs/lab_3/journal-camera-integration.md.
  ******************************************************************************
  */
#ifndef SL_CAMERA_LINK_H
#define SL_CAMERA_LINK_H

#include <stdbool.h>
#include <stdint.h>

/* TBD: PRD не задає. Скільки запитів чекають результату одночасно. Камера знімає
 * один кадр за раз (tasks.md §4), але MOTION + TAMPER приходять в одному
 * проході, а результат — через 1–2 проходи; 4 — запас. */
#define TBD_CAMLINK_PENDING_MAX    4U

/* TBD: PRD не задає. Скільки чекати результату після запиту. Розрахунок
 * tasks.md §5: захоплення 0,53–1,07 с + запис кадру 0,6 с типово / 4,6 с у
 * найгіршому (datasheet) + проходи інших задач; 10 с — з запасом. Уточнити за
 * вимірами H14 / I09. */
#define TBD_CAM_RESULT_TIMEOUT_MS  10000U

/* Статус зйомки: SL_LOG_PHOTO_ERR.arg. OK — лише в SmartLock_OnCameraResult().
 * Значення — частина формату журналу: нові додавати в кінець. */
typedef enum
{
    SL_CAM_OK = 0,
    SL_CAM_ERR_REJECTED,   /* камера не прийняла запит: зайнята або її немає */
    SL_CAM_ERR_CAPTURE,    /* камера не отримала кадр (SCCB, VSYNC, тайм-аут захоплення) */
    SL_CAM_ERR_STORE,      /* кадр отримано, але не збережено у Flash (H10) */
    SL_CAM_ERR_TIMEOUT,    /* результат не прийшов за TBD_CAM_RESULT_TIMEOUT_MS */
    SL_CAM_STATUS_COUNT
} sl_cam_status_t;

typedef struct
{
    uint32_t requested;         /* запитів до камери (SmartLock_CameraRequest викликано) */
    uint32_t rejected;          /* камера не прийняла (+ список повний) */
    uint32_t ok;                /* PHOTO записано */
    uint32_t failed;            /* PHOTO_ERR від камери (CAPTURE / STORE) */
    uint32_t timeouts;          /* PHOTO_ERR за тайм-аутом */
    uint32_t unknown;           /* результат для ID, якого ніхто не чекає (в т. ч. після
                                 * тайм-ауту: кадр тоді, ймовірно, є у Flash без посилання) */
    uint32_t skipped_no_id;     /* подію не записано в журнал — кадр не запитано */
} sl_camlink_stats_t;

/* ---- API камери (I09) -------------------------------------------------------- */

/* Запит ОДНОГО кадру для події event_id (ID запису MOTION / TAMPER у журналі).
 * true — запит прийнято, результат прийде в SmartLock_OnCameraResult();
 * false — камери немає / зайнята попереднім кадром. Не чекає: лише ставить запит
 * у чергу камери, захоплення — у задачі camera. Слабка заглушка: false (керується
 * з тестів, див. нижче); сильну визначає модуль камери. */
bool SmartLock_CameraRequest(uint32_t event_id);

/* Камера повідомляє про завершення (успішне чи ні) запиту event_id. Викликати
 * РІВНО ОДИН РАЗ на кожен прийнятий запит (true з SmartLock_CameraRequest), з
 * головного циклу, коли відомий результат запису кадру у Flash:
 *   status = SL_CAM_OK            — frame_id = ID кадру (FrameStore), PHOTO
 *   status = SL_CAM_ERR_CAPTURE / SL_CAM_ERR_STORE — frame_id ігнорується, PHOTO_ERR
 * Невідомий status трактується як SL_CAM_ERR_CAPTURE. Допустимо викликати і
 * синхронно з SmartLock_CameraRequest(). */
void SmartLock_OnCameraResult(uint32_t event_id, uint32_t frame_id, sl_cam_status_t status);

/* ---- для FSM ---------------------------------------------------------------- */

/* Скидає список очікуваних запитів і лічильники. Викликає SmartLock_FsmInit(). */
void SmartLock_CameraLinkInit(void);

/* event_id — ID щойно записаної події (SL_LOG_ID_NONE = запис втрачено). Повертає
 * true, якщо камера прийняла запит. При false запис PHOTO_ERR вже поставлено
 * (або буде поставлено в наступному проході). */
bool SmartLock_CameraLinkStart(uint32_t event_id, uint32_t now_ms);

/* Кожен прохід (з SmartLock_FsmStep): тайм-аути і повтор запису в журнал.
 * true — щось змінилось. */
bool SmartLock_CameraLinkPoll(uint32_t now_ms);

/* ---- діагностика і тести ----------------------------------------------------- */
unsigned                   SmartLock_CameraLinkPending(void);   /* запитів без запису в журнал */
const sl_camlink_stats_t  *SmartLock_CameraLinkStats(void);
const char                *SmartLock_CameraStatusName(sl_cam_status_t status);

/* Керування слабкою заглушкою (лише поки камери немає; сильна
 * SmartLock_CameraRequest() їх не використовує). За замовчуванням accept = false. */
void     SmartLock_CameraStubSetAccept(bool accept);
unsigned SmartLock_CameraStubCalls(void);
uint32_t SmartLock_CameraStubLastId(void);
void     SmartLock_CameraStubReset(void);

#endif /* SL_CAMERA_LINK_H */
