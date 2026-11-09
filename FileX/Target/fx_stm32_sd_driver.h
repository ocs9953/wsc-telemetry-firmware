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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef FX_STM32_SD_DRIVER_H
#define FX_STM32_SD_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "fx_api.h"

#include "stm32u5xx_hal.h"
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

extern TX_SEMAPHORE sd_tx_semaphore;
extern TX_SEMAPHORE sd_rx_semaphore;

/* I/O 대기가 한도를 넘겼을 때의 정리 (glue). 블로킹하지 않는다. */
void fx_sd_io_timeout(void);
/* 전송 완료를 기다릴 시간(틱). 연속 타임아웃 뒤에는 짧게 끊어 호출자를 풀어 준다. */
ULONG fx_sd_io_wait_ticks(void);
extern volatile uint32_t g_sd_io_timeout;   /* 관측용: 타임아웃 정리 횟수 */
extern volatile uint8_t  g_sd_recover_req;  /* 1 = 주변장치 재초기화가 필요하다 */
extern volatile uint32_t g_sd_timeout_run;  /* 연속 타임아웃 — 재초기화 시 0 으로 */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */
/* Default timeout used to wait for fx operations */
#define FX_STM32_SD_DEFAULT_TIMEOUT                           (10 * TX_TIMER_TICKS_PER_SECOND)

/* let the filex low-level driver initialize the SD driver */
#define FX_STM32_SD_INIT                                      0

/* Enable the cache maintenance, needed when using SD DMA
 * and accessing buffers in cacheable area
 * this is valid only for CM7 based products or those
 * with dedicated cache IP.
 * For STM32U5 this flag should be always set to 0 unless external
 * memories are being used.
 */
#define FX_STM32_SD_CACHE_MAINTENANCE                         0

/* Use the SD DMA API */
#define FX_STM32_SD_DMA_API                                   1

/* SDIO instance to be used by FileX */
#define FX_STM32_SD_INSTANCE                                  0

/* Default sector size, used by the driver */
#define FX_STM32_SD_DEFAULT_SECTOR_SIZE                       512

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */
#undef  FX_STM32_SD_DEFAULT_TIMEOUT
#define FX_STM32_SD_DEFAULT_TIMEOUT   fx_sd_status_wait_ticks()

ULONG fx_sd_status_wait_ticks(void);
extern volatile uint32_t g_sd_cmd13_timeout;
/* USER CODE END EM */

/* Define the macro to get the current time in ticks */
#define FX_STM32_SD_CURRENT_TIME()                            tx_time_get()

/* Macro called before initializing the SD driver
 * e.g. create a semaphore used for transfer notification */
/* USER CODE BEGIN FX_STM32_SD_PRE_INIT_TX */

#define FX_STM32_SD_PRE_INIT(_media_ptr)                do { \
                                                          if ((tx_semaphore_create(&sd_rx_semaphore, "sd rx transfer semaphore", 0) != TX_SUCCESS) || \
                                                              (tx_semaphore_create(&sd_tx_semaphore, "sd tx transfer semaphore", 0) != TX_SUCCESS))  \
                                                          { \
                                                            _media_ptr->fx_media_driver_status = FX_IO_ERROR; \
                                                          } \
                                                        } while(0)

/* USER CODE END FX_STM32_SD_PRE_INIT_TX */

/* Macro called after initializing the SD driver */
/* USER CODE BEGIN FX_STM32_SD_POST_INIT */

#define FX_STM32_SD_POST_INIT(_media_ptr)

/* USER CODE END FX_STM32_SD_POST_INIT */

/* Macro called after the SD deinit */

/* USER CODE BEGIN FX_STM32_SD_POST_DEINIT_TX */

#define FX_STM32_SD_POST_DEINIT(_media_ptr)             do { \
                                                          tx_semaphore_delete(&sd_rx_semaphore); \
                                                          tx_semaphore_delete(&sd_tx_semaphore); \
                                                        } while(0)

/* USER CODE END FX_STM32_SD_POST_DEINIT_TX */

/* Macro called after the abort request */
/* USER CODE BEGIN FX_STM32_SD_POST_ABORT */

#define FX_STM32_SD_POST_ABORT(_media_ptr)              do {                                                           tx_semaphore_delete(&sd_rx_semaphore);                                                           tx_semaphore_delete(&sd_tx_semaphore);                                                         } while(0)

