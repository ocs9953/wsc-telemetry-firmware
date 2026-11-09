#include "timer_scheduler.h"
#include "app_threadx.h"
#include "gps.h"
#include "dummy_data.h"
#include "can_receiver.h"
#include "adc_sensor.h"
#include "freshness.h"

#include "phase_config.h"
#include "types.h"
#include "packet_builder.h"
#include "rtc_manager.h"
#include "main.h"
#include "tim.h"
#include "iwdg.h"
#include "tx_api.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External references
 * --------------------------------------------------------------------------- */
extern VehicleData_t VehicleData;
extern SYSTEM_STATE  SystemState;
extern TX_QUEUE      UART_Queue;
extern TX_QUEUE      SD_Queue;

#define SCHED_PRIMARY_TICKS      10
#define SCHED_DETAIL_STEP_TICKS   5
#define SCHED_DETAIL_STEPS       10
#define SCHED_TIMEOUT_TICKS      10
#define SCHED_RTC_TICKS         100
#define SCHED_IWDG_TICKS         20

#define SCHED_PHASE_PRIMARY       0
#define SCHED_PHASE_DETAIL        2
#define SCHED_PHASE_TIMEOUT       5
#define SCHED_PHASE_RTC           4
#define SCHED_PHASE_IWDG         13

/* ---------------------------------------------------------------------------
 * Alive-Flag / 전원 강하 상태 (Phase 6)
 * --------------------------------------------------------------------------- */
static volatile uint32_t alive_flags = 0;
volatile uint8_t g_power_down = 0;

volatile uint32_t g_init_fail_mask = 0;

#if PHASE6_SAFETY
static uint8_t Sched_Send(const LOG *pkt, uint8_t to_uart, uint8_t to_sd);

void Telemetry_NotifyPowerDown(void)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    uint8_t already = g_power_down;
    g_power_down = 1;
    TX_RESTORE

    if (already)
        return;

    LOG      crit;
    uint8_t  v[8];
    uint32_t now     = HAL_GetTick();
    uint32_t pending = 0;
    memcpy(&v[0], &now,     4);
    memcpy(&v[4], &pending, 4);
    PacketBuilder_Create(&crit, LOG_CRITICAL, SRC_SYS,
                         (uint8_t)ERR_POWER_DOWN, v, 8);
    Sched_Send(&crit, 0, 1);
}
#endif

void Telemetry_AliveSet(uint32_t bit)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    alive_flags |= bit;
    TX_RESTORE
}

/* ---------------------------------------------------------------------------
 * 큐 전송 헬퍼 — Full 감지 시 교차 보고 (Req 6.5, 6.6)
 *   UART_Queue Full → ERR_UART_QUEUE_FULL WARNING → SD_Queue만 + LED_RFD900x
 *   SD_Queue   Full → ERR_SD_QUEUE_FULL   WARNING → UART_Queue만 + LED_SD
 *   래치로 1초당 1회만 보고 (다음 1s tick에서 해제)
 * --------------------------------------------------------------------------- */
static uint8_t uart_full_latch = 0;
static uint8_t sd_full_latch   = 0;

static uint8_t Sched_LatchOnce(uint8_t *latch)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    uint8_t was = *latch;
    *latch = 1u;
    TX_RESTORE
    return (uint8_t)(was == 0u);
}

