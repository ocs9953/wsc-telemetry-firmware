#ifndef MPPT_DUMMY_H
#define MPPT_DUMMY_H

#include "main.h"

/** CAN 시작 후 1회 호출 (USER CODE BEGIN 2) */
void MPPT_Dummy_Init(CAN_HandleTypeDef *hcan);

/** 메인 while 루프에서 계속 호출 (USER CODE BEGIN WHILE) — 논블로킹 */
void MPPT_Dummy_Task(void);

#endif /* MPPT_DUMMY_H */
