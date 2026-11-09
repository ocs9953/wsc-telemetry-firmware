/**
  ******************************************************************************
  * @file    ext_can_controller.c
  * @brief   MCP2518FD SPI 제어 구현 (Emandhal/MCP251XFD 래퍼)
  *
  *          Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 1.6
  ******************************************************************************
  */
#include "ext_can_controller.h"
#include "app_threadx.h"
#include "phase_config.h"
#include "main.h"
#include "spi.h"
#include "iwdg.h"
#include "types.h"
#include "packet_builder.h"
#include "vcu_lcd_bridge.h"
#include "MCP251XFD.h"
#include "tx_api.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External references
 * --------------------------------------------------------------------------- */
extern VehicleData_t VehicleData;
extern SYSTEM_STATE  SystemState;
extern TX_QUEUE      UART_Queue;
extern TX_QUEUE      SD_Queue;
extern TX_SEMAPHORE  mcp_rx_semaphore;   /* EXTI(set) → MCP_RX_Thread(get) */

#if PHASE25_CAN_TX
extern TX_MUTEX      mcp_spi_mutex;      /* SPI1 op 직렬화 (RX 드레인 ↔ TX)  */
#endif

/* ---------------------------------------------------------------------------
 * SPI1 NSS Pin — Software GPIO Chip Select (CubeMX: CAN_CS, PA4)
 * --------------------------------------------------------------------------- */
#define SPI1_NSS_Pin        CAN_CS_Pin
#define SPI1_NSS_GPIO_Port  CAN_CS_GPIO_Port

/* ---------------------------------------------------------------------------
 * MCP2518FD(Bus B) 역할: 향후 저우선순위 주변장치용 버스로 예약.
 *   - 확정 용도: 지상국→LCD 메시지 TX (req 8)
 *   - 미확정   : LCD 외 일부 센서 RX 가능성 (대상·ID 확정 시 추가)
 * 과거 예제코드의 BMS/MPPT/Motor RX 파싱(0x100/0x200/0x300)은 FDCAN1
 * (can_receiver.c)과 중복되는 잔재였으므로 제거했다. 차량 센서 RX는 FDCAN1 전담.
 * --------------------------------------------------------------------------- */

#define EXT_CAN_ID_LCD_TX   0x500

/* ---------------------------------------------------------------------------
 * SPI timeout for HAL_SPI calls (ms)
 * --------------------------------------------------------------------------- */
#define MCP_SPI_TIMEOUT_MS  10

/* RX 세마포어 폴백 폴링 주기 — 드레인 중 SPI 일시오류로 빠져나와 INT(active-low)가
   물린 채 새 falling 엣지가 없는 'stuck' 상황, 부팅/경쟁으로 놓친 통지를 주기적으로
   복구한다. (TX_WAIT_FOREVER 대신 유한 타임아웃 사용) */
#define MCP_RX_POLL_TICKS   (TX_TIMER_TICKS_PER_SECOND / 10)   /* ~100ms */

/* 동일 SPI 에러 LOG 최소 간격 (드라이버 콜백이 전송 1건마다 호출되어 발생하는
   LOG 폭주 방지) */
#define MCP_SPI_ERR_LOG_MIN_INTERVAL_MS  500

/* ---------------------------------------------------------------------------
 * Static objects — MCP251XFD device instance & buffers
 * --------------------------------------------------------------------------- */
static MCP251XFD mcp2518fd;
static uint8_t   mcp_spi_buf[MCP251XFD_TRANS_BUF_SIZE];

/* RX message & payload buffers */
static MCP251XFD_CANMessage mcp_rx_msg;
static uint8_t              mcp_rx_payload[8];  /* Classic CAN max 8 bytes */

/* Initialization flag */
static bool mcp_initialized = false;

/* SPI error retry counter (for error LOG value field) */
static uint32_t spi_retry_count = 0;

static bool mcp_asleep = false;
volatile uint32_t g_mcp_tec        = 0;  volatile uint32_t g_mcp_err_reads  = 0;
volatile uint32_t g_mcp_opmode     = 0xFF;
volatile uint32_t g_mcp_mode_recover = 0;
volatile uint32_t g_mcp_rec        = 0;
volatile uint32_t g_mcp_rx_frames   = 0;
volatile uint32_t g_mcp_rx_last_id  = 0;
volatile uint32_t g_mcp_rx_last_dlc = 0;

volatile uint32_t g_mcp_rx_ovf      = 0;
volatile uint32_t g_mcp_gap_max_ms  = 0;
volatile uint32_t g_mcp_gap_last_ms = 0;
volatile uint32_t g_mcp_seq_last    = 0;
volatile uint32_t g_mcp_seq_missed  = 0;
volatile uint32_t g_mcp_seq_frames  = 0;
volatile uint32_t g_mcp_seq_restarts = 0;

#define EXTCAN_SEQ_RESTART_STEP  0x10000u

#define EXTCAN_ID_TEST_HEARTBEAT  0x51Fu

volatile uint32_t g_mcp_nbtcfg = 0;
volatile uint32_t g_mcp_osc    = 0;

