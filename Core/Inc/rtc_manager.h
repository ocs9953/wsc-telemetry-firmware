/**
  ******************************************************************************
  * @file    rtc_manager.h
  * @brief   RTC 시각 관리 인터페이스
  ******************************************************************************
  */
#ifndef __RTC_MANAGER_H
#define __RTC_MANAGER_H

#include "stm32u5xx_hal.h"
#include "types.h"
#include <stddef.h>

HAL_StatusTypeDef RTC_Manager_GetDateTime(DATETIME *dt);

/* DATETIME을 LOG 패킷의 value 필드(8B)에 패킹 */
void RTC_Manager_PackToValue(const DATETIME *dt, uint8_t *value_field);

/* 부팅 시각 기반 파일명 생성: "YYMMDD_HHMMSS.log" */
void RTC_Manager_MakeFilename(const DATETIME *dt, char *buf, size_t buf_len);

#ifndef RTC_LOCAL_UTC_OFFSET_MIN
#define RTC_LOCAL_UTC_OFFSET_MIN  (9 * 60)
#endif

uint8_t RTC_Manager_SetFromGps(uint32_t utc_time, uint32_t utc_date, DATETIME *before);
#endif /* __RTC_MANAGER_H */
