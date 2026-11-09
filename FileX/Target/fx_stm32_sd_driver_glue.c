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

#include "fx_stm32_sd_driver.h"

TX_SEMAPHORE sd_tx_semaphore;
TX_SEMAPHORE sd_rx_semaphore;

extern SD_HandleTypeDef hsd1;
#if (FX_STM32_SD_INIT == 1)
extern void MX_SDMMC1_SD_Init(void);
#endif

/* USER CODE BEGIN  0 */

#define FX_SD_ABORT_SPIN_LIMIT  2000u

#define FX_SD_TIMEOUT_RUN_LIMIT  2u

#define FX_SD_WEDGED_TICKS       (TX_TIMER_TICKS_PER_SECOND / 4u)
#define FX_SD_FASTFAIL_TICKS     1u

_Static_assert(SDMMC_SWDATATIMEOUT <= 1000U,
               "SDMMC_SWDATATIMEOUT is back to the HAL default - CubeMX regeneration dropped "
               "the override in stm32u5xx_hal_conf.h. Re-add: #define SDMMC_SWDATATIMEOUT 1000U");

volatile uint32_t g_sd_io_timeout = 0;
volatile uint8_t  g_sd_recover_req = 0;
volatile uint32_t g_sd_hard_reset  = 0;
volatile uint32_t g_sd_timeout_run = 0;
#include "phase_config.h"
#include "timer_scheduler.h"

ULONG fx_sd_io_wait_ticks(void)
{
  return (g_sd_timeout_run >= FX_SD_TIMEOUT_RUN_LIMIT)
           ? (ULONG)FX_SD_FASTFAIL_TICKS
           : (ULONG)FX_STM32_SD_DEFAULT_TIMEOUT;
}

ULONG fx_sd_status_wait_ticks(void)
{
  if ((g_sd_recover_req != 0u) || (hsd1.State == HAL_SD_STATE_RESET))
  {
    return 1u;
  }
  return (ULONG)((750u * TX_TIMER_TICKS_PER_SECOND) / 1000u);
}

static inline void sd_io_progress(void)
{
#if PHASE6_SAFETY
  Telemetry_AliveSet(ALIVE_BIT_SD);
#endif
  g_sd_timeout_run = 0u;
}

void fx_sd_io_timeout(void)
{
  __HAL_SD_DISABLE_IT(&hsd1, SDMMC_IT_DATAEND | SDMMC_IT_DCRCFAIL | SDMMC_IT_DTIMEOUT |
                             SDMMC_IT_TXUNDERR | SDMMC_IT_RXOVERR);
  hsd1.Instance->IDMACTRL = SDMMC_DISABLE_IDMA;
  __SDMMC_CMDTRANS_DISABLE(hsd1.Instance);
  hsd1.Instance->DCTRL &= ~SDMMC_DCTRL_DTEN;
  __HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_DATA_FLAGS);

  uint32_t spin = 0u;
  while ((hsd1.Instance->STA & (SDMMC_STA_DPSMACT | SDMMC_STA_CPSMACT)) != 0u)
  {
    if (++spin > FX_SD_ABORT_SPIN_LIMIT)
    {
      __HAL_RCC_SDMMC1_FORCE_RESET();
      __HAL_RCC_SDMMC1_RELEASE_RESET();
      hsd1.State = HAL_SD_STATE_RESET;
      g_sd_hard_reset++;
      break;
    }
  }

  hsd1.ErrorCode = HAL_SD_ERROR_TIMEOUT;
  hsd1.Context   = SD_CONTEXT_NONE;
  if (hsd1.State != HAL_SD_STATE_RESET)
    hsd1.State = HAL_SD_STATE_READY;

  while (tx_semaphore_get(&sd_tx_semaphore, TX_NO_WAIT) == TX_SUCCESS) { }
  while (tx_semaphore_get(&sd_rx_semaphore, TX_NO_WAIT) == TX_SUCCESS) { }

  g_sd_recover_req = 1u;
  if (g_sd_timeout_run < 0xFFFFFFFFu) g_sd_timeout_run++;
  g_sd_io_timeout++;
}


