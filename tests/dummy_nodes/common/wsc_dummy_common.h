#ifndef WSC_DUMMY_COMMON_H
#define WSC_DUMMY_COMMON_H

#include <stdint.h>
#include <string.h>


#define ID_BMSHV_BASE           0x040
#define ID_BMSLV_BASE           0x600
#define BMS_OFS_HEARTBEAT       0x00
#define BMS_OFS_CMU_FIRST       0x01
#define BMS_OFS_SOC             0xF4
#define BMS_OFS_BAL_SOC         0xF5
#define BMS_OFS_CHARGER         0xF6
#define BMS_OFS_PRECHARGE       0xF7
#define BMS_OFS_CELL_V          0xF8
#define BMS_OFS_CELL_T          0xF9
#define BMS_OFS_PACK_VI         0xFA
#define BMS_OFS_STATUS          0xFB
#define BMS_OFS_FAN             0xFC
#define BMS_OFS_EXT_STATUS      0xFD

#define BMS_CELL_ABSENT         ((int16_t)-32768)

#define ID_MPPT1_BASE           0x310
#define ID_MPPT2_BASE           0x320
#define ID_MPPT3_BASE           0x330
#define ID_MPPT4_BASE           0x340
#define MPPT_UNIT_COUNT         4
#define MPPT_FRAME_COUNT        7

#define ID_MOTOR_FLAGS          0x401
#define ID_MOTOR_BUS            0x402
#define ID_MOTOR_VEL            0x403
#define ID_MOTOR_PHASE_I        0x404
#define ID_MOTOR_V_VECTOR       0x405
#define ID_MOTOR_I_VECTOR       0x406
#define ID_MOTOR_BEMF           0x407
#define ID_MOTOR_RAIL_A         0x408
#define ID_MOTOR_RAIL_B         0x409
#define ID_MOTOR_TEMP           0x40B
#define ID_MOTOR_DSP_TEMP       0x40C
#define ID_MOTOR_ODO            0x40E
#define ID_MOTOR_SLIP           0x417


static inline void lef(uint8_t *d, uint8_t off, float v)
{
    memcpy(&d[off], &v, sizeof(float));
}

static inline void lei16(uint8_t *d, uint8_t off, int16_t v)
{
    d[off]     = (uint8_t)((uint16_t)v & 0xFF);
    d[off + 1] = (uint8_t)((uint16_t)v >> 8);
}

static inline void leu32(uint8_t *d, uint8_t off, uint32_t v)
{
    d[off]     = (uint8_t)(v & 0xFF);
    d[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    d[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    d[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void le16(uint8_t *d, uint8_t off, uint16_t v)
{
    d[off]     = (uint8_t)(v & 0xFF);
    d[off + 1] = (uint8_t)(v >> 8);
}

static inline int32_t tri_wave(uint32_t t, uint32_t period, int32_t lo, int32_t hi)
{
    if (period == 0u) return lo;
    uint32_t p = t % period;
    uint32_t half = period / 2u;
    int32_t span = hi - lo;
    if (p < half)
        return lo + (int32_t)((uint64_t)span * p / (half ? half : 1u));
    return hi - (int32_t)((uint64_t)span * (p - half) / (half ? half : 1u));
}

static inline float tri_wave_f(uint32_t t, uint32_t period, float lo, float hi)
{
    int32_t scaled = tri_wave(t, period, 0, 10000);
    return lo + (hi - lo) * ((float)scaled / 10000.0f);
}

#ifdef HAL_CAN_MODULE_ENABLED
uint8_t wsc_can_send(CAN_HandleTypeDef *hcan, uint16_t std_id, const uint8_t *data);

uint8_t wsc_can_send_dlc(CAN_HandleTypeDef *hcan, uint16_t std_id,
                         const uint8_t *data, uint8_t dlc);

uint8_t wsc_can_send_dlc(CAN_HandleTypeDef *hcan, uint16_t std_id,
                         const uint8_t *data, uint8_t dlc);
#endif


#define ID_TEST_HEARTBEAT_BUS_A  0x4FE
#define ID_TEST_HEARTBEAT_BUS_B  0x51F

#ifdef HAL_CAN_MODULE_ENABLED
void wsc_heartbeat_tick(CAN_HandleTypeDef *hcan, uint16_t std_id,
                        uint32_t period_ms);
#endif

#endif
