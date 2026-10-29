
#include "self_test.h"

#include <stdio.h>

#define SELF_TEST_LINE_SIZE 192U

/* Налаштований спосіб виведення повідомлень */
static self_test_output_fn s_output;
static void *s_output_context;

/* Перетворення статусу на текст */
static const char *state_text(self_test_state_t state)
{
    switch (state)
    {
        case SELF_TEST_OK:   return "OK";
        case SELF_TEST_WARN: return "WARN";
        case SELF_TEST_FAIL: return "FAIL";
        case SELF_TEST_SKIP: return "SKIP";
        default:             return NULL;
    }
}

/* Перетворення помилки драйвера на текст */
static const char *error_text(dev_status_t error)
{
    switch (error)
    {
        case DEV_ERR_NACK:    return "nack";
        case DEV_ERR_TIMEOUT: return "timeout";
        case DEV_ERR_BUS:     return "bus";
        case DEV_ERR_ID:      return "bad_id";
        case DEV_ERR_STATE:   return "state";
        case DEV_ERR_PARAM:   return "param";
        default:              return "unknown";
    }
}

/* Встановлює функцію для виведення повідомлень */
void SelfTest_SetOutput(
    self_test_output_fn output,
    void *context
)
{
    s_output = output;
    s_output_context = context;
}

/* Формує один рядок про результат перевірки */
bool SelfTest_FormatLine(
    char *buffer,
    size_t size,
    const self_test_device_t *device
)
{
    int n;
    const char *state;
    const char *extra;

    if (buffer == NULL || size == 0U ||
        device == NULL || device->name == NULL ||
        device->name[0] == '\0')
    {
        return false;
    }

    state = state_text(device->state);

    if (state == NULL)
    {
        return false;
    }

    extra = (device->fields != NULL &&
             device->fields[0] != '\0')
                ? device->fields : NULL;

    if (device->state == SELF_TEST_FAIL)
    {
        n = snprintf(
            buffer, size,
            "ST,%s,FAIL,err=%s%s%s",
            device->name,
            error_text(device->error),
            extra != NULL ? "," : "",
            extra != NULL ? extra : ""
        );
    }
    else
    {
        n = snprintf(
            buffer, size,
            "ST,%s,%s%s%s",
            device->name,
            state,
            extra != NULL ? "," : "",
            extra != NULL ? extra : ""
        );
    }

    return n >= 0 && (size_t)n < size;
}

/* Формує підсумок по всіх пристроях */
bool SelfTest_FormatSummary(
    char *buffer,
    size_t size,
    const self_test_summary_t *summary
)
{
    const char *state;
    int n;

    if (buffer == NULL || size == 0U ||
        summary == NULL)
    {
        return false;
    }

    if (summary->fail > 0U ||
        (summary->ok + summary->warn +
         summary->fail + summary->skip) == 0U)
    {
        state = "FAIL";
    }
    else if (summary->warn > 0U ||
             summary->skip > 0U)
    {
        state = "WARN";
    }
    else
    {
        state = "OK";
    }

    n = snprintf(
    buffer, size,
    "ST,SUMMARY,%s,ok=%lu,warn=%lu,fail=%lu,skip=%lu",
    state,
    (unsigned long)summary->ok,
    (unsigned long)summary->warn,
    (unsigned long)summary->fail,
    (unsigned long)summary->skip
);

    return n >= 0 && (size_t)n < size;
}

/* Формує повний звіт перевірки пристроїв */
bool SelfTest_Report(
    const self_test_device_t *devices,
    size_t count
)
{
    self_test_summary_t summary = {0U, 0U, 0U, 0U};
    char line[SELF_TEST_LINE_SIZE];
    size_t i;

    if (s_output == NULL ||
        devices == NULL || count == 0U)
    {
        return false;
    }

    s_output("ST,START", s_output_context);

    for (i = 0U; i < count; ++i)
    {
        if (!SelfTest_FormatLine(
                line, sizeof(line), &devices[i]))
        {
            s_output(
                "ST,REPORT,FAIL,err=format",
                s_output_context
            );
            return false;
        }

        switch (devices[i].state)
        {
            case SELF_TEST_OK:
                ++summary.ok;
                break;

            case SELF_TEST_WARN:
                ++summary.warn;
                break;

            case SELF_TEST_FAIL:
                ++summary.fail;
                break;

            case SELF_TEST_SKIP:
                ++summary.skip;
                break;

            default:
                return false;
        }

        s_output(line, s_output_context);
    }

    if (!SelfTest_FormatSummary(
            line, sizeof(line), &summary))
    {
        s_output(
            "ST,REPORT,FAIL,err=format",
            s_output_context
        );
        return false;
    }

    s_output(line, s_output_context);

    return summary.fail == 0U &&
           summary.warn == 0U &&
           summary.skip == 0U;
}

/* Передає один готовий рядок у налаштований вивід */
bool SelfTest_Emit(const char *line)
{
    if (s_output == NULL || line == NULL)
    {
        return false;
    }

    s_output(line, s_output_context);

    return true;
}

/* Повертає текстовий код помилки драйвера */
const char *SelfTest_ErrorText(dev_status_t error)
{
    return error_text(error);
}