#define FX_SD_CMD13_TICKS   (TX_TIMER_TICKS_PER_SECOND / 20u)

volatile uint32_t g_sd_cmd13_timeout = 0;

static INT fx_sd_card_ready_bounded(void)
{
  SDMMC_CmdInitTypeDef cmd;
  uint32_t start;
  uint32_t sta;

  __HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_CMD_FLAGS);

  cmd.Argument         = (uint32_t)(hsd1.SdCard.RelCardAdd << 16u);
  cmd.CmdIndex         = SDMMC_CMD_SEND_STATUS;
  cmd.Response         = SDMMC_RESPONSE_SHORT;
  cmd.WaitForInterrupt = SDMMC_WAIT_NO;
  cmd.CPSM             = SDMMC_CPSM_ENABLE;
  (void)SDMMC_SendCommand(hsd1.Instance, &cmd);

  start = (uint32_t)tx_time_get();
  for (;;)
  {
    sta = hsd1.Instance->STA;
    if ((sta & (SDMMC_FLAG_CCRCFAIL | SDMMC_FLAG_CTIMEOUT | SDMMC_FLAG_CMDREND)) != 0u)
    {
      break;
    }
    if (((uint32_t)tx_time_get() - start) > (uint32_t)FX_SD_CMD13_TICKS)
    {
      CLEAR_BIT(hsd1.Instance->CMD, SDMMC_CMD_CPSMEN);
      __HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_CMD_FLAGS);
      g_sd_cmd13_timeout++;
      g_sd_recover_req = 1u;
      return 1;
    }
  }

  if ((sta & SDMMC_FLAG_CMDREND) == 0u)
  {
    __HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_CMD_FLAGS);
    return 1;
  }

  __HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_CMD_FLAGS);

  return (((SDMMC_GetResponse(hsd1.Instance, SDMMC_RESP1) >> 9u) & 0x0Fu)
          == (uint32_t)HAL_SD_CARD_TRANSFER) ? 0 : 1;
}

/* USER CODE END  0 */

/**
* @brief Initializes the SD IP instance
* @param UINT instance SD instance to initialize
* @retval 0 on success error value otherwise
*/
INT fx_stm32_sd_init(UINT instance)
{
  INT ret = 0;

  /* USER CODE BEGIN PRE_FX_SD_INIT */
  UNUSED(instance);
  /* USER CODE END PRE_FX_SD_INIT */

#if (FX_STM32_SD_INIT == 1)
	MX_SDMMC1_SD_Init();
#endif

  /* USER CODE BEGIN POST_FX_SD_INIT */

  /* USER CODE END POST_FX_SD_INIT */

  return ret;
}

/**
* @brief Deinitializes the SD IP instance
* @param UINT instance SD instance to deinitialize
* @retval 0 on success error value otherwise
*/
INT fx_stm32_sd_deinit(UINT instance)
{
  INT ret = 0;

  /* USER CODE BEGIN PRE_FX_SD_DEINIT */
  UNUSED(instance);
  /* USER CODE END PRE_FX_SD_DEINIT */

#if (FX_STM32_SD_INIT == 1)
  if(HAL_SD_DeInit(&hsd1) != HAL_OK)
  {
    ret = 1;
  }
#endif

  /* USER CODE BEGIN POST_FX_SD_DEINIT */

  /* USER CODE END POST_FX_SD_DEINIT */

  return ret;
}