#define MCP_WAKE_STABILIZE_TICKS  4
#define MCP_WAKE_STABILIZE_MS     4u

#define MCP_IDLE_SLEEP_MS       500u
#define MCP_TX_CHUNK_GAP_TICKS  5u

#define MCP_WAKEUP_FILTER       MCP251XFD_T10FILTER_170ns

static uint32_t mcp_last_activity_ms = 0;

static void ExtCAN_Sleep(void);static bool ExtCAN_EnterNormalMode(void);
#if PHASE25_CAN_TX
static bool ExtCAN_Wake(void);
static bool ExtCAN_WaitTxDrain(void);
#endif

/* ---------------------------------------------------------------------------
 * Private helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief  Send a LOG packet to both UART_Queue and SD_Queue (non-blocking).
 */
static void ExtCAN_SendLogToQueues(const LOG *pkt)
{
    ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
    memcpy(msg, pkt, sizeof(LOG));
    TelemQueue_Send(&UART_Queue, msg);
    TelemQueue_Send(&SD_Queue, msg);
}

/**
 * @brief  Generate SPI error LOG and turn on LED_ERR(PE11).
 * @param  err_key  ERR_SPI_TIMEOUT or ERR_SPI_TRANSFER
 * @param  hal_err  HAL SPI error code
 */
static void ExtCAN_ReportSPIError(ERR_KEY err_key, uint32_t hal_err)
{
    /* LED/상태는 매번 갱신(멱등). LOG는 폭주 방지를 위해 레이트리밋한다.
       드라이버 콜백이 SPI 전송 1건마다 호출되므로, 지속 SPI 불량 시
       동일 LOG가 수십 개 쏟아지는 것을 억제한다. */
    HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
    SystemState.mcp_ok = 0;
    SystemState.error  = 1;
    SystemState_RefreshBoardLed();

    static uint32_t last_log_tick = 0;
    uint32_t now = HAL_GetTick();
    if (last_log_tick != 0 &&
        (now - last_log_tick) < MCP_SPI_ERR_LOG_MIN_INTERVAL_MS)
    {
        return;  /* 최근에 보고함 → LOG 생략 (retry count는 계속 누적되어 다음 보고에 반영) */
    }
    last_log_tick = now;

    LOG err_pkt;
    uint8_t val[8] = {0};

    /* value: HAL SPI error code (4B) + retry count (4B) */
    memcpy(&val[0], &hal_err, 4);
    memcpy(&val[4], &spi_retry_count, 4);

    PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_SPI_ERR,
                         (uint8_t)err_key, val, 8);
    ExtCAN_SendLogToQueues(&err_pkt);
}

/* ---------------------------------------------------------------------------
 * HAL SPI Callback implementations for MCP251XFD driver (Req 4.2, 4.3)
 *
 * 드라이버는 하드웨어 독립적 설계로, 아래 콜백을 디바이스 구조체에 등록한다.
 * --------------------------------------------------------------------------- */

/**
 * @brief  fnSPI_Init 콜백: SPI 초기화
 *         CubeMX MX_SPI1_Init()에서 이미 초기화되므로 ERR_OK 반환.
 */
static eERRORRESULT MCP_SPIInit(void *pIntDev, uint8_t chipSelect,
                                 const uint32_t sckFreq)
{
    (void)pIntDev;
    (void)chipSelect;
    (void)sckFreq;
    return ERR_OK;
}

/**
 * @brief  fnSPI_Transfer 콜백: HAL_SPI 기반 전이중 전송 (Req 4.2, 4.3)
 *         NSS(Chip Select)는 Software GPIO 제어.
 * @param  txData  송신 데이터 (NULL이면 0xFF 더미 전송)
 * @param  rxData  수신 데이터 (NULL이면 수신 무시)
 * @param  size    전송 바이트 수
 */