static uint8_t Sched_Send(const LOG *pkt, uint8_t to_uart, uint8_t to_sd)
{
    ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
    memcpy(msg, pkt, sizeof(LOG));
    uint8_t uart_queued = 0;
    uint8_t sd_queued   = 0;

    if (to_uart)
    {
        UINT st = TelemQueue_Send(&UART_Queue, msg);
        uart_queued = (uint8_t)(st == TX_SUCCESS);

        if (st == TX_QUEUE_FULL && Sched_LatchOnce(&uart_full_latch))
        {
            LOG w;
            uint8_t v[8] = {0};
            PacketBuilder_Create(&w, LOG_WARNING, SRC_UART_ERR,
                                 (uint8_t)ERR_UART_QUEUE_FULL, v, 8);
            ULONG m2[LOG_QUEUE_MSG_WORDS] = {0};
            memcpy(m2, &w, sizeof(LOG));
            TelemQueue_Send(&SD_Queue, m2);
            HAL_GPIO_WritePin(LED_RFD900x_GPIO_Port, LED_RFD900x_Pin,
                              GPIO_PIN_SET);
            SystemState_MarkFault(FAULT_IDX_UART);
            SystemState.uart_ok = 0;
            SystemState_RefreshBoardLed();
        }
    }
    if (to_sd)
    {
        UINT st = TelemQueue_Send(&SD_Queue, msg);
        sd_queued = (uint8_t)(st == TX_SUCCESS);

        if (st == TX_QUEUE_FULL && Sched_LatchOnce(&sd_full_latch))
        {
            LOG w;
            uint8_t v[8] = {0};
            PacketBuilder_Create(&w, LOG_WARNING, SRC_SD_ERR,
                                 (uint8_t)ERR_SD_QUEUE_FULL, v, 8);
            ULONG m2[LOG_QUEUE_MSG_WORDS] = {0};
            memcpy(m2, &w, sizeof(LOG));
            TelemQueue_Send(&UART_Queue, m2);
            HAL_GPIO_WritePin(LED_SD_GPIO_Port, LED_SD_Pin, GPIO_PIN_SET);
            SystemState_MarkFault(FAULT_IDX_SD);
            SystemState.sd_ok = 0;
            SystemState_RefreshBoardLed();
        }
    }

    return to_uart ? uart_queued : sd_queued;
}

/** @brief 데이터 패킷 생성 + SD_Queue만 전송 (SD_ONLY 대상, 무선 미전송) */
static void Sched_EmitSdOnly(LOG_SOURCE src, uint8_t key, const uint8_t *value)
{
    LOG pkt;
    PacketBuilder_Create(&pkt, LOG_INFO, src, key, value, 8);
    (void)Sched_Send(&pkt, 0, 1);
}

/** @brief 데이터 패킷 생성 + 양쪽 큐 전송 (무선 대상) */
static uint8_t Sched_EmitBoth(LOG_SOURCE src, uint8_t key, const uint8_t *value)
{
    LOG pkt;
    PacketBuilder_Create(&pkt, LOG_INFO, src, key, value, 8);
    return Sched_Send(&pkt, 1, 1);
}

