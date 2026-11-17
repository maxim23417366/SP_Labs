/**
  ******************************************************************************
  * @file    storage_ops.c
  * @brief   Сильна SmartLock_JournalOps(): журнал застосунку (sl_journal.c) пише
  *          записи у W25Q64 через Storage_Append() (storage.c).
  *
  *          Окремим файлом, бо тест sl_journal.c (tests/unit/test_sl_journal.c)
  *          підставляє власний бекенд з тим самим ім'ям і лінкується без цього
  *          файлу (tests/Makefile).
  ******************************************************************************
  */
#include "sl_journal.h"
#include "storage.h"

const sl_journal_ops_t *SmartLock_JournalOps(void)
{
  static const sl_journal_ops_t ops = { Storage_Append };

  return &ops;
}
