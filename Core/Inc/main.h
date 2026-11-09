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
void SystemState_RefreshBoardLed(void);

#define SUBSYS_FAULT_HOLD_MS   3000u

#define FAULT_IDX_CAN    0u
#define FAULT_IDX_UART   1u
#define FAULT_IDX_SD     2u
#define FAULT_IDX_COUNT  3u

void    SystemState_MarkFault(uint8_t idx);
uint8_t SystemState_FaultHoldExpired(uint8_t idx);

extern volatile uint32_t g_boot_reset_csr;
extern volatile uint32_t g_boot_stack_thread;

#define BOOT_STACK_MARKER       0x53544B00u
#define BOOT_STACK_MARKER_MASK  0xFFFFFF00u
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define CAN_CS_Pin GPIO_PIN_4
#define CAN_CS_GPIO_Port GPIOA
#define CAN_INT_Pin GPIO_PIN_0
#define CAN_INT_GPIO_Port GPIOB
#define CAN_INT_EXTI_IRQn EXTI0_IRQn
/* 2026-08 재배정: PB1 → PD6. 회로도(WSC.kicad_sch) 넷리스트 실측 결과
   PD6(pin 87)가 `3.3V` 넷에 직결되고 PB1(pin 33)은 미접속으로 확인됨. */
#define Power_check_Pin GPIO_PIN_6
#define Power_check_GPIO_Port GPIOD
#define CAN_RX_INT_Pin GPIO_PIN_9
#define CAN_RX_INT_GPIO_Port GPIOE
#define CAN_RX_INT_EXTI_IRQn EXTI9_IRQn
#define LED_ERR_Pin GPIO_PIN_11
#define LED_ERR_GPIO_Port GPIOE
#define LED_BOARD_CHECK_Pin GPIO_PIN_12
#define LED_BOARD_CHECK_GPIO_Port GPIOE
/* 2026-08 정정: 회로도(WSC.kicad_sch) 실측 대조 결과 LED_SD/LED_CAN이
   PE13/PE14가 아니라 PD8/PB15에 배선되어 있음이 확인됨 — 재배정. */
#define LED_SD_Pin GPIO_PIN_8
#define LED_SD_GPIO_Port GPIOD
#define LED_CAN_Pin GPIO_PIN_15
#define LED_CAN_GPIO_Port GPIOB
#define LED_RFD900x_Pin GPIO_PIN_15
#define LED_RFD900x_GPIO_Port GPIOE
#define SD_DETECT_Pin GPIO_PIN_0
#define SD_DETECT_GPIO_Port GPIOD

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
