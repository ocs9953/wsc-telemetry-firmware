/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_filex.c
  * @author  MCD Application Team
  * @brief   FileX applicative file
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
#include "app_filex.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "phase_config.h"
#include "types.h"
#include "main.h"
#include "sdmmc.h"
#include "fx_stm32_sd_driver.h"
#if PHASE5_SD_LOGGING
#include "packet_builder.h"
#include "rtc_manager.h"
#include "sd_logger.h"
#include <string.h>
#endif
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* Main thread stack size */
#define FX_APP_THREAD_STACK_SIZE         2048
/* Main thread priority */
#define FX_APP_THREAD_PRIO               10
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

#undef  FX_APP_THREAD_PRIO
#define FX_APP_THREAD_PRIO               12

#define SD_MOUNT_RETRY_TICKS             (5 * TX_TIMER_TICKS_PER_SECOND)
#define SD_MOUNT_REPORT_EVERY            12u

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* Main thread global data structures.  */
TX_THREAD       fx_app_thread;

/* Buffer for FileX FX_MEDIA sector cache. */
ALIGN_32BYTES (uint32_t fx_sd_media_memory[FX_STM32_SD_DEFAULT_SECTOR_SIZE / sizeof(uint32_t)]);
/* Define FileX global data structures.  */
FX_MEDIA        sdio_disk;

/* 복구 요청 신호 — SD_Logger 가 올리고 이 스레드가 받아 재마운트한다.
   복구를 이 스레드로 옮긴 이유: HAL_SD_Init() 안에는 한도가 0xFFFFFFFF ms 인
   대기가 있어 죽은 카드에서는 영영 돌아오지 않을 수 있다. 로거 스레드가 직접
   하면 그동안 SD 생존 비트를 세우지 못해 멀쩡한 보드가 워치독으로 리셋된다.
   이 스레드는 생존 비트 집계 대상이 아니라 여기서 막혀도 보드는 계속 산다. */
TX_SEMAPHORE     sd_recover_sem;
volatile uint8_t g_sd_media_ok = 0u;   /* 1 = 매체가 열려 있다 */
/* 재마운트가 주변장치를 강제 리셋한 횟수. g_sd_hard_reset(전송 타임아웃 정리
   경로)과 원인이 달라 따로 센다. */
volatile uint32_t g_sd_remount_reset = 0u;

/* USER CODE BEGIN PV */
#if PHASE5_SD_LOGGING
/* FileX fault-tolerant 작업 버퍼 (시스템 점검 발견: FX_ENABLE_FAULT_TOLERANT가
   fx_user.h에 켜져 있었으나 fx_fault_tolerant_enable()을 아무도 호출하지 않아
   실제로는 미보호 상태였음). 주행 중 전원 컷 시 FAT/디렉터리 손상을 막는다.
   4096B(≥ 512B 섹터 요구, ST 예제 권장치) — RTOS byte pool 대비 미미한 비용. */
#define FX_FAULT_TOLERANT_BUFFER_SIZE  4096
ALIGN_32BYTES (uint8_t fx_fault_tolerant_buffer[FX_FAULT_TOLERANT_BUFFER_SIZE]);
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/

/* Main thread entry function.  */
void fx_app_thread_entry(ULONG thread_input);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/**
  * @brief  Application FileX Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
*/
UINT MX_FileX_Init(VOID *memory_ptr)
{
  UINT ret = FX_SUCCESS;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;
  VOID *pointer;

/* USER CODE BEGIN MX_FileX_MEM_POOL */

/* USER CODE END MX_FileX_MEM_POOL */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*Allocate memory for the main thread's stack*/
  ret = tx_byte_allocate(byte_pool, &pointer, FX_APP_THREAD_STACK_SIZE, TX_NO_WAIT);

/* Check FX_APP_THREAD_STACK_SIZE allocation*/
  if (ret != FX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }

/* Create the main thread.  */
  ret = tx_thread_create(&fx_app_thread, FX_APP_THREAD_NAME, fx_app_thread_entry, 0, pointer, FX_APP_THREAD_STACK_SIZE,
                         FX_APP_THREAD_PRIO, FX_APP_PREEMPTION_THRESHOLD, FX_APP_THREAD_TIME_SLICE, FX_APP_THREAD_AUTO_START);

/* Check main thread creation */
  if (ret != FX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }

/* USER CODE BEGIN MX_FileX_Init */

/* USER CODE END MX_FileX_Init */

/* Initialize FileX.  */
  fx_system_initialize();

/* USER CODE BEGIN MX_FileX_Init 1*/

/* USER CODE END MX_FileX_Init 1*/

  return ret;
}

