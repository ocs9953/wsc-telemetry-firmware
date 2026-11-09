/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "app_threadx.h"
#include "main.h"
#include "fdcan.h"
#include "gpdma.h"
#include "icache.h"
#include "iwdg.h"
#include "rtc.h"
#include "sdmmc.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "phase_config.h"
#include "types.h"
#include "can_receiver.h"
#include "ext_can_controller.h"
#include "gps.h"
#include "timer_scheduler.h"      /* Timer_Scheduler_Start/Tick (Phase 4)  */
#include "uart_transmitter.h"     /* UART 콜백 훅 (Phase 4/4.5)            */
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

/* USER CODE BEGIN PV */
volatile uint8_t g_init_nonfatal = 0;

volatile uint8_t g_sd_hw_ok = 0;

volatile uint32_t g_boot_reset_csr    = 0;
volatile uint32_t g_boot_stack_thread = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void SystemPower_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
extern SYSTEM_STATE SystemState;

static volatile uint32_t fault_tick[FAULT_IDX_COUNT] = {0};

void SystemState_MarkFault(uint8_t idx)
{
  if (idx < FAULT_IDX_COUNT)
  {
    uint32_t now = HAL_GetTick();
    fault_tick[idx] = (now == 0u) ? 1u : now;
  }
}

uint8_t SystemState_FaultHoldExpired(uint8_t idx)
{
  if (idx >= FAULT_IDX_COUNT)
    return 1u;

  uint32_t t = fault_tick[idx];
  if (t == 0u)
    return 1u;

  return (uint8_t)((HAL_GetTick() - t) >= SUBSYS_FAULT_HOLD_MS);
}

void SystemState_RefreshBoardLed(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  uint8_t healthy = (uint8_t)(SystemState.can_ok && SystemState.mcp_ok);
#if PHASE4_UART_PIPELINE
  healthy = (uint8_t)(healthy && SystemState.uart_ok);
#endif
#if PHASE5_SD_LOGGING
  healthy = (uint8_t)(healthy && SystemState.sd_ok);
#endif
  HAL_GPIO_WritePin(LED_BOARD_CHECK_GPIO_Port, LED_BOARD_CHECK_Pin,
                     healthy ? GPIO_PIN_SET : GPIO_PIN_RESET);

  __set_PRIMASK(primask);
}
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
  MX_GPDMA1_Init();
  MX_RTC_Init();
  MX_FDCAN1_Init();
  MX_ICACHE_Init();
  MX_USART2_UART_Init();
  MX_IWDG_Init();
  /* MX_SDMMC1_SD_Init() 는 여기서 부르지 않는다 — .ioc functionlistsort 에서
     자동 호출을 끄고 USER CODE BEGIN 2 에서 비치명으로 초기화한다. */
  MX_SPI1_Init();
  MX_TIM5_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */

  g_boot_reset_csr = RCC->CSR & (RCC_CSR_IWDGRSTF | RCC_CSR_SFTRSTF | RCC_CSR_PINRSTF
                                 | RCC_CSR_BORRSTF | RCC_CSR_WWDGRSTF | RCC_CSR_LPWRRSTF
                                 | RCC_CSR_OBLRSTF);
  __HAL_RCC_CLEAR_RESET_FLAGS();
  {
    uint32_t mark = TAMP->BKP2R;
    if ((mark & BOOT_STACK_MARKER_MASK) == BOOT_STACK_MARKER)
    {
      g_boot_stack_thread = mark & 0xFFu;
      if (g_boot_stack_thread == 0u)
        g_boot_stack_thread = BOOT_THREAD_OTHER;
    }
    TAMP->BKP2R = 0u;
  }

  g_init_nonfatal = 1u;
  MX_SDMMC1_SD_Init();
  if (g_init_nonfatal == 2u)
  {
    SystemState.sd_ok = 0;
    HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
  }
  g_sd_hw_ok = (g_init_nonfatal == 2u) ? 0u : 1u;
  g_init_nonfatal = 0u;

  /* CAN 수신 모듈 초기화 (FDCAN1) */
  if (CAN_Receiver_Init() != HAL_OK)
  {
    HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
    g_init_fail_mask |= 0x1u;
    SystemState.error = 1;
  }

  /* 외부 CAN 컨트롤러 초기화 (MCP2518FD via SPI1) */
  if (ExtCAN_Init() != HAL_OK)
  {
    HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
    g_init_fail_mask |= 0x2u;
    SystemState.error = 1;
  }

  /* ── VehicleData 무임계구역 설계의 불변식 검증 (시스템 점검 발견) ──
     FDCAN1 ISR(CAN 수신 → VehicleData 쓰기)과 TIM5 ISR(VehicleData 읽기
     → 패킷 생성)이 서로 선점하지 못해야 별도 락 없이 안전하다. 이는
     "두 IRQ의 preemption priority가 같다"는 CubeMX NVIC 설정에만 의존하는
     전제라, CubeMX 재생성/실수로 어긋나도 컴파일은 통과해 조용히 데이터
     경합이 생길 수 있다. 부팅 시 직접 확인해 어긋나면 즉시 정지시킨다. */
  {
    uint32_t pg = HAL_NVIC_GetPriorityGrouping();
    uint32_t fdcan0_pre, fdcan0_sub, fdcan1_pre, fdcan1_sub, tim5_pre, tim5_sub;
    HAL_NVIC_GetPriority(FDCAN1_IT0_IRQn, pg, &fdcan0_pre, &fdcan0_sub);
    HAL_NVIC_GetPriority(FDCAN1_IT1_IRQn, pg, &fdcan1_pre, &fdcan1_sub);
    HAL_NVIC_GetPriority(TIM5_IRQn,       pg, &tim5_pre,   &tim5_sub);

    if (fdcan0_pre != tim5_pre || fdcan1_pre != tim5_pre)
    {
      /* 불변식 위반 — VehicleData 데이터 경합 위험. 계속 진행하지 않는다. */
      HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
      Error_Handler();
    }
  }

  SystemState_RefreshBoardLed();