static void Sched_EmitPrimary(void)
{
    uint8_t v[8];

    BmsData_t *b = &VehicleData.bms;
    PacketBuilder_PackU16x4(v, b->pack_v_cv, (uint16_t)b->pack_current,
                               b->min_cell_mv, b->max_cell_mv);
    Sched_EmitBoth(SRC_BMS, (uint8_t)KEY_BMS_PACK_VI, v);

    PacketBuilder_PackU16x4(v, b->soc_permille, (uint16_t)b->min_cell_temp,
                               (uint16_t)b->max_cell_temp, 0);
    Sched_EmitBoth(SRC_BMS, (uint8_t)KEY_BMS_SOC_TEMP, v);

    uint32_t st = b->ext_status | b->ext_status_seen;
    memcpy(&v[0], &st, 4);
    v[4] = (uint8_t)(b->contactor_status | b->contactor_err_seen);
    v[5] = b->precharge_state;
    v[6] = b->status_flags;
    v[7] = b->cmu_count;
    if (Sched_EmitBoth(SRC_BMS, (uint8_t)KEY_BMS_STATUS, v))
    {
        b->ext_status_seen    = 0u;
        b->contactor_err_seen = 0u;
    }

    {
        const BmsData_t *lv = &VehicleData.bms_lv;
        PacketBuilder_PackU16x4(v, lv->pack_v_cv, (uint16_t)lv->pack_current,
                                   lv->min_cell_mv, lv->max_cell_mv);
        Sched_EmitBoth(SRC_BMS_LV, (uint8_t)KEY_BMS_PACK_VI, v);

        PacketBuilder_PackU16x4(v, lv->soc_permille, (uint16_t)lv->min_cell_temp,
                                   (uint16_t)lv->max_cell_temp, 0);
        Sched_EmitBoth(SRC_BMS_LV, (uint8_t)KEY_BMS_SOC_TEMP, v);
    }

    for (uint8_t i = 0; i < MPPT_UNIT_COUNT; i++)
    {
        const MpptData_t *m = &VehicleData.mppt[i];
        LOG_SOURCE src = (LOG_SOURCE)(SRC_MPPT1 + i);

        PacketBuilder_PackU16x4(v, m->in_v, m->in_i, m->out_v, m->out_i);
        Sched_EmitBoth(src, (uint8_t)KEY_MPPT_IV_IN_OUT, v);
    }

    const MotorData_t *mo = &VehicleData.motor;
    PacketBuilder_PackU16x4(v, mo->bus_v, (uint16_t)mo->bus_i,
                               (uint16_t)mo->motor_rpm, mo->veh_speed);
    Sched_EmitBoth(SRC_MOTOR, (uint8_t)KEY_MOTOR_BUS_VEL, v);

    PacketBuilder_PackU16x4(v, (uint16_t)mo->heatsink_temp,
                               (uint16_t)mo->motor_temp, 0, 0);
    Sched_EmitBoth(SRC_MOTOR, (uint8_t)KEY_MOTOR_TEMP, v);

    PacketBuilder_PackU16x4(v, (uint16_t)mo->bemf_d, (uint16_t)mo->bemf_q, 0, 0);
    Sched_EmitBoth(SRC_MOTOR, (uint8_t)KEY_MOTOR_BEMF, v);

    PacketBuilder_PackFloat2(v, VehicleData.distance_traveled,
                                VehicleData.battery_accumulated_energy);
    Sched_EmitBoth(SRC_MOTOR, (uint8_t)KEY_MOTOR_ODO, v);
}


static void Sched_EmitBmsPackDetail(void)
{
    uint8_t v[8];
    const BmsData_t *b = &VehicleData.bms;

    memset(v, 0, 8);
    v[0] = b->min_cell_mv_cmu;   v[1] = b->min_cell_mv_cell;
    v[2] = b->max_cell_mv_cmu;   v[3] = b->max_cell_mv_cell;
    v[4] = b->min_cell_temp_cmu; v[5] = b->max_cell_temp_cmu;
    Sched_EmitBoth(SRC_BMS, (uint8_t)KEY_BMS_CELL_LOC, v);

    PacketBuilder_PackU16x4(v, b->soc_ah_cah, b->bal_soc_ah_cah,
                               b->bal_soc_permille, 0);
    Sched_EmitBoth(SRC_BMS, (uint8_t)KEY_BMS_SOC_DETAIL, v);

    PacketBuilder_PackU16x4(v, (uint16_t)b->chg_cell_v_err_mv,
                               (uint16_t)b->cell_temp_margin,
                               (uint16_t)b->dchg_cell_v_err_mv,
                               b->pack_capacity_ah);
    Sched_EmitBoth(SRC_BMS, (uint8_t)KEY_BMS_CHARGER, v);

    PacketBuilder_PackU16x4(v, b->fan0_rpm, b->fan1_rpm,
                               b->i12v_fan_ma, b->i12v_cmu_ma);
    Sched_EmitBoth(SRC_BMS, (uint8_t)KEY_BMS_FAN_12V, v);

    memcpy(&v[0], &b->bal_rise_mv, 2);
    memcpy(&v[2], &b->bal_fall_mv, 2);
    v[4] = b->precharge_timer_elapsed;
    v[5] = b->precharge_timer_count;
    v[6] = 0;
    v[7] = 0;
    Sched_EmitSdOnly(SRC_BMS, (uint8_t)KEY_BMS_BAL_PRECHG, v);
}

