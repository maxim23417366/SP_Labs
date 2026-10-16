/* Підміна firmware/Core/Inc/main.h для тестів на ПК: ті самі імена пінів */
#ifndef MAIN_H
#define MAIN_H

#include "stm32_mock.h"

#define LD2_Pin             GPIO_PIN_5
#define LD2_GPIO_Port       GPIOA
#define FLASH_CS_Pin        GPIO_PIN_10
#define FLASH_CS_GPIO_Port  GPIOB

void Error_Handler(void);

#endif /* MAIN_H */
