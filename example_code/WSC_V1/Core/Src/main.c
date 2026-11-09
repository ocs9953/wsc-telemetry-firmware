/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body (Fixed Linker Error)
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "fdcan.h"
#include "app_filex.h"
#include "gpdma.h"
#include "icache.h"
#include "rtc.h"
#include "logger.h"
#include "sdmmc.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include <string.h>
#include "logger.h"
#include "types.h"
#include "ringbuffer.h"
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
// [시스템 상태]
SYSTEM_STATE sys_state = {0};

// [전역 로그 변수]
LOG syslog;

// [ADC 플래그]
volatile uint32_t adc_flag = 0;

// [링버퍼 참조]
extern ring_buffer_t LORA_BUFFER;
extern ring_buffer_t SD_BUFFER;

// [LED 깜빡임 보정용 타이머 변수]
volatile uint32_t led_can_off_tick = 0;
volatile uint32_t led_lora_off_tick = 0;
volatile uint32_t led_sd_off_tick = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void SystemPower_Config(void);
/* USER CODE BEGIN PFP */
extern int FDCAN_SETUP(void);
extern int LORA_SETUP(void);
extern int ANALOG_SETUP(void);
extern void ANALOG_READ(void);
extern void DIGITAL_READ(void);
extern void RTC_READ(DATETIME *boot);
extern void LORA_TRANSMIT_LOG(void);

// [SD카드 관련 함수]
extern int SD_SETUP(DATETIME *boot);
extern int SD_WRITE(void);
extern int SD_SYNC(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
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
  MX_ICACHE_Init();
  MX_FDCAN1_Init();
  MX_SDMMC1_SD_Init();
  MX_ADC4_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();
  MX_RTC_Init();
  MX_FileX_Init();
  MX_TIM5_Init();
  /* USER CODE BEGIN 2 */

  // 0. 초기 부팅 시간 읽기
  DATETIME boot;
  RTC_READ(&boot);

  // [초기화 로그] Safe Logging 적용
  sys_state.SD = true;
  SYS_LOG(LOG_INFO, SYS, SYS_CORE_INIT, NULL);

  // 1. SD 카드 설정
  if (SD_SETUP(&boot) != 0) {
      sys_state.SD = false;
      HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
      SYS_LOG(LOG_ERROR, SYS, SYS_SD_FAIL, NULL);
  } else {
      sys_state.SD = true;
      SYS_LOG(LOG_INFO, SYS, SYS_SD_INIT, NULL);
  }

  // 2. FDCAN 설정
  if (FDCAN_SETUP() != 0) {
      SYS_LOG(LOG_ERROR, CAN, CAN_ERR_CANERR, NULL);
  } else {
      SYS_LOG(LOG_INFO, SYS, CAN_INIT, NULL);
  }

  // 3. LoRa 설정
  if (LORA_SETUP() == 0) {
      SYS_LOG(LOG_INFO, SYS, SYS_SERIAL_INIT, NULL);
  }

  // 4. ADC 설정
  if (ANALOG_SETUP() == 0) {
      SYS_LOG(LOG_INFO, SYS, ANALOG_INIT, NULL);
  }

  // 5. 준비 완료
  SYS_LOG(LOG_INFO, SYS, SYS_READY, NULL);

  // [시작 알림] LED 테스트
  HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED_LORA_GPIO_Port, LED_LORA_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
  HAL_Delay(500);
  HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_LORA_GPIO_Port, LED_LORA_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_RESET);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t last_tick_100ms = 0;
  uint32_t last_tick_1s = 0;

  while (1)
  {
    uint32_t current_tick = HAL_GetTick();

    // ---------------------------------------------------------
    // [LED 자동 끄기 로직] (Pulse Stretching)
    // ---------------------------------------------------------
    if (led_can_off_tick > 0) {
        if (current_tick > led_can_off_tick) {
            HAL_GPIO_WritePin(LED_CAN_GPIO_Port, LED_CAN_Pin, GPIO_PIN_RESET);
            led_can_off_tick = 0;
        } else {
            HAL_GPIO_WritePin(LED_CAN_GPIO_Port, LED_CAN_Pin, GPIO_PIN_SET);
        }
    }

    if (led_lora_off_tick > 0) {
        if (current_tick > led_lora_off_tick) {
            HAL_GPIO_WritePin(LED_LORA_GPIO_Port, LED_LORA_Pin, GPIO_PIN_RESET);
            led_lora_off_tick = 0;
        } else {
            HAL_GPIO_WritePin(LED_LORA_GPIO_Port, LED_LORA_Pin, GPIO_PIN_SET);
        }
    }

    if (led_sd_off_tick > 0) {
        if (current_tick > led_sd_off_tick) {
            HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_RESET);
            led_sd_off_tick = 0;
        } else {
            HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
        }
    }
    // ---------------------------------------------------------

    // [100ms 주기 작업]
    if (current_tick - last_tick_100ms >= 100) {
        last_tick_100ms = current_tick;

        // [삭제됨] 에러 원인이었던 MX_FileX_Process() 제거
        // MX_FileX_Process();

        // (1) 시스템 센서 데이터
        if (adc_flag & (1 << FLAG_ADC_SYS)) {
            adc_flag &= ~(1 << FLAG_ADC_SYS);
            uint16_t safe_sys_data[4] = {0};
            safe_sys_data[0] = (uint16_t)adc_sys_value[0];
            safe_sys_data[1] = (uint16_t)adc_sys_value[1];
            SYS_LOG(LOG_INFO, ADC, KEY_ADC_TEMP, safe_sys_data);
        }

        // (2) 외부 센서 데이터
        if (adc_flag & (1 << FLAG_ADC_AIN)) {
            adc_flag &= ~(1 << FLAG_ADC_AIN);
            uint16_t safe_ain_data[4] = {0};
            safe_ain_data[0] = (uint16_t)adc_ain_value[0];
            safe_ain_data[1] = (uint16_t)adc_ain_value[1];
            SYS_LOG(LOG_INFO, ADC, KEY_ADC_PC1, safe_ain_data);
        }

        // (3) 센서 읽기 (GPIO 변화 감지는 여기서 내부적으로 SYS_LOG 호출)
        ANALOG_READ();
        DIGITAL_READ();

        // (4) LoRa 전송
        LORA_TRANSMIT_LOG();

        // (5) SD 카드 저장 (버퍼 -> 파일)
        if (sys_state.SD) {
            SD_WRITE();
        }

        // Heartbeat
        HAL_GPIO_TogglePin(LED_BOARD_CHECK_GPIO_Port, LED_BOARD_CHECK_Pin);
    }

    // [1초 주기 작업]
    if (current_tick - last_tick_1s >= 1000) {
        last_tick_1s = current_tick;

        DATETIME now;
        RTC_READ(&now);

        // (6) SD 카드 동기화 (Flush)
        if (sys_state.SD) {
            SD_SYNC();
        }
    }
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

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSE
                              |RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMBOOST = RCC_PLLMBOOST_DIV1;
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
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV4;
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
  if (HAL_PWREx_ConfigSupply(PWR_SMPS_SUPPLY) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
  HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
  __disable_irq();
  while (1)
  {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif /* USE_FULL_ASSERT */