/* USER CODE END FX_STM32_SD_POST_ABORT */

/* Macro called before performing read operation */
/* USER CODE BEGIN FX_STM32_SD_PRE_READ_TRANSFER_DMA */

#define FX_STM32_SD_PRE_READ_TRANSFER(_media_ptr) do { \
                                                          while (tx_semaphore_get(&sd_rx_semaphore, TX_NO_WAIT) == TX_SUCCESS) { } \
                                                        } while(0)
/* USER CODE END FX_STM32_SD_PRE_READ_TRANSFER_DMA */

/* Macro called after performing read operation */
/* USER CODE BEGIN FX_STM32_SD_POST_READ_TRANSFER_TX */

#define FX_STM32_SD_POST_READ_TRANSFER(_media_ptr)

/* USER CODE END FX_STM32_SD_POST_READ_TRANSFER_TX */

/* Macro for read error handling */
/* USER CODE BEGIN FX_STM32_SD_READ_TRANSFER_ERROR_TX */

#define FX_STM32_SD_READ_TRANSFER_ERROR(_status_)

/* USER CODE END FX_STM32_SD_READ_TRANSFER_ERROR_TX */

/* Define how to notify about Read completion operation */
/* USER CODE BEGIN FX_STM32_SD_READ_CPLT_NOTIFY_TX */

#define FX_STM32_SD_READ_CPLT_NOTIFY()                  do { \
                                                          if(tx_semaphore_get(&sd_rx_semaphore, fx_sd_io_wait_ticks()) != TX_SUCCESS) \
                                                            { \
                                                              fx_sd_io_timeout(); \
                                                              return FX_IO_ERROR; \
                                                            } \
                                                        } while(0)

/* USER CODE END FX_STM32_SD_READ_CPLT_NOTIFY_TX */

/* Define how to notify about write completion operation */
/* USER CODE BEGIN FX_STM32_SD_WRITE_CPLT_NOTIFY_TX */

#define FX_STM32_SD_WRITE_CPLT_NOTIFY()                 do { \
                                                          if(tx_semaphore_get(&sd_tx_semaphore, fx_sd_io_wait_ticks()) != TX_SUCCESS) \
                                                            { \
                                                              fx_sd_io_timeout(); \
                                                              return FX_IO_ERROR; \
                                                            } \
                                                        } while(0)

/* USER CODE END FX_STM32_SD_WRITE_CPLT_NOTIFY_TX */

/* Macro called before performing write operation */
/* USER CODE BEGIN FX_STM32_SD_PRE_WRITE_TRANSFER_TX */

#define FX_STM32_SD_PRE_WRITE_TRANSFER(_media_ptr) do { \
                                                          while (tx_semaphore_get(&sd_tx_semaphore, TX_NO_WAIT) == TX_SUCCESS) { } \
                                                        } while(0)

/* USER CODE END FX_STM32_SD_PRE_WRITE_TRANSFER_TX */

/* Macro called after performing write operation */
/* USER CODE BEGIN FX_STM32_SD_POST_WRITE_TRANSFER */

#define FX_STM32_SD_POST_WRITE_TRANSFER					FX_STM32_SD_POST_READ_TRANSFER

/* USER CODE END FX_STM32_SD_POST_WRITE_TRANSFER */

/* Macro for write error handling */
/* USER CODE BEGIN FX_STM32_SD_WRITE_TRANSFER_ERROR */

#define FX_STM32_SD_WRITE_TRANSFER_ERROR				FX_STM32_SD_READ_TRANSFER_ERROR

/* USER CODE END FX_STM32_SD_WRITE_TRANSFER_ERROR */

/* Exported functions prototypes ---------------------------------------------*/

INT fx_stm32_sd_init(UINT instance);
INT fx_stm32_sd_deinit(UINT instance);

INT fx_stm32_sd_get_status(UINT instance);

INT fx_stm32_sd_read_blocks(UINT instance, UINT *buffer, UINT start_block, UINT total_blocks);
INT fx_stm32_sd_write_blocks(UINT instance, UINT *buffer, UINT start_block, UINT total_blocks);

VOID fx_stm32_sd_driver(FX_MEDIA *media_ptr);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif

#endif /* FX_STM32_SD_DRIVER_H */
