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
#include "evt.h"
#include "perf.h"
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
    uint64_t press_us;   /* перший сирий фронт натискання (мкс МК), 0 — немає */
} btn_t;

static struct
{
    bool     started;
    btn_t    btn[BTN_COUNT];
    uint8_t  entry[TBD_CODE_LEN];   /* введені символи; стирається після оцінки */
    uint8_t  count;
    uint32_t last_ms;               /* час останнього прийнятого натискання коду */
    uint32_t dropped;
    /* issue #51: коли опитування востаннє прийняло натискання кнопки коду — щоб
     * завершене натискання з EXTI-черги не порахувати вдруге */
    bool     poll_seen[BTN_CODE_COUNT];
    uint32_t poll_cyc[BTN_CODE_COUNT];   /* такт DWT (Platform_Cycles) прийняття */
    uint32_t late;                  /* натискань, які додала EXTI-черга */
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
        s_b.btn[i].press_us = 0U;
    }
    for (i = 0U; i < TBD_CODE_LEN; i++)
    {
        s_b.entry[i] = 0U;
    }
    s_b.count = 0U;
    s_b.last_ms = 0U;
    s_b.dropped = 0U;
    for (i = 0U; i < BTN_CODE_COUNT; i++)
    {
        s_b.poll_seen[i] = false;
        s_b.poll_cyc[i] = 0U;
    }
    s_b.late = 0U;
}

