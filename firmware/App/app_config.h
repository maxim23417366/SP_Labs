/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   ЛР3 (I02): параметри Super Loop.
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
#define PERIOD_STORAGE_US          0U        /* подієва: черга записів журналу */
#define PERIOD_UART_US             0U
/* I06: перевірка максимуму стека / heap; рядок MEM друкується лише при зростанні */
#define TBD_PERIOD_MEM_US          1000000U

/* ---- вимірювання ---------------------------------------------------------------- */
#define PERF_CPU_WINDOW_MS         1000U     /* вікно CPU Load (методичка §5.1, приклад 4) */
#define EVT_RING_LEN               64U       /* I04: EVT-записи в RAM до виводу */
#define EVT_FLUSH_PER_PASS         2U        /* I04: рядків EVT за один прохід uart-задачі */
#define UART_RX_PER_PASS           32U       /* символів за один прохід (решта — наступного разу) */
#define TBD_LOAD_PRINT             1U        /* I03: рядок LOAD pct= раз на секунду (0 — вимкнено) */
#define CMD_LINE_MAX               48U       /* I03: довжина рядкової команди (STATS, PING ...) */

/* ---- камера OV7670 (I07, docs/camera.md) ---------------------------------------- */
#define CAMERA_WIDTH               160U      /* QQVGA */
#define CAMERA_HEIGHT              120U
#define CAMERA_BPP                 2U        /* RGB565 */
#define CAMERA_FRAME_BYTES         (CAMERA_WIDTH * CAMERA_HEIGHT * CAMERA_BPP)   /* 38 400 */

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

/* ЛР3 (виправлення кадру, лог 23). OV7670 з нашою таблицею віддає молодший байт
 * пікселя першим; кадр у прошивці, W25Q64 і tools/frame_decode.py — старший байт
 * першим. 1 — переставляти байти після захоплення (ov7670_rgb565_swap_bytes). */
#define CAMERA_RGB565_SWAP         1U
/* AEC/AGC/AWB сходяться за кілька кадрів після ініціалізації (кадр ≈ 0,53 с). Камера
 * ініціалізується при старті (App_Init), тож до першої тривоги (≥ EXIT_DELAY 15 с)
 * експозиція вже встановилась. Стендова CAM TEST, якщо її набрали раніше, дочікує
 * решту цього часу (settle_ms). Шлях тривоги (camera.c) НЕ чекає — Super Loop не
 * блокується на секунди; кадр, знятий до сходження (камера щойно (пере)ініціалізована),
 * позначено в CAM,photo як aec=settling.
 * TBD: виміряти в H14 — скільки кадрів CAM TEST поспіль до стабільної яскравості. */
#define TBD_CAMERA_AEC_SETTLE_MS   5000U

/* I08: байтів кадру в одному рядку FRM (frame_export.h). 32 Б → 64 hex-символи, рядок
 * ≈ 82 символи ≈ 7 мс TX при 115200; кадр 38 400 Б = 1200 рядків ≈ 8,5 с. */
#define FRAME_EXPORT_CHUNK         32U

/* Черга подій до FSM (event_queue.h). Має бути більшою за EVQ max_depth у S1–S3 */
#define TBD_EVQ_LEN                8U

#endif /* APP_CONFIG_H */
