/**
  ******************************************************************************
  * @file    uart_transmitter.c
  * @brief   UART_Thread(송신, task 10.1) + UART_RX_Thread(수신, task 13A.1)
  *
  *          송신: UART_Queue → [0x01|LOG 16B] → COBS(≤18B) + 0x00 → DMA
  *          수신: DMA+IDLE → 링버퍼 → 0x00 분리 → COBS 디코드 → 타입 0x02
  *                → ExtCAN_Transmit() (Phase 4.5)
  *          Requirements: 7.1~7.11, 9.1, 9.3, 9.5, 9.7, 13.1
  ******************************************************************************
  */
#include "uart_transmitter.h"
#include "app_threadx.h"
#include "phase_config.h"
#include "types.h"
#include "packet_builder.h"
#include "cobs.h"
#include "timer_scheduler.h"
#include "ext_can_controller.h"
#include "main.h"
#include "usart.h"
#include "iwdg.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External references
 * --------------------------------------------------------------------------- */
extern TX_QUEUE     UART_Queue;
extern TX_QUEUE     SD_Queue;
extern SYSTEM_STATE SystemState;

#if PHASE45_UART_RX_CAN_TX
extern DMA_HandleTypeDef handle_GPDMA1_Channel1;   /* USART2_RX DMA (usart.c) */
#endif

/* ---------------------------------------------------------------------------
 * 상수
 * --------------------------------------------------------------------------- */
#define UART_FRAME_TYPE_CAN_DATA  0x01   /* 보드→지상국: LOG 패킷           */
#define UART_FRAME_TYPE_TEXT_MSG  0x02   /* 지상국→보드: LCD 텍스트 메시지  */

#define UART_RX_QUEUE_TIMEOUT     2000   /* tick=1ms → 2초 (Req 9.3)       */
#define UART_RX_ALIVE_TIMEOUT      500

#define UART_DMA_DONE_TIMEOUT     1000   /* DMA 완료 대기 1초              */
#define UART_TIMEOUT_WARN_COUNT   5      /* 연속 타임아웃 경고 임계        */

/* ---------------------------------------------------------------------------
 * Static objects
 * --------------------------------------------------------------------------- */
static TX_SEMAPHORE uart_tx_complete_sem;

static volatile uint8_t uart_tx_dma_error = 0;

static volatile uint8_t uart_tx_in_flight = 0;

/* DMA가 전송 중 접근하므로 static 유지 필수 */
static uint8_t uart_tx_frame[20];        /* COBS(17B)≤18B + 0x00 delimiter */
_Static_assert(sizeof(uart_tx_frame) >= (1u + sizeof(LOG)) + 1u + 1u,
               "uart_tx_frame must hold COBS(type+LOG) plus delimiter");

#if PHASE45_UART_RX_CAN_TX
static TX_SEMAPHORE uart_rx_sem;
static uint8_t rx_dma_buf[64];           /* ReceiveToIdle DMA 버퍼          */

volatile uint32_t g_uart_rx_frames = 0;
volatile uint32_t g_uart_rx_msgs   = 0;
volatile uint32_t g_uart_rx_drops  = 0;

/* RX 링버퍼 (콜백 ISR이 쓰고 UART_RX_Thread가 읽음) */
#define RX_RING_SIZE 256                 /* 2의 거듭제곱                    */
_Static_assert((RX_RING_SIZE & (RX_RING_SIZE - 1)) == 0,
               "RX_RING_SIZE must be a power of two for the index mask");
static uint8_t          rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;        /* ISR 쓰기 위치                   */
static volatile uint16_t rx_tail;        /* 스레드 읽기 위치                */

#define UART_RX_FRAME_BUF_SIZE      64
#define UART_RX_TEXT_MSG_MAX_BYTES  (UART_RX_FRAME_BUF_SIZE - 2)
#endif

/* ---------------------------------------------------------------------------
 * Private helpers
 * --------------------------------------------------------------------------- */
static void UART_SendLogToSDQueue(const LOG *pkt)
{
    ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
    memcpy(msg, pkt, sizeof(LOG));
    TelemQueue_Send(&SD_Queue, msg);
}

