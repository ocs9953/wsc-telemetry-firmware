/**
  ******************************************************************************
  * @file    can_receiver.h
  * @brief   FDCAN1 수신 콜백 인터페이스
  ******************************************************************************
  */
#ifndef __CAN_RECEIVER_H
#define __CAN_RECEIVER_H

#include "stm32u5xx_hal.h"

/* FDCAN1 초기화: 필터 설정, 인터럽트 활성화, 수신 시작 */
HAL_StatusTypeDef CAN_Receiver_Init(void);

/* FDCAN1 Rx FIFO0 콜백 (HAL에서 호출) — CAN ID 판별 → VehicleData 갱신 */
void CAN_Receiver_RxCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs);

/* FDCAN1 버스 오류 콜백 — ERR_CAN_BUS_OFF LOG 생성 (Req 3.5) */
void CAN_Receiver_ErrorCallback(FDCAN_HandleTypeDef *hfdcan);

/* FDCAN1 에러 상태 콜백 — Bus-Off/Error Passive/Warning 처리 */
void CAN_Receiver_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan,
                                       uint32_t ErrorStatusITs);

void CAN_Receiver_PollBusOff(void);

HAL_StatusTypeDef CAN_Receiver_Transmit(uint32_t can_id,
                                        const uint8_t *data, uint8_t dlc);

#endif /* __CAN_RECEIVER_H */
