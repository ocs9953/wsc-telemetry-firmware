/**************************************************************************/
/*                                                                        */
/*       Copyright (c) Microsoft Corporation. All rights reserved.        */
/*                                                                        */
/*       This software is licensed under the Microsoft Software License   */
/*       Terms for Microsoft Azure RTOS. Full text of the license can be  */
/*       found in the LICENSE file at https://aka.ms/AzureRTOS_EULA       */
/*       and in the root directory of this software.                      */
/*                                                                        */
/**************************************************************************/

/**************************************************************************/
/**************************************************************************/
/**                                                                       */
/** ThreadX Component                                                     */
/**                                                                       */
/**   User Specific                                                       */
/**                                                                       */
/**************************************************************************/
/**************************************************************************/

/**************************************************************************/
/*                                                                        */
/*  PORT SPECIFIC C INFORMATION                            RELEASE        */
/*                                                                        */
/*    tx_user.h                                           PORTABLE C      */
/*                                                           6.3.0        */
/*                                                                        */
/*  AUTHOR                                                                */
/*                                                                        */
/*    William E. Lamie, Microsoft Corporation                             */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */
/*    This file contains user defines for configuring ThreadX in specific */
/*    ways. This file will have an effect only if the application and     */
/*    ThreadX library are built with TX_INCLUDE_USER_DEFINE_FILE defined. */
/*    Note that all the defines in this file may also be made on the      */
/*    command line when building ThreadX library and application objects. */
/*                                                                        */
/*  RELEASE HISTORY                                                       */
/*                                                                        */
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  05-19-2020      William E. Lamie        Initial Version 6.0           */
/*  09-30-2020      Yuxin Zhou              Modified comment(s),          */
/*                                            resulting in version 6.1    */
/*  03-02-2021      Scott Larson            Modified comment(s),          */
/*                                            added option to remove      */
/*                                            FileX pointer,              */
/*                                            resulting in version 6.1.5  */
/*  06-02-2021      Scott Larson            Added options for multiple    */
/*                                            block pool search & delay,  */
/*                                            resulting in version 6.1.7  */
/*  10-15-2021      Yuxin Zhou              Modified comment(s), added    */
/*                                            user-configurable symbol    */
/*                                            TX_TIMER_TICKS_PER_SECOND   */
/*                                            resulting in version 6.1.9  */
/*  04-25-2022      Wenhui Xie              Modified comment(s),          */
/*                                            optimized the definition of */
/*                                            TX_TIMER_TICKS_PER_SECOND,  */
/*                                            resulting in version 6.1.11 */
/*  10-31-2023      Xiuwen Cai              Modified comment(s),          */
/*                                            added option for random     */
/*                                            number stack filling,       */
/*                                            resulting in version 6.3.0  */
/*                                                                        */
/**************************************************************************/

#ifndef TX_USER_H
#define TX_USER_H

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/* Define various build options for the ThreadX port.  The application should either make changes
   here by commenting or un-commenting the conditional compilation defined OR supply the defines
   though the compiler's equivalent of the -D option.

   For maximum speed, the following should be defined:

        TX_MAX_PRIORITIES                       32
        TX_DISABLE_PREEMPTION_THRESHOLD
        TX_DISABLE_REDUNDANT_CLEARING
        TX_DISABLE_NOTIFY_CALLBACKS
        TX_NOT_INTERRUPTABLE
        TX_TIMER_PROCESS_IN_ISR
        TX_REACTIVATE_INLINE
        TX_DISABLE_STACK_FILLING
        TX_INLINE_THREAD_RESUME_SUSPEND
        TX_DISABLE_ERROR_CHECKING

   For minimum size, the following should be defined:

        TX_MAX_PRIORITIES                       32
        TX_DISABLE_PREEMPTION_THRESHOLD
        TX_DISABLE_REDUNDANT_CLEARING
        TX_DISABLE_NOTIFY_CALLBACKS
        TX_NO_FILEX_POINTER
        TX_NOT_INTERRUPTABLE
        TX_TIMER_PROCESS_IN_ISR

   Of course, many of these defines reduce functionality and/or change the behavior of the
   system in ways that may not be worth the trade-off. For example, the TX_TIMER_PROCESS_IN_ISR
   results in faster and smaller code, however, it increases the amount of processing in the ISR.
   In addition, some services that are available in timers are not available from ISRs and will
   therefore return an error if this option is used. This may or may not be desirable for a
   given application.  */