/**
 * @brief  루프 1회마다의 IWDG 처리.
 * @note   작업 목록 10.1은 '타임아웃 시 리프레시'지만, 데이터가 계속 흐르면
 *         타임아웃 분기를 영영 안 타서 IWDG(4초)가 만료된다. Phase 6(Alive
 *         Flag) 전까지는 성공/타임아웃 모든 경로에서 리프레시한다.
 *         (작업 목록 알려진 이슈에 편차 기록)
 */
static inline void UART_ThreadKickWatchdog(void)
{
#if PHASE6_SAFETY
    Telemetry_AliveSet(ALIVE_BIT_UART);
#else
    HAL_IWDG_Refresh(&hiwdg);
#endif
}

/* ---------------------------------------------------------------------------
 * UART_Transmitter_Init  (task 10.1 / 12.1)
 *
 * App_ThreadX_Init(커널 진입 전)에서 호출된다.
 * --------------------------------------------------------------------------- */
void UART_Transmitter_Init(void)
{
    tx_semaphore_create(&uart_tx_complete_sem, "UART_TX_Done", 0);

    SystemState.uart_ok = 1;

    SystemState_RefreshBoardLed();

#if PHASE45_UART_RX_CAN_TX
    tx_semaphore_create(&uart_rx_sem, "UART_RX_Sem", 0);
    rx_head = rx_tail = 0;

    /* IDLE 라인 기반 가변길이 수신 시작 (GPDMA1_Channel1) */
    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart2, rx_dma_buf,
                                     sizeof(rx_dma_buf)) == HAL_OK)
    {
        /* Half-Transfer 이벤트 불필요 → 비활성 */
        __HAL_DMA_DISABLE_IT(&handle_GPDMA1_Channel1, DMA_IT_HT);
    }
#endif
}

/* ---------------------------------------------------------------------------
 * UART_Thread_Entry  (task 10.1, Priority 5)
 * --------------------------------------------------------------------------- */