static eERRORRESULT MCP_SPITransfer(void *pIntDev, uint8_t chipSelect,
                                     uint8_t *txData, uint8_t *rxData,
                                     size_t size)
{
    (void)pIntDev;
    (void)chipSelect;
    HAL_StatusTypeDef status;

    /* NSS Low — select device */
    HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_RESET);

    if (txData != NULL && rxData != NULL)
    {
        status = HAL_SPI_TransmitReceive(&hspi1, txData, rxData,
                                          (uint16_t)size, MCP_SPI_TIMEOUT_MS);
    }
    else if (txData != NULL)
    {
        status = HAL_SPI_Transmit(&hspi1, txData,
                                   (uint16_t)size, MCP_SPI_TIMEOUT_MS);
    }
    else if (rxData != NULL)
    {
        /* 더미 0xFF 전송하며 수신.
           size가 mcp_spi_buf(16B)보다 클 수 있으므로, NSS를 유지한 채
           버퍼 크기 단위로 분할 전송하여 송신 버퍼 초과 읽기를 방지한다. */
        memset(mcp_spi_buf, 0xFF, sizeof(mcp_spi_buf));
        size_t   remaining = size;
        uint8_t *rxp       = rxData;
        status = HAL_OK;
        while (remaining > 0 && status == HAL_OK)
        {
            uint16_t chunk = (remaining < sizeof(mcp_spi_buf))
                             ? (uint16_t)remaining
                             : (uint16_t)sizeof(mcp_spi_buf);
            status = HAL_SPI_TransmitReceive(&hspi1, mcp_spi_buf, rxp,
                                              chunk, MCP_SPI_TIMEOUT_MS);
            rxp       += chunk;
            remaining -= chunk;
        }
    }
    else
    {
        status = HAL_OK;
    }

    /* NSS High — deselect device */
    HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_SET);

    if (status == HAL_TIMEOUT)
    {
        spi_retry_count++;
        ExtCAN_ReportSPIError(ERR_SPI_TIMEOUT,
                              HAL_SPI_GetError(&hspi1));
        return ERR__SPI_COMM_ERROR;
    }
    else if (status != HAL_OK)
    {
        spi_retry_count++;
        ExtCAN_ReportSPIError(ERR_SPI_TRANSFER,
                              HAL_SPI_GetError(&hspi1));
        return ERR__SPI_COMM_ERROR;
    }

    if (mcp_initialized && !SystemState.mcp_ok)
    {
        SystemState.mcp_ok = 1;
        SystemState_RefreshBoardLed();
    }

    return ERR_OK;
}

/**
 * @brief  fnGetCurrentms 콜백: HAL_GetTick() 매핑
 */
static uint32_t MCP_GetCurrentms(void)
{
    return HAL_GetTick();
}

/* ---------------------------------------------------------------------------
 * ExtCAN_ProcessMessage — MCP2518FD 수신 프레임 처리 (현재 stub)
 *
 * MCP2518FD(Bus B)는 향후 저우선순위 주변장치 버스로 예약됨. 과거 예제코드의
 * BMS/MPPT/Motor 파싱은 FDCAN1(can_receiver.c)과 중복되는 잔재여서 제거했다.
 * 실제 센서 RX 대상·ID(LCD 외, 미확정)가 확정되면 여기에 파싱을 추가한다.
 * 그때 VehicleData를 쓰면 FDCAN ISR과의 경쟁 때문에 TX_DISABLE/TX_RESTORE 보호 필요.
 * (드레인 머신/RX 스레드 골격은 향후 사용 위해 유지 — 현재는 수신 프레임 드롭)
 * --------------------------------------------------------------------------- */
static void ExtCAN_ProcessMessage(uint32_t can_id,
                                  const uint8_t *data, uint8_t len)
{
    g_mcp_rx_frames++;
    g_mcp_rx_last_id = can_id;
    g_mcp_rx_last_dlc = len;

    {
        uint32_t now = HAL_GetTick();
        if (g_mcp_gap_last_ms != 0u)
        {
            uint32_t gap = now - g_mcp_gap_last_ms;
            if (gap > g_mcp_gap_max_ms) g_mcp_gap_max_ms = gap;
        }
        g_mcp_gap_last_ms = (now != 0u) ? now : 1u;
    }

    if (can_id == EXTCAN_ID_TEST_HEARTBEAT)
    {
        if (len >= 4u)
        {
            uint32_t seq = (uint32_t)data[0] | ((uint32_t)data[1] << 8)
                         | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
            if (g_mcp_seq_frames != 0u)
            {
                uint32_t step = seq - g_mcp_seq_last;

                if (step > EXTCAN_SEQ_RESTART_STEP) g_mcp_seq_restarts++;
                else if (step > 1u)                 g_mcp_seq_missed += (step - 1u);
            }
            g_mcp_seq_last = seq;
            g_mcp_seq_frames++;
        }
        return;
    }

    (void)VcuLcd_OnLcdFrame(can_id, data, len);
}

/* ---------------------------------------------------------------------------
 * ExtCAN_Init  (Req 4.1, 4.2, 1.6)
 *
 * 시퀀스: 디바이스 구조체 설정 → Init_MCP251XFD() → ConfigureFIFO()
 *       → ConfigureFilter() → StartCAN20()
 * --------------------------------------------------------------------------- */
