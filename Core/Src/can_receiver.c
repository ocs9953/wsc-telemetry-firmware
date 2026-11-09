/**
  ******************************************************************************
  * @file    can_receiver.c
  * @brief   FDCAN1 수신 콜백 구현 — CAN ID 판별 → VehicleData 갱신
  *
  *          BMS·MPPT·Motor 모두 리틀엔디안 (float→scaled int)
  *          매핑·스케일 상세(SSOT): can-signal-summary.md §2~7
  *          Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 1.5
  ******************************************************************************
  */
#include "can_receiver.h"
#include "app_threadx.h"
#include "fdcan.h"
#include "main.h"
#include "types.h"
#include "packet_builder.h"
#include "freshness.h"
#include "tx_api.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External references
 * --------------------------------------------------------------------------- */
extern VehicleData_t VehicleData;
extern SYSTEM_STATE  SystemState;
extern TX_QUEUE      UART_Queue;
extern TX_QUEUE      SD_Queue;

#define CAN_MarkError()   SystemState_MarkFault(FAULT_IDX_CAN)

#define CAN_ID_BMS_BASE          0x040u

#define CAN_OFS_BMS_HEARTBEAT    0x00u
#define CAN_OFS_BMS_CMU_FIRST    0x01u
#define CAN_OFS_BMS_CMU_LAST     0xEFu
#define CAN_OFS_BMS_FACTORY      0xF0u
#define CAN_OFS_BMS_SOC          0xF4u
#define CAN_OFS_BMS_BAL_SOC      0xF5u
#define CAN_OFS_BMS_CHARGER      0xF6u
#define CAN_OFS_BMS_PRECHARGE    0xF7u
#define CAN_OFS_BMS_CELL_V       0xF8u
#define CAN_OFS_BMS_CELL_T       0xF9u
#define CAN_OFS_BMS_PACK_VI      0xFAu
#define CAN_OFS_BMS_STATUS       0xFBu
#define CAN_OFS_BMS_FAN          0xFCu
#define CAN_OFS_BMS_EXT_STATUS   0xFDu
#define CAN_OFS_BMS_BLOCK_LAST   0xFFu

#define CAN_ID_BMS_LAST     (CAN_ID_BMS_BASE + CAN_OFS_BMS_BLOCK_LAST)

#define CAN_ID_BMSLV_BASE   0x600u
#define CAN_ID_BMSLV_LAST   (CAN_ID_BMSLV_BASE + CAN_OFS_BMS_BLOCK_LAST)
#define CAN_ID_BMS_CMU_LAST (CAN_ID_BMS_BASE + CAN_OFS_BMS_CMU_FIRST + 3u * BMS_CMU_COUNT - 1u)

#define CAN_ID_PROHELION_BOOT_LO  0x7F0u
#define CAN_ID_PROHELION_BOOT_HI  0x7F4u

#define CAN_ID_TEST_HEARTBEAT     0x4FEu

#define CAN_ID_MPPT1_BASE      0x310
#define CAN_ID_MPPT2_BASE      0x320
#define CAN_ID_MPPT3_BASE      0x330
#define CAN_ID_MPPT4_BASE      0x340

#define CAN_ID_MPPT_FRAME_COUNT  7
#define CAN_ID_MPPT_LAST   (CAN_ID_MPPT4_BASE + CAN_ID_MPPT_FRAME_COUNT - 1)

#define CAN_ID_MOTOR_FLAGS     0x401   /* 🟠 Limit u16@0, Error u16@2          */
#define CAN_ID_MOTOR_BUS       0x402   /* 🟠 Bus_V f@0 ×100, Bus_I f@4 ×10     */
#define CAN_ID_MOTOR_VEL       0x403   /* 🟠 Motor_Vel f@0 rpm, Veh_Vel f@4 ×100 */
#define CAN_ID_MOTOR_TEMP      0x40B   /* 🟠 Motor_Temp f@0 ×10, HeatSink f@4 ×10 */
#define CAN_ID_MOTOR_ODO       0x40E   /* 🟠 Odometer f@0(m), DC_Bus_Ah f@4(Ah) → reg */

/* Motor DETAIL/SD_ONLY 진단 프레임 (🟠 잠정 — 기존 Motor 스케일 컨벤션 적용)
 * 바이트 오더는 매뉴얼(Appendix C) 규칙 준수: 표의 첫 변수 = byte4~7, 둘째 = byte0~3 */
#define CAN_ID_MOTOR_DSP_TEMP  0x40C
#define CAN_ID_MOTOR_PHASE_I   0x404
#define CAN_ID_MOTOR_V_VECTOR  0x405   /* 🟠 Vd f@4-7, Vq f@0-3, ×100 (SD_ONLY)            */
#define CAN_ID_MOTOR_I_VECTOR  0x406   /* 🟠 Id f@4-7, Iq f@0-3, ×10 (SD_ONLY)             */
#define CAN_ID_MOTOR_BEMF      0x407
#define CAN_ID_MOTOR_RAIL_A    0x408   /* 🟠 15V f@4-7 ×100 (byte0-3 Reserved, SD_ONLY)    */
#define CAN_ID_MOTOR_RAIL_B    0x409   /* 🟠 3.3V f@4-7, 1.9V f@0-3, ×100 (SD_ONLY)        */
#define CAN_ID_MOTOR_SLIP      0x417   /* 🟠 SlipSpeed f@4-7 ×10 (byte0-3 Reserved, SD_ONLY) */