static void Sched_EmitBmsCmuDetail(uint8_t first, uint8_t count)
{
    uint8_t v[8];

    for (uint8_t n = first; n < (uint8_t)(first + count) && n < BMS_CMU_COUNT; n++)
    {
        const BmsCmu_t *c = &VehicleData.bms.cmu[n];

        PacketBuilder_PackI16x4(v, c->cell_mv[0], c->cell_mv[1],
                                   c->cell_mv[2], c->cell_mv[3]);
        Sched_EmitBoth(SRC_BMS, (uint8_t)(KEY_BMS_CMU_CELL_A + n), v);

        PacketBuilder_PackI16x4(v, c->cell_mv[4], c->cell_mv[5],
                                   c->cell_mv[6], c->cell_mv[7]);
        Sched_EmitBoth(SRC_BMS, (uint8_t)(KEY_BMS_CMU_CELL_B + n), v);

        PacketBuilder_PackI16x4(v, c->pcb_temp, c->cell_temp, 0, 0);
        Sched_EmitBoth(SRC_BMS, (uint8_t)(KEY_BMS_CMU_TEMP + n), v);
    }
}

static void Sched_EmitBmsCmuSerial(void)
{
    uint8_t v[8];

    for (uint8_t n = 0; n < BMS_CMU_COUNT; n++)
    {
        PacketBuilder_PackU32x2(v, VehicleData.bms.cmu[n].serial, 0);
        Sched_EmitSdOnly(SRC_BMS, (uint8_t)(KEY_BMS_CMU_SERIAL + n), v);
    }
}

static void Sched_EmitAdcDetail(void)
{
#if !PHASE9_ADC_SENSOR && !TEST_DUMMY_DATA
    return;
#else
    uint8_t v[8];

    for (uint8_t n = 0; n < ADC_POT_CHANNELS; n++)
    {
        AdcPotAcc_t acc = VehicleData.adc_pot[n];
        ADC_Sensor_PackSummary(&acc, v);
        if (Sched_EmitBoth(SRC_ADC, (uint8_t)(KEY_ADC_POT + n), v))
            memset(&VehicleData.adc_pot[n], 0, sizeof(AdcPotAcc_t));
    }
#endif
}

static void Sched_EmitMpptDetail(void)
{
    uint8_t v[8];

    for (uint8_t i = 0; i < MPPT_UNIT_COUNT; i++)
    {
        MpptData_t *m = &VehicleData.mppt[i];
        LOG_SOURCE src = (LOG_SOURCE)(SRC_MPPT1 + i);

        memset(v, 0, 8);
        memcpy(&v[0], &m->mosfet_temp, 2);
        memcpy(&v[2], &m->ctrl_temp,   2);
        v[4] = m->error_flags;
        v[5] = m->mode;
        if (Sched_EmitBoth(src, (uint8_t)KEY_MPPT_TEMP_FLAGS, v))
            m->error_flags = 0;

        PacketBuilder_PackU16x4(v, m->max_out_v, m->max_in_i, 0, 0);
        Sched_EmitBoth(src, (uint8_t)KEY_MPPT_MAXLIM, v);

        memset(v, 0, 8);
        memcpy(&v[0], &m->powerconn_v,    2);
        memcpy(&v[2], &m->powerconn_temp, 2);
        Sched_EmitBoth(src, (uint8_t)KEY_MPPT_AUX_2, v);
    }
}