/**
 * @brief  Main thread entry.
 * @param thread_input: ULONG user argument used by the thread entry
 * @retval none
*/
 void fx_app_thread_entry(ULONG thread_input)
 {

  UINT sd_status = FX_SUCCESS;

/* USER CODE BEGIN fx_app_thread_entry 0*/
  uint8_t  sd_fail_reported = 0;
  uint32_t sd_retry_count   = 0;
  UINT     sd_last_err      = 0;

sd_mount_retry:
  {
    extern volatile uint8_t g_sd_hw_ok;
    extern volatile uint8_t g_init_nonfatal;
    extern SYSTEM_STATE     SystemState;

    while (!g_sd_hw_ok)
    {
      if (!sd_fail_reported || (sd_retry_count % SD_MOUNT_REPORT_EVERY) == 0u)
      {
        sd_fail_reported = 1;
#if PHASE5_SD_LOGGING
        {
          extern TX_QUEUE UART_Queue;
          LOG err_pkt;
          uint8_t val[8] = {0};
          uint32_t det = (uint32_t)sd_last_err;
          memcpy(&val[0], &det,            4);
          memcpy(&val[4], &sd_retry_count, 4);
          PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_SD_ERR,
                               (uint8_t)(sd_last_err ? ERR_SD_INIT : ERR_SD_NOT_DETECTED),
                               val, 8);
          ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
          memcpy(msg, &err_pkt, sizeof(LOG));
          tx_queue_send(&UART_Queue, msg, TX_NO_WAIT);
        }
#else
        (void)sd_last_err;
#endif
        HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
        SystemState.sd_ok = 0;
        SystemState.error = 1;
      }

      tx_thread_sleep(SD_MOUNT_RETRY_TICKS);
      sd_retry_count++;

  (void)tx_semaphore_delete(&sd_rx_semaphore);
  (void)tx_semaphore_delete(&sd_tx_semaphore);

      HAL_SD_DeInit(&hsd1);
      g_init_nonfatal = 1u;
      MX_SDMMC1_SD_Init();
      if (g_init_nonfatal != 2u)
      {
        g_sd_hw_ok  = 1u;
        sd_last_err = 0;
      }
      g_init_nonfatal = 0u;
    }
  }
/* USER CODE END fx_app_thread_entry 0*/

/* Open the SD disk driver */
  sd_status =  fx_media_open(&sdio_disk, FX_SD_VOLUME_NAME, fx_stm32_sd_driver, (VOID *)FX_NULL, (VOID *) fx_sd_media_memory, sizeof(fx_sd_media_memory));

/* Check the media open sd_status */
  if (sd_status != FX_SUCCESS)
  {
     /* USER CODE BEGIN SD DRIVER get info error */
    {
      extern volatile uint8_t g_sd_hw_ok;
      extern SYSTEM_STATE     SystemState;

      HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
      SystemState.sd_ok = 0;
      SystemState.error = 1;

      sd_last_err = sd_status;
      g_sd_hw_ok  = 0u;
      goto sd_mount_retry;
    }
    /* USER CODE END SD DRIVER get info error */
  }

/* USER CODE BEGIN fx_app_thread_entry 1*/
  if (sd_fail_reported)
  {
    extern SYSTEM_STATE SystemState;
    HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_RESET);
    SystemState.sd_ok = 1;
    SystemState_RefreshBoardLed();
  }

