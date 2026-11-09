/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  * of all used GPIO pins. (Safe Logging & Key Separation Applied)
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

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"

/* USER CODE BEGIN 0 */
#include "logger.h"
#include "types.h" // [필수] KEY_IO_CHANGE 정의 포함
#include "ringbuffer.h"

// [변경] 전역 변수 syslog 직접 접근 제거 (데이터 오염 방지)
// extern LOG syslog;

// LoRa 전송용 버퍼 참조
extern ring_buffer_t LORA_BUFFER;

// [내부 변수]
// 0: Low, 1: High
uint8_t din_state[2] = {0};
static uint8_t old_din_state = 0xFF; // 초기 변화 감지를 위해 0xFF로 설정
/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */
/* USER CODE END 1 */

/** Configure pins
     PC14-OSC32_IN (PC14)   ------> RCC_OSC32_IN
     PC15-OSC32_OUT (PC15)   ------> RCC_OSC32_OUT
     PH0-OSC_IN (PH0)   ------> RCC_OSC_IN
     PH1-OSC_OUT (PH1)   ------> RCC_OSC_OUT
     PA11   ------> FDCAN1_RX
     PA12   ------> FDCAN1_TX
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, LED_ERR_Pin|LED_BOARD_CHECK_Pin|LED_SD_Pin|LED_CAN_Pin
                          |LED_LORA_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LORA_NRST_Pin|LORA_M0_Pin|LORA_M1_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : LED_ERR_Pin LED_BOARD_CHECK_Pin LED_SD_Pin LED_CAN_Pin
                           LED_LORA_Pin */
  GPIO_InitStruct.Pin = LED_ERR_Pin|LED_BOARD_CHECK_Pin|LED_SD_Pin|LED_CAN_Pin
                          |LED_LORA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pins : SDIO_DETECT_Pin LORA_AUX_Pin */
  GPIO_InitStruct.Pin = SDIO_DETECT_Pin|LORA_AUX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : DIN0_Pin DIN1_Pin */
  // [중요] 입력 핀이 플로팅되지 않도록 Pull-Down 설정
  GPIO_InitStruct.Pin = DIN0_Pin|DIN1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pins : LORA_NRST_Pin LORA_M0_Pin LORA_M1_Pin */
  GPIO_InitStruct.Pin = LORA_NRST_Pin|LORA_M0_Pin|LORA_M1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/**
  * @brief  디지털 입력 변화 감지 및 LoRa 전송 (main.c에서 호출)
  * DIN0(PD8), DIN1(PD9) 핀의 상태가 변할 때만 LoRa로 데이터를 전송합니다.
  */
void DIGITAL_READ(void) {
    uint8_t current_state = 0;

    // 1. 핀 상태 읽기 (DIN0 -> 0번 비트)
    if (HAL_GPIO_ReadPin(DIN0_GPIO_Port, DIN0_Pin) == GPIO_PIN_SET) {
        current_state |= (1 << 0);
        din_state[0] = 1;
    } else {
        din_state[0] = 0;
    }

    // (DIN1 -> 1번 비트)
    if (HAL_GPIO_ReadPin(DIN1_GPIO_Port, DIN1_Pin) == GPIO_PIN_SET) {
        current_state |= (1 << 1);
        din_state[1] = 1;
    } else {
        din_state[1] = 0;
    }

    // 2. 변화 감지 (Edge Detection)
    if (current_state != old_din_state) {
        old_din_state = current_state;

        // [안전한 데이터 전송] 지역 배열 사용 (Thread-Safe)
        // 1바이트 상태값을 8바이트 배열의 첫 번째 바이트에 저장
        uint8_t safe_io_data[8] = {0};
        safe_io_data[0] = current_state;

        // [적용] types.h에서 분리된 KEY_IO_CHANGE 사용
        // ring_buffer_queue 직접 호출 대신 SYS_LOG 사용 (프로토콜 준수)
        SYS_LOG(LOG_INFO, IO, KEY_IO_CHANGE, safe_io_data);
    }
}

/* USER CODE END 2 */