HAL_StatusTypeDef ExtCAN_Init(void)
{
    eERRORRESULT err;

    /* ── NSS 핀 초기 상태: High (deselected) ── */
    HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_SET);

    /* ── MCP251XFD 디바이스 구조체 설정 ── */
    memset(&mcp2518fd, 0, sizeof(mcp2518fd));
    mcp2518fd.UserDriverData  = NULL;
    mcp2518fd.InterfaceDevice = &hspi1;
    mcp2518fd.SPI_ChipSelect  = 0;
    mcp2518fd.SPIClockSpeed   = 10000000;       /* 10MHz (160MHz / 16) */
    mcp2518fd.fnSPI_Init      = MCP_SPIInit;
    mcp2518fd.fnSPI_Transfer  = MCP_SPITransfer;
    mcp2518fd.fnGetCurrentms  = MCP_GetCurrentms;
    mcp2518fd.fnComputeCRC16  = NULL;            /* CRC 모드 미사용 */

    /* ── MCP251XFD Config: Classic CAN 500kbps ── */
    MCP251XFD_Config mcp_config;
    memset(&mcp_config, 0, sizeof(mcp_config));
    mcp_config.XtalFreq       = 0;
    mcp_config.OscFreq        = 40000000;        /* 40MHz 외부 오실레이터 */
    mcp_config.SysclkConfig   = MCP251XFD_SYSCLK_IS_CLKIN;
    mcp_config.ClkoPinConfig  = MCP251XFD_CLKO_SOF;
    mcp_config.SYSCLK_Result  = NULL;
    mcp_config.NominalBitrate = 500000;              /* 500kbps Classic CAN */
    mcp_config.DataBitrate    = MCP251XFD_NO_CANFD;  /* Classic CAN → 데이터 비트레이트 없음 */
    mcp_config.BitTimeStats   = NULL;
    mcp_config.Bandwidth      = MCP251XFD_NO_DELAY;
    mcp_config.ControlFlags   = MCP251XFD_CAN_RESTRICTED_MODE_ON_ERROR;
    mcp_config.GPIO0PinMode   = MCP251XFD_PIN_AS_INT0_TX;
    mcp_config.GPIO1PinMode   = MCP251XFD_PIN_AS_INT1_RX;
    mcp_config.INTsOutMode    = MCP251XFD_PINS_PUSHPULL_OUT;
    mcp_config.TXCANOutMode   = MCP251XFD_PINS_PUSHPULL_OUT;
    mcp_config.SysInterruptFlags = MCP251XFD_INT_RX_EVENT
                                 | MCP251XFD_INT_BUS_ERROR_EVENT
                                 | MCP251XFD_INT_BUS_WAKEUP_EVENT;

    {
        uint8_t osc_cfg = 0;
        if ((MCP251XFD_ReadSFR8(&mcp2518fd, RegMCP251XFD_OSC_CONFIG, &osc_cfg) == ERR_NONE) &&
            ((osc_cfg & MCP251XFD_SFR_OSC8_OSCDIS) != 0u))
        {
            osc_cfg &= (uint8_t)(~MCP251XFD_SFR_OSC8_OSCDIS);
            (void)MCP251XFD_WriteSFR8(&mcp2518fd, RegMCP251XFD_OSC_CONFIG, osc_cfg);
            HAL_Delay(MCP_WAKE_STABILIZE_MS);
        }
    }

    /* 1단계: 디바이스 초기화 (SPI 통신 확인 + 컨트롤러 리셋) */
    err = Init_MCP251XFD(&mcp2518fd, &mcp_config);
    if (err != ERR_OK) return HAL_ERROR;

    /* ── RX FIFO 설정 ── */
    MCP251XFD_FIFO mcp_rx_fifo;
    memset(&mcp_rx_fifo, 0, sizeof(mcp_rx_fifo));
    mcp_rx_fifo.Name           = MCP251XFD_FIFO1;
    mcp_rx_fifo.Size           = MCP251XFD_FIFO_8_MESSAGE_DEEP;
    mcp_rx_fifo.Payload        = MCP251XFD_PAYLOAD_8BYTE;
    mcp_rx_fifo.Direction      = MCP251XFD_RECEIVE_FIFO;
    mcp_rx_fifo.Attempts       = MCP251XFD_THREE_ATTEMPTS;
    mcp_rx_fifo.Priority       = MCP251XFD_MESSAGE_TX_PRIORITY16;
    mcp_rx_fifo.ControlFlags   = MCP251XFD_FIFO_NO_RTR_RESPONSE;
    mcp_rx_fifo.InterruptFlags = MCP251XFD_FIFO_RECEIVE_FIFO_NOT_EMPTY_INT
                               | MCP251XFD_FIFO_OVERFLOW_INT;
    mcp_rx_fifo.RAMInfos       = NULL;

    /* 2단계: RX FIFO 설정 */
    err = MCP251XFD_ConfigureFIFO(&mcp2518fd, &mcp_rx_fifo);
    if (err != ERR_OK) return HAL_ERROR;

#if PHASE25_CAN_TX
    /* ── TX FIFO 설정 (task 6A.1): FIFO2, 8메시지, 8B payload ── */
    MCP251XFD_FIFO mcp_tx_fifo;
    memset(&mcp_tx_fifo, 0, sizeof(mcp_tx_fifo));
    mcp_tx_fifo.Name           = MCP251XFD_FIFO2;
    mcp_tx_fifo.Size           = MCP251XFD_FIFO_8_MESSAGE_DEEP;
    mcp_tx_fifo.Payload        = MCP251XFD_PAYLOAD_8BYTE;
    mcp_tx_fifo.Direction      = MCP251XFD_TRANSMIT_FIFO;
    mcp_tx_fifo.Attempts       = MCP251XFD_THREE_ATTEMPTS;
    mcp_tx_fifo.Priority       = MCP251XFD_MESSAGE_TX_PRIORITY16;
    mcp_tx_fifo.ControlFlags   = MCP251XFD_FIFO_NO_CONTROL_FLAGS;
    mcp_tx_fifo.InterruptFlags = MCP251XFD_FIFO_NO_INTERRUPT_FLAGS;
    mcp_tx_fifo.RAMInfos       = NULL;

    err = MCP251XFD_ConfigureFIFO(&mcp2518fd, &mcp_tx_fifo);
    if (err != ERR_OK) return HAL_ERROR;
