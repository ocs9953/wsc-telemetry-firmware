/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_threadx.c
  * @author  MCD Application Team
  * @brief   ThreadX applicative file
  ******************************************************************************
    * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_threadx.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "phase_config.h"
#include "types.h"
#include "tx_api.h"
#include "ext_can_controller.h"   /* ExtCAN_RxThread_Entry */
#include "uart_transmitter.h"     /* UART_Thread / UART_RX_Thread (Phase 4)  */
#include "sd_logger.h"            /* SD_Logger_Thread (Phase 4~5)            */
#include "vcu_lcd_bridge.h"
#include "gps.h"
#include "adc_sensor.h"
#include "main.h"
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

/* 전역 데이터 구조체 (Req 2.1) */
VehicleData_t VehicleData = {0};
SYSTEM_STATE  SystemState = {0};

/* ThreadX RTOS 객체 (Req 1.2) — 생성은 Phase 4(task 12.1) 게이트 */
TX_QUEUE UART_Queue;
TX_QUEUE SD_Queue;

volatile ULONG g_uart_q_drops = 0;
volatile ULONG g_sd_q_drops   = 0;

UINT TelemQueue_Send(TX_QUEUE *queue, ULONG *msg)
{
    UINT st = tx_queue_send(queue, msg, TX_NO_WAIT);

    if (st == TX_QUEUE_FULL)
    {
        TX_INTERRUPT_SAVE_AREA
        TX_DISABLE
        if (queue == &UART_Queue)     g_uart_q_drops++;
        else if (queue == &SD_Queue)  g_sd_q_drops++;
        TX_RESTORE
    }
    return st;
}

/* MCP2518FD 수신 전용 스레드/세마포어
   (Issue #1: 블로킹 SPI를 EXTI ISR 밖으로 분리, Req 9.7) */
TX_THREAD     MCP_RX_Thread;

#if PHASE7_VCU_LCD_BRIDGE
TX_THREAD     VcuLcd_Thread;
#endif
TX_SEMAPHORE  mcp_rx_semaphore;

#if PHASE25_CAN_TX
/* SPI1 op 직렬화 뮤텍스 (task 6A.2, 리뷰 #1): RX 드레인 ↔ CAN TX */
TX_MUTEX      mcp_spi_mutex;
#endif

#if PHASE4_UART_PIPELINE
/* Consumer 스레드 (task 12.1) */
TX_THREAD     UART_Thread;        /* Priority 5,  Stack 1024B */
TX_THREAD     UART_RX_Thread;     /* Priority 7,  Stack 1024B */
TX_THREAD     SD_Logger_Thread;   /* Priority 10, Stack 2048B */
#endif

#if PHASE8_GPS
TX_THREAD     GPS_Thread;
#endif

#if PHASE9_ADC_SENSOR
TX_THREAD     ADC_Thread;
#endif

#if PHASE4_UART_PIPELINE

#define UART_QUEUE_DEPTH   40
#define SD_QUEUE_DEPTH     100
static ULONG uart_queue_storage[UART_QUEUE_DEPTH * LOG_QUEUE_MSG_WORDS];
static ULONG sd_queue_storage[SD_QUEUE_DEPTH * LOG_QUEUE_MSG_WORDS];
#endif

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static VOID App_StackErrorHandler(TX_THREAD *thread_ptr);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */
static VOID App_StackErrorHandler(TX_THREAD *thread_ptr)
{
  uint32_t id = BOOT_THREAD_OTHER;
  if      (thread_ptr == &MCP_RX_Thread)    id = BOOT_THREAD_MCP_RX;
#if PHASE4_UART_PIPELINE
  else if (thread_ptr == &UART_Thread)      id = BOOT_THREAD_UART;
  else if (thread_ptr == &UART_RX_Thread)   id = BOOT_THREAD_UART_RX;
  else if (thread_ptr == &SD_Logger_Thread) id = BOOT_THREAD_SD_LOGGER;
#endif
#if PHASE7_VCU_LCD_BRIDGE
  else if (thread_ptr == &VcuLcd_Thread)    id = BOOT_THREAD_VCU_LCD;
#endif
#if PHASE8_GPS
  else if (thread_ptr == &GPS_Thread)       id = BOOT_THREAD_GPS;
#endif
#if PHASE9_ADC_SENSOR
  else if (thread_ptr == &ADC_Thread)       id = BOOT_THREAD_ADC;
#endif

  LED_ERR_GPIO_Port->BSRR = LED_ERR_Pin;
  TAMP->BKP2R = BOOT_STACK_MARKER | id;
  NVIC_SystemReset();
}
/* USER CODE END 0 */