static void Sched_EmitMpptSdAndMotor(void)
{
    uint8_t v[8];

    for (uint8_t i = 0; i < MPPT_UNIT_COUNT; i++)
    {
        const MpptData_t *m = &VehicleData.mppt[i];
        LOG_SOURCE src = (LOG_SOURCE)(SRC_MPPT1 + i);

        PacketBuilder_PackU16x4(v, m->aux_12v, m->aux_3v, 0, 0);
        Sched_EmitSdOnly(src, (uint8_t)KEY_MPPT_AUX_1, v);
    }

    MotorData_t *mo = &VehicleData.motor;
    memset(v, 0, 8);
    memcpy(&v[0], &mo->dsp_temp, 2);
    Sched_EmitBoth(SRC_MOTOR, (uint8_t)KEY_MOTOR_DSP_TEMP, v);

    memcpy(&v[0], &mo->limit_flags,  2);
    memcpy(&v[2], &mo->error_flags,  2);
    memcpy(&v[4], &mo->active_motor, 2);
    v[6] = mo->can_tx_err_count;
    v[7] = mo->can_rx_err_count;
    if (Sched_EmitBoth(SRC_MOTOR, (uint8_t)KEY_MOTOR_STATUS, v))
    {
        mo->limit_flags = 0;
        mo->error_flags = 0;
    }

    memset(v, 0, 8);
    memcpy(&v[0], &mo->phase_b_i, 2);
    memcpy(&v[2], &mo->phase_c_i, 2);
    Sched_EmitBoth(SRC_MOTOR, (uint8_t)KEY_MOTOR_PHASE_I, v);

    memset(v, 0, 8);
    memcpy(&v[0], &mo->v_vector_d, 2);
    memcpy(&v[2], &mo->v_vector_q, 2);
    Sched_EmitSdOnly(SRC_MOTOR, (uint8_t)KEY_MOTOR_V_VECTOR, v);

    memset(v, 0, 8);
    memcpy(&v[0], &mo->i_vector_d, 2);
    memcpy(&v[2], &mo->i_vector_q, 2);
    Sched_EmitSdOnly(SRC_MOTOR, (uint8_t)KEY_MOTOR_I_VECTOR, v);

    PacketBuilder_PackU16x4(v, mo->rail_15v, mo->rail_3v3, mo->rail_1v9, 0);
    Sched_EmitSdOnly(SRC_MOTOR, (uint8_t)KEY_MOTOR_RAILS, v);

    memset(v, 0, 8);
    memcpy(&v[0], &mo->slip_speed, 2);
    Sched_EmitSdOnly(SRC_MOTOR, (uint8_t)KEY_MOTOR_SLIP, v);
}

/* ---------------------------------------------------------------------------
 * 타임아웃 검사 1s — 핵심 데이터 1000ms 미갱신 시 WARNING 1회 (래치)
 * value: [0:4)=source id (감시 단위가 소스이므로 key 대신 source 기록),
 *        [4:8)=마지막 갱신 이후 경과 ms
 * --------------------------------------------------------------------------- */
#define SCHED_TIMEOUT_MS      1000
#define SCHED_TIMEOUT_CMU_MS  3000
#define SCHED_TIMEOUT_SOURCES 13

volatile uint32_t g_timeout_mask  = 0;
volatile uint32_t g_timeout_count = 0;