#define CAN_FILTER0_ID1   0x040
#define CAN_FILTER0_ID2   0x34F
#define CAN_FILTER1_ID1   0x400
#define CAN_FILTER1_ID2   0x4FF
#define CAN_FILTER2_ID1   0x600
#define CAN_FILTER2_ID2   0x6FF
#define CAN_STD_FILTERS   3u

_Static_assert(CAN_ID_BMSLV_BASE >= CAN_FILTER2_ID1 && CAN_ID_BMSLV_LAST <= CAN_FILTER2_ID2,
               "LV BMS block (0x600-0x6FF) must sit inside filter2");
_Static_assert(CAN_FILTER1_ID2 < CAN_FILTER2_ID1,
               "filter1 and filter2 overlap - the 0x5xx gap between them is the point");
_Static_assert(CAN_ID_BMSLV_BASE > CAN_ID_BMS_LAST,
               "LV and HV BMS blocks overlap - one of the BMUs needs another base ID");
_Static_assert(CAN_ID_BMS_BASE >= CAN_FILTER0_ID1,
               "BMS base ID below filter0 start - lower CAN_FILTER0_ID1");
_Static_assert(CAN_ID_BMS_BASE + CAN_OFS_BMS_EXT_STATUS <= CAN_FILTER0_ID2,
               "BMS pack frames exceed filter0 - widen CAN_FILTER0_ID2");
_Static_assert(CAN_ID_BMS_LAST < CAN_ID_MPPT1_BASE,
               "BMS 256-ID block overlaps MPPT - move CAN_ID_BMS_BASE");
_Static_assert(CAN_ID_BMS_CMU_LAST <= CAN_ID_BMS_BASE + CAN_OFS_BMS_CMU_LAST,
               "Too many CMUs for the Prohelion CMU ID range");
_Static_assert(CAN_ID_MPPT_LAST <= CAN_FILTER0_ID2,
               "MPPT range exceeds filter0 - widen CAN_FILTER0_ID2");
_Static_assert(CAN_ID_MOTOR_FLAGS >= CAN_FILTER1_ID1,
               "Motor base ID below filter1 start - lower CAN_FILTER1_ID1");
_Static_assert(CAN_ID_MOTOR_SLIP <= CAN_FILTER1_ID2,
               "Motor range exceeds filter1 - widen CAN_FILTER1_ID2");

/* ---------------------------------------------------------------------------
 * Private helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief  Send a LOG packet to both UART_Queue and SD_Queue (non-blocking).
 */
static void CAN_SendLogToQueues(const LOG *pkt)
{
    ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
    memcpy(msg, pkt, sizeof(LOG));
    TelemQueue_Send(&UART_Queue, msg);
    TelemQueue_Send(&SD_Queue, msg);
}

/**
 * @brief  Extract a little-endian IEEE-754 float from a CAN payload (MPPT/Motor).
 */
static float CAN_ExtractFloat(const uint8_t *data, uint8_t offset)
{
    float val;
    memcpy(&val, &data[offset], sizeof(float));
    return val;
}

static inline uint16_t le_u16(const uint8_t *d, uint8_t off)
{ uint16_t v; memcpy(&v, &d[off], sizeof(v)); return v; }
static inline int16_t le_i16(const uint8_t *d, uint8_t off)
{ int16_t v; memcpy(&v, &d[off], sizeof(v)); return v; }
static inline uint32_t le_u32(const uint8_t *d, uint8_t off)
{ uint32_t v; memcpy(&v, &d[off], sizeof(v)); return v; }
static inline int32_t le_i32(const uint8_t *d, uint8_t off)
{ int32_t v; memcpy(&v, &d[off], sizeof(v)); return v; }

