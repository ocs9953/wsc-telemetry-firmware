/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.c
  * @brief   This file provides code for the configuration
  * of the RTC instances.
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "rtc.h"

/* USER CODE BEGIN 0 */
/* rtc.h 내부에서 types.h를 include 하고 있으므로 별도 include 불필요 */

// [시간 설정 트리거]
// 이 값을 변경하면(예: 0x1234 -> 0x1235), MCU가 "새로운 설정이구나!"라고 인식하고
// 아래에 적은 sTime/sDate 값으로 시간을 강제 초기화합니다.
#define RTC_BKP_SIGNATURE  0x1235  // <--- 시간을 다시 맞추려면 이 숫자를 바꾸세요!
/* USER CODE END 0 */

RTC_HandleTypeDef hrtc;

/* RTC init function */
void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */
  /* USER CODE END RTC_Init 0 */

  RTC_PrivilegeStateTypeDef privilegeState = {0};

  /* USER CODE BEGIN RTC_Init 1 */
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};
  // [필수] 백업 도메인 접근 권한 활성화 (이게 없으면 BKP 레지스터 못 씀)
  HAL_PWR_EnableBkUpAccess();
  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  hrtc.Init.OutPutPullUp = RTC_OUTPUT_PULLUP_NONE;
  hrtc.Init.BinMode = RTC_BINARY_NONE;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }
  privilegeState.rtcPrivilegeFull = RTC_PRIVILEGE_FULL_NO;
  privilegeState.backupRegisterPrivZone = RTC_PRIVILEGE_BKUP_ZONE_NONE;
  privilegeState.backupRegisterStartZone2 = RTC_BKP_DR0;
  privilegeState.backupRegisterStartZone3 = RTC_BKP_DR0;
  if (HAL_RTCEx_PrivilegeModeSet(&hrtc, &privilegeState) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  // =================================================================
  // =================================================================

  // 1. 이미 시간이 설정되어 있는지 확인 (백업 레지스터 검사)
  if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) == RTC_BKP_SIGNATURE)
  {
      // 암호가 같으면 이미 시간이 잘 가고 있다는 뜻이므로,
      // 시간 초기화 코드를 건너뛰고 함수를 종료합니다.
      return;
  }

  // 2. 암호가 다르면(초기 상태) 시간을 새로 설정
  // ---------------------------------------------------------
  // [설정할 시간 입력] (빌드 후 리셋 누를 미래 시간)
  // ---------------------------------------------------------
  sTime.Hours = 14;      // 시
  sTime.Minutes = 30;    // 분
  sTime.Seconds = 0;     // 초
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;

  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK)
  {
    Error_Handler();
  }

  // ---------------------------------------------------------
  // [설정할 날짜 입력]
  // ---------------------------------------------------------
  sDate.WeekDay = RTC_WEEKDAY_TUESDAY;
  sDate.Month = RTC_MONTH_DECEMBER;
  sDate.Date = 23;
  sDate.Year = 25;                     // 2025년

  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
  {
    Error_Handler();
  }

  // 3. 설정 완료 도장 찍기 (다음 부팅 시 초기화 방지)
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_BKP_SIGNATURE);

  /* USER CODE END RTC_Init 2 */

}

void HAL_RTC_MspInit(RTC_HandleTypeDef* rtcHandle)
{

  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspInit 0 */
  /* USER CODE END RTC_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /* RTC clock enable */
    __HAL_RCC_RTC_ENABLE();
    __HAL_RCC_RTCAPB_CLK_ENABLE();
    __HAL_RCC_RTCAPB_CLKAM_ENABLE();
  /* USER CODE BEGIN RTC_MspInit 1 */
  /* USER CODE END RTC_MspInit 1 */
  }
}

void HAL_RTC_MspDeInit(RTC_HandleTypeDef* rtcHandle)
{

  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspDeInit 0 */
  /* USER CODE END RTC_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_RTC_DISABLE();
    __HAL_RCC_RTCAPB_CLK_DISABLE();
    __HAL_RCC_RTCAPB_CLKAM_DISABLE();
  /* USER CODE BEGIN RTC_MspDeInit 1 */
  /* USER CODE END RTC_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/**
  * @brief  RTC 시간 및 날짜 읽기 함수
  * @param  boot: 시간 정보를 저장할 DATETIME 구조체 포인터
  * @retval None
  */
void RTC_READ(DATETIME *boot) {
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    // 1. 시간과 날짜 읽기
    // 주의: 섀도우 레지스터 동기화를 위해 반드시 Time -> Date 순서로 읽어야 합니다.
    HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

    // 2. 구조체에 값 복사
    boot->year   = sDate.Year;
    boot->month  = sDate.Month;
    boot->date   = sDate.Date;
    boot->hour   = sTime.Hours;
    boot->minute = sTime.Minutes;
    boot->second = sTime.Seconds;
}

/* USER CODE END 1 */
