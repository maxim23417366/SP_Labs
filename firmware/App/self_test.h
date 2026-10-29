
#ifndef SELF_TEST_H
#define SELF_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include "dev_bus.h"

/* Результат перевірки пристрою */
typedef enum
{
    SELF_TEST_OK,
    SELF_TEST_WARN,
    SELF_TEST_FAIL,
    SELF_TEST_SKIP
} self_test_state_t;

/* Інформація про перевірений пристрій */
typedef struct
{
    const char *name;
    self_test_state_t state;
    dev_status_t error;
    const char *fields;
} self_test_device_t;

/* Кількість пристроїв за статусами */
typedef struct
{
    size_t ok;
    size_t warn;
    size_t fail;
    size_t skip;
} self_test_summary_t;

/* Функція для передачі готового рядка */
typedef void (*self_test_output_fn)(
    const char *line,
    void *context
);

/* Налаштування способу виведення */
void SelfTest_SetOutput(
    self_test_output_fn output,
    void *context
);

/* Формування рядка одного пристрою */
bool SelfTest_FormatLine(
    char *buffer,
    size_t size,
    const self_test_device_t *device
);

/* Формування загального підсумку */
bool SelfTest_FormatSummary(
    char *buffer,
    size_t size,
    const self_test_summary_t *summary
);

/* Формування повного звіту */
bool SelfTest_Report(
    const self_test_device_t *devices,
    size_t count
);

/* Передача одного готового рядка (без символу нового рядка)
 * у налаштований вивід; false, якщо вивід не налаштовано */
bool SelfTest_Emit(const char *line);

/* Текстовий код помилки драйвера, як у полі err= */
const char *SelfTest_ErrorText(dev_status_t error);

#endif /* SELF_TEST_H */
