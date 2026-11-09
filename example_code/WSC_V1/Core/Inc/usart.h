/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.h
  * @brief   This file contains all the function prototypes for
  * the usart.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USART_H__
#define __USART_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern UART_HandleTypeDef huart1;

/* USER CODE BEGIN Private defines */

// =================================================================
// [설정] LoRa 주파수 지역 선택 (사용할 지역의 주석을 해제하세요)
// =================================================================
#define USE_REGION_KOREA       // 한국 (테스트용)
// #define USE_REGION_AUSTRALIA   // 호주 (WSC 대회용)

// [채널 계산]
// 사용 중인 Ebyte 모듈의 Base Frequency(기본 주파수) 확인 필수!
// (보통 E32-900 시리즈는 850MHz가 Base입니다.)
// 공식: 실제 주파수 = 850MHz + CHANNEL

#ifdef USE_REGION_KOREA
    // 한국: 921MHz ~ 923MHz 대역 사용
    // 921MHz - 850MHz = 71 (0x47)
    #define LORA_CHANNEL_SETTING  0x47
#endif

#ifdef USE_REGION_AUSTRALIA
    // 호주: 915MHz ~ 928MHz 대역 사용 (ISM Band)
    // 923MHz - 850MHz = 73 (0x49)
    #define LORA_CHANNEL_SETTING  0x49
#endif

/* USER CODE END Private defines */

void MX_USART1_UART_Init(void);

/* USER CODE BEGIN Prototypes */

// [LoRa 동작 모드 정의] (M0, M1 핀 제어용)
typedef enum {
    LORA_MODE_NORMAL = 0,   // M0=0, M1=0 (UART 송수신 가능, 평상시 사용)
    LORA_MODE_WAK                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     EUP = 1,   // M0=1, M1=0 (송신 시 Preamble 추가, 수신측 깨우기용)
    LORA_MODE_POWERSAVE = 2,// M0=0, M1=1 (수신 전용, Wakeup 패킷 기다림)
    LORA_MODE_SLEEP = 3     // M0=1, M1=1 (초저전력, 송수신 불가, 설정 모드)
} LORA_MODE;

// [함수 선언]
int LORA_SETUP(void);                  // LoRa 초기화 (링버퍼 + 주파수 설정)
void LORA_TRANSMIT_LOG(void);          // 로그 전송 (메인 루프 호출)
void LORA_Set_Mode(LORA_MODE mode);    // 모드 변경 (Normal <-> Sleep)
void LORA_Init_Configuration(void);    // 설정 패킷 전송 (주파수 변경 등)

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */

