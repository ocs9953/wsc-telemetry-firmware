/**
  ******************************************************************************
  * @file    sd_logger.c
  * @brief   SD_Logger_Thread — 더블 버퍼 SD 기록 (task 15.1)
  *
  *          SD_Queue → 활성 버퍼 append → 32패킷(512B) 도달 시 버퍼 스왑 +
  *          fx_file_write → 10초 주기 fx_media_flush.
  *          2026-08: WSC 규정의 10초 주기 기록 조항이 없어져 규정 전용 패킷과
  *          `*_reg.log` 파일을 제거했다. 이제 예외 분기 없이 **모든 패킷이**
  *          더블 버퍼를 거쳐 전체 로그 한 파일로만 기록된다.
  *          Requirements: 8.1~8.11, 9.1, 9.3
  ******************************************************************************
  */
#include "sd_logger.h"
#include "app_threadx.h"
#include "phase_config.h"
#include "packet_builder.h"
#include "rtc_manager.h"
#include "timer_scheduler.h"
#include "main.h"
#include "iwdg.h"
#include "app_filex.h"
#include "fx_stm32_sd_driver.h"

extern TX_SEMAPHORE     sd_recover_sem;
extern volatile uint8_t g_sd_media_ok;
extern FX_MEDIA         sdio_disk;
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * External references
 * --------------------------------------------------------------------------- */
extern TX_QUEUE     UART_Queue;
extern TX_QUEUE     SD_Queue;
extern SYSTEM_STATE SystemState;

/* ---------------------------------------------------------------------------
 * 상수
 * --------------------------------------------------------------------------- */
#define SD_BUF_SIZE            512
_Static_assert(SD_BUF_SIZE % sizeof(LOG) == 0,
               "SD_BUF_SIZE must be a multiple of sizeof(LOG)");
#define SD_QUEUE_TIMEOUT       2000                      /* tick=1ms → 2초   */
#define SD_TIMEOUT_WARN_COUNT  5
#define SD_FLUSH_INTERVAL_MS   10000                     /* 미디어 flush 10s */
#define SD_FLUSH_RETRY_MS      1000

#define SD_FLUSH_ERR_TAG       0xF1000000u

/* SD_DETECT(PD0) 극성: 카드 소켓 CD 스위치가 삽입 시 GND 연결(=RESET) 가정.
   ⚠ 보드 실측 후 반대면 이 매크로만 반전. */
#define SD_DETECT_ABSENT_LEVEL GPIO_PIN_SET

/* ---------------------------------------------------------------------------
 * Static objects — 더블 버퍼 + 파일 핸들
 * --------------------------------------------------------------------------- */
__attribute__((aligned(4))) static uint8_t sd_buf_a[SD_BUF_SIZE];
__attribute__((aligned(4))) static uint8_t sd_buf_b[SD_BUF_SIZE];

static uint8_t  *active_buf;          /* 현재 append 대상                    */
static uint16_t  buf_used;            /* 활성 버퍼 사용 바이트               */

static FX_MEDIA *sd_media  = FX_NULL;
static FX_FILE   log_file;            /* 전체 로그: YYMMDD_HHMMSS.log        */
static uint8_t   sd_ready  = 0;       /* Init 성공 여부 — 실패 시 수신 드롭  */
static uint8_t   sd_write_fail_run = 0;
static uint8_t   sd_flush_fail_run = 0;
static uint32_t  sd_recover_ms = 0;
static uint8_t   sd_recover_pending = 0;

volatile uint32_t g_sd_flush_err = 0;
volatile uint32_t g_sd_discarded  = 0;
volatile uint32_t g_sd_write_lost = 0;
volatile uint32_t g_sd_recover_ok = 0;

/* ---------------------------------------------------------------------------
 * Private helpers
 * --------------------------------------------------------------------------- */
static void SD_SendLogToUartQueue(const LOG *pkt)
{
    ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
    memcpy(msg, pkt, sizeof(LOG));
    TelemQueue_Send(&UART_Queue, msg);
}