void UART_Thread_Entry(ULONG thread_input)
{
    (void)thread_input;
    ULONG    msg[LOG_QUEUE_MSG_WORDS];
    uint32_t timeout_count = 0;

    for (;;)
    {
        UINT st = tx_queue_receive(&UART_Queue, msg, UART_RX_QUEUE_TIMEOUT);

        UART_ThreadKickWatchdog();

        if (st == TX_SUCCESS)
        {
            timeout_count = 0;

            /* [타입 0x01][LOG 16B] → COBS → +0x00 delimiter */
            uint8_t plain[17];
            plain[0] = UART_FRAME_TYPE_CAN_DATA;
            memcpy(&plain[1], msg, sizeof(LOG));

            size_t n = COBS_Encode(plain, sizeof(plain), uart_tx_frame);
            uart_tx_frame[n++] = 0x00;

            /* DMA 실패 로그 래치 — 지속 실패 시 폭주 방지 (성공하면 해제) */
            static uint8_t dma_err_latch = 0;

            uart_tx_dma_error = 0;

            while (tx_semaphore_get(&uart_tx_complete_sem, TX_NO_WAIT) == TX_SUCCESS)
            {  }

            if (HAL_UART_Transmit_DMA(&huart2, uart_tx_frame,
                                      (uint16_t)n) == HAL_OK)
            {
                uart_tx_in_flight = 1;

                UINT sem_st = tx_semaphore_get(&uart_tx_complete_sem,
                                               UART_DMA_DONE_TIMEOUT);
                uart_tx_in_flight = 0;

                if (sem_st != TX_SUCCESS || uart_tx_dma_error)
                {
                    HAL_UART_AbortTransmit(&huart2);

                    if (!dma_err_latch)
                    {
                        dma_err_latch = 1;
                        LOG err_pkt;
                        uint8_t val[8] = {0};
                        uint32_t hal_err = HAL_UART_GetError(&huart2);
                        memcpy(&val[0], &hal_err, 4);
                        PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_UART_ERR,
                                             (uint8_t)ERR_UART_DMA, val, 8);
                        UART_SendLogToSDQueue(&err_pkt);
                    }
                    SystemState_MarkFault(FAULT_IDX_UART);
                    HAL_GPIO_WritePin(LED_RFD900x_GPIO_Port, LED_RFD900x_Pin,
                                      GPIO_PIN_SET);
                    SystemState.uart_ok = 0;
                    SystemState_RefreshBoardLed();
                }
                else
                {
                    if (!SystemState.uart_ok &&
                        SystemState_FaultHoldExpired(FAULT_IDX_UART))
                    {
                        HAL_GPIO_WritePin(LED_RFD900x_GPIO_Port, LED_RFD900x_Pin,
                                          GPIO_PIN_RESET);
                        SystemState.uart_ok = 1;
                        SystemState_RefreshBoardLed();
                    }
                    dma_err_latch = 0;
                }
            }
            else
            {
                uart_tx_in_flight = 0;
                SystemState_MarkFault(FAULT_IDX_UART);

                if (!dma_err_latch)
                {
                    dma_err_latch = 1;
                    LOG err_pkt;
                    uint8_t val[8] = {0};
                    uint32_t hal_err = HAL_UART_GetError(&huart2);
                    memcpy(&val[0], &hal_err, 4);
                    PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_UART_ERR,
                                         (uint8_t)ERR_UART_DMA, val, 8);
                    UART_SendLogToSDQueue(&err_pkt);
                }
                HAL_GPIO_WritePin(LED_RFD900x_GPIO_Port, LED_RFD900x_Pin,
                                  GPIO_PIN_SET);
                SystemState.uart_ok = 0;
                SystemState_RefreshBoardLed();
            }
        }
        else /* 큐 타임아웃 (2초 무데이터) */
        {
            timeout_count++;
            if (timeout_count == UART_TIMEOUT_WARN_COUNT)
            {
                LOG warn_pkt;
                uint8_t val[8] = {0};
                memcpy(&val[0], &timeout_count, 4);
                PacketBuilder_Create(&warn_pkt, LOG_WARNING, SRC_UART_ERR,
                                     (uint8_t)ERR_TIMEOUT, val, 8);
                UART_SendLogToSDQueue(&warn_pkt);
                timeout_count = 0;   /* 재누적 */
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * UART_RX_Thread_Entry  (task 13A.1, Priority 7)
 *
 * PHASE45 비활성 시: 스레드가 생성되어도 대기만 한다 (배선은 12.1에서 생성).
 * --------------------------------------------------------------------------- */
void UART_RX_Thread_Entry(ULONG thread_input)
{
    (void)thread_input;

#if PHASE45_UART_RX_CAN_TX
    /* 0x00 delimiter 기준 프레임 조립 버퍼 (COBS 인코딩 최대 길이 여유) */
    static uint8_t frame_buf[UART_RX_FRAME_BUF_SIZE];
    static uint8_t decode_buf[UART_RX_FRAME_BUF_SIZE];
    uint16_t frame_len = 0;
    uint8_t  frame_ovf = 0;

    for (;;)
    {
        UINT st = tx_semaphore_get(&uart_rx_sem, UART_RX_ALIVE_TIMEOUT);

#if PHASE6_SAFETY
        Telemetry_AliveSet(ALIVE_BIT_UARTRX);
#endif
        if (st != TX_SUCCESS)
            continue;   /* 주기 타임아웃 — alive만 갱신 */

        /* 링버퍼 드레인 */
        while (rx_tail != rx_head)
        {
            uint8_t b = rx_ring[rx_tail];
            rx_tail = (uint16_t)((rx_tail + 1) & (RX_RING_SIZE - 1));

            if (b != 0x00)
            {
                if (frame_len < sizeof(frame_buf))
                {
                    frame_buf[frame_len++] = b;
                }
                else
                {
                    frame_ovf = 1;
                }
                continue;
            }

            /* --- 0x00 delimiter: 프레임 완성 --- */
            if (frame_ovf)
            {
                frame_ovf = 0;
                frame_len = 0;
                g_uart_rx_drops++;
                continue;
            }
            if (frame_len == 0)
                continue;            /* 연속 delimiter 무시 */

            size_t dec_len = COBS_Decode(frame_buf, frame_len, decode_buf);
            frame_len = 0;

            if (dec_len < 2)
            {
                g_uart_rx_drops++;
                continue;            /* 형식 오류/빈 페이로드 — 폐기 (Req 7.11) */
            }
            g_uart_rx_frames++;

            /* 타입 헤더 확인: 0x02 = 지상국 텍스트 메시지 → LCD로 CAN TX */
            if (decode_buf[0] == UART_FRAME_TYPE_TEXT_MSG)
            {
#if PHASE25_CAN_TX
                if (ExtCAN_Transmit(&decode_buf[1], dec_len - 1) == HAL_OK)
                    g_uart_rx_msgs++;
#endif
            }
            /* 0x01(CAN 데이터)은 지상국 방향 전용 — 수신 측에서는 무시 */
        }
    }
#else
    /* Phase 4.5 전: 수신 처리 없음 — 슬립 유지 */
    for (;;)
    {
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
#endif
}

/* ---------------------------------------------------------------------------
 * HAL 콜백 훅 (main.c USER CODE 4에서 호출)
 * --------------------------------------------------------------------------- */

/** @brief DMA 송신 완료 → 세마포어 put (Req 7.5)
 *  @note  ceiling 1: 중복 put(에러+완료 경쟁)으로 카운트가 부풀어 다음
 *         전송의 완료 대기가 무력화되는 것을 방지 */
void UART_Transmitter_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        uart_tx_in_flight = 0;
        tx_semaphore_ceiling_put(&uart_tx_complete_sem, 1);
    }
}

void UART_Transmitter_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2)
        return;

    uint32_t hal_err = HAL_UART_GetError(huart);

    if (hal_err & HAL_UART_ERROR_DMA)
    {
        uint8_t tx_dma_fault = (huart->hdmatx != NULL) &&
                               (huart->hdmatx->ErrorCode != HAL_DMA_ERROR_NONE);

        LOG err_pkt;
        uint8_t val[8] = {0};
        memcpy(&val[0], &hal_err, 4);
        val[4] = tx_dma_fault;
        val[5] = uart_tx_in_flight;
        PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_UART_ERR,
                             (uint8_t)ERR_UART_DMA, val, 8);
        UART_SendLogToSDQueue(&err_pkt);

        if (uart_tx_in_flight)
        {
            uart_tx_dma_error = 1;
            tx_semaphore_ceiling_put(&uart_tx_complete_sem, 1);
        }
        else if (tx_dma_fault)
        {
            SystemState_MarkFault(FAULT_IDX_UART);
            HAL_GPIO_WritePin(LED_RFD900x_GPIO_Port, LED_RFD900x_Pin, GPIO_PIN_SET);
            SystemState.uart_ok = 0;
            SystemState_RefreshBoardLed();
        }
    }

