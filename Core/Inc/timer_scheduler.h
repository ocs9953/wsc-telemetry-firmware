/**
  ******************************************************************************
  * @file    timer_scheduler.h
  * @brief   TIM5 ISR 스케줄러 인터페이스
  ******************************************************************************
  */
#ifndef __TIMER_SCHEDULER_H
#define __TIMER_SCHEDULER_H

#include <stdint.h>

/* TIM5 인터럽트 시작 (HAL_TIM_Base_Start_IT) */
void Timer_Scheduler_Start(void);

/* TIM5 콜백 처리 (HAL_TIM_PeriodElapsedCallback에서 호출, 100ms 주기) */
void Timer_Scheduler_Tick(void);

/* --- Phase 6: Alive-Flag IWDG (task 18.3) ---
 * 각 핵심 스레드는 루프마다 자신의 비트를 SET한다. 스케줄러가 2초 주기로
 * 전부 SET 확인 시에만 HAL_IWDG_Refresh() 후 플래그를 초기화한다. */
#define ALIVE_BIT_UART    (1u << 0)   /* UART_Thread      */
#define ALIVE_BIT_UARTRX  (1u << 1)   /* UART_RX_Thread   */
#define ALIVE_BIT_SD      (1u << 2)   /* SD_Logger_Thread */
#define ALIVE_BIT_VCULCD  (1u << 3)

void Telemetry_AliveSet(uint32_t bit);

/* --- Phase 6: 전원 강하 플래그 (task 18.2) ---
 * 스케줄러가 Power_check(PD6) 강하 감지 시 1로 SET.
 * SD_Logger_Thread가 이를 보고 긴급 Flush 후 무한 루프 진입. */
extern volatile uint8_t g_power_down;

extern volatile uint32_t g_init_fail_mask;

void Telemetry_NotifyPowerDown(void);

void Telemetry_EmitGps(void);

#endif /* __TIMER_SCHEDULER_H */