/**
* @brief Check the SD IP status.
* @param UINT instance SD instance to check
* @retval 0 when ready 1 when busy
*/
INT fx_stm32_sd_get_status(UINT instance)
{
  INT ret = 0;

  /* USER CODE BEGIN PRE_GET_STATUS */
  UNUSED(instance);

  if ((g_sd_recover_req != 0u) || (hsd1.State == HAL_SD_STATE_RESET))
  {
    return 1;
  }
  {
    static uint32_t busy_since = 0u;
    uint32_t sta = hsd1.Instance->STA;

    if ((sta & (SDMMC_STA_DPSMACT | SDMMC_STA_CPSMACT)) != 0u)
    {
      uint32_t now = (uint32_t)tx_time_get();
      if (busy_since == 0u)
      {
        busy_since = (now != 0u) ? now : 1u;
      }
      else if ((now - busy_since) > (uint32_t)FX_SD_WEDGED_TICKS)
      {
        busy_since = 0u;
        g_sd_recover_req = 1u;
      }
      return 1;
    }
    else
    {
      busy_since = 0u;
    }
  }

  return fx_sd_card_ready_bounded();
  /* USER CODE END PRE_GET_STATUS */

  if(HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
  {
    ret = 1;
  }

  /* USER CODE BEGIN POST_GET_STATUS */

  /* USER CODE END POST_GET_STATUS */

  return ret;
}

/**
* @brief Read Data from the SD device into a buffer.
* @param UINT instance SD IP instance to read from.
* @param UINT *buffer buffer into which the data is to be read.
* @param UINT start_block the first block to start reading from.
* @param UINT total_blocks total number of blocks to read.
* @retval 0 on success error code otherwise
*/
INT fx_stm32_sd_read_blocks(UINT instance, UINT *buffer, UINT start_block, UINT total_blocks)
{
  INT ret = 0;

  /* USER CODE BEGIN PRE_READ_BLOCKS */
  /* USER CODE END PRE_READ_BLOCKS */

  if(HAL_SD_ReadBlocks_DMA(&hsd1, (uint8_t *)buffer, start_block, total_blocks) != HAL_OK)
  {
    ret = 1;
  }

  /* USER CODE BEGIN POST_READ_BLOCKS */

  /* USER CODE END POST_READ_BLOCKS */

  return ret;
}

/**
* @brief Write data buffer into the SD device.
* @param UINT instance SD IP instance to write into.
* @param UINT *buffer buffer to write into the SD device.
* @param UINT start_block the first block to start writing into.
* @param UINT total_blocks total number of blocks to write.
* @retval 0 on success error code otherwise
*/
INT fx_stm32_sd_write_blocks(UINT instance, UINT *buffer, UINT start_block, UINT total_blocks)
{
  INT ret = 0;

  /* USER CODE BEGIN PRE_WRITE_BLOCKS */
  /* USER CODE END PRE_WRITE_BLOCKS */

  if(HAL_SD_WriteBlocks_DMA(&hsd1, (uint8_t *)buffer, start_block, total_blocks) != HAL_OK)
  {
    ret = 1;
  }

  /* USER CODE BEGIN POST_WRITE_BLOCKS */

  /* USER CODE END POST_WRITE_BLOCKS */

  return ret;
}

/**
* @brief SD DMA Tx Transfer completed callbacks
* @param Instance the sd instance
* @retval None
*/
void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hsd)
{
  /* USER CODE BEGIN PRE_TX_CMPLT */
  sd_io_progress();
  /* USER CODE END PRE_TX_CMPLT */

  tx_semaphore_put(&sd_tx_semaphore);

  /* USER CODE BEGIN POST_TX_CMPLT */

  /* USER CODE END POST_TX_CMPLT */
}

/**
* @brief SD DMA Rx Transfer completed callbacks
* @param Instance the sd instance
* @retval None
*/
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hsd)
{

   /* USER CODE BEGIN PRE_RX_CMPLT */
  sd_io_progress();
  /* USER CODE END PRE_RX_CMPLT */

  tx_semaphore_put(&sd_rx_semaphore);

  /* USER CODE BEGIN POST_RX_CMPLT */

  /* USER CODE END POST_RX_CMPLT */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