static void Sched_CheckTimeouts(void)
{
    static uint8_t latched[SCHED_TIMEOUT_SOURCES] = {0};

    const uint32_t now = HAL_GetTick();
    const struct {
        LOG_SOURCE      src;
        uint8_t         unit;
        uint16_t        limit_ms;
        const uint32_t *last_tick;
    } watch[SCHED_TIMEOUT_SOURCES] = {
        { SRC_BMS,   0, SCHED_TIMEOUT_MS,     &VehicleData.bms.bms_last_tick       },
        { SRC_BMS,   1, SCHED_TIMEOUT_CMU_MS, &VehicleData.bms.cmu[0].last_tick    },
        { SRC_BMS,   2, SCHED_TIMEOUT_CMU_MS, &VehicleData.bms.cmu[1].last_tick    },
        { SRC_BMS,   3, SCHED_TIMEOUT_CMU_MS, &VehicleData.bms.cmu[2].last_tick    },
        { SRC_BMS,   4, SCHED_TIMEOUT_CMU_MS, &VehicleData.bms.cmu[3].last_tick    },
        { SRC_MPPT1, 0, SCHED_TIMEOUT_MS,     &VehicleData.mppt[0].mppt_last_tick  },
        { SRC_MPPT2, 0, SCHED_TIMEOUT_MS,     &VehicleData.mppt[1].mppt_last_tick  },
        { SRC_MPPT3, 0, SCHED_TIMEOUT_MS,     &VehicleData.mppt[2].mppt_last_tick  },
        { SRC_MPPT4, 0, SCHED_TIMEOUT_MS,     &VehicleData.mppt[3].mppt_last_tick  },
        { SRC_MOTOR, 0, SCHED_TIMEOUT_MS,     &VehicleData.motor.motor_last_tick   },
        { SRC_SYS,   0, SCHED_TIMEOUT_MS,     &VehicleData.odo_last_tick           },
        { SRC_BMS_LV, 0, SCHED_TIMEOUT_MS,     &VehicleData.bms_lv.bms_last_tick    },
        { SRC_BMS_LV, 1, SCHED_TIMEOUT_CMU_MS, &VehicleData.bms_lv.cmu[0].last_tick },
    };
    _Static_assert(BMS_LV_CMU_COUNT == 1,
                   "LV CMU timeout watch list assumes 1 CMU - update watch[]");
    _Static_assert(BMS_CMU_COUNT == 4,
                   "CMU timeout watch list assumes 4 CMUs - update watch[]");

    for (uint8_t i = 0; i < SCHED_TIMEOUT_SOURCES; i++)
    {
        uint32_t elapsed = now - *watch[i].last_tick;   /* last=0이면 부팅 후 경과 */

        if (elapsed > watch[i].limit_ms)
        {
            if (!latched[i])
            {
                if ((g_timeout_mask & (1u << i)) == 0u)
                    g_timeout_count++;
                g_timeout_mask |= (1u << i);

                LOG w;
                uint8_t v[8];
                uint32_t src_id = (uint32_t)watch[i].src
                                | ((uint32_t)watch[i].unit << 8);
                memcpy(&v[0], &src_id,  4);
                memcpy(&v[4], &elapsed, 4);
                PacketBuilder_Create(&w, LOG_WARNING, SRC_SYS,
                                     (uint8_t)ERR_TIMEOUT, v, 8);
                if (Sched_Send(&w, 1, 1))
                    latched[i] = 1;
            }
        }
        else
        {
            latched[i] = 0;   /* 갱신 재개 → 래치 해제 */

            g_timeout_mask &= ~(1u << i);
        }
    }
}

static void Sched_EmitFreshness(void)
{
    uint32_t stale, never;
    uint8_t  v[8];

    Fresh_Evaluate(HAL_GetTick(), &stale, &never);
    PacketBuilder_PackU32x2(v, stale, never);
    (void)Sched_EmitBoth(SRC_SYS, (uint8_t)KEY_SYS_FRESHNESS, v);
}


#if PHASE8_GPS
void Telemetry_EmitGps(void)
{
    GPS_DATA g;
    GPS_GetSnapshot(&g);

    uint8_t v[8];

    memcpy(&v[0], &g.lat_1e7, 4);
    memcpy(&v[4], &g.lon_1e7, 4);
    Sched_EmitBoth(SRC_GPS, (uint8_t)KEY_GPS_POS, v);

    memcpy(&v[0], &g.speed_cms,   2);
    memcpy(&v[2], &g.course_cdeg, 2);
    memcpy(&v[4], &g.alt_dm,      2);
    v[6] = g.sats;
    v[7] = g.fix;
    Sched_EmitBoth(SRC_GPS, (uint8_t)KEY_GPS_MOTION, v);

    memcpy(&v[0], &g.utc_time, 4);
    memcpy(&v[4], &g.utc_date, 4);
    Sched_EmitSdOnly(SRC_GPS, (uint8_t)KEY_GPS_TIME, v);
}
#endif