static inline uint16_t f_to_u16(float v)
{
    if (v != v)          return 0;
    if (v <= 0.0f)       return 0;
    if (v >= 65535.0f)   return 65535;
    return (uint16_t)(v + 0.5f);
}
static inline int16_t f_to_i16(float v)
{
    if (v != v)          return 0;
    if (v >=  32767.0f)  return  32767;
    if (v <= -32768.0f)  return -32768;
    return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* ---------------------------------------------------------------------------
 * 소스별 저장 핸들러 (RxCallback에서 호출, ISR 컨텍스트)
 * --------------------------------------------------------------------------- */

static void CAN_StoreBmsCmu(BmsData_t *b, uint32_t rel,
                            const uint8_t *d, uint32_t tick, uint32_t cmu_count)
{
    uint32_t n = rel / 3u;
    if (n >= cmu_count)
        return;

    BmsCmu_t *c = &b->cmu[n];
    switch (rel % 3u)
    {
    case 0:
        c->serial    = le_u32(d, 0);
        c->pcb_temp  = le_i16(d, 4);
        c->cell_temp = le_i16(d, 6);
        break;

    case 1:
        for (uint8_t k = 0; k < 4u; k++)
            c->cell_mv[k] = le_i16(d, (uint8_t)(k * 2u));
        break;

    default:
        for (uint8_t k = 0; k < 4u; k++)
            c->cell_mv[4u + k] = le_i16(d, (uint8_t)(k * 2u));
        break;
    }
    c->last_tick = tick;
}

static inline uint16_t u32_div_sat_u16(uint32_t v, uint32_t div)
{
    v /= div;
    return (v > 0xFFFFu) ? 0xFFFFu : (uint16_t)v;
}
static inline int16_t i32_div_sat_i16(int32_t v, int32_t div)
{
    v /= div;
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void CAN_StoreBmsPack(BmsData_t *b, uint32_t ofs,
                             const uint8_t *d, uint32_t tick)
{
    switch (ofs)
    {
    case CAN_OFS_BMS_SOC:
        b->soc_ah_cah   = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        b->soc_permille = f_to_u16(CAN_ExtractFloat(d, 4) * 10.0f);
        break;

    case CAN_OFS_BMS_BAL_SOC:
        b->bal_soc_ah_cah   = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        b->bal_soc_permille = f_to_u16(CAN_ExtractFloat(d, 4) * 10.0f);
        break;

    case CAN_OFS_BMS_CHARGER:
        b->chg_cell_v_err_mv  = le_i16(d, 0);
        b->cell_temp_margin   = le_i16(d, 2);
        b->dchg_cell_v_err_mv = le_i16(d, 4);
        b->pack_capacity_ah   = le_u16(d, 6);
        break;

    case CAN_OFS_BMS_PRECHARGE:
        b->contactor_status        = d[0];
        b->precharge_state         = d[1];
        b->contactor_err_seen     |= (uint8_t)(d[0] & BMS_CONTACTOR_ERR_MASK);
        if (d[1] == 0u)
            b->ext_status_seen    |= BMS_SEEN_PRECHARGE_ERROR;
        b->precharge_timer_elapsed = (uint8_t)(d[4] | d[6]);
        b->precharge_timer_count   = d[7];
        break;

    case CAN_OFS_BMS_CELL_V:
        b->min_cell_mv      = le_u16(d, 0);
        b->max_cell_mv      = le_u16(d, 2);
        b->min_cell_mv_cmu  = d[4];
        b->min_cell_mv_cell = d[5];
        b->max_cell_mv_cmu  = d[6];
        b->max_cell_mv_cell = d[7];
        break;

    case CAN_OFS_BMS_CELL_T:
        b->min_cell_temp     = le_i16(d, 0);
        b->max_cell_temp     = le_i16(d, 2);
        b->min_cell_temp_cmu = d[4];
        b->max_cell_temp_cmu = d[6];
        break;

    case CAN_OFS_BMS_PACK_VI:
        b->pack_v_cv    = u32_div_sat_u16(le_u32(d, 0), 10u);
        b->pack_current = i32_div_sat_i16(le_i32(d, 4), 10);
        break;

    case CAN_OFS_BMS_STATUS:
        b->bal_rise_mv  = le_u16(d, 0);
        b->bal_fall_mv  = le_u16(d, 2);
        b->status_flags = d[4];
        b->ext_status_seen |= d[4];
        b->cmu_count    = d[5];
        break;

    case CAN_OFS_BMS_FAN:
        b->fan0_rpm    = le_u16(d, 0);
        b->fan1_rpm    = le_u16(d, 2);
        b->i12v_fan_ma = le_u16(d, 4);
        b->i12v_cmu_ma = le_u16(d, 6);
        break;

    case CAN_OFS_BMS_EXT_STATUS:
        b->ext_status = le_u32(d, 0);
        b->ext_status_seen |= b->ext_status;
        break;

    default:
        return;
    }
    b->bms_last_tick = tick;
}

/**
 * @brief  MPPT 저장. off = CAN ID - base (0/1/2/5 처리, 그 외 DETAIL은 추후).
 */
static void CAN_StoreMppt(MpptData_t *m, uint8_t off,
                          const uint8_t *d, uint32_t tick)
{
    switch (off)
    {
    case 0x0:  /* Input_Voltage ×100, Input_Current ×100 */
        m->in_v = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        m->in_i = f_to_u16(CAN_ExtractFloat(d, 4) * 100.0f);
        break;

    case 0x1:  /* Output_Voltage ×100, Output_Current ×100 */
        m->out_v = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        m->out_i = f_to_u16(CAN_ExtractFloat(d, 4) * 100.0f);
        break;

    case 0x2:
        m->mosfet_temp = f_to_i16(CAN_ExtractFloat(d, 0) * 10.0f);
        m->ctrl_temp   = f_to_i16(CAN_ExtractFloat(d, 4) * 10.0f);
        break;

    case 0x5:
        m->error_flags |= d[3];
        m->mode         = d[4];
        break;

    case 0x3:
        m->aux_12v = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        m->aux_3v  = f_to_u16(CAN_ExtractFloat(d, 4) * 1000.0f);
        break;

    case 0x4:
        m->max_out_v = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        m->max_in_i  = f_to_u16(CAN_ExtractFloat(d, 4) * 100.0f);
        break;

    case 0x6:
        m->powerconn_v    = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        m->powerconn_temp = f_to_i16(CAN_ExtractFloat(d, 4) * 10.0f);
        break;

    default:
        return;
    }
    m->mppt_last_tick = tick;
}

/**
 * @brief  Motor 저장 (🟠 잠정 — WaveSculptor22 virtual, 리틀엔디안).
 * @param  vd  대상 VehicleData. 0x40E가 motor 하위가 아닌 규정 누적 필드
 *             (distance_traveled 등)도 갱신하므로 최상위 포인터를 받는다.
 */
static void CAN_StoreMotor(VehicleData_t *vd, uint32_t id,
                           const uint8_t *d, uint32_t tick)
{
    MotorData_t *mo = &vd->motor;

    switch (id)
    {
    case CAN_ID_MOTOR_FLAGS:
        mo->limit_flags     |= (uint16_t)(d[0] | (d[1] << 8));
        mo->error_flags     |= (uint16_t)(d[2] | (d[3] << 8));
        mo->active_motor    = (uint16_t)(d[4] | (d[5] << 8));
        mo->can_tx_err_count = d[6];
        mo->can_rx_err_count = d[7];
        break;

    case CAN_ID_MOTOR_BUS:    /* 0x402: Bus_V ×100 u16, Bus_I ×10 i16(regen 음수) */
        mo->bus_v = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        mo->bus_i = f_to_i16(CAN_ExtractFloat(d, 4) * 10.0f);
        break;

    case CAN_ID_MOTOR_VEL:    /* 0x403: Motor_Vel rpm i16, Veh_Vel ×100 u16 */
        mo->motor_rpm = f_to_i16(CAN_ExtractFloat(d, 0));
        mo->veh_speed = f_to_u16(CAN_ExtractFloat(d, 4) * 100.0f);
        break;

    case CAN_ID_MOTOR_TEMP:   /* 0x40B: Motor_Temp@0 ×10, HeatSink_Temp@4 ×10 */
        mo->motor_temp    = f_to_i16(CAN_ExtractFloat(d, 0) * 10.0f);
        mo->heatsink_temp = f_to_i16(CAN_ExtractFloat(d, 4) * 10.0f);
        break;

    case CAN_ID_MOTOR_ODO:
        vd->distance_traveled          = CAN_ExtractFloat(d, 0);
        vd->battery_accumulated_energy = CAN_ExtractFloat(d, 4);
        vd->odo_last_tick              = tick;
        return;

    case CAN_ID_MOTOR_DSP_TEMP:
        mo->dsp_temp = f_to_i16(CAN_ExtractFloat(d, 0) * 10.0f);
        break;

    case CAN_ID_MOTOR_PHASE_I:
        mo->phase_b_i = f_to_i16(CAN_ExtractFloat(d, 0) * 10.0f);
        mo->phase_c_i = f_to_i16(CAN_ExtractFloat(d, 4) * 10.0f);
        break;

    case CAN_ID_MOTOR_V_VECTOR:  /* 0x405: Vd@byte4-7, Vq@byte0-3 ×100 (SD_ONLY)
                                    ⚠ 2026-07 수정: 공식 매뉴얼(Appendix C §18.4.6) 대조 결과
                                    바이트 순서가 반대였음 — "Vd 63..32"(byte4-7) 먼저, "Vq 31..0"(byte0-3) */
        mo->v_vector_d = f_to_i16(CAN_ExtractFloat(d, 4) * 100.0f);
        mo->v_vector_q = f_to_i16(CAN_ExtractFloat(d, 0) * 100.0f);
        break;

    case CAN_ID_MOTOR_I_VECTOR:  /* 0x406: Id@byte4-7, Iq@byte0-3 ×10 (SD_ONLY)
                                    ⚠ 2026-07 수정: §18.4.7 "Id 63..32"(byte4-7), "Iq 31..0"(byte0-3) */
        mo->i_vector_d = f_to_i16(CAN_ExtractFloat(d, 4) * 10.0f);
        mo->i_vector_q = f_to_i16(CAN_ExtractFloat(d, 0) * 10.0f);
        break;

    case CAN_ID_MOTOR_BEMF:
        mo->bemf_d = f_to_i16(CAN_ExtractFloat(d, 4) * 100.0f);
        mo->bemf_q = f_to_i16(CAN_ExtractFloat(d, 0) * 100.0f);
        break;

    case CAN_ID_MOTOR_RAIL_A:    /* 0x408: 15V supply @ byte4-7만 (byte0-3 Reserved, SD_ONLY)
                                    ⚠ 2026-07 수정: §18.4.9 — 이 프레임엔 15V만 있고 3.3V는 없음
                                    (기존 구현이 이 프레임에서 3.3V까지 읽던 것은 오류, 0x409에 별도 존재) */
        mo->rail_15v = f_to_u16(CAN_ExtractFloat(d, 4) * 100.0f);
        break;

    case CAN_ID_MOTOR_RAIL_B:    /* 0x409: 3.3V@byte4-7, 1.9V@byte0-3 ×100 (SD_ONLY)
                                    ⚠ 2026-07 수정: §18.4.10 "3.3V supply 63..32"(byte4-7), "1.9V supply 31..0"(byte0-3) */
        mo->rail_3v3 = f_to_u16(CAN_ExtractFloat(d, 4) * 100.0f);
        mo->rail_1v9 = f_to_u16(CAN_ExtractFloat(d, 0) * 100.0f);
        break;

    case CAN_ID_MOTOR_SLIP:      /* 0x417: SlipSpeed @ byte4-7만 (byte0-3 Reserved, SD_ONLY)
                                    ⚠ 2026-07 수정: §18.4.16 "Slip Speed 63..32"(byte4-7) — byte0-3에서 읽던 것은 Reserved 영역이었음 */
        mo->slip_speed = f_to_i16(CAN_ExtractFloat(d, 4) * 10.0f);
        break;

    default:
        return;  /* 미정의 ID — 무시 */
    }
    mo->motor_last_tick = tick;
}

volatile uint32_t g_can_rx_frames = 0;


volatile uint32_t g_can_rx_lost   = 0;
volatile uint32_t g_can_gap_max_ms = 0;
volatile uint32_t g_can_gap_last_ms = 0;
volatile uint32_t g_can_seq_last   = 0;
volatile uint32_t g_can_seq_missed = 0;
volatile uint32_t g_can_seq_frames = 0;
volatile uint32_t g_can_seq_restarts = 0;

#define CAN_SEQ_RESTART_STEP  0x10000u


static void CAN_TrackHeartbeat(const uint8_t *d)
{
    uint32_t seq = (uint32_t)d[0] | ((uint32_t)d[1] << 8)
                 | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);

    if (g_can_seq_frames != 0u)
    {
        uint32_t step = seq - g_can_seq_last;

        if (step > CAN_SEQ_RESTART_STEP)
        {
            g_can_seq_restarts++;
        }
        else if (step > 1u)
        {
            g_can_seq_missed += (step - 1u);
        }
    }
    g_can_seq_last = seq;
    g_can_seq_frames++;
}
volatile uint32_t g_can_short_frames = 0;

static uint32_t CAN_MinDlc(uint32_t id)
{
    if ((id >= CAN_ID_BMS_BASE && id <= CAN_ID_BMS_LAST) ||
        (id >= CAN_ID_BMSLV_BASE && id <= CAN_ID_BMSLV_LAST))
        return 8u;
    if (id >= CAN_ID_MPPT1_BASE && id <= CAN_ID_MPPT_LAST)
        return (((id - CAN_ID_MPPT1_BASE) % 0x10u) == 0x5u) ? 5u
                                                            : 8u;
    if (id == CAN_ID_MOTOR_DSP_TEMP)
        return 4u;
    return 8u;
}

static uint32_t CAN_FreshFrame(uint32_t id)
{
    if (id >= CAN_ID_BMSLV_BASE && id <= CAN_ID_BMSLV_LAST)
    {
        switch (id - CAN_ID_BMSLV_BASE)
        {
        case CAN_OFS_BMS_SOC:     return FRESH_FR_BMSLV_SOC;
        case CAN_OFS_BMS_CELL_V:  return FRESH_FR_BMSLV_CELL_V;
        case CAN_OFS_BMS_CELL_T:  return FRESH_FR_BMSLV_CELL_T;
        case CAN_OFS_BMS_PACK_VI: return FRESH_FR_BMSLV_PACK_VI;
        case CAN_OFS_BMS_STATUS:  return FRESH_FR_BMSLV_STATUS;
        default:                  return FRESH_FR_COUNT;
        }
    }
    if (id >= CAN_ID_BMS_BASE && id <= CAN_ID_BMS_LAST)
    {
        uint32_t ofs = id - CAN_ID_BMS_BASE;
        if (ofs >= CAN_OFS_BMS_CMU_FIRST && ofs <= CAN_ID_BMS_CMU_LAST - CAN_ID_BMS_BASE)
            return FRESH_FR_CMU_FIRST + (ofs - CAN_OFS_BMS_CMU_FIRST);
        if (ofs >= CAN_OFS_BMS_SOC && ofs <= CAN_OFS_BMS_EXT_STATUS)
            return FRESH_FR_BMS_SOC + (ofs - CAN_OFS_BMS_SOC);
        return FRESH_FR_COUNT;
    }
    if (id >= CAN_ID_MPPT1_BASE && id <= CAN_ID_MPPT_LAST)
    {
        uint32_t unit = (id - CAN_ID_MPPT1_BASE) / 0x10u;
        uint32_t off  = (id - CAN_ID_MPPT1_BASE) % 0x10u;
        uint32_t slot;
        switch (off)
        {
        case 0x0: slot = 0u; break;
        case 0x1: slot = 1u; break;
        case 0x2: slot = 2u; break;
        case 0x5: slot = 3u; break;
        case 0x4: slot = 4u; break;
        case 0x6: slot = 5u; break;
        default:  return FRESH_FR_COUNT;
        }
        return FRESH_FR_MPPT_FIRST + unit * FRESH_MPPT_SLOTS + slot;
    }
    switch (id)
    {
    case CAN_ID_MOTOR_FLAGS:    return FRESH_FR_MOTOR_STATUS;
    case CAN_ID_MOTOR_BUS:      return FRESH_FR_MOTOR_BUS;
    case CAN_ID_MOTOR_VEL:      return FRESH_FR_MOTOR_VEL;
    case CAN_ID_MOTOR_TEMP:     return FRESH_FR_MOTOR_TEMP;
    case CAN_ID_MOTOR_DSP_TEMP: return FRESH_FR_MOTOR_DSP_TEMP;
    case CAN_ID_MOTOR_ODO:      return FRESH_FR_MOTOR_ODO;
    default:                    return FRESH_FR_COUNT;
    }
}

_Static_assert(FRESH_FR_BMS_EXT_STATUS - FRESH_FR_BMS_SOC == CAN_OFS_BMS_EXT_STATUS - CAN_OFS_BMS_SOC &&
               FRESH_FR_BMS_PACK_VI  - FRESH_FR_BMS_SOC == CAN_OFS_BMS_PACK_VI  - CAN_OFS_BMS_SOC,
               "freshness BMS frame order must follow CAN offsets");

_Static_assert(CAN_ID_MPPT2_BASE - CAN_ID_MPPT1_BASE == 0x10u &&
               CAN_ID_MPPT3_BASE - CAN_ID_MPPT2_BASE == 0x10u &&
               CAN_ID_MPPT4_BASE - CAN_ID_MPPT3_BASE == 0x10u,
               "MPPT bases must be 0x10 apart for CAN_MinDlc");

/* ---------------------------------------------------------------------------
 * CAN_Receiver_Init  (Req 1.5, 3.3, 3.7)
 *
 * FDCAN1 하드웨어 필터 설정, 인터럽트 활성화, 시작.
 * CubeMX가 NominalPrescaler=10, TimeSeg1=12, TimeSeg2=3 (Classic CAN 500kbps)와
 * StdFiltersNbr=3을 MX_FDCAN1_Init()에 설정함 → 본 함수는 필터 값만 구성.
 * (2026-09-28 LV BMS 전용 필터 2 추가로 2→3. 값이 3보다 작으면 아래에서 초기화를 실패시킨다.)
 * --------------------------------------------------------------------------- */
HAL_StatusTypeDef CAN_Receiver_Init(void)
{
    FDCAN_FilterTypeDef filter;

    filter.IdType       = FDCAN_STANDARD_ID;
    filter.FilterIndex  = 0;
    filter.FilterType   = FDCAN_FILTER_RANGE;
    filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID1    = CAN_FILTER0_ID1;
    filter.FilterID2    = CAN_FILTER0_ID2;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK)
        return HAL_ERROR;

    filter.FilterIndex  = 1;
    filter.FilterID1    = CAN_FILTER1_ID1;
    filter.FilterID2    = CAN_FILTER1_ID2;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK)
        return HAL_ERROR;

    if (hfdcan1.Init.StdFiltersNbr < CAN_STD_FILTERS)
        return HAL_ERROR;
    filter.FilterIndex  = 2;
    filter.FilterID1    = CAN_FILTER2_ID1;
    filter.FilterID2    = CAN_FILTER2_ID2;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK)
        return HAL_ERROR;

    /* Reject non-matching standard frames, reject all remote/extended */
    if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1,
            FDCAN_REJECT, FDCAN_REJECT,
            FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK)
        return HAL_ERROR;

    /* Enable Rx FIFO0 new-message + FIFO0 full + message lost notifications */
    if (HAL_FDCAN_ActivateNotification(&hfdcan1,
            FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_FULL
            | FDCAN_IT_RX_FIFO0_MESSAGE_LOST,
            0) != HAL_OK)
        return HAL_ERROR;

    /* Enable bus-off, error warning, error passive notifications */
    if (HAL_FDCAN_ActivateNotification(&hfdcan1,
            FDCAN_IT_BUS_OFF | FDCAN_IT_ERROR_WARNING
            | FDCAN_IT_ERROR_PASSIVE,
            0) != HAL_OK)
        return HAL_ERROR;

    /* Start FDCAN1 */
    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)
        return HAL_ERROR;

    SystemState.can_ok = 1;

    return HAL_OK;
}

