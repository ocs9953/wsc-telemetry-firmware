/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sdmmc.c
  * @brief   This file provides code for the configuration
  * of the SDMMC instances.
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "sdmmc.h"

/* USER CODE BEGIN 0 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "fx_api.h"
#include "logger.h"
#include "types.h"
#include "ringbuffer.h"

// [외부 변수]
// app_filex.c 에서 정의된 미디어 객체 (이름은 CubeMX 설정에 따라 다를 수 있으나 보통 sdio_disk)
FX_MEDIA sdio_disk;

// [내부 변수]
// 기존 FIL logfile 대신 FX_FILE 사용
FX_FILE logfile;

extern SYSTEM_STATE sys_state;
ring_buffer_t SD_BUFFER;
uint8_t SD_BUFFER_ARR[4096];

char logname[40];
LOG syslog_buffer;

/* USER CODE END 0 */

SD_HandleTypeDef hsd1;

/* SDMMC1 init function */
void MX_SDMMC1_SD_Init(void)
{
  /* USER CODE BEGIN SDMMC1_Init 0 */
  /* USER CODE END SDMMC1_Init 0 */

  /* USER CODE BEGIN SDMMC1_Init 1 */
  /* USER CODE END SDMMC1_Init 1 */
  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_1B;

  // [권장] FileX/ThreadX 환경에서는 흐름 제어 활성화가 유리함
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
  hsd1.Init.ClockDiv = 12;

  if (HAL_SD_Init(&hsd1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SDMMC1_Init 2 */
  /* USER CODE END SDMMC1_Init 2 */
}

void HAL_SD_MspInit(SD_HandleTypeDef* sdHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(sdHandle->Instance==SDMMC1)
  {
  /* USER CODE BEGIN SDMMC1_MspInit 0 */
  /* USER CODE END SDMMC1_MspInit 0 */

  /** Initializes the peripherals clock */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_SDMMC;
    PeriphClkInit.SdmmcClockSelection = RCC_SDMMCCLKSOURCE_PLL1;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* SDMMC1 clock enable */
    __HAL_RCC_SDMMC1_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /**SDMMC1 GPIO Configuration
    PC8     ------> SDMMC1_D0
    PC12    ------> SDMMC1_CK
    PD2     ------> SDMMC1_CMD
    */
    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC1;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC1;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /* SDMMC1 interrupt Init */
    HAL_NVIC_SetPriority(SDMMC1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
  /* USER CODE BEGIN SDMMC1_MspInit 1 */
  /* USER CODE END SDMMC1_MspInit 1 */
  }
}

void HAL_SD_MspDeInit(SD_HandleTypeDef* sdHandle)
{
  if(sdHandle->Instance==SDMMC1)
  {
  /* USER CODE BEGIN SDMMC1_MspDeInit 0 */
  /* USER CODE END SDMMC1_MspDeInit 0 */
    __HAL_RCC_SDMMC1_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_8|GPIO_PIN_12);
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);
    HAL_NVIC_DisableIRQ(SDMMC1_IRQn);
  /* USER CODE BEGIN SDMMC1_MspDeInit 1 */
  /* USER CODE END SDMMC1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/**
  * @brief  [FileX 버전] SD 카드 파일 열기
  * 주의: FileX 마운트는 app_filex.c에서 이미 수행되었다고 가정합니다.
  */
int SD_SETUP(DATETIME *boot) {
    UINT status;

    // 1. 링버퍼 초기화
    ring_buffer_init(&SD_BUFFER, (char *)SD_BUFFER_ARR, sizeof(SD_BUFFER_ARR));

    // 2. 미디어 준비 확인 (app_filex.c 초기화 완료 대기)
    // ID가 0이면 아직 마운트 안 된 상태일 수 있음
    if (sdio_disk.fx_media_id == 0) {
         DEBUG_MSG("Media not ready\r\n");
         return -1;
    }

    // 3. 파일명 생성
    sprintf(logname, "20%02d-%02d-%02d %02d-%02d-%02d.log",
    boot->year, boot->month, boot->date, boot->hour, boot->minute, boot->second);

    // 4. 파일 생성 (없으면 생성, 있으면 이미 존재 에러 무시)
    status = fx_file_create(&sdio_disk, logname);
    if (status != FX_SUCCESS && status != FX_ALREADY_CREATED) {
        sys_state.SD = false;
        #ifdef LED_SD_Pin
        HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
        #endif
        DEBUG_MSG("File create fail: 0x%X\r\n", status);
        return status;
    }

    // 5. 파일 열기 (쓰기 모드)
    status = fx_file_open(&sdio_disk, &logfile, logname, FX_OPEN_FOR_WRITE);
    if (status != FX_SUCCESS) {
        sys_state.SD = false;
        #ifdef LED_SD_Pin
        HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
        #endif
        DEBUG_MSG("File open fail: 0x%X\r\n", status);
        return status;
    }

    // 6. 파일 끝으로 이동 (Append 모드 구현)
    // 현재 파일 크기만큼 커서를 이동시킵니다.
    fx_file_seek(&logfile, logfile.fx_file_current_file_size);

    sys_state.SD = true;
    #ifdef LED_SD_Pin
    HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_RESET);
    #endif

    DEBUG_MSG("SD Ready (FileX): %s\r\n", logname);
    return 0; // SYS_OK
}

/**
  * @brief  [FileX 버전] 데이터 쓰기
  */
int SD_WRITE(void) {
    if (sys_state.SD == false) return -1;
    if (ring_buffer_is_empty(&SD_BUFFER)) return 0;

    UINT status = FX_SUCCESS;

    while (!ring_buffer_is_empty(&SD_BUFFER))
    {
        ring_buffer_dequeue_arr(&SD_BUFFER, (char *)&syslog_buffer, sizeof(LOG));

        // FileX 쓰기 함수
        status = fx_file_write(&logfile, &syslog_buffer, sizeof(LOG));

        #ifdef LED_SD_Pin
        HAL_GPIO_TogglePin(LED_SD_GPIO_Port, LED_SD_Pin);
        #endif

        if (status != FX_SUCCESS) break;
    }

    if (status != FX_SUCCESS) {
        sys_state.SD = false;
        #ifdef LED_SD_Pin
        HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
        #endif
        DEBUG_MSG("SD write fail: 0x%X\r\n", status);
    }

    return (int)status;
}

/**
  * @brief  [FileX 버전] 동기화 (Flush)
  */
int SD_SYNC(void) {
    if (sys_state.SD == false) return -1;

    // 미디어 전체를 플러시하여 데이터를 물리적으로 기록
    UINT status = fx_media_flush(&sdio_disk);

    if (status != FX_SUCCESS) {
        sys_state.SD = false;
        #ifdef LED_SD_Pin
        HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
        #endif
        DEBUG_MSG("SD flush fail: 0x%X\r\n", status);
    }
    return (int)status;
}

/* USER CODE END 1 */
