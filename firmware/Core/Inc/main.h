/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define B1_Pin GPIO_PIN_13
#define B1_GPIO_Port GPIOC
#define B1_EXTI_IRQn EXTI15_10_IRQn
#define CAM_D0_Pin GPIO_PIN_0
#define CAM_D0_GPIO_Port GPIOC
#define CAM_D1_Pin GPIO_PIN_1
#define CAM_D1_GPIO_Port GPIOC
#define CAM_D2_Pin GPIO_PIN_2
#define CAM_D2_GPIO_Port GPIOC
#define CAM_D3_Pin GPIO_PIN_3
#define CAM_D3_GPIO_Port GPIOC
#define PIR_SENSOR_Pin GPIO_PIN_0
#define PIR_SENSOR_GPIO_Port GPIOA
#define PIR_SENSOR_EXTI_IRQn EXTI0_IRQn
#define BTN_2_Pin GPIO_PIN_1
#define BTN_2_GPIO_Port GPIOA
#define USART_TX_Pin GPIO_PIN_2
#define USART_TX_GPIO_Port GPIOA
#define USART_RX_Pin GPIO_PIN_3
#define USART_RX_GPIO_Port GPIOA
#define BTN_3_Pin GPIO_PIN_4
#define BTN_3_GPIO_Port GPIOA
#define LD2_Pin GPIO_PIN_5
#define LD2_GPIO_Port GPIOA
#define BTN_4_Pin GPIO_PIN_7
#define BTN_4_GPIO_Port GPIOA
#define CAM_D4_Pin GPIO_PIN_4
#define CAM_D4_GPIO_Port GPIOC
#define CAM_D5_Pin GPIO_PIN_5
#define CAM_D5_GPIO_Port GPIOC
#define SPARE_PB0_Pin GPIO_PIN_0
#define SPARE_PB0_GPIO_Port GPIOB
#define DOOR_REED_Pin GPIO_PIN_4
#define DOOR_REED_GPIO_Port GPIOB
#define DHT11_DATA_Pin GPIO_PIN_6
#define DHT11_DATA_GPIO_Port GPIOB
#define FLASH_CS_Pin GPIO_PIN_10
#define FLASH_CS_GPIO_Port GPIOB
#define CAM_D6_Pin GPIO_PIN_6
#define CAM_D6_GPIO_Port GPIOC
#define CAM_D7_Pin GPIO_PIN_7
#define CAM_D7_GPIO_Port GPIOC
#define CAM_VSYNC_Pin GPIO_PIN_8
#define CAM_VSYNC_GPIO_Port GPIOC
#define CAM_VSYNC_EXTI_IRQn EXTI9_5_IRQn
#define CAM_XCLK_Pin GPIO_PIN_8
#define CAM_XCLK_GPIO_Port GPIOA
#define BTN_1_Pin GPIO_PIN_9
#define BTN_1_GPIO_Port GPIOA
#define CAM_PCLK_Pin GPIO_PIN_10
#define CAM_PCLK_GPIO_Port GPIOA
#define TMS_Pin GPIO_PIN_13
#define TMS_GPIO_Port GPIOA
#define TCK_Pin GPIO_PIN_14
#define TCK_GPIO_Port GPIOA
#define CAM_HREF_Pin GPIO_PIN_10
#define CAM_HREF_GPIO_Port GPIOC
#define CAM_RESET_Pin GPIO_PIN_11
#define CAM_RESET_GPIO_Port GPIOC
#define CAM_PWDN_Pin GPIO_PIN_12
#define CAM_PWDN_GPIO_Port GPIOC
#define SWO_Pin GPIO_PIN_3
#define SWO_GPIO_Port GPIOB
#define MPU_INT_Pin GPIO_PIN_5
#define MPU_INT_GPIO_Port GPIOB
#define MPU_INT_EXTI_IRQn EXTI9_5_IRQn

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