HAL_StatusTypeDef CAN_Receiver_Transmit(uint32_t can_id,
                                        const uint8_t *data, uint8_t dlc)
{
    if (data == NULL || dlc > 8u)
        return HAL_ERROR;

    if ((can_id >= CAN_ID_BMS_BASE && can_id <= CAN_ID_BMS_LAST) ||
        (can_id >= CAN_ID_BMSLV_BASE && can_id <= CAN_ID_BMSLV_LAST) ||
        (can_id >= CAN_ID_PROHELION_BOOT_LO && can_id <= CAN_ID_PROHELION_BOOT_HI) ||
        (can_id >= CAN_ID_MPPT1_BASE  && can_id <= CAN_ID_MPPT_LAST) ||
        (can_id >= CAN_ID_MOTOR_FLAGS && can_id <= CAN_ID_MOTOR_SLIP))
        return HAL_ERROR;

    static const uint32_t dlc_map[9] = {
        FDCAN_DLC_BYTES_0, FDCAN_DLC_BYTES_1, FDCAN_DLC_BYTES_2,
        FDCAN_DLC_BYTES_3, FDCAN_DLC_BYTES_4, FDCAN_DLC_BYTES_5,
        FDCAN_DLC_BYTES_6, FDCAN_DLC_BYTES_7, FDCAN_DLC_BYTES_8
    };

    FDCAN_TxHeaderTypeDef tx;
    tx.Identifier          = can_id;
    tx.IdType              = FDCAN_STANDARD_ID;
    tx.TxFrameType         = FDCAN_DATA_FRAME;
    tx.DataLength          = dlc_map[dlc];
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch       = FDCAN_BRS_OFF;
    tx.FDFormat            = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
    tx.MessageMarker       = 0;

    if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0u)
        return HAL_ERROR;

    return HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &tx, (uint8_t *)data);
}

