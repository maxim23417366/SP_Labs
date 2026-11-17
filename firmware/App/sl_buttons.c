/**
  ******************************************************************************
  * @file    sl_buttons.c
  * @brief   ЛР3 (K03/K04): USER B1 і код з 4 кнопок (див. sl_buttons.h).
  *
  *          Сильна SmartLock_ButtonsPoll() не блокує: за один виклик читає входи
  *          (Platform_ReadInputs), проганяє антидребезг п'яти кнопок, за
  *          потреби скидає прострочене введення і ставить події в чергу. Час —
  *          лише з параметра now_ms (різниці uint32 стійкі до переповнення).
  ******************************************************************************
  */
#include "sl_buttons.h"

#include <stdbool.h>
#include <stdint.h>

#include "event_queue.h"
#include "platform.h"
#include "smartlock_hooks.h"

#define BTN_COUNT        5U   /* BTN1..BTN4 (індекси 0..3) + B1 (індекс 4) */
#define BTN_CODE_COUNT   4U
#define BTN_IDX_B1       4U

_Static_assert((TBD_CODE_LEN >= 1U) && (TBD_CODE_LEN <= 255U), "TBD_CODE_LEN: 1..255");

/* Код — у const-масиві (flash); розмір масиву перевіряється нижче */
static const uint8_t s_code[TBD_CODE_LEN] = TBD_CODE_VALUE;

_Static_assert(sizeof((const uint8_t[])TBD_CODE_VALUE) == TBD_CODE_LEN,
               "TBD_CODE_VALUE: кількість елементів має дорівнювати TBD_CODE_LEN");

/* Маски входів: BTN1..BTN4 дають символи 1..4, останній елемент — B1 */
static const uint32_t s_mask[BTN_COUNT] =
{
    PLATFORM_IN_BTN1, PLATFORM_IN_BTN2, PLATFORM_IN_BTN3, PLATFORM_IN_BTN4,
    PLATFORM_IN_LOCK
};

typedef struct
{
    bool     stable;     /* прийнятий (антидребезгом) стан: true = натиснуто */
    bool     cand;       /* останній побачений сирий стан */
    uint32_t since_ms;   /* коли cand побачили вперше (або фронт b1_edge) */
} btn_t;

static struct
{
    bool     started;
    btn_t    btn[BTN_COUNT];
    uint8_t  entry[TBD_CODE_LEN];   /* введені символи; стирається після оцінки */
    uint8_t  count;
    uint32_t last_ms;               /* час останнього прийнятого натискання коду */
    uint32_t dropped;
} s_b;

void SmartLock_ButtonsInit(void)
{
    uint8_t i;

    s_b.started = false;
    for (i = 0U; i < BTN_COUNT; i++)
    {
        s_b.btn[i].stable = false;
        s_b.btn[i].cand = false;
        s_b.btn[i].since_ms = 0U;
    }
    for (i = 0U; i < TBD_CODE_LEN; i++)
    {
        s_b.entry[i] = 0U;
    }
    s_b.count = 0U;
    s_b.last_ms = 0U;
    s_b.dropped = 0U;
}

uint32_t SmartLock_ButtonsDropped(void)
{
    return s_b.dropped;
}

unsigned SmartLock_ButtonsCodeProgress(void)
{
    return s_b.count;
}

/* Подія в чергу; повна черга — не губимо мовчки */
static void post(sl_event_type_t type, uint32_t arg)
{
    if (!SmartLock_PostEvent(type, arg))
    {
        s_b.dropped++;
    }
}

static bool is_pressed(uint32_t in, uint8_t i)
{
    return (in & s_mask[i]) == 0U;   /* pull-up: натиснуто = 0 */
}

static void clear_entry(void)
{
    uint8_t i;

    for (i = 0U; i < TBD_CODE_LEN; i++)
    {
        s_b.entry[i] = 0U;
    }
    s_b.count = 0U;
}

/* Один крок антидребезгу. true — стабільний стан змінився (дивись b->stable). */
static bool debounce(btn_t *b, bool pressed, uint32_t now_ms)
{
    if (pressed != b->cand)
    {
        b->cand = pressed;
        b->since_ms = now_ms;
        return false;
    }
    if ((b->cand != b->stable) && ((uint32_t)(now_ms - b->since_ms) >= TBD_BTN_DEBOUNCE_MS))
    {
        b->stable = b->cand;
        return true;
    }
    return false;
}

/* Оцінка повної послідовності: без раннього виходу, далі введення стирається */
static void evaluate(void)
{
    uint8_t diff = 0U;
    uint8_t i;

    for (i = 0U; i < TBD_CODE_LEN; i++)
    {
        diff |= (uint8_t)(s_b.entry[i] ^ s_code[i]);
    }
    post((diff == 0U) ? SL_EV_CODE_OK : SL_EV_CODE_WRONG, (uint32_t)TBD_CODE_LEN);
    clear_entry();
}

static void add_symbol(uint8_t symbol, uint32_t now_ms)
{
    s_b.entry[s_b.count] = symbol;
    s_b.count++;
    s_b.last_ms = now_ms;
    if (s_b.count >= TBD_CODE_LEN)
    {
        evaluate();
    }
}

/* Ледача ініціалізація: вихідні рівні вважаємо стабільними (затиснута кнопка
 * не дає події до відпускання) */
static void start(uint32_t in, uint32_t now_ms)
{
    uint8_t i;

    for (i = 0U; i < BTN_COUNT; i++)
    {
        bool p = is_pressed(in, i);

        s_b.btn[i].stable = p;
        s_b.btn[i].cand = p;
        s_b.btn[i].since_ms = now_ms;
    }
    s_b.started = true;
}

bool SmartLock_ButtonsPoll(uint32_t now_ms, bool b1_edge)
{
    uint32_t in = Platform_ReadInputs();
    bool     did = false;
    bool     code_press[BTN_CODE_COUNT] = { false, false, false, false };
    bool     b1_press = false;
    unsigned presses = 0U;
    uint8_t  i;

    if (!s_b.started)
    {
        start(in, now_ms);
        return false;
    }

    for (i = 0U; i < BTN_COUNT; i++)
    {
        bool changed;

        if ((i == BTN_IDX_B1) && b1_edge)
        {
            s_b.btn[i].since_ms = now_ms;   /* фронт: відлік стабільності заново */
        }
        changed = debounce(&s_b.btn[i], is_pressed(in, i), now_ms);
        if (changed)
        {
            did = true;
        }
        if (i == BTN_IDX_B1)
        {
            b1_press = changed && s_b.btn[i].stable;
        }
        else
        {
            code_press[i] = changed && s_b.btn[i].stable;
            if (code_press[i])
            {
                presses++;
            }
        }
    }

    /* пауза між натисканнями: незавершене введення скидається без події */
    if ((s_b.count > 0U) && ((uint32_t)(now_ms - s_b.last_ms) >= TBD_CODE_TIMEOUT_MS))
    {
        clear_entry();
        did = true;
    }

    if (presses == 1U)   /* два і більше одночасно — акорд, відкидаємо */
    {
        for (i = 0U; i < BTN_CODE_COUNT; i++)
        {
            if (code_press[i])
            {
                add_symbol((uint8_t)(i + 1U), now_ms);
            }
        }
    }

    if (b1_press)
    {
        post(SL_EV_LOCK_REQUEST, now_ms);   /* контракт sl_fsm.h: мітка прийнятого B1 */
    }

    return did;
}