#if PHASE4_UART_PIPELINE || TEMP_S3_TIMEOUT_TEST
  /* TIM5 스케줄러 시작 (task 12.2) — 커널 진입 전 tick의 큐 전송은
     ThreadX queue-id 검사로 안전하게 드롭됨 (알려진 이슈 '초기화 순서') */
  Timer_Scheduler_Start();
#endif

  /* USER CODE END 2 */

  MX_ThreadX_Init();

  /* We should never get here as control is now taken by the scheduler */

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

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_HIGH);

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_LSI
                              |RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.LSIDiv = RCC_LSI_DIV1;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMBOOST = RCC_PLLMBOOST_DIV2;
  RCC_OscInitStruct.PLL.PLLM = 1;
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
  * @brief  FDCAN Rx FIFO0 콜백 — CAN_Receiver로 전달
  */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
  CAN_Receiver_RxCallback(hfdcan, RxFifo0ITs);
}

/**
  * @brief  FDCAN 프로토콜 에러 콜백 — CAN_Receiver로 전달
  */
void HAL_FDCAN_ErrorCallback(FDCAN_HandleTypeDef *hfdcan)
{
  CAN_Receiver_ErrorCallback(hfdcan);
}

/**
  * @brief  FDCAN 에러 상태 콜백 — Bus-Off/Error Passive/Warning
  */
void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
{
  CAN_Receiver_ErrorStatusCallback(hfdcan, ErrorStatusITs);
}

/**
  * @brief  GPIO EXTI Falling 콜백 — MCP2518FD 인터럽트 통지
  * @note   MCP2518FD INT는 active-low이므로 falling 엣지로 감지한다.
  *         블로킹 SPI 수신을 ISR에서 수행하지 않기 위해, 세마포어만 set하고
  *         즉시 반환한다. 실제 수신은 MCP_RX_Thread에서 처리한다 (Req 9.7).
  *         CAN_INT(PB0, nINT/INT0)와 CAN_RX_INT(PE9, INT1/RX) 모두 연결.
  */
void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == CAN_INT_Pin || GPIO_Pin == CAN_RX_INT_Pin)
  {
    ExtCAN_NotifyInterrupt();
  }
}

#if PHASE4_UART_PIPELINE
/**
  * @brief  UART DMA 송신 완료 콜백 — 세마포어 put (task 12.3, Req 7.5)
  */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  UART_Transmitter_TxCpltCallback(huart);
}

/**
  * @brief  UART 오류 콜백 — ERR_UART_DMA LOG + LED (task 10.1, Req 7.7)
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  UART_Transmitter_ErrorCallback(huart);
#if PHASE8_GPS
  GPS_ErrorCallback(huart);
#endif

}
#endif /* PHASE4_UART_PIPELINE */

#if PHASE8_GPS
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  GPS_RxCpltCallback(huart);
}
#endif

#if PHASE45_UART_RX_CAN_TX
/**
  * @brief  UART RX IDLE 이벤트 콜백 — 지상국 프레임 수신 (task 13A.1)
  */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  UART_Transmitter_RxEventCallback(huart, Size);
}
#endif /* PHASE45_UART_RX_CAN_TX */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
#if PHASE4_UART_PIPELINE || TEMP_S3_TIMEOUT_TEST
  if (htim->Instance == TIM5)
  {
    Timer_Scheduler_Tick();   /* 100ms 스케줄러 (task 12.3) */
  }
#endif
  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  if (g_init_nonfatal != 0u)
  {
    g_init_nonfatal = 2u;
    return;
  }

  /* User can add his own implementation to report the HAL error return state */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  {
    GPIO_InitTypeDef led = {0};
    led.Pin   = LED_ERR_Pin;
    led.Mode  = GPIO_MODE_OUTPUT_PP;
    led.Pull  = GPIO_NOPULL;
    led.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_ERR_GPIO_Port, &led);
  }
  HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);

  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
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