static void Sched_EmitDetailStep(uint8_t step)
{
    switch (step)
    {
    case 0: Sched_EmitBmsPackDetail();      break;
    case 1: Sched_EmitBmsCmuDetail(0, 2);   break;
    case 2: Sched_EmitBmsCmuSerial();       break;
    case 3: Sched_EmitAdcDetail();          break;
    case 4: Sched_EmitBmsCmuDetail(2, 2);   break;
    case 6: Sched_EmitMpptDetail();         break;
    case 7: Sched_EmitMpptSdAndMotor();     break;
    default: break;
    }
}

static void Sched_Emit10s(void)
{
    DATETIME dt;
    if (RTC_Manager_GetDateTime(&dt) == HAL_OK)
    {
        LOG pkt;
        uint8_t v[8];
        RTC_Manager_PackToValue(&dt, v);
        PacketBuilder_Create(&pkt, LOG_INFO, SRC_SYS,
                             (uint8_t)KEY_SYS_RTC_TIME, v, 8);
        Sched_Send(&pkt, 1, 1);
    }

}

/* ---------------------------------------------------------------------------
 * Timer_Scheduler_Start  (task 12.2)
 * --------------------------------------------------------------------------- */
void Timer_Scheduler_Start(void)
{
    HAL_TIM_Base_Start_IT(&htim5);
}

#define SCHED_POWER_LOW_TICKS  2u

/* ---------------------------------------------------------------------------
 * Timer_Scheduler_Tick — TIM5 ISR (100ms)
 * --------------------------------------------------------------------------- */