static void SD_ReportError(ERR_KEY key, uint32_t detail)
{
    LOG err_pkt;
    uint8_t val[8] = {0};
    memcpy(&val[0], &detail, 4);
    PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_SD_ERR,
                         (uint8_t)key, val, 8);
    SD_SendLogToUartQueue(&err_pkt);

    SystemState_MarkFault(FAULT_IDX_SD);
    HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
    SystemState.sd_ok = 0;
    SystemState.error = 1;
    SystemState_RefreshBoardLed();
}

static uint8_t sd_write_failing = 0;
static uint8_t sd_flush_failing = 0;

static void SD_ReportRecovery(void)
{
    if (sd_write_failing || sd_flush_failing)
        return;
    if (!SystemState.sd_ok && SystemState_FaultHoldExpired(FAULT_IDX_SD))
    {
        SystemState.sd_ok = 1;
        HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_RESET);
        SystemState_RefreshBoardLed();
    }
}

static inline void SD_ThreadKickWatchdog(void)
{
#if PHASE6_SAFETY
    Telemetry_AliveSet(ALIVE_BIT_SD);
#else
    HAL_IWDG_Refresh(&hiwdg);
#endif
}

/** @brief 활성 버퍼를 파일에 쓰고 버퍼 스왑 (가득 찼거나 긴급 시 부분 기록) */
static void SD_FlushActiveBuffer(void)
{
    if (buf_used == 0 || !sd_ready)
        return;

    uint8_t  *writing = active_buf;
    uint16_t  len     = buf_used;

    /* 스왑 먼저 — 기록 중 도착 패킷은 반대 버퍼로 */
    active_buf = (active_buf == sd_buf_a) ? sd_buf_b : sd_buf_a;
    buf_used   = 0;

    if (fx_file_write(&log_file, writing, len) != FX_SUCCESS)
    {
        g_sd_write_lost += (uint32_t)(len / sizeof(LOG));
        sd_write_failing = 1;
        if (sd_write_fail_run < 255u) sd_write_fail_run++;
        SD_ReportError(ERR_SD_WRITE, (uint32_t)len);
    }
    else
    {
        sd_write_failing = 0;
        sd_write_fail_run = 0;
        SD_ReportRecovery();
    }
}

static UINT SD_MediaFlush(void)
{
    UINT st = fx_media_flush(sd_media);
    if (st != FX_SUCCESS)
    {
        g_sd_flush_err++;
        if (sd_flush_fail_run < 255u) sd_flush_fail_run++;
    }
    return st;
}

#define SD_FAIL_RUN_LIMIT     3u
#define SD_RECOVER_GAP_MS  5000u

static void SD_TryRecover(void)
{
    if (sd_recover_pending)
        return;

    uint32_t now = HAL_GetTick();
    if (sd_recover_ms != 0u && (now - sd_recover_ms) < SD_RECOVER_GAP_MS)
        return;
    sd_recover_ms = now;

    sd_recover_pending = 1u;
    g_sd_media_ok      = 0u;

    sd_ready = 0;

    g_sd_discarded += (uint32_t)(buf_used / sizeof(LOG));
    buf_used = 0;

    (void)fx_file_close(&log_file);

    SystemState.sd_ok = 0;
    SystemState.error = 1;
    HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);

    (void)tx_semaphore_put(&sd_recover_sem);
}

static void SD_ResumeAfterRecovery(void)
{
    DATETIME t;
    if (RTC_Manager_GetDateTime(&t) != HAL_OK)
    {
        t.year = 0; t.month  = 1; t.date   = 1;
        t.hour = 0; t.minute = 0; t.second = 0;
    }
    SD_Logger_Init(&sdio_disk, &t);

    if (sd_ready)
    {
        sd_write_fail_run = 0;
        sd_flush_fail_run = 0;
        sd_write_failing  = 0;
        sd_flush_failing  = 0;
        g_sd_recover_req  = 0u;
        g_sd_recover_ok++;
        HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_RESET);
    }
}