/* ---------------------------------------------------------------------------
 * CAN_Receiver_RxCallback  (Req 3.1, 3.2, 3.4)
 *
 * Called from HAL_FDCAN_RxFifo0Callback() in main.c USER CODE block.
 * Runs in ISR context — no blocking calls.
 * --------------------------------------------------------------------------- */
void CAN_Receiver_RxCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    FDCAN_RxHeaderTypeDef rx_header;
    uint8_t               rx_data[8];

    /* --- 1. 신규 메시지 수신 --- */
    if (RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE)
    {
        /* Read all available messages from FIFO0 */
        while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) > 0u)
        {
            memset(rx_data, 0, sizeof(rx_data));

            if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0,
                                       &rx_header, rx_data) != HAL_OK)
                break;

            g_can_rx_frames++;

            uint32_t tick = HAL_GetTick();
            uint32_t id   = rx_header.Identifier;

            if (g_can_gap_last_ms != 0u)
            {
                uint32_t gap = tick - g_can_gap_last_ms;
                if (gap > g_can_gap_max_ms) g_can_gap_max_ms = gap;
            }
            g_can_gap_last_ms = (tick != 0u) ? tick : 1u;

            if (id == CAN_ID_TEST_HEARTBEAT)
            {
                if (rx_header.DataLength >= 4u) CAN_TrackHeartbeat(rx_data);
                continue;
            }

            if (rx_header.DataLength < CAN_MinDlc(id))
            {
                g_can_short_frames++;
                continue;
            }

            Fresh_Note(CAN_FreshFrame(id), tick);

            /* 본 콜백은 FDCAN ISR(우선순위 1)에서 실행된다. VehicleData를
               쓰는 다른 주체(TIM5=읽기/동일 우선순위, MCP_RX_Thread=하위
               우선순위)에게 선점되지 않으므로 별도 임계구역이 필요 없다.
               (MCP 측은 스레드 컨텍스트라 TX_DISABLE/TX_RESTORE로 보호) */

            if (id >= CAN_ID_MPPT1_BASE && id <= CAN_ID_MPPT_LAST)
            {
                uint32_t base;
                uint8_t  unit;
                if      (id >= CAN_ID_MPPT4_BASE) { unit = 3; base = CAN_ID_MPPT4_BASE; }
                else if (id >= CAN_ID_MPPT3_BASE) { unit = 2; base = CAN_ID_MPPT3_BASE; }
                else if (id >= CAN_ID_MPPT2_BASE) { unit = 1; base = CAN_ID_MPPT2_BASE; }
                else                              { unit = 0; base = CAN_ID_MPPT1_BASE; }
                CAN_StoreMppt(&VehicleData.mppt[unit],
                              (uint8_t)(id - base), rx_data, tick);
            }
            else if (id >= CAN_ID_BMS_BASE && id <= CAN_ID_BMS_LAST)
            {
                uint32_t ofs = id - CAN_ID_BMS_BASE;
                if (ofs >= CAN_OFS_BMS_CMU_FIRST && ofs <= CAN_OFS_BMS_CMU_LAST)
                    CAN_StoreBmsCmu(&VehicleData.bms, ofs - CAN_OFS_BMS_CMU_FIRST,
                                    rx_data, tick, BMS_CMU_COUNT);
                else
                    CAN_StoreBmsPack(&VehicleData.bms, ofs, rx_data, tick);
            }
            else if (id >= CAN_ID_BMSLV_BASE && id <= CAN_ID_BMSLV_LAST)
            {
                uint32_t ofs = id - CAN_ID_BMSLV_BASE;
                if (ofs >= CAN_OFS_BMS_CMU_FIRST && ofs <= CAN_OFS_BMS_CMU_LAST)
                    CAN_StoreBmsCmu(&VehicleData.bms_lv, ofs - CAN_OFS_BMS_CMU_FIRST,
                                    rx_data, tick, BMS_LV_CMU_COUNT);
                else
                    CAN_StoreBmsPack(&VehicleData.bms_lv, ofs, rx_data, tick);
            }
            else if (id >= CAN_FILTER1_ID1 && id <= CAN_FILTER1_ID2)
            {
                CAN_StoreMotor(&VehicleData, id, rx_data, tick);
            }

        }

        if (!SystemState.can_ok && SystemState_FaultHoldExpired(FAULT_IDX_CAN))
        {
            SystemState.can_ok = 1;
            HAL_GPIO_WritePin(LED_CAN_GPIO_Port, LED_CAN_Pin, GPIO_PIN_RESET);
            SystemState_RefreshBoardLed();
        }
    }

    if (RxFifo0ITs & FDCAN_IT_RX_FIFO0_FULL)
    {
        static uint32_t last_full_log_ms = 0;
        uint32_t now_ms = HAL_GetTick();

        if ((uint32_t)(now_ms - last_full_log_ms) >= 1000u)
        {
            LOG     warn_pkt;
            uint8_t val[8] = {0};

            last_full_log_ms = now_ms;
            PacketBuilder_Create(&warn_pkt, LOG_WARNING, SRC_CAN_ERR,
                                 (uint8_t)ERR_CAN_RX_OVERFLOW, val, 8);
            CAN_SendLogToQueues(&warn_pkt);
        }
    }

    if (RxFifo0ITs & FDCAN_IT_RX_FIFO0_MESSAGE_LOST)
    {
        g_can_rx_lost++;
        LOG     warn_pkt;
        uint8_t val[8] = {0};

        PacketBuilder_Create(&warn_pkt, LOG_WARNING, SRC_CAN_ERR,
                             (uint8_t)ERR_CAN_RX_OVERFLOW, val, 8);
        CAN_SendLogToQueues(&warn_pkt);

        CAN_MarkError();
        SystemState.can_ok = 0;
        HAL_GPIO_WritePin(LED_CAN_GPIO_Port, LED_CAN_Pin, GPIO_PIN_SET);
        SystemState_RefreshBoardLed();
    }
}

