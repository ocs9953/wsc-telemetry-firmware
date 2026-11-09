/**
  ******************************************************************************
  * @file    ext_can_controller.h
  * @brief   MCP2518FD SPI 제어 인터페이스 (Emandhal/MCP251XFD 래퍼)
  ******************************************************************************
  */
#ifndef __EXT_CAN_CONTROLLER_H
#define __EXT_CAN_CONTROLLER_H

#include "stm32u5xx_hal.h"
#include "tx_api.h"
#include <stdbool.h>
#include <stddef.h>

/* MCP2518FD 초기화 (Init → ConfigureFIFO → ConfigureFilter → StartCAN20) */
HAL_StatusTypeDef ExtCAN_Init(void);

/* CAN TX (task 6A.2, Phase 2.5): 페이로드를 [제어 1B|데이터 7B] 프레임으로
   분할하여 LCD 보드에 전송. 스레드 컨텍스트 전용. (정의는 PHASE25 게이트) */
HAL_StatusTypeDef ExtCAN_Transmit(const uint8_t *data, size_t len);

/* EXTI ISR(HAL_GPIO_EXTI_Falling_Callback)에서 호출 — 세마포어만 set.
   실제 블로킹 SPI 수신은 MCP_RX_Thread에서 처리한다 (Req 9.7). */
void ExtCAN_NotifyInterrupt(void);

/* MCP2518FD 수신 전용 ThreadX 스레드 엔트리 (세마포어 대기 → FIFO 드레인) */
void ExtCAN_RxThread_Entry(ULONG thread_input);

/* MCP2518FD 초기화 완료 및 정상 동작 여부 확인 */
bool ExtCAN_IsReady(void);


HAL_StatusTypeDef ExtCAN_TransmitFrame(uint32_t can_id,
                                       const uint8_t *data, uint8_t dlc);

#endif /* __EXT_CAN_CONTROLLER_H */