/* Override various options with default values already assigned in tx_port.h. Please also refer
   to tx_port.h for descriptions on each of these options.  */

/*#define TX_MAX_PRIORITIES                32*/
/*#define TX_TIMER_THREAD_STACK_SIZE                1024*/
/*#define TX_TIMER_THREAD_PRIORITY                0*/

/*#define TX_MINIMUM_STACK                200*/

/* Determine if timer expirations (application timers, timeouts, and tx_thread_sleep) calls
   should be processed within the a system timer thread or directly in the timer ISR.
   By default, the timer thread is used. When the following is defined, the timer expiration
   processing is done directly from the timer ISR, thereby eliminating the timer thread control
   block, stack, and context switching to activate it.  */

/*#define TX_TIMER_PROCESS_IN_ISR*/

/* Determine if in-line timer reactivation should be used within the timer expiration processing.
   By default, this is disabled and a function call is used. When the following is defined,
   reactivating is performed in-line resulting in faster timer processing but slightly larger
   code size.  */

/*#define TX_REACTIVATE_INLINE*/

/* Determine is stack filling is enabled. By default, ThreadX stack filling is enabled,
   which places an 0xEF pattern in each byte of each thread's stack.  This is used by
   debuggers with ThreadX-awareness and by the ThreadX run-time stack checking feature.  */

/*#define TX_DISABLE_STACK_FILLING*/

/* Determine whether or not stack checking is enabled. By default, ThreadX stack checking is
   disabled. When the following is defined, ThreadX thread stack checking is enabled.  If stack
   checking is enabled (TX_ENABLE_STACK_CHECKING is defined), the TX_DISABLE_STACK_FILLING
   define is negated, thereby forcing the stack fill which is necessary for the stack checking
   logic.  */

/* 2026-08-21 활성화: 스택 오버플로를 조용한 메모리 파손이 아니라 진단 가능한
   고장으로 만든다. 스레드 스택이 1024B(MCP_RX/UART/UART_RX)·2048B(SD/FileX)인데
   FileX·HAL 호출이 얹혀 여유가 크지 않고, 넘치면 인접 제어블록을 덮어써 원인
   추적이 사실상 불가능해진다. ThreadX 기본 핸들러는 콜백 미등록 시 아무것도 하지
   않으므로 app_threadx.c에서 tx_thread_stack_error_notify()로 핸들러를 등록한다.
   (TX_DISABLE_STACK_FILLING은 미정의 상태여야 하며 현재 그렇다 — 위 주석 참고) */
#define TX_ENABLE_STACK_CHECKING

/* Determine if random number is used for stack filling. By default, ThreadX uses a fixed pattern
   for stack filling. When the following is defined, ThreadX uses a random number for stack filling.
   This is effective only when TX_ENABLE_STACK_CHECKING is defined.  */

/*#define TX_ENABLE_RANDOM_NUMBER_STACK_FILLING*/

/* Determine if preemption-threshold should be disabled. By default, preemption-threshold is
   enabled. If the application does not use preemption-threshold, it may be disabled to reduce
   code size and improve performance.  */

#define TX_DISABLE_PREEMPTION_THRESHOLD

/* Determine if global ThreadX variables should be cleared. If the compiler startup code clears
   the .bss section prior to ThreadX running, the define can be used to eliminate unnecessary
   clearing of ThreadX global variables.  */

/*#define TX_DISABLE_REDUNDANT_CLEARING*/

/* Determine if no timer processing is required. This option will help eliminate the timer
   processing when not needed. The user will also have to comment out the call to
   tx_timer_interrupt, which is typically made from assembly language in
   tx_initialize_low_level. Note: if TX_NO_TIMER is used, the define TX_TIMER_PROCESS_IN_ISR
   must also be used.  */

/*
#define TX_NO_TIMER
#ifndef TX_TIMER_PROCESS_IN_ISR
#define TX_TIMER_PROCESS_IN_ISR
#endif
*/

/* Determine if the notify callback option should be disabled. By default, notify callbacks are
   enabled. If the application does not use notify callbacks, they may be disabled to reduce
   code size and improve performance.  */

#define TX_DISABLE_NOTIFY_CALLBACKS

