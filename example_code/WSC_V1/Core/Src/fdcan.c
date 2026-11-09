/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    fdcan.c
  * @brief   This file provides code for the configuration
  * of the FDCAN instances. (Safe Logging Applied)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "fdcan.h"

/* USER CODE BEGIN 0 */
#include <stdbool.h>
#include <string.h>
#include "logger.h"
#include "types.h"

extern SYSTEM_STATE sys_state;

// [추가] LED 깜빡임 보정용 타이머 변수 (main.c에 정의됨)
extern volatile uint32_t led_can_off_tick;

/* USER CODE END 0 */

FDCAN_HandleTypeDef hfdcan1;

/* FDCAN1 init function */
void MX_FDCAN1_Init(void)
{
  /* USER CODE BEGIN FDCAN1_Init 0 */
  /* USER CODE END FDCAN1_Init 0 */

  /* USER CODE BEGIN FDCAN1_Init 1 */
  /* USER CODE END FDCAN1_Init 1 */
  hfdcan1.Instance = FDCAN1;
  hfdcan1.Init.ClockDivider = FDCAN_CLOCK_DIV1;
  hfdcan1.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
  hfdcan1.Init.AutoRetransmission = DISABLE;
  hfdcan1.Init.TransmitPause = DISABLE;
  hfdcan1.Init.ProtocolException = DISABLE;
  hfdcan1.Init.NominalPrescaler = 16;
  hfdcan1.Init.NominalSyncJumpWidth = 1;
  hfdcan1.Init.NominalTimeSeg1 = 7;
  hfdcan1.Init.NominalTimeSeg2 = 2;
  hfdcan1.Init.DataPrescaler = 1;
  hfdcan1.Init.DataSyncJumpWidth = 1;
  hfdcan1.Init.DataTimeSeg1 = 1;
  hfdcan1.Init.DataTimeSeg2 = 1;
  hfdcan1.Init.StdFiltersNbr = 0;
  hfdcan1.Init.ExtFiltersNbr = 0;
  hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_QUEUE_OPERATION;
  if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN FDCAN1_Init 2 */
  /* USER CODE END FDCAN1_Init 2 */
}

void HAL_FDCAN_MspInit(FDCAN_HandleTypeDef* fdcanHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(fdcanHandle->Instance==FDCAN1)
  {
  /* USER CODE BEGIN FDCAN1_MspInit 0 */
  /* USER CODE END FDCAN1_MspInit 0 */

  /** Initializes the peripherals clock */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_FDCAN1;
    PeriphClkInit.Fdcan1ClockSelection = RCC_FDCAN1CLKSOURCE_PLL1;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* FDCAN1 clock enable */
    __HAL_RCC_FDCAN1_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**FDCAN1 GPIO Configuration
    PA11     ------> FDCAN1_RX
    PA12     ------> FDCAN1_TX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF9_FDCAN1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* FDCAN1 interrupt Init */
    HAL_NVIC_SetPriority(FDCAN1_IT0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    HAL_NVIC_SetPriority(FDCAN1_IT1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(FDCAN1_IT1_IRQn);
  /* USER CODE BEGIN FDCAN1_MspInit 1 */
  /* USER CODE END FDCAN1_MspInit 1 */
  }
}

void HAL_FDCAN_MspDeInit(FDCAN_HandleTypeDef* fdcanHandle)
{
  if(fdcanHandle->Instance==FDCAN1)
  {
  /* USER CODE BEGIN FDCAN1_MspDeInit 0 */
  /* USER CODE END FDCAN1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_FDCAN1_CLK_DISABLE();

    /**FDCAN1 GPIO Configuration
    PA11     ------> FDCAN1_RX
    PA12     ------> FDCAN1_TX
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_11|GPIO_PIN_12);

    /* FDCAN1 interrupt Deinit */
    HAL_NVIC_DisableIRQ(FDCAN1_IT0_IRQn);
    HAL_NVIC_DisableIRQ(FDCAN1_IT1_IRQn);
  /* USER CODE BEGIN FDCAN1_MspDeInit 1 */
  /* USER CODE END FDCAN1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/**
  * @brief  FDCAN 초기 설정 및 시작 (성공한 RX 코드 로직 이식 완료)
  */
int FDCAN_SETUP(void) {
    FDCAN_FilterTypeDef sFilterConfig;

    // 1. [이식] 필터 설정: 모든 ID(0x000 ~ 0x7FF)를 수신하도록 설정 (RANGE 모드)
    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_RANGE;   // [핵심] 성공 코드 로직 반영
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x000; // 시작 ID
    sFilterConfig.FilterID2 = 0x7FF; // 끝 ID

    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK) {
        return -1;
    }

    // 2. [이식] 글로벌 필터: 필터에 맞지 않는 데이터도 FIFO0로 수락
    if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) {
        return -2;
    }

    // 3. [이식] 인터럽트 활성화 (Start 이전에 수행)
    uint32_t interrupts = FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_FULL |
                          FDCAN_IT_RX_FIFO0_MESSAGE_LOST | FDCAN_IT_BUS_OFF |
                          FDCAN_IT_ERROR_PASSIVE | FDCAN_IT_ERROR_WARNING;

    if (HAL_FDCAN_ActivateNotification(&hfdcan1, interrupts, 0) != HAL_OK) {
        return -4;
    }

    // 4. [이식] FDCAN 모듈 시작
    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) {
        return -3;
    }

    return 0; // SYS_OK
}

/**
  * @brief  FDCAN 수신 인터럽트 콜백 (Safe Logging 적용)
  */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs) {
    FDCAN_RxHeaderTypeDef RxHeader;
    uint8_t RxData[8];

    // 1. 신규 메시지 도착
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) != RESET) {

        // 메시지 읽기
        if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &RxHeader, RxData) == HAL_OK) {

            // LED Pulse Stretching
            led_can_off_tick = HAL_GetTick() + 50;

            // 상태 업데이트
            if (sys_state.CAN != true) {
                sys_state.CAN = true;
            }

            // [유지] Safe Logging
            SYS_LOG(LOG_INFO, CAN, (int)RxHeader.Identifier, RxData);

        } else {
            // 읽기 실패 처리
            sys_state.CAN = false;
            uint64_t error_val = 0xFF;
            SYS_LOG(LOG_ERROR, SYS, CAN_ERR_RXMSGFAIL, &error_val);
        }
    }

    // 2. FIFO 가득 참
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_FULL) != RESET) {
        sys_state.CAN = false;
        uint64_t error_code = (uint64_t)HAL_FDCAN_GetError(hfdcan);
        SYS_LOG(LOG_ERROR, SYS, CAN_ERR_FIFOFULL, &error_code);
    }

    // 3. 메시지 유실
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_MESSAGE_LOST) != RESET) {
         // 오버런 처리
    }
}

/**
  * @brief  FDCAN 에러 콜백
  */
void HAL_FDCAN_ErrorCallback(FDCAN_HandleTypeDef *hfdcan) {
    sys_state.CAN = false;
    uint64_t error_code = (uint64_t)HAL_FDCAN_GetError(hfdcan);
    SYS_LOG(LOG_ERROR, SYS, CAN_ERR_CANERR, &error_code);
}

/* USER CODE END 1 */