void Timer_Scheduler_Tick(void)
{
    static uint32_t tick = 0;
    tick++;

#if PHASE6_SAFETY
    {
        static uint8_t pd6_low_ticks = 0u;
        if (HAL_GPIO_ReadPin(Power_check_GPIO_Port, Power_check_Pin) == GPIO_PIN_RESET)
        {
            if (pd6_low_ticks < SCHED_POWER_LOW_TICKS)
                pd6_low_ticks++;
            if (pd6_low_ticks >= SCHED_POWER_LOW_TICKS)
                Telemetry_NotifyPowerDown();
        }
        else
        {
            pd6_low_ticks = 0u;
        }
    }
    if (g_power_down)
        return;   /* 이후 패킷 생성 중지 — SD_Logger가 긴급 Flush + 정지 수행 */
#endif

    {
        static uint32_t init_fail_reported = 0u;
        uint32_t mask = g_init_fail_mask;
        if ((mask & ~init_fail_reported) != 0u)
        {
            LOG      p;
            uint8_t  v[8] = {0};
            uint32_t now  = HAL_GetTick();
            memcpy(&v[0], &mask, 4);
            memcpy(&v[4], &now,  4);
            PacketBuilder_Create(&p, LOG_ERROR, SRC_SYS,
                                 (uint8_t)ERR_INIT_FAIL, v, 8);
            if (Sched_Send(&p, 1, 0))
                init_fail_reported |= mask;
        }
    }

    {
        static uint8_t wdg_reported = 0u, stack_reported = 0u, boot_reported = 0u;
        uint32_t csr = g_boot_reset_csr;

        if (!boot_reported)
        {
            LOG     p;
            uint8_t v[8] = {0};
            memcpy(&v[0], &csr, 4);
            PacketBuilder_Create(&p, LOG_INFO, SRC_SYS, (uint8_t)ERR_BOOT_REPORT, v, 8);
            if (Sched_Send(&p, 1, 0))
                boot_reported = 1u;
        }

        if (!wdg_reported && (csr & RCC_CSR_IWDGRSTF) != 0u)
        {
            LOG     p;
            uint8_t v[8] = {0};
            memcpy(&v[0], &csr, 4);
            PacketBuilder_Create(&p, LOG_ERROR, SRC_SYS, (uint8_t)ERR_WATCHDOG, v, 8);
            if (Sched_Send(&p, 1, 0))
                wdg_reported = 1u;
        }
        if (!stack_reported && g_boot_stack_thread != 0u)
        {
            LOG      p;
            uint8_t  v[8];
            uint32_t thr = g_boot_stack_thread;
            memcpy(&v[0], &thr, 4);
            memcpy(&v[4], &csr, 4);
            PacketBuilder_Create(&p, LOG_CRITICAL, SRC_SYS, (uint8_t)ERR_STACK_OVERFLOW, v, 8);
            if (Sched_Send(&p, 1, 0))
                stack_reported = 1u;
        }
    }

    {
        static uint32_t vbat_sent_seq = 0u;
        uint32_t seq = g_vbat_seq;
        if (seq != vbat_sent_seq)
        {
            uint8_t  v[8] = {0};
            uint16_t mv   = g_vbat_mv;
            uint16_t raw  = g_vbat_raw;
            uint16_t vdda = (uint16_t)g_adc_vdda_mv;
            memcpy(&v[0], &mv,   2);
            memcpy(&v[2], &raw,  2);
            memcpy(&v[4], &vdda, 2);
            if (Sched_EmitBoth(SRC_SYS, (uint8_t)KEY_SYS_VBAT, v))
                vbat_sent_seq = seq;
        }
    }


#if TEST_DUMMY_DATA
    {
        static uint8_t dummy_notice_sent = 0;
        if (!dummy_notice_sent) {
            LOG      w;
            uint8_t  v[8] = {0};
            uint32_t mark = 0x0000DDDDu;
            memcpy(&v[0], &mark, 4);
            PacketBuilder_Create(&w, LOG_CRITICAL, SRC_SYS,
                                 (uint8_t)ERR_DUMMY_DATA, v, 8);
            if (Sched_Send(&w, 1, 0))
                dummy_notice_sent = 1;
        }
    }
#endif

#if TEST_DUMMY_DATA
    Dummy_Update();
#endif

    if (tick % SCHED_PRIMARY_TICKS == SCHED_PHASE_PRIMARY)
    {
        uart_full_latch = 0;
        sd_full_latch   = 0;

        Sched_EmitPrimary();
    }

    if (tick % SCHED_DETAIL_STEP_TICKS == SCHED_PHASE_DETAIL)
    {
        uint8_t step = (uint8_t)((tick / SCHED_DETAIL_STEP_TICKS) % SCHED_DETAIL_STEPS);
        Sched_EmitDetailStep(step);
    }

    if (tick % SCHED_TIMEOUT_TICKS == SCHED_PHASE_TIMEOUT)
    {
        Sched_CheckTimeouts();
        Sched_EmitFreshness();
        CAN_Receiver_PollBusOff();
    }

    if (tick % SCHED_RTC_TICKS == SCHED_PHASE_RTC)
        Sched_Emit10s();

#if PHASE6_SAFETY
    if (tick % SCHED_IWDG_TICKS == SCHED_PHASE_IWDG)
    {
        uint32_t expected = ALIVE_BIT_UART;
#if PHASE45_UART_RX_CAN_TX
        expected |= ALIVE_BIT_UARTRX;
#endif
#if PHASE5_SD_LOGGING
        expected |= ALIVE_BIT_SD;
#endif
#if PHASE7_VCU_LCD_BRIDGE
        expected |= ALIVE_BIT_VCULCD;
#endif
        if ((alive_flags & expected) == expected)
        {
            HAL_IWDG_Refresh(&hiwdg);
            alive_flags = 0;
        }
        /* 하나라도 미SET → 리프레시 생략 → IWDG 타임아웃 리셋 (Req 9.6) */
    }
#endif
}
