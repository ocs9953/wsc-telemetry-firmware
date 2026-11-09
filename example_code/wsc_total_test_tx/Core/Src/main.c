/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body (FDCAN Logic Transplanted)
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fdcan.h"
#include "icache.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

COM_InitTypeDef BspCOMInit;

/* USER CODE BEGIN PV */
FDCAN_TxHeaderTypeDef TxHeader;
uint8_t TxData[8];
FDCAN_FilterTypeDef sFilterConfig;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void SystemPower_Config(void);
/* USER CODE BEGIN PFP */
void FDCAN_RxConfig(void);
void FDCAN_TxHeader_Config(FDCAN_TxHeaderTypeDef* TxHeader, uint32_t ID);
void FDCAN_Error_Handler(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/**
 * @brief printf 리다이렉션 (Tera Term 출력용)
 */
#ifdef __GNUC__
int _write(int file, char *ptr, int len)
{
    // Nucleo 보드 BSP의 COM1 핸들을 통해 전송
    extern UART_HandleTypeDef hcom_uart[COMn];
    HAL_UART_Transmit(&hcom_uart[COM1], (uint8_t *)ptr, len, 10);
    return len;
}
#endif
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the System Power */
  SystemPower_Config();

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ICACHE_Init();
  MX_FDCAN1_Init();
  /* USER CODE BEGIN 2 */

  /* Initialize led */
  BSP_LED_Init(LED_GREEN);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Initialize COM1 port (115200, 8 bits (7-bit data + 1 stop bit), no parity */
  BspCOMInit.BaudRate   = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits   = COM_STOPBITS_1;
  BspCOMInit.Parity     = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl  = COM_HWCONTROL_NONE;
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }

  /* [Transplanted Code] Start Message & FDCAN Config */
  printf("\r\n=== FDCAN Transmitter Started ===\r\n");

  /* FDCAN 설정 및 시작 (Start 명령 포함) */
  FDCAN_RxConfig();
  printf("FDCAN Initialized & Started.\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_0;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
  RCC_OscInitStruct.PLL.PLLMBOOST = RCC_PLLMBOOST_DIV4;
  RCC_OscInitStruct.PLL.PLLM = 3;
  RCC_OscInitStruct.PLL.PLLN = 10;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 1;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLLVCIRANGE_1;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Power Configuration
  * @retval None
  */
static void SystemPower_Config(void)
{
  /*
   * Disable the internal Pull-Up in Dead Battery pins of UCPD peripheral
   */

  /*
   * Switch to SMPS regulator instead of LDO
   */
  if (HAL_PWREx_ConfigSupply(PWR_SMPS_SUPPLY) != HAL_OK)
  {
    Error_Handler();
  }
/* USER CODE BEGIN PWR */
/* USER CODE END PWR */
}

/* USER CODE BEGIN 4 */

/**
  * @brief  버튼 인터럽트 콜백 (데이터 전송 로직)
  */
void BSP_PB_Callback(Button_TypeDef Button)
{
    if (Button == BUTTON_USER)
    {
        // 1. 채터링(Debounce) 방지
        static uint32_t last_tick = 0;
        if ((HAL_GetTick() - last_tick) < 200) return;
        last_tick = HAL_GetTick();

        // 2. 보낼 값 설정 (실수형 Float)
        static float sim_voltage = 100.0f;
        static float sim_speed = 0.0f;

        sim_voltage += 0.5f;
        sim_speed += 1.5f;
        if(sim_speed > 150.0f) sim_speed = 0.0f;

        /* ============================================== */
        /* 1. 전압 데이터 전송 (ID: 0x50)                 */
        /* ============================================== */
        uint8_t data1[8] = {0};

        // [핵심] Float 변수(4바이트)를 배열에 복사
        memcpy(data1, &sim_voltage, sizeof(float));

        memcpy(TxData, data1, 8);
        if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) > 0)
        {
            FDCAN_TxHeader_Config(&TxHeader, 0x50); // ID: 0x50 (전압)
            if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &TxHeader, TxData) == HAL_OK)
            {
                // 디버깅용: 정수부만 출력 (printf가 float 미지원일 수 있음)
                printf("Tx1 CAN Sent (ID: 0x50) | Voltage: %d\r\n", (int)sim_voltage);
            }
        }

        /* ============================================== */
        /* 2. 속도 데이터 전송 (ID: 0x60)                 */
        /* ============================================== */
        uint8_t data2[8] = {0};

        // [핵심] Float 변수 복사
        memcpy(data2, &sim_speed, sizeof(float));

        memcpy(TxData, data2, 8);
        if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) > 0)
        {
            FDCAN_TxHeader_Config(&TxHeader, 0x60); // ID: 0x60 (속도)
            if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &TxHeader, TxData) == HAL_OK)
            {
                printf("Tx2 CAN Sent (ID: 0x60) | Speed: %d\r\n", (int)sim_speed);
                BSP_LED_Toggle(LED_GREEN);
            }
        }
    }
}

/**
  * @brief  FDCAN 초기 설정 및 Start
  */
void FDCAN_RxConfig(void)
{
  /* Configure Rx filter */
  sFilterConfig.IdType = FDCAN_STANDARD_ID;
  sFilterConfig.FilterIndex = 0;
  sFilterConfig.FilterType = FDCAN_FILTER_RANGE;
  sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  sFilterConfig.FilterID1 = 0x000;
  sFilterConfig.FilterID2 = 0x7FF;

  if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_REJECT, FDCAN_REJECT, FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK)
  {
    Error_Handler();
  }

  /* Start the FDCAN module */
  if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief  Tx Header 설정 도우미 함수
  */
void FDCAN_TxHeader_Config(FDCAN_TxHeaderTypeDef* TxHeader, uint32_t ID)
{
  TxHeader->Identifier = ID;
  TxHeader->IdType = FDCAN_STANDARD_ID;
  TxHeader->TxFrameType = FDCAN_DATA_FRAME;
  TxHeader->DataLength = FDCAN_DLC_BYTES_8;
  TxHeader->ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  TxHeader->BitRateSwitch = FDCAN_BRS_OFF;
  TxHeader->FDFormat = FDCAN_CLASSIC_CAN;
  TxHeader->TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  TxHeader->MessageMarker = 0;
}

void FDCAN_Error_Handler(void)
{
     if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
     {
       Error_Handler();
     }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  * where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