#endif

    MCP251XFD_Filter mcp_filter0;
    memset(&mcp_filter0, 0, sizeof(mcp_filter0));
    mcp_filter0.Filter         = MCP251XFD_FILTER0;
    mcp_filter0.EnableFilter   = true;
    mcp_filter0.Match          = MCP251XFD_MATCH_ONLY_SID;
    mcp_filter0.PointTo        = MCP251XFD_FIFO1;
    mcp_filter0.AcceptanceID   = 0x510;
    mcp_filter0.AcceptanceMask = 0x7F0;
    mcp_filter0.ExtendedID     = false;          /* Standard 11-bit ID */

    /* 3단계: 수신 필터 설정 */
    err = MCP251XFD_ConfigureFilter(&mcp2518fd, &mcp_filter0);
    if (err != ERR_OK) return HAL_ERROR;

    /* 4단계: Classic CAN 2.0 모드 시작 */
    err = ExtCAN_EnterNormalMode() ? ERR_OK : ERR__CONFIGURATION;
    if (err != ERR_OK) return HAL_ERROR;

    (void)MCP251XFD_ReadSFR32(&mcp2518fd, RegMCP251XFD_CiNBTCFG, (uint32_t *)&g_mcp_nbtcfg);
    (void)MCP251XFD_ReadSFR8 (&mcp2518fd, RegMCP251XFD_OSC,      (uint8_t  *)&g_mcp_osc);

    mcp_initialized    = true;
    spi_retry_count    = 0;
    SystemState.mcp_ok = 1;
    SystemState_RefreshBoardLed();

    if (MCP251XFD_ConfigureSleepMode(&mcp2518fd, false,
                                     MCP_WAKEUP_FILTER, true) == ERR_OK)
    {
        mcp_last_activity_ms = HAL_GetTick();
        ExtCAN_Sleep();
    }
    return HAL_OK;
}

/* ---------------------------------------------------------------------------
 * ExtCAN_DrainRxFIFO — FIFO1을 빌 때까지 수신 → VehicleData 갱신
 *
 * 스레드 컨텍스트(MCP_RX_Thread)에서만 호출된다. 블로킹 SPI를 ISR 밖으로
 * 분리했으므로(Req 9.7), HAL_SPI timeout(HAL_GetTick 기반)도 정상 동작한다.
 * EXTI는 edge-trigger이므로, 한 번 깨어나면 FIFO가 빌 때까지 모두 드레인하여
 * 처리 도중 도착한 메시지의 유실을 방지한다.
 * --------------------------------------------------------------------------- */
static bool ExtCAN_EnterNormalMode(void)
{
    if (MCP251XFD_RequestOperationMode(&mcp2518fd,
                                       MCP251XFD_NORMAL_CAN20_MODE, true) != ERR_OK)
        return false;

    eMCP251XFD_OperationMode om = MCP251XFD_CONFIGURATION_MODE;
    if (MCP251XFD_GetActualOperationMode(&mcp2518fd, &om) != ERR_OK)
        return false;

    g_mcp_opmode = (uint32_t)om;
    return (om == MCP251XFD_NORMAL_CAN20_MODE);
}

static void ExtCAN_Sleep(void)
{
    if (!mcp_initialized || mcp_asleep) return;
    if (MCP251XFD_EnterSleepMode(&mcp2518fd) == ERR_OK)
        mcp_asleep = true;
}

#if PHASE25_CAN_TX
static bool ExtCAN_Wake(void)
{
    if (!mcp_initialized) return false;

    if (!mcp_asleep)
    {
        eMCP251XFD_OperationMode om = MCP251XFD_CONFIGURATION_MODE;
        if (MCP251XFD_GetActualOperationMode(&mcp2518fd, &om) != ERR_OK)
            return false;

        g_mcp_opmode = (uint32_t)om;
        if (om == MCP251XFD_NORMAL_CAN20_MODE)
            return true;

        g_mcp_mode_recover++;
        return ExtCAN_EnterNormalMode();
    }

    if (MCP251XFD_WakeUp(&mcp2518fd, NULL) != ERR_OK)
        return false;

    mcp_asleep = false;

    tx_thread_sleep(MCP_WAKE_STABILIZE_TICKS);

    if (!ExtCAN_EnterNormalMode())
        return false;

    return true;
}