static void SD_WriteSessionHeader(void)
{
    LOG     p;
    uint8_t v[8];

    if (g_init_fail_mask != 0u)
    {
        uint32_t mask = g_init_fail_mask;
        uint32_t now  = HAL_GetTick();
        memcpy(&v[0], &mask, 4);
        memcpy(&v[4], &now,  4);
        PacketBuilder_Create(&p, LOG_ERROR, SRC_SYS, (uint8_t)ERR_INIT_FAIL, v, 8);
        memcpy(&active_buf[buf_used], &p, sizeof(LOG));
        buf_used += sizeof(LOG);
    }

    {
        uint32_t csr = g_boot_reset_csr;
        memset(v, 0, sizeof(v));
        memcpy(&v[0], &csr, 4);
        PacketBuilder_Create(&p, LOG_INFO, SRC_SYS, (uint8_t)ERR_BOOT_REPORT, v, 8);
        memcpy(&active_buf[buf_used], &p, sizeof(LOG));
        buf_used += sizeof(LOG);
    }

    if ((g_boot_reset_csr & RCC_CSR_IWDGRSTF) != 0u)
    {
        uint32_t csr = g_boot_reset_csr;
        memset(v, 0, sizeof(v));
        memcpy(&v[0], &csr, 4);
        PacketBuilder_Create(&p, LOG_ERROR, SRC_SYS, (uint8_t)ERR_WATCHDOG, v, 8);
        memcpy(&active_buf[buf_used], &p, sizeof(LOG));
        buf_used += sizeof(LOG);
    }
    if (g_boot_stack_thread != 0u)
    {
        uint32_t thr = g_boot_stack_thread;
        uint32_t csr = g_boot_reset_csr;
        memcpy(&v[0], &thr, 4);
        memcpy(&v[4], &csr, 4);
        PacketBuilder_Create(&p, LOG_CRITICAL, SRC_SYS, (uint8_t)ERR_STACK_OVERFLOW, v, 8);
        memcpy(&active_buf[buf_used], &p, sizeof(LOG));
        buf_used += sizeof(LOG);
    }

#if TEST_DUMMY_DATA
    {
        uint32_t mark = 0x0000DDDDu;
        memset(v, 0, sizeof(v));
        memcpy(&v[0], &mark, 4);
        PacketBuilder_Create(&p, LOG_CRITICAL, SRC_SYS, (uint8_t)ERR_DUMMY_DATA, v, 8);
        memcpy(&active_buf[buf_used], &p, sizeof(LOG));
        buf_used += sizeof(LOG);
    }
#endif
}

/* ---------------------------------------------------------------------------
 * SD_Logger_Init  (task 15.1 / 16.2)
 *
 * fx_app_thread_entry(app_filex.c)에서 fx_media_open 성공 후 호출된다.
 * --------------------------------------------------------------------------- */
void SD_Logger_Init(FX_MEDIA *media, const DATETIME *boot_time)
{
    /* SD 물리 감지 (Req 8.5) */
    if (HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin)
        == SD_DETECT_ABSENT_LEVEL)
    {
        SD_ReportError(ERR_SD_NOT_DETECTED, 0);
        return;                      /* sd_ready=0 → 로깅 비활성, 무선은 지속 */
    }

    char log_name[24];
    RTC_Manager_MakeFilename(boot_time, log_name, sizeof(log_name));

    UINT st = fx_file_create(media, log_name);

    if (st == FX_ALREADY_CREATED)
    {
        char base[14];
        memcpy(base, log_name, 13);
        base[13] = '\0';

        for (unsigned i = 1u; i <= 9999u; i++)
        {
            snprintf(log_name, sizeof(log_name), "%s_%04u.log", base, i);
            st = fx_file_create(media, log_name);
            if (st != FX_ALREADY_CREATED)
                break;
        }
    }

    if (st != FX_SUCCESS)
    {
        SD_ReportError(ERR_SD_INIT, st);
        return;
    }


    if (fx_file_open(media, &log_file, log_name,
                     FX_OPEN_FOR_WRITE) != FX_SUCCESS)
    {
        SD_ReportError(ERR_SD_INIT, 0);
        return;
    }

    sd_media   = media;
    active_buf = sd_buf_a;
    buf_used   = 0;
    SD_WriteSessionHeader();
    sd_ready   = 1;
    SystemState.sd_ok = 1;
    SystemState_RefreshBoardLed();
}