/* ---------------------------------------------------------------------------
 * CAN_Receiver_ErrorCallback  (Req 3.5)
 *
 * Called from HAL_FDCAN_ErrorCallback() — protocol errors (PEA, PED, etc).
 * Generates ERR_CAN_BUS_OFF ERROR LOG → UART_Queue + SD_Queue, LED_CAN on.
 * --------------------------------------------------------------------------- */
void CAN_Receiver_ErrorCallback(FDCAN_HandleTypeDef *hfdcan)
{
    LOG      err_pkt;
    uint8_t  val[8]  = {0};
    uint32_t hal_err = HAL_FDCAN_GetError(hfdcan);

    const uint32_t ignore_mask = HAL_FDCAN_ERROR_FIFO_EMPTY
                               | HAL_FDCAN_ERROR_FIFO_FULL;

    hfdcan->ErrorCode &= ~ignore_mask;

    if ((hal_err & ~ignore_mask) == 0u)
        return;

    hfdcan->ErrorCode = HAL_FDCAN_ERROR_NONE;

    /* value: HAL error code (4B) + 0 (4B) */
    memcpy(&val[0], &hal_err, 4);

    PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_CAN_ERR,
                         (uint8_t)ERR_CAN_BUS_OFF, val, 8);
    CAN_SendLogToQueues(&err_pkt);

    CAN_MarkError();
    SystemState.can_ok = 0;
    SystemState.error  = 1;
    HAL_GPIO_WritePin(LED_CAN_GPIO_Port, LED_CAN_Pin, GPIO_PIN_SET);
    SystemState_RefreshBoardLed();
}