/**
  * @brief  Application ThreadX Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT App_ThreadX_Init(VOID *memory_ptr)
{
  UINT ret = TX_SUCCESS;

  /* USER CODE BEGIN App_ThreadX_MEM_POOL */

  /* USER CODE END App_ThreadX_MEM_POOL */

  /* USER CODE BEGIN App_ThreadX_Init */
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL *)memory_ptr;
  CHAR         *stack_ptr;

  if (tx_thread_stack_error_notify(App_StackErrorHandler) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }

  /* MCP2518FD 수신 세마포어 (EXTI가 set → MCP_RX_Thread가 get), 초기 count 0 */
  if (tx_semaphore_create(&mcp_rx_semaphore, "MCP_RX_Sem", 0) != TX_SUCCESS)
  {
    return TX_SEMAPHORE_ERROR;
  }

  /* MCP_RX_Thread: MCP2518FD 수신 전용 (Priority 6, Stack 1024B).
     블로킹 SPI 수신을 EXTI ISR 밖으로 분리한다 (Issue #1 / Req 9.7). */
  if (tx_byte_allocate(byte_pool, (VOID **)&stack_ptr, 1024, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
  if (tx_thread_create(&MCP_RX_Thread, "MCP_RX_Thread",
                       ExtCAN_RxThread_Entry, 0,
                       stack_ptr, 1024,
                       6, 6, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }

#if PHASE25_CAN_TX
  /* SPI1 뮤텍스 (task 6A.2) — 우선순위 상속으로 pri6(RX)↔pri7(TX) 역전 방지 */
  if (tx_mutex_create(&mcp_spi_mutex, "MCP_SPI_Mutex", TX_INHERIT) != TX_SUCCESS)
  {
    return TX_MUTEX_ERROR;
  }
#endif

#if PHASE4_UART_PIPELINE
  if (tx_queue_create(&UART_Queue, "UART_Queue", LOG_QUEUE_MSG_WORDS,
                      uart_queue_storage, sizeof(uart_queue_storage)) != TX_SUCCESS)
  {
    return TX_QUEUE_ERROR;
  }
  if (tx_queue_create(&SD_Queue, "SD_Queue", LOG_QUEUE_MSG_WORDS,
                      sd_queue_storage, sizeof(sd_queue_storage)) != TX_SUCCESS)
  {
    return TX_QUEUE_ERROR;
  }

  /* uart_tx_complete_sem 생성 + (Phase 4.5) RX DMA 시작 (task 10.1) */
  UART_Transmitter_Init();

  /* ── UART_Thread (Priority 5, 1024B) ── */
  if (tx_byte_allocate(byte_pool, (VOID **)&stack_ptr, 1024, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
  if (tx_thread_create(&UART_Thread, "UART_Thread",
                       UART_Thread_Entry, 0,
                       stack_ptr, 1024,
                       5, 5, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }

  /* ── UART_RX_Thread (Priority 7, 1024B) — Phase 4.5 전엔 슬립 유지 ── */
  if (tx_byte_allocate(byte_pool, (VOID **)&stack_ptr, 1024, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
  if (tx_thread_create(&UART_RX_Thread, "UART_RX_Thread",
                       UART_RX_Thread_Entry, 0,
                       stack_ptr, 1024,
                       7, 7, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }

  /* ── SD_Logger_Thread (Priority 10, 2048B) —
        Phase 5 전(SD_Logger_Init 미호출)에는 수신-드롭만 수행 ── */
  if (tx_byte_allocate(byte_pool, (VOID **)&stack_ptr, 2048, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
  if (tx_thread_create(&SD_Logger_Thread, "SD_Logger_Thread",
                       SD_Logger_Thread_Entry, 0,
                       stack_ptr, 2048,
                       10, 10, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }
#endif /* PHASE4_UART_PIPELINE */

#if PHASE7_VCU_LCD_BRIDGE
  if (tx_byte_allocate(byte_pool, (VOID **)&stack_ptr, 1024, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
  if (tx_thread_create(&VcuLcd_Thread, "VcuLcd_Thread",
                       VcuLcd_Thread_Entry, 0,
                       stack_ptr, 1024,
                       8, 8, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }


#endif

#if PHASE8_GPS
  if (tx_byte_allocate(byte_pool, (VOID **)&stack_ptr, 1024, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
  GPS_Init();

  if (tx_thread_create(&GPS_Thread, "GPS_Thread",
                       GPS_Thread_Entry, 0,
                       stack_ptr, 1024,
                       11, 11, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }
#endif

#if PHASE9_ADC_SENSOR
  if (tx_byte_allocate(byte_pool, (VOID **)&stack_ptr, 1024, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
  if (tx_thread_create(&ADC_Thread, "ADC_Thread",
                       ADC_Sensor_Thread_Entry, 0,
                       stack_ptr, 1024,
                       12, 12, TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }
#endif
  /* USER CODE END App_ThreadX_Init */

  return ret;
}

  /**
  * @brief  Function that implements the kernel's initialization.
  * @param  None
  * @retval None
  */
void MX_ThreadX_Init(void)
{
  /* USER CODE BEGIN Before_Kernel_Start */

  /* USER CODE END Before_Kernel_Start */

  tx_kernel_enter();

  /* USER CODE BEGIN Kernel_Start_Error */

  /* USER CODE END Kernel_Start_Error */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
