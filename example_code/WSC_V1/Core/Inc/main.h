/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include "stm32u5xx_hal.h"

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
#define AIN0_Pin GPIO_PIN_1
#define AIN0_GPIO_Port GPIOC
#define AIN1_Pin GPIO_PIN_2
#define AIN1_GPIO_Port GPIOC
#define LED_ERR_Pin GPIO_PIN_11
#define LED_ERR_GPIO_Port GPIOE
#define LED_BOARD_CHECK_Pin GPIO_PIN_12
#define LED_BOARD_CHECK_GPIO_Port GPIOE
#define LED_SD_Pin GPIO_PIN_13
#define LED_SD_GPIO_Port GPIOE
#define LED_CAN_Pin GPIO_PIN_14
#define LED_CAN_GPIO_Port GPIOE
#define LED_LORA_Pin GPIO_PIN_15
#define LED_LORA_GPIO_Port GPIOE
#define SDIO_DETECT_Pin GPIO_PIN_10
#define SDIO_DETECT_GPIO_Port GPIOB
#define DIN0_Pin GPIO_PIN_8
#define DIN0_GPIO_Port GPIOD
#define DIN1_Pin GPIO_PIN_9
#define DIN1_GPIO_Port GPIOD
#define LORA_NRST_Pin GPIO_PIN_4
#define LORA_NRST_GPIO_Port GPIOB
#define LORA_AUX_Pin GPIO_PIN_5
#define LORA_AUX_GPIO_Port GPIOB
#define LORA_TX_Pin GPIO_PIN_6
#define LORA_TX_GPIO_Port GPIOB
#define LORA_RX_Pin GPIO_PIN_7
#define LORA_RX_GPIO_Port GPIOB
#define LORA_M0_Pin GPIO_PIN_8
#define LORA_M0_GPIO_Port GPIOB
#define LORA_M1_Pin GPIO_PIN_9
#define LORA_M1_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
