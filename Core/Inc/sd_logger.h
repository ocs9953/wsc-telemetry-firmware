/**
  ******************************************************************************
  * @file    sd_logger.h
  * @brief   SD_Logger_Thread 인터페이스
  ******************************************************************************
  */
#ifndef __SD_LOGGER_H
#define __SD_LOGGER_H

#include "fx_api.h"
#include "types.h"
#include "tx_api.h"

/* SD_Logger 초기화: 더블 버퍼 초기화, 파일 생성 */
void SD_Logger_Init(FX_MEDIA *media, const DATETIME *boot_time);

/* SD_Logger_Thread 엔트리 함수 (ThreadX 스레드) */
void SD_Logger_Thread_Entry(ULONG thread_input);

#endif /* __SD_LOGGER_H */