#if PHASE5_SD_LOGGING
  /* Fault-tolerant 활성화 — media_open 성공 직후, 첫 fx_file_write 이전에
     호출해야 한다(로그 파일이 아직 없어야 함). 실패해도 치명적이지 않으므로
     (FAT 손상 방지가 안 될 뿐, 정상 쓰기는 계속 가능) LOG만 남기고 진행. */
  {
    UINT ft_status = fx_fault_tolerant_enable(&sdio_disk,
                                              fx_fault_tolerant_buffer,
                                              sizeof(fx_fault_tolerant_buffer));
    if (ft_status != FX_SUCCESS)
    {
      extern TX_QUEUE UART_Queue;
      LOG warn_pkt;
      uint8_t val[8] = {0};
      uint32_t st32 = (uint32_t)ft_status;
      memcpy(&val[0], &st32, 4);
      PacketBuilder_Create(&warn_pkt, LOG_WARNING, SRC_SD_ERR,
                           (uint8_t)ERR_SD_INIT, val, 8);
      ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
      memcpy(msg, &warn_pkt, sizeof(LOG));
      tx_queue_send(&UART_Queue, msg, TX_NO_WAIT);
    }
  }

  /* task 16.2: 부팅 시각 획득 → SD_Logger 초기화 (파일 생성/오픈) */
  {
    DATETIME boot_time;
    if (RTC_Manager_GetDateTime(&boot_time) != HAL_OK)
    {
      /* RTC 실패 시 기본 시각으로 파일명 생성 (로깅은 계속) */
      boot_time.year = 0;  boot_time.month  = 1; boot_time.date   = 1;
      boot_time.hour = 0;  boot_time.minute = 0; boot_time.second = 0;
    }
    (void)fx_system_date_set(2000u + (UINT)boot_time.year,
                             (UINT)boot_time.month, (UINT)boot_time.date);
    (void)fx_system_time_set((UINT)boot_time.hour,
                             (UINT)boot_time.minute, (UINT)boot_time.second);

    SD_Logger_Init(&sdio_disk, &boot_time);
  }
#endif
  g_sd_media_ok = 1u;
  if (tx_semaphore_create(&sd_recover_sem, "SD recover", 0) == TX_SUCCESS)
  {
    for (;;)
    {
      if (tx_semaphore_get(&sd_recover_sem, TX_WAIT_FOREVER) != TX_SUCCESS)
        continue;

      g_sd_media_ok = 0u;

      while (FX_SD_Remount() != FX_SUCCESS)
      {
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
      }

      while (tx_semaphore_get(&sd_recover_sem, TX_NO_WAIT) == TX_SUCCESS) { }

      g_sd_media_ok = 1u;
    }
  }

/* USER CODE END fx_app_thread_entry 1*/
  }

/* USER CODE BEGIN 1 */
UINT FX_SD_Remount(void)
{
  extern volatile uint8_t g_sd_hw_ok;
  extern volatile uint8_t g_init_nonfatal;

  (void)fx_media_abort(&sdio_disk);
  (void)fx_media_close(&sdio_disk);

  HAL_SD_DeInit(&hsd1);

  (void)tx_semaphore_delete(&sd_rx_semaphore);
  (void)tx_semaphore_delete(&sd_tx_semaphore);

  __HAL_RCC_SDMMC1_FORCE_RESET();
  __HAL_RCC_SDMMC1_RELEASE_RESET();
  hsd1.State = HAL_SD_STATE_RESET;
  g_sd_remount_reset++;

  g_init_nonfatal = 1u;
  MX_SDMMC1_SD_Init();
  if (g_init_nonfatal == 2u)
  {
    g_init_nonfatal = 0u;
    g_sd_hw_ok = 0u;
    return FX_IO_ERROR;
  }
  g_init_nonfatal = 0u;
  g_sd_hw_ok = 1u;

  g_sd_recover_req = 0u;

  g_sd_timeout_run = 0u;

  UINT st = fx_media_open(&sdio_disk, FX_SD_VOLUME_NAME, fx_stm32_sd_driver,
                          (VOID *)FX_NULL, (VOID *)fx_sd_media_memory,
                          sizeof(fx_sd_media_memory));
  if (st != FX_SUCCESS)
    return st;

#if PHASE5_SD_LOGGING
  (void)fx_fault_tolerant_enable(&sdio_disk, fx_fault_tolerant_buffer,
                                 sizeof(fx_fault_tolerant_buffer));
#endif
  return FX_SUCCESS;
}


/* USER CODE END 1 */