/* Defined, the basic parameter error checking is disabled. */

/*#define TX_DISABLE_ERROR_CHECKING*/

/* Determine if the tx_thread_resume and tx_thread_suspend services should have their internal
   code in-line. This results in a larger image, but improves the performance of the thread
   resume and suspend services.  */

/*#define TX_INLINE_THREAD_RESUME_SUSPEND*/

/* Determine if the internal ThreadX code is non-interruptable. This results in smaller code
   size and less processing overhead, but increases the interrupt lockout time.  */

/*#define TX_NOT_INTERRUPTABLE*/

/* Determine if the trace event logging code should be enabled. This causes slight increases in
   code size and overhead, but provides the ability to generate system trace information which
   is available for viewing in TraceX.  */

/*#define TX_ENABLE_EVENT_TRACE*/

/* Determine if block pool performance gathering is required by the application. When the following is
   defined, ThreadX gathers various block pool performance information. */

/*#define TX_BLOCK_POOL_ENABLE_PERFORMANCE_INFO*/

/* Determine if byte pool performance gathering is required by the application. When the following is
   defined, ThreadX gathers various byte pool performance information. */

/*#define TX_BYTE_POOL_ENABLE_PERFORMANCE_INFO*/

/* Determine if event flags performance gathering is required by the application. When the following is
   defined, ThreadX gathers various event flags performance information. */

/*#define TX_EVENT_FLAGS_ENABLE_PERFORMANCE_INFO*/

/* Determine if mutex performance gathering is required by the application. When the following is
   defined, ThreadX gathers various mutex performance information. */

/*#define TX_MUTEX_ENABLE_PERFORMANCE_INFO*/

/* Determine if queue performance gathering is required by the application. When the following is
   defined, ThreadX gathers various queue performance information. */

/*#define TX_QUEUE_ENABLE_PERFORMANCE_INFO*/

/* Determine if semaphore performance gathering is required by the application. When the following is
   defined, ThreadX gathers various semaphore performance information. */

/*#define TX_SEMAPHORE_ENABLE_PERFORMANCE_INFO*/

/* Determine if thread performance gathering is required by the application. When the following is
   defined, ThreadX gathers various thread performance information. */

/*#define TX_THREAD_ENABLE_PERFORMANCE_INFO*/

/* Determine if timer performance gathering is required by the application. When the following is
   defined, ThreadX gathers various timer performance information. */

/*#define TX_TIMER_ENABLE_PERFORMANCE_INFO*/

/* Define the common timer tick reference for use by other middleware components. */

#define TX_TIMER_TICKS_PER_SECOND                1000

/* 유휴 시 WFI 진입 (2026-08 추가).
   미정의 시 ThreadX 스케줄러의 idle 대기(tx_thread_schedule.S __tx_ts_wait)가
   빈 루프를 풀클럭으로 돌아, 모든 스레드가 블록된 구간에도 MCU가 Run 모드
   최대 전류를 계속 소비한다. 정의하면 그 구간에서 DSB->WFI->ISB로 절전에 들어가고
   다음 인터럽트(FDCAN1 수신, TIM5 틱, 세마포어/큐 put 등)에 즉시 깨어난다.

   [적용 범위] 평상시 "평균" 전류만 낮춘다. SD 카드 쓰기처럼 스레드가 실제로
   일하는 구간의 "피크" 전류는 줄지 않으므로, 3.3V LDO 여유 문제의 해결책은
   아니다(전원 예산 검토 결과 참조).
   [실시간성] WFI는 인터럽트로 즉시 해제되고 클럭 설정을 바꾸지 않으므로
   TIM5 100ms 스케줄러 주기나 FDCAN 수신 지연에 영향을 주지 않는다. */
#define TX_ENABLE_WFI

/* Determine if there is a FileX pointer in the thread control block.
   By default, the pointer is there for legacy/backwards compatibility.
   The pointer must also be there for applications using FileX.
   Define this to save space in the thread control block.
*/

/*#define TX_NO_FILEX_POINTER*/

#ifdef __ICCARM__
/* Define if the IAR library is supported. */
/*#define TX_ENABLE_IAR_LIBRARY_SUPPORT*/
#endif

/* Define if the safety critical configuration is enabled. */

/*#define TX_SAFETY_CRITICAL*/

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */

#endif