uint32_t SmartLock_ButtonsLate(void)
{
    return s_b.late;
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

/* I04: пара code_key_press → code_key_handled (docs/metrics/metrics-spec.md §3.3).
 * Кнопки коду опитуються (не EXTI), тож «натискання» — перший сирий фронт, який
 * побачило опитування; затримка до handled = антидребезг + фаза опитування. */
static void note_raw_press(btn_t *b)
{
    if (b->cand && !b->stable && (b->press_us == 0U))
    {
        b->press_us = Perf_TimeUs64();
        if (b->press_us == 0U)
        {
            b->press_us = 1U;   /* 0 зарезервовано як «немає» */
        }
    }
    else if (!b->cand && !b->stable && (b->press_us != 0U))
    {
        b->press_us = 0U;       /* короткий імпульс, що не став натисканням */
    }
}

static void log_code_key(btn_t *b)
{
    if (b->press_us != 0U)
    {
        Evt_LogAt(b->press_us, "code_key_press", EVT_NOARG);
        Evt_Log("code_key_handled");
    }
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

/* ---- issue #51: натискання, яких не побачило опитування ---------------------------
 * Під час зйомки й запису кадру цикл стоїть до ~2,9 с, і натискання, що почалося й
 * закінчилося за цей час, опитування не бачить. Платформа ловить кнопки коду через
 * EXTI і віддає завершені натискання (не коротші за TBD_BTN_DEBOUNCE_MS) з часом
 * натискання і відпускання (Platform_TakeKey). Тут кожне таке натискання:
 *   - відкидається як повтор, якщо опитування прийняло натискання тієї ж кнопки
 *     між його натисканням і відпусканням (звичайний випадок — цикл не стояв);
 *     EVT code_key_dup;
 *   - відкидається разом із сусіднім, якщо дві різні кнопки натиснуто майже
 *     одночасно (ближче за TBD_BTN_DEBOUNCE_MS) — акорд, як і в опитуванні, де
 *     акорд — це два натискання, прийняті в одному проході; EVT code_key_chord.
 *     Натискання «внахлест» (наступна кнопка до відпускання попередньої) — не
 *     акорд: опитування теж приймає їх по черзі;
 *   - інакше стає символом коду з часом t_down + TBD_BTN_DEBOUNCE_MS (так само,
 *     як прийняло б опитування). Тайм-аут між символами рахується від цих часів,
 *     а не від моменту, коли цикл звільнився.
 * Повтор і акорд перевіряються тактами DWT: під час читання кадру SysTick
 * замаскований, мілісекунди майже стоять, і різні натискання мали б однакові мітки. */

/* a - b у мілісекундах зі знаком (різниця uint32 стійка до переповнення) */
static int32_t ms_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

static bool polled_within(const platform_key_t *k)
{
    uint8_t i = k->btn;

    return s_b.poll_seen[i] && ((int32_t)(s_b.poll_cyc[i] - k->t_down_cyc) >= 0) &&
           ((int32_t)(k->t_up_cyc - s_b.poll_cyc[i]) >= 0);
}

static bool keys_chord(const platform_key_t *a, const platform_key_t *b)
{
    uint32_t d = (a->t_down_cyc > b->t_down_cyc) ? (a->t_down_cyc - b->t_down_cyc)
                                                 : (b->t_down_cyc - a->t_down_cyc);

    return (a->btn != b->btn) && (Platform_CyclesToUs(d) < TBD_BTN_DEBOUNCE_MS * 1000U);
}

static void add_late_symbol(const platform_key_t *k)
{
    uint32_t t_sym = k->t_down_ms + TBD_BTN_DEBOUNCE_MS;

    if ((s_b.count > 0U) && (ms_diff(t_sym, s_b.last_ms) >= (int32_t)TBD_CODE_TIMEOUT_MS))
    {
        clear_entry();   /* пауза перед цим натисканням довша за тайм-аут */
    }
    Evt_LogAt(Perf_CyclesStampToUs64(k->t_down_cyc), "code_key_press", EVT_NOARG);
    Evt_Log("code_key_handled");
    s_b.late++;
    add_symbol((uint8_t)(k->btn + 1U), t_sym);
}

/* Буфер — статичний, не на стеку: 16 записів = 256 Б, а резерв стека малий
 * (docs/superloop.md §5) */
static platform_key_t s_late[PLATFORM_KEY_QUEUE];

static bool take_late_keys(void)
{
    platform_key_t *k = s_late;
    unsigned       n = 0U;
    unsigned       j;
    bool           did = false;

    while ((n < PLATFORM_KEY_QUEUE) && (Platform_TakeKey(&k[n]) != 0))
    {
        if (k[n].btn >= BTN_CODE_COUNT)
        {
            continue;
        }
        if (polled_within(&k[n]))
        {
            Evt_LogArg("code_key_dup", (uint32_t)k[n].btn + 1U);
            continue;
        }
        n++;
    }
    for (j = 0U; j < n; j++)
    {
        if ((j + 1U < n) && keys_chord(&k[j], &k[j + 1U]))
        {
            Evt_LogArg("code_key_chord", (uint32_t)k[j].btn + 1U);
            j++;   /* акорд: обидва відкидаємо, введення не змінюється */
            continue;
        }
        add_late_symbol(&k[j]);
        did = true;
    }
    return did;
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
    Platform_KeySetMinPress(TBD_BTN_DEBOUNCE_MS);
    while (Platform_TakeKey(NULL) != 0)
    {
        /* натискання до старту модуля — не код */
    }
    s_b.started = true;
}

bool SmartLock_ButtonsPoll(uint32_t now_ms, bool b1_edge)
{
    uint32_t in = Platform_ReadInputs();
    bool     did = false;
    bool     code_press[BTN_CODE_COUNT] = { false, false, false, false };
    bool     b1_press = false;
    bool     pending_clear[BTN_CODE_COUNT] = { false, false, false, false };
    unsigned presses = 0U;
    uint8_t  i;

    if (!s_b.started)
    {
        start(in, now_ms);
        return false;
    }

    /* issue #51: спершу натискання, що завершились, поки цикл стояв, — вони
     * раніші за все, що опитування прийме зараз */
    did = take_late_keys();

    for (i = 0U; i < BTN_COUNT; i++)
    {
        bool changed;

        if ((i == BTN_IDX_B1) && b1_edge)
        {
            s_b.btn[i].since_ms = now_ms;   /* фронт: відлік стабільності заново */
        }
        changed = debounce(&s_b.btn[i], is_pressed(in, i), now_ms);
        if (i != BTN_IDX_B1)
        {
            note_raw_press(&s_b.btn[i]);
        }
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
                s_b.poll_seen[i] = true;   /* issue #51: це натискання з черги не брати */
                s_b.poll_cyc[i] = Platform_Cycles();
            }
            if (changed)
            {
                pending_clear[i] = true;   /* фронт використано (лог або акорд) */
            }
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
                log_code_key(&s_b.btn[i]);
                add_symbol((uint8_t)(i + 1U), now_ms);
            }
        }
    }

    for (i = 0U; i < BTN_CODE_COUNT; i++)
    {
        if (pending_clear[i])
        {
            s_b.btn[i].press_us = 0U;
        }
    }

    if (b1_press)
    {
        post(SL_EV_LOCK_REQUEST, now_ms);   /* контракт sl_fsm.h: мітка прийнятого B1 */
    }

    return did;
}