#if PHASE45_UART_RX_CAN_TX
    /* RX 라인 에러(ORE/FE/NE/PE) 시 HAL이 수신을 중단하므로 재무장.
       손상 프레임은 COBS/CRC 검증에서 걸러진다. */
    if (huart->RxState == HAL_UART_STATE_READY)
    {
        if (HAL_UARTEx_ReceiveToIdle_DMA(&huart2, rx_dma_buf,
                                         sizeof(rx_dma_buf)) == HAL_OK)
        {
            __HAL_DMA_DISABLE_IT(&handle_GPDMA1_Channel1, DMA_IT_HT);
        }
    }
#endif
}

/** @brief RX IDLE 이벤트 — 수신 바이트를 링버퍼로 복사 후 재무장 (task 13A.1) */
void UART_Transmitter_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
#if PHASE45_UART_RX_CAN_TX
    if (huart->Instance != USART2)
        return;

    for (uint16_t i = 0; i < Size; i++)
    {
        uint16_t next = (uint16_t)((rx_head + 1) & (RX_RING_SIZE - 1));
        if (next == rx_tail)
            break;               /* 링 가득 — 이후 바이트 드롭 (프레임 오류로 폐기됨) */
        rx_ring[rx_head] = rx_dma_buf[i];
        rx_head = next;
    }

    /* one-shot 수신 재무장 */
    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart2, rx_dma_buf,
                                     sizeof(rx_dma_buf)) == HAL_OK)
    {
        __HAL_DMA_DISABLE_IT(&handle_GPDMA1_Channel1, DMA_IT_HT);
    }

    tx_semaphore_put(&uart_rx_sem);
#else
    (void)huart;
    (void)Size;
#endif
}
