/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
  * @brief   This file provides code for the configuration
  * of the USART instances. (LBT Applied for E22)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "usart.h"

/* USER CODE BEGIN 0 */
#include "ringbuffer.h"
#include "types.h"

// [LoRa 전용] 링버퍼 및 데이터 저장소
ring_buffer_t LORA_BUFFER;
char lora_data_storage[1024];

// [수신용] 1바이트 임시 저장 공간
uint8_t lora_rx_data;

// [상태 플래그]
static volatile uint8_t lora_tx_busy = 0;
static uint8_t lora_tx_payload[64];

// [추가] LED 깜빡임 보정용 타이머 (main.c에 정의됨)
extern volatile uint32_t led_lora_off_tick;

/**
  * @brief  LoRa 모듈 동작 모드 변경 (M0, M1 핀 제어)
  */
void LORA_Set_Mode(LORA_MODE mode) {
    GPIO_PinState m0_state = GPIO_PIN_RESET;
    GPIO_PinState m1_state = GPIO_PIN_RESET;

    switch (mode) {
        case LORA_MODE_NORMAL:    m0_state = 0; m1_state = 0; break;
        case LORA_MODE_WAKEUP:    m0_state = 1; m1_state = 0; break;
        case LORA_MODE_POWERSAVE: m0_state = 0; m1_state = 1; break;
        case LORA_MODE_SLEEP:     m0_state = 1; m1_state = 1; break;
    }

    HAL_GPIO_WritePin(LORA_M0_GPIO_Port, LORA_M0_Pin, m0_state);
    HAL_GPIO_WritePin(LORA_M1_GPIO_Port, LORA_M1_Pin, m1_state);
    HAL_Delay(20);
}

/**
  * @brief  UART 전송 완료 인터럽트 콜백
  */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    if (!ring_buffer_is_empty(&LORA_BUFFER))
    {
        char data;
        if (ring_buffer_dequeue(&LORA_BUFFER, &data) == 1)
        {
            lora_tx_payload[0] = (uint8_t)data;
            HAL_UART_Transmit_IT(huart, lora_tx_payload, 1);
            lora_tx_busy = 1;
        }
    }
    else
    {
      lora_tx_busy = 0;
    }
  }
}
/* USER CODE END 0 */

UART_HandleTypeDef huart1;

/* USART1 init function */

void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */
  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */
  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */
  /* USER CODE END USART1_Init 2 */

}

void HAL_UART_MspInit(UART_HandleTypeDef* uartHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspInit 0 */
  /* USER CODE END USART1_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1;
    PeriphClkInit.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* USART1 clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**USART1 GPIO Configuration
    PB6     ------> USART1_TX
    PB7     ------> USART1_RX
    */
    GPIO_InitStruct.Pin = LORA_TX_Pin|LORA_RX_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* USART1 interrupt Init */
    HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
  /* USER CODE BEGIN USART1_MspInit 1 */
  /* USER CODE END USART1_MspInit 1 */
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef* uartHandle)
{

  if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspDeInit 0 */
  /* USER CODE END USART1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART1_CLK_DISABLE();

    /**USART1 GPIO Configuration
    PB6     ------> USART1_TX
    PB7     ------> USART1_RX
    */
    HAL_GPIO_DeInit(GPIOB, LORA_TX_Pin|LORA_RX_Pin);

    /* USART1 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USART1_IRQn);
  /* USER CODE BEGIN USART1_MspDeInit 1 */
  /* USER CODE END USART1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/**
  * @brief  LoRa 모듈 초기 설정 (E22-900T22S 전용 + LBT 활성화)
  */
void LORA_Init_Configuration(void) {
    // 1. 설정 모드 진입 (E22는 Sleep 모드에서 설정을 받음)
    LORA_Set_Mode(LORA_MODE_SLEEP);
    HAL_Delay(50); // 모드 전환 대기

    // 2. 설정 패킷 전송 (E22 레지스터 설정)
    // - Command: 0xC0 (Register Write), Addr: 0x00, Len: 0x09
    // - REG0(0x62): UART 9600, 8N1 / AirRate 2.4k
    // - REG1(0x00): SubPacket 200B, RSSI OFF, 22dBm
    // - REG2: LORA_CHANNEL_SETTING (국가별 채널)
    // - REG3(0x10): [중요] LBT Enable (Bit 4 = 1)

    uint8_t e22_config_packet[] = {
        0xC0, 0x00, 0x09,         // Header (Cmd, Addr, Len)
        0x00, 0x00, 0x00,         // ADDH, ADDL, NETID
        0x62, 0x00, LORA_CHANNEL_SETTING, // REG0, REG1, REG2
        0x10, 0x00, 0x00          // REG3(LBT ON), CRYPT_H, CRYPT_L
    };

    HAL_UART_Transmit(&huart1, e22_config_packet, sizeof(e22_config_packet), 100);
    HAL_Delay(100); // 설정 적용 대기

    // 3. 일반 모드 복귀
    LORA_Set_Mode(LORA_MODE_NORMAL);
    HAL_Delay(100); // 모드 전환 대기
}

/**
  * @brief  LoRa 초기화
  */
int LORA_SETUP(void) {
    ring_buffer_init(&LORA_BUFFER, lora_data_storage, sizeof(lora_data_storage));

    // 1. 설정 및 LBT 활성화
    LORA_Init_Configuration();

    // 2. [필수] 수신 인터럽트 시작
    HAL_UART_Receive_IT(&huart1, &lora_rx_data, 1);

    return 0;
}

/**
  * @brief  로그 전송
  */
void LORA_TRANSMIT_LOG(void) {
    if (lora_tx_busy) return;

    // AUX 핀 확인 (Busy 체크 - LBT 대기 중일 때도 Busy 상태가 됨)
    if (HAL_GPIO_ReadPin(LORA_AUX_GPIO_Port, LORA_AUX_Pin) == GPIO_PIN_RESET) return;

    if (!ring_buffer_is_empty(&LORA_BUFFER)) {
        char data;
        if (ring_buffer_dequeue(&LORA_BUFFER, &data) == 1) {
            lora_tx_payload[0] = (uint8_t)data;
            lora_tx_busy = 1;
            HAL_UART_Transmit_IT(&huart1, lora_tx_payload, 1);
        }
    }
}

/**
  * @brief  UART 수신 완료 콜백 (인터럽트)
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        // 1. [Pulse Stretching] LED 타이머 50ms 연장
        led_lora_off_tick = HAL_GetTick() + 50;

        // 2. 수신된 데이터 처리 (필요시 구현)

        // 3. [필수] 다음 바이트 수신 대기 (재활성화)
        HAL_UART_Receive_IT(huart, &lora_rx_data, 1);
    }
}
/* USER CODE END 1 */
