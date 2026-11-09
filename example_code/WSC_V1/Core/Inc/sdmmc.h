/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sdmmc.h
  * @brief   This file contains all the function prototypes for
  * the sdmmc.c file
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
#ifndef __SDMMC_H__
#define __SDMMC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern SD_HandleTypeDef hsd1;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_SDMMC1_SD_Init(void);

/* USER CODE BEGIN Prototypes */

#include "types.h"

int SD_SETUP(DATETIME *boot);

// [추가] 다른 곳에서 호출할 함수들
int SD_WRITE(void);
int SD_SYNC(void);

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __SDMMC_H__ */