static bool ExtCAN_WaitTxDrain(void)
{
    bool ok = true;
    for (uint8_t wait = 0u; ; wait++)
    {
        setMCP251XFD_FIFOstatus st;
        if (MCP251XFD_GetFIFOStatus(&mcp2518fd, MCP251XFD_FIFO2, &st) != ERR_OK)
            break;
        if ((st & MCP251XFD_TX_FIFO_EMPTY) != 0)
            break;
        if (wait >= 10u)
        {
            ok = false;
            break;
        }
        tx_thread_sleep(1);
    }
    mcp_last_activity_ms = HAL_GetTick();
    return ok;
}
#endif

static bool ExtCAN_DrainRxFIFO(void)
{
    if (!mcp_initialized) return false;

    eERRORRESULT err;
    setMCP251XFD_FIFOstatus fifo_status;
    bool     drained = false;

#if PHASE25_CAN_TX
    /* SPI1 op 직렬화 (리뷰 #1): TX(UART_RX_Thread, pri 7)와 드레인(pri 6)이
       SPI 트랜잭션을 서로 끊지 않도록 상위 op 단위로 뮤텍스 보호 */
    if (tx_mutex_get(&mcp_spi_mutex, TX_WAIT_FOREVER) != TX_SUCCESS)
        return false;
#endif

    if (mcp_asleep)
    {
#if PHASE25_CAN_TX
        tx_mutex_put(&mcp_spi_mutex);
#endif
        return false;
    }

    for (;;)
    {
        err = MCP251XFD_GetFIFOStatus(&mcp2518fd, MCP251XFD_FIFO1, &fifo_status);
        if (err != ERR_OK) break;

        if ((fifo_status & MCP251XFD_RX_FIFO_OVERFLOW) != 0)
        {
            g_mcp_rx_ovf++;
            (void)MCP251XFD_ClearFIFOOverflowEvent(&mcp2518fd, MCP251XFD_FIFO1);
        }

        if ((fifo_status & MCP251XFD_RX_FIFO_NOT_EMPTY) == 0) break;

        mcp_rx_msg.PayloadData = mcp_rx_payload;
        err = MCP251XFD_ReceiveMessageFromFIFO(&mcp2518fd,
                                                &mcp_rx_msg,
                                                MCP251XFD_PAYLOAD_8BYTE,
                                                NULL,
                                                MCP251XFD_FIFO1);
        if (err != ERR_OK) break;

        ExtCAN_ProcessMessage(mcp_rx_msg.MessageID,
                              mcp_rx_payload,
                              MCP251XFD_DLCToByte(mcp_rx_msg.DLC, false));
        drained = true;
    }

    (void)MCP251XFD_ClearInterruptEvents(&mcp2518fd,
                                         MCP251XFD_INT_BUS_WAKEUP_EVENT
                                       | MCP251XFD_INT_BUS_ERROR_EVENT);

#if PHASE25_CAN_TX
    tx_mutex_put(&mcp_spi_mutex);
#endif
    return drained;
}

#if PHASE25_CAN_TX
/* ---------------------------------------------------------------------------
 * ExtCAN_Transmit  (task 6A.2) — 지상국 메시지를 LCD 보드로 CAN 전송
 *
 * 프레임 형식: [제어 1B | 데이터 ≤7B] × N
 *   제어 바이트: bit7 = 마지막 프레임, bit6~0 = 시퀀스 번호(0부터)
 * 7바이트 초과 페이로드는 복수 프레임으로 분할. 전송 실패 시
 * ERR_CAN_TX_FAIL ERROR LOG → UART_Queue + SD_Queue (Req 13.6).
 * 스레드 컨텍스트 전용 (UART_RX_Thread에서 호출).
 * --------------------------------------------------------------------------- */
