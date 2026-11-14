/**
  ******************************************************************************
  * @file    platform_mem.c
  * @brief   ЛР3, вимірювання пам'яті (методичка §5.4): фактична глибина стека
  *          (stack painting) і статистика heap (mallinfo).
  *
  *          Межі стека беруться з лінкер-скрипта CubeMX (STM32F411xx_FLASH.ld):
  *            _estack         = кінець RAM (вершина стека, стек росте вниз);
  *            _Min_Stack_Size = зарезервований розмір (абсолютний символ:
  *                              його АДРЕСА і є значенням).
  *          sysmem.c (_sbrk) не дає heap зайти нижче _estack - _Min_Stack_Size,
  *          тому пофарбована ділянка не перетинається з heap.
  ******************************************************************************
  */
#include <malloc.h>

#include "board.h"
#include "platform.h"

#define STACK_PATTERN    0xDEADBEEFUL
#define STACK_GUARD_WORDS 16U   /* не фарбувати поточний кадр main() і запас під виклик */

extern uint32_t _estack;
extern uint32_t _Min_Stack_Size;

static uint32_t *stack_bottom(void)
{
  return (uint32_t *)((uint32_t)&_estack - (uint32_t)&_Min_Stack_Size);
}

void Platform_StackPaint(void)
{
  uint32_t *p  = stack_bottom();
  uint32_t *sp = (uint32_t *)__get_MSP();

  while (p < (sp - STACK_GUARD_WORDS))
  {
    *p++ = STACK_PATTERN;
  }
}

uint32_t Platform_StackUsedBytes(void)
{
  const uint32_t *p = stack_bottom();

  /* Перше зіпсоване слово знизу = найглибша точка, куди доходив стек */
  while ((p < &_estack) && (*p == STACK_PATTERN))
  {
    p++;
  }
  return (uint32_t)&_estack - (uint32_t)p;
}

uint32_t Platform_StackReservedBytes(void)
{
  return (uint32_t)&_Min_Stack_Size;
}

void Platform_HeapInfo(platform_heap_t *heap)
{
  struct mallinfo mi;

  if (heap == NULL)
  {
    return;
  }
  mi = mallinfo();
  heap->arena = (uint32_t)mi.arena;
  heap->used  = (uint32_t)mi.uordblks;
  heap->free  = (uint32_t)mi.fordblks;
}
