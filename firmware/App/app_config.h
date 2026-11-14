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
#define UART_RX_PER_PASS           32U       /* символів за один прохід (решта — наступного разу) */

/* Черга подій до FSM (event_queue.h). Має бути більшою за EVQ max_depth у S1–S3 */
#define TBD_EVQ_LEN                8U

#endif /* APP_CONFIG_H */