HAL_StatusTypeDef ExtCAN_Transmit(const uint8_t *data, size_t len)
{
    if (!mcp_initialized || data == NULL || len == 0)
        return HAL_ERROR;

    uint8_t seq = 0;
    size_t  offset = 0;
    HAL_StatusTypeDef result = HAL_OK;

    if (tx_mutex_get(&mcp_spi_mutex, TX_WAIT_FOREVER) != TX_SUCCESS)
        return HAL_ERROR;

    if (!ExtCAN_Wake())
    {
        ExtCAN_Sleep();
        tx_mutex_put(&mcp_spi_mutex);
        return HAL_ERROR;
    }

    while (offset < len)
    {
        size_t  chunk   = (len - offset > 7) ? 7 : (len - offset);
        uint8_t payload[8] = {0};
        uint8_t is_last = (offset + chunk >= len) ? 0x80 : 0x00;

        payload[0] = (uint8_t)(is_last | (seq & 0x7F));
        memcpy(&payload[1], &data[offset], chunk);

        /* TX FIFO full 대기 — 드라이버의 TransmitMessageToFIFO는 full 검사를
           하지 않아 8프레임 초과 연속 전송 시 미전송 프레임을 덮어쓴다.
           500kbps에서 프레임당 ~250µs이므로 1ms 폴링 ×20회면 충분. */
        {
            setMCP251XFD_FIFOstatus tx_status;
            uint8_t wait_cnt = 0;
            for (;;)
            {
                if (MCP251XFD_GetFIFOStatus(&mcp2518fd, MCP251XFD_FIFO2,
                                            &tx_status) != ERR_OK)
                {
                    ExtCAN_Sleep();
                    tx_mutex_put(&mcp_spi_mutex);
                    return HAL_ERROR;
                }
                if (tx_status & MCP251XFD_TX_FIFO_NOT_FULL)
                    break;
                if (++wait_cnt > 20)   /* ~20ms 초과 — 버스 문제로 판단 */
                {
                    LOG err_pkt;
                    uint8_t val[8] = {0};
                    uint32_t detail = 0xFFFF0000u | seq;   /* full-timeout 표식 */
                    memcpy(&val[0], &detail, 4);
                    PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_CAN_ERR,
                                         (uint8_t)ERR_CAN_TX_FAIL, val, 8);
                    ExtCAN_SendLogToQueues(&err_pkt);
                    ExtCAN_Sleep();
                    tx_mutex_put(&mcp_spi_mutex);
                    return HAL_ERROR;
                }
                tx_thread_sleep(1);    /* 1ms (tick=1ms) — 스레드 컨텍스트 전용 */
            }
        }

        MCP251XFD_CANMessage tx_msg;
        memset(&tx_msg, 0, sizeof(tx_msg));
        tx_msg.MessageID    = EXT_CAN_ID_LCD_TX;
        tx_msg.MessageSEQ   = seq;
        tx_msg.ControlFlags = MCP251XFD_CAN20_FRAME;   /* Standard ID, CAN2.0 */
        tx_msg.DLC          = MCP251XFD_DLC_8BYTE;
        tx_msg.PayloadData  = payload;

        eERRORRESULT err = MCP251XFD_TransmitMessageToFIFO(&mcp2518fd, &tx_msg,
                                                           MCP251XFD_FIFO2,
                                                           true /* flush */);
        if (err != ERR_OK)
        {
            LOG err_pkt;
            uint8_t val[8] = {0};
            uint32_t detail = ((uint32_t)err << 8) | seq;
            memcpy(&val[0], &detail, 4);
            PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_CAN_ERR,
                                 (uint8_t)ERR_CAN_TX_FAIL, val, 8);
            ExtCAN_SendLogToQueues(&err_pkt);
            result = HAL_ERROR;
            break;
        }

        offset += chunk;
        seq++;

        if (offset < len)
            tx_thread_sleep(MCP_TX_CHUNK_GAP_TICKS);
    }

    if (!ExtCAN_WaitTxDrain())
        result = HAL_ERROR;
    tx_mutex_put(&mcp_spi_mutex);
    return result;
}
#endif /* PHASE25_CAN_TX */

#if PHASE25_CAN_TX
HAL_StatusTypeDef ExtCAN_TransmitFrame(uint32_t can_id,
                                       const uint8_t *data, uint8_t dlc)
{
    if (!mcp_initialized || data == NULL || dlc > 8u)
        return HAL_ERROR;

    static const eMCP251XFD_DataLength dlc_map[9] = {
        MCP251XFD_DLC_0BYTE, MCP251XFD_DLC_1BYTE, MCP251XFD_DLC_2BYTE,
        MCP251XFD_DLC_3BYTE, MCP251XFD_DLC_4BYTE, MCP251XFD_DLC_5BYTE,
        MCP251XFD_DLC_6BYTE, MCP251XFD_DLC_7BYTE, MCP251XFD_DLC_8BYTE
    };

    if (tx_mutex_get(&mcp_spi_mutex, TX_WAIT_FOREVER) != TX_SUCCESS)
        return HAL_ERROR;

    if (!ExtCAN_Wake())
    {
        ExtCAN_Sleep();
        tx_mutex_put(&mcp_spi_mutex);
        return HAL_ERROR;
    }

    HAL_StatusTypeDef result = HAL_OK;
    {
        setMCP251XFD_FIFOstatus tx_status;
        uint8_t wait_cnt = 0;
        for (;;)
        {
            if (MCP251XFD_GetFIFOStatus(&mcp2518fd, MCP251XFD_FIFO2,
                                        &tx_status) != ERR_OK)
            {
                result = HAL_ERROR; break;
            }
            if (tx_status & MCP251XFD_TX_FIFO_NOT_FULL) break;
            if (++wait_cnt > 20) { result = HAL_ERROR; break; }
            tx_thread_sleep(1);
        }
    }

    if (result == HAL_OK)
    {
        uint8_t payload[8] = {0};
        memcpy(payload, data, dlc);

        MCP251XFD_CANMessage tx_msg;
        memset(&tx_msg, 0, sizeof(tx_msg));
        tx_msg.MessageID    = can_id;
        tx_msg.MessageSEQ   = 0;
        tx_msg.ControlFlags = MCP251XFD_CAN20_FRAME;
        tx_msg.DLC          = dlc_map[dlc];
        tx_msg.PayloadData  = payload;

        if (MCP251XFD_TransmitMessageToFIFO(&mcp2518fd, &tx_msg,
                                            MCP251XFD_FIFO2, true) != ERR_OK)
            result = HAL_ERROR;
    }
    if (!ExtCAN_WaitTxDrain())
        result = HAL_ERROR;
    tx_mutex_put(&mcp_spi_mutex);
    return result;
}
#endif