volatile uint32_t g_can_busoff_count   = 0;
volatile uint32_t g_can_busoff_recover = 0;

void CAN_Receiver_PollBusOff(void)
{
    if (hfdcan1.State != HAL_FDCAN_STATE_BUSY)
        return;
    if ((hfdcan1.Instance->CCCR & FDCAN_CCCR_INIT) == 0u)
        return;

    CLEAR_BIT(hfdcan1.Instance->CCCR, FDCAN_CCCR_INIT);
    g_can_busoff_recover++;
}

/* ---------------------------------------------------------------------------
 * CAN_Receiver_ErrorStatusCallback
 *
 * Called from HAL_FDCAN_ErrorStatusCallback() — Bus-Off, Error Passive,
 * Error Warning status changes.
 * ErrorCode parameter contains FDCAN_IR_BO / FDCAN_IR_EP / FDCAN_IR_EW bits.
 * --------------------------------------------------------------------------- */
void CAN_Receiver_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan,
                                       uint32_t ErrorStatusITs)
{
    LOG err_pkt;
    uint8_t val[8] = {0};
    FDCAN_ProtocolStatusTypeDef psr;

    HAL_FDCAN_GetProtocolStatus(hfdcan, &psr);

    /* value: ErrorStatusITs (4B) + PSR.LastErrorCode + PSR.Activity (4B) */
    memcpy(&val[0], &ErrorStatusITs, 4);
    uint32_t psr_info = ((uint32_t)psr.LastErrorCode) | ((uint32_t)psr.Activity << 8);
    memcpy(&val[4], &psr_info, 4);

    if (ErrorStatusITs & FDCAN_IR_BO)
    {
        g_can_busoff_count++;
        PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_CAN_ERR,
                             (uint8_t)ERR_CAN_BUS_OFF, val, 8);
        CAN_MarkError();
        SystemState.can_ok = 0;
        SystemState.error  = 1;
        HAL_GPIO_WritePin(LED_CAN_GPIO_Port, LED_CAN_Pin, GPIO_PIN_SET);
        SystemState_RefreshBoardLed();
    }
    else if (ErrorStatusITs & FDCAN_IR_EP)
    {
        PacketBuilder_Create(&err_pkt, LOG_WARNING, SRC_CAN_ERR,
                             (uint8_t)ERR_CAN_ERROR_PASSIVE, val, 8);
    }
    else if (ErrorStatusITs & FDCAN_IR_EW)
    {
        PacketBuilder_Create(&err_pkt, LOG_WARNING, SRC_CAN_ERR,
                             (uint8_t)ERR_CAN_ERROR_WARNING, val, 8);
    }
    else
    {
        return;  /* Unknown status — skip */
    }

    CAN_SendLogToQueues(&err_pkt);
}

/* (RxFifo0FullCallback은 RxCallback 내부에서 RxFifo0ITs 플래그로 통합 처리) */
