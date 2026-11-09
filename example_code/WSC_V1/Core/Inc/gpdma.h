/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpdma.h
  * @brief   This file contains all the function prototypes for
  * the gpdma.c file
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
#ifndef __GPDMA_H__
#define __GPDMA_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_GPDMA1_Init(void);

/* USER CODE BEGIN Prototypes */

/* [F시리즈 스타일 호환] 다른 파일에서 DMA 핸들을 사용할 수 있도록 extern 선언 */
/* 주의: main.c나 gpdma.c에 정의된 핸들 이름과 정확히 일치해야 합니다. */
/* CubeMX가 생성하는 기본 이름은 보통 아래와 같습니다. */
extern DMA_HandleTypeDef handle_GPDMA1_Channel0; // ADC1용 (CH0)
extern DMA_HandleTypeDef handle_GPDMA1_Channel1; // ADC4용 (CH1)

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __GPDMA_H__ */