/* ---------------------------------------------------------------------------
 * ExtCAN_NotifyInterrupt  (Req 4.4, 9.7)
 *
 * EXTI ISR(HAL_GPIO_EXTI_Falling_Callback)에서 호출된다.
 * 블로킹 SPI를 ISR에서 수행하지 않기 위해 세마포어만 set하고 즉시 반환한다.
 * (세마포어 미생성 시점엔 tx_semaphore_put이 안전하게 오류 반환)
 * --------------------------------------------------------------------------- */
void ExtCAN_NotifyInterrupt(void)
{
    tx_semaphore_put(&mcp_rx_semaphore);
}

/* ---------------------------------------------------------------------------
 * ExtCAN_RxThread_Entry — MCP2518FD 수신 전용 ThreadX 스레드
 *
 * EXTI가 set한 세마포어를 대기하다 깨어나면 FIFO를 드레인한다.
 * --------------------------------------------------------------------------- */
void ExtCAN_RxThread_Entry(ULONG thread_input)
{
    (void)thread_input;

    /* 부팅 레이스 방지: 세마포어 생성(커널 init) 전에 도착해 통지를 놓친
       메시지가 FIFO에 남아 있을 수 있으므로, 시작 시 1회 드레인한다.
       (ExtCAN_Init은 main에서 커널 진입 전에 이미 실행됨) */
    (void)ExtCAN_DrainRxFIFO();

    for (;;)
    {
        /* 유한 타임아웃으로 대기한다.
           - 정상: EXTI가 세마포어 set → 즉시 깨어나 드레인.
           - 폴백(타임아웃, TX_NO_INSTANCE): 드레인 중 SPI 일시오류로 빠져나와
             INT가 물린 채 새 falling 엣지가 없는 'stuck' 상황 및 부팅/경쟁으로
             놓친 통지를 ~100ms마다 복구한다. */
        UINT st = tx_semaphore_get(&mcp_rx_semaphore, MCP_RX_POLL_TICKS);
        if (st == TX_SUCCESS || st == TX_NO_INSTANCE)
        {
            if (mcp_asleep)
            {
#if PHASE25_CAN_TX
                if (tx_mutex_get(&mcp_spi_mutex, TX_WAIT_FOREVER) == TX_SUCCESS)
                {
#endif
                    (void)MCP251XFD_BusWakeUpFromState(&mcp2518fd);
                    if (ExtCAN_EnterNormalMode())
                        mcp_asleep = false;
#if PHASE25_CAN_TX
                    tx_mutex_put(&mcp_spi_mutex);
                }
#endif
                mcp_last_activity_ms = HAL_GetTick();
            }
            if (ExtCAN_DrainRxFIFO())
                mcp_last_activity_ms = HAL_GetTick();
            if (!mcp_asleep)
            {
                uint8_t tec = 0, rec = 0;
#if PHASE25_CAN_TX
                if (tx_mutex_get(&mcp_spi_mutex, 5) == TX_SUCCESS)
                {
#endif
                    if (MCP251XFD_GetTransmitReceiveErrorCountAndStatus(
                            &mcp2518fd, &tec, &rec, NULL) == ERR_OK)
                    {
                        g_mcp_tec = tec;
                        g_mcp_rec = rec;
                        g_mcp_err_reads++;

                        {
                            eMCP251XFD_OperationMode om;
                            if (MCP251XFD_GetActualOperationMode(&mcp2518fd, &om) == ERR_OK)
                                g_mcp_opmode = (uint32_t)om;
                        }
                    }
#if PHASE25_CAN_TX
                    tx_mutex_put(&mcp_spi_mutex);
                }
#endif
            }

        }

        if (!mcp_asleep &&
            (HAL_GetTick() - mcp_last_activity_ms) > MCP_IDLE_SLEEP_MS)
        {
#if PHASE25_CAN_TX
            if (tx_mutex_get(&mcp_spi_mutex, TX_NO_WAIT) == TX_SUCCESS)
            {
                ExtCAN_Sleep();
                tx_mutex_put(&mcp_spi_mutex);
            }
#else
            ExtCAN_Sleep();
#endif
        }

        
#if TEMP_IWDG_REFRESH
        /* 임시 (작업 목록 알려진 이슈): Phase 4 전 보드 테스트용 IWDG 리프레시.
           본 스레드는 ~100ms 주기로 반드시 깨어나므로 4초 타임아웃에 안전.
           PHASE4/PHASE6 활성화 시 phase_config.h에서 이 스위치를 끌 것. */
        HAL_IWDG_Refresh(&hiwdg);
#endif
    }
}

/* ---------------------------------------------------------------------------
 * ExtCAN_IsReady
 *
 * MCP2518FD 초기화 완료 및 정상 동작 여부 반환.
 * --------------------------------------------------------------------------- */
bool ExtCAN_IsReady(void)
{
    return mcp_initialized;
}
