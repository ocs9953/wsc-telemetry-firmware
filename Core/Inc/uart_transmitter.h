/**
  ******************************************************************************
  * @file    uart_transmitter.h
  * @brief   UART_Thread / UART_RX_Thread 인터페이스
  ******************************************************************************
  */
#ifndef __UART_TRANSMITTER_H
#define __UART_TRANSMITTER_H

#include "stm32u5xx_hal.h"
#include "tx_api.h"

/* UART_Thread 초기화: 세마포어 생성, (Phase 4.5) UART RX DMA 수신 시작 */
void UART_Transmitter_Init(void);

/* UART_Thread 엔트리 함수 (ThreadX, Priority 5) — 큐 수신 → COBS → DMA 송신 */
void UART_Thread_Entry(ULONG thread_input);

/* UART_RX_Thread 엔트리 함수 (ThreadX, Priority 7) — 지상국 프레임 수신 처리 */
void UART_RX_Thread_Entry(ULONG thread_input);

/* --- main.c USER CODE 4의 HAL 콜백에서 호출하는 훅 --- */
void UART_Transmitter_TxCpltCallback(UART_HandleTypeDef *huart);   /* DMA 완료 */
void UART_Transmitter_ErrorCallback(UART_HandleTypeDef *huart);    /* DMA 오류 */
void UART_Transmitter_RxEventCallback(UART_HandleTypeDef *huart,
                                      uint16_t Size);              /* RX IDLE */

#endif /* __UART_TRANSMITTER_H */
