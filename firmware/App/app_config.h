/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   ЛР3: параметри Super Loop, вимірювань і камери.
  *
  *          Префікс TBD_ = значення НЕ затверджене PRD (docs/PRD.md) і не
  *          погоджене командою. Це тимчасове значення для bring-up і вимірювань;
  *          у звіт воно не йде як вимога. Перелік відкритих рішень — docs/tasks.md §4.
  ******************************************************************************
  */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Ідентифікатор збірки для першого рядка UART-логу (методичка §5.2, §6).
 * firmware.mk передає -DGIT_HASH=\"<git rev-parse --short HEAD>[-dirty]\". */
#ifndef GIT_HASH
#define GIT_HASH "nogit"
#endif

/* ---- періоди runtime-задач, мкс (0 = перевірка в кожному проході) ----------- */
#define PERIOD_FSM_US              0U        /* FR-03: 15 с рахує сам FSM за часом */
#define TBD_PERIOD_BUTTONS_US      10000U    /* опитування 4 кнопок / B1 */
#define TBD_PERIOD_SECURITY_US     20000U    /* MPU-6050, геркон */
#define TBD_PERIOD_ENV_US          2000000U  /* DHT11 + BMP180; DHT11 не частіше 1 раз/с (datasheet) */
#define PERIOD_CAMERA_US           0U        /* подієва: черга запитів */
#define PERIOD_STORAGE_US          0U        /* подієва: черга записів журналу */
#define PERIOD_UART_US             0U

/* ---- вимірювання ---------------------------------------------------------------- */
#define PERF_CPU_WINDOW_MS         1000U     /* вікно CPU Load (методичка §5.1, приклад 4) */
#define EVT_RING_LEN               64U       /* EVT-записи в RAM до виводу */
#define EVT_FLUSH_PER_PASS         2U        /* рядків EVT за один прохід uart-задачі */
#define UART_RX_PER_PASS           32U       /* символів за один прохід (решта — наступного разу) */
#define CMD_LINE_MAX               48U       /* довжина рядкової команди (STATS, CAM ...) */

/* Черга подій до FSM (event_queue.h). Має бути більшою за EVQ max_depth у S1–S3 */
#define TBD_EVQ_LEN                8U

/* ---- камера OV7670 (docs/camera.md) ------------------------------------------------ */
#define CAMERA_WIDTH               160U      /* QQVGA */
#define CAMERA_HEIGHT              120U
#define CAMERA_BPP                 2U        /* RGB565 */
#define CAMERA_FRAME_BYTES         (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_BPP)   /* 38 400 */
#define CAMERA_QUEUE_LEN           4U        /* запитів MOTION/TAMPER в черзі */

/* CLKRC[5:0]: f_int = 24 МГц / (CLKRC + 1). 0x0F → 1,5 МГц, кадр VGA-таймінгу
 * 784·510·2 / 1,5 МГц ≈ 0,53 с. Виміряти в H14; швидше — ризик не встигнути за PCLK. */
#define TBD_CAMERA_CLKRC           0x0FU
#define TBD_CAMERA_VSYNC_TIMEOUT_US 1500000U /* ≈ 3 кадри при CLKRC = 0x0F */
#define TBD_CAMERA_HREF_TIMEOUT_US  100000U
/* Мінімальна ширина імпульсу VSYNC. За datasheet імпульс = 3 лінії (3 × 784 t_P);
 * при CLKRC = 0x0F і RGB565 це ≈ 3,1 мс. 1 мс — третина очікуваного, з запасом
 * проти завад (лог 15 ЛР3). Перевіряється в H14 записом PulseView. */
#define TBD_CAMERA_VSYNC_MIN_US     1000U
#define TBD_CAMERA_PCLK_SPIN_MAX    20000U
#define CAMERA_IRQ_LOCK_PER_LINE   1U

/* Експорт кадру на ПК текстовими рядками (docs/camera.md §4) */
#define FRAME_EXPORT_CHUNK         32U       /* байт кадру в одному рядку FRM */

#endif /* APP_CONFIG_H */