/* ---------------------------------------------------------------------------
 * SD_Logger_Thread_Entry  (task 15.1, Priority 10)
 *
 * Phase 4에서 생성되어도 안전: sd_ready=0이면 수신만 하고 드롭한다
 * (SD_Queue 소비 + IWDG kick 역할 수행).
 * --------------------------------------------------------------------------- */
void SD_Logger_Thread_Entry(ULONG thread_input)
{
    (void)thread_input;
    ULONG    msg[LOG_QUEUE_MSG_WORDS];
    uint32_t timeout_count  = 0;
    uint32_t last_flush_ms  = 0;

    for (;;)
    {
#if PHASE6_SAFETY
        /* --- 전원 강하 (task 18.2): 긴급 Flush 후 정지 ---
           직전 receive로 꺼낸 패킷(CRITICAL 포함)은 아래 정상 경로가 이미
           버퍼에 append한 상태다. 여기서는 큐에 남은 잔여만 회수한다. */
        if (g_power_down)
        {
            if (sd_ready)
            {
                while (tx_queue_receive(&SD_Queue, msg, TX_NO_WAIT) == TX_SUCCESS)
                {
                    memcpy(&active_buf[buf_used], msg, sizeof(LOG));
                    buf_used += sizeof(LOG);
                    if (buf_used >= SD_BUF_SIZE)
                        SD_FlushActiveBuffer();
                }
                SD_FlushActiveBuffer();          /* 부분 버퍼까지 기록      */
                if (SD_MediaFlush() != FX_SUCCESS)
                    (void)SD_MediaFlush();
            }
            for (;;) { /* 전원 소실 대기 — 의도된 정지 (Req 12.6) */ }
        }
#endif

        UINT st = tx_queue_receive(&SD_Queue, msg, SD_QUEUE_TIMEOUT);

        SD_ThreadKickWatchdog();

        if (sd_recover_pending && g_sd_media_ok)
        {
            SD_ResumeAfterRecovery();
            sd_recover_pending = 0u;
            SD_ThreadKickWatchdog();
        }
        else if (g_sd_recover_req != 0u ||
                 sd_write_fail_run >= SD_FAIL_RUN_LIMIT ||
                 sd_flush_fail_run >= SD_FAIL_RUN_LIMIT)
        {
            SD_TryRecover();
            SD_ThreadKickWatchdog();
        }

        if (st == TX_SUCCESS)
        {
            timeout_count = 0;

            if (!sd_ready)
            {
                g_sd_discarded++;
                continue;
            }

            const LOG *pkt = (const LOG *)msg;


            memcpy(&active_buf[buf_used], pkt, sizeof(LOG));
            buf_used += sizeof(LOG);

            if (buf_used >= SD_BUF_SIZE)
                SD_FlushActiveBuffer();          /* 512B 도달 — 스왑 + 기록 */

            uint32_t now = HAL_GetTick();
            if ((now - last_flush_ms) >= SD_FLUSH_INTERVAL_MS)
            {
                UINT fst = SD_MediaFlush();
                if (fst == FX_SUCCESS)
                {
                    last_flush_ms = now;
                    sd_flush_failing = 0;
                    sd_flush_fail_run = 0;
                    SD_ReportRecovery();
                }
                else
                {
                    last_flush_ms = now - SD_FLUSH_INTERVAL_MS + SD_FLUSH_RETRY_MS;
                    if (!sd_flush_failing)
                    {
                        sd_flush_failing = 1;
                        SD_ReportError(ERR_SD_WRITE, SD_FLUSH_ERR_TAG | (uint32_t)fst);
                    }
                }
            }
        }
        else /* 큐 타임아웃 (2초 무데이터) */
        {
            timeout_count++;
            if (timeout_count == SD_TIMEOUT_WARN_COUNT)
            {
                LOG warn_pkt;
                uint8_t val[8] = {0};
                memcpy(&val[0], &timeout_count, 4);
                PacketBuilder_Create(&warn_pkt, LOG_WARNING, SRC_SD_ERR,
                                     (uint8_t)ERR_TIMEOUT, val, 8);
                SD_SendLogToUartQueue(&warn_pkt);
                timeout_count = 0;
            }
        }
    }
}
