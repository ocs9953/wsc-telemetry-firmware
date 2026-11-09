#include "vcu_lcd_bridge.h"
#include "app_threadx.h"
#include "phase_config.h"
#include "can_receiver.h"
#include "ext_can_controller.h"
#include "timer_scheduler.h"
#include "packet_builder.h"
#include "types.h"
#include <string.h>

#if PHASE7_VCU_LCD_BRIDGE

extern VehicleData_t VehicleData;
extern TX_QUEUE      UART_Queue;
extern TX_QUEUE      SD_Queue;

volatile uint32_t g_vcu_lcd_gap_min  = 0;
volatile uint32_t g_vcu_lcd_gap_max  = 0;
volatile uint32_t g_vcu_lcd_gap_last = 0;
volatile uint32_t g_lcd_last_payload_lo = 0;  volatile uint32_t g_lcd_last_payload_hi = 0;
volatile uint32_t g_vcu_lcd_tx_ok   = 0;
volatile uint32_t g_vcu_lcd_tx_fail = 0;

#define VCULCD_TO_LCD_ID_MIN    0x501u
#define VCULCD_TO_LCD_ID_MAX    0x50Fu
#define VCULCD_FROM_LCD_ID_MIN  0x510u
#define VCULCD_FROM_LCD_ID_MAX  0x51Fu

#define VCULCD_PAYLOAD_PROBE    0

#define VCULCD_TICK_MS          20u

typedef void (*VcuLcd_PackFn)(const VehicleData_t *vd, uint8_t *out);

typedef struct {
    uint16_t      lcd_id;
    uint16_t      period_ms;
    uint8_t       dlc;
    VcuLcd_PackFn pack;
} VcuLcd_ToLcdEntry;


static void pack_dash_summary(const VehicleData_t *vd, uint8_t *out)
{
    int32_t p_w = ((int32_t)vd->motor.bus_v * (int32_t)vd->motor.bus_i) / 1000;
    if (p_w >  32767)  p_w =  32767;
    if (p_w < -32768)  p_w = -32768;
    int16_t motor_power_w = (int16_t)p_w;

    memcpy(&out[0], &vd->motor.veh_speed,      2);
    memcpy(&out[2], &vd->motor.bus_i,          2);
#if VCULCD_PAYLOAD_PROBE
    {
        static uint16_t probe_seq = 0;
        uint16_t magic = 0xA55Au;
        probe_seq++;
        memcpy(&out[4], &magic,     2);
        memcpy(&out[6], &probe_seq, 2);
    }
#else
    memcpy(&out[4], &vd->bms.soc_permille, 2);
    memcpy(&out[6], &motor_power_w,            2);
#endif
}

static const VcuLcd_ToLcdEntry to_lcd_table[] = {
    { 0x501u, 100u, 8u, pack_dash_summary },
    { 0u, 0u, 0u, NULL }
};
#define TO_LCD_COUNT  (sizeof(to_lcd_table) / sizeof(to_lcd_table[0]))

static uint32_t to_lcd_last[TO_LCD_COUNT];

typedef uint8_t (*VcuLcd_XlatFn)(const uint8_t *in, uint8_t in_dlc, uint8_t *out);

typedef struct {
    uint16_t      lcd_id;
    uint16_t      vcu_id;
    uint16_t      min_gap_ms;
    VcuLcd_XlatFn xlat;
} VcuLcd_ToVcuEntry;


#define LCD_ACC_SPEED_MAX_KPH  150u

static uint8_t xlat_lcd_cmd(const uint8_t *in, uint8_t in_dlc, uint8_t *out)
{
    if (in_dlc < 2u) return 0u;
    if (in[0] > LCD_ACC_SPEED_MAX_KPH) return 0u;
    if (in[1] > 1u) return 0u;

    out[0] = in[0];
    out[1] = in[1];
    return 2u;
}

static const VcuLcd_ToVcuEntry to_vcu_table[] = {
    { 0x510u, 0x420u, 100u, xlat_lcd_cmd },
    { 0u, 0u, 0u, NULL }
};
#define TO_VCU_COUNT  (sizeof(to_vcu_table) / sizeof(to_vcu_table[0]))

static uint32_t to_vcu_last[TO_VCU_COUNT];

#define VCULCD_ERR_MIN_INTERVAL_MS  1000u

typedef struct {
    bool     valid;
    uint8_t  key;
    uint32_t detail;
    uint32_t last_ms;
    uint32_t suppressed;
} VcuLcd_ErrLimiter;

static VcuLcd_ErrLimiter err_lim_to_vcu;
static VcuLcd_ErrLimiter err_lim_to_lcd;

static void VcuLcd_EmitError(uint8_t err_key, uint32_t detail, uint32_t suppressed)
{
    LOG     pkt;
    uint8_t val[8] = {0};

    memcpy(&val[0], &detail,     4);
    memcpy(&val[4], &suppressed, 4);
    PacketBuilder_Create(&pkt, LOG_WARNING, SRC_CAN_ERR, err_key, val, 8);

    ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
    memcpy(msg, &pkt, sizeof(LOG));
    TelemQueue_Send(&UART_Queue, msg);
    TelemQueue_Send(&SD_Queue, msg);
}

static void VcuLcd_ReportError(VcuLcd_ErrLimiter *lim,
                               uint8_t err_key, uint32_t detail)
{
    uint32_t now = HAL_GetTick();

    if (lim->valid && lim->key == err_key && lim->detail == detail)
    {
        if ((uint32_t)(now - lim->last_ms) < VCULCD_ERR_MIN_INTERVAL_MS)
        {
            lim->suppressed++;
            return;
        }
    }
    else if (lim->valid && lim->suppressed > 0u)
    {
        VcuLcd_EmitError(lim->key, lim->detail, lim->suppressed);
        lim->suppressed = 0u;
    }

    VcuLcd_EmitError(err_key, detail, lim->suppressed);

    lim->valid      = true;
    lim->key        = err_key;
    lim->detail     = detail;
    lim->last_ms    = now;
    lim->suppressed = 0u;
}

void VcuLcd_Init(void)
{
    uint32_t now = HAL_GetTick();

    for (uint32_t i = 0; i < TO_LCD_COUNT; i++) to_lcd_last[i] = now;
    for (uint32_t i = 0; i < TO_VCU_COUNT; i++) to_vcu_last[i] = now;
}

bool VcuLcd_OnLcdFrame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    if (data == NULL || dlc > 8u)
        return false;

    if (can_id < VCULCD_FROM_LCD_ID_MIN || can_id > VCULCD_FROM_LCD_ID_MAX)
        return false;

    for (uint32_t i = 0; i < TO_VCU_COUNT; i++)
    {
        if (to_vcu_table[i].lcd_id == 0u)
            continue;
        if (to_vcu_table[i].lcd_id != (uint16_t)can_id)
            continue;

        uint32_t now = HAL_GetTick();
        if (to_vcu_table[i].min_gap_ms != 0u &&
            (now - to_vcu_last[i]) < to_vcu_table[i].min_gap_ms)
        {
            return false;
        }

        uint8_t out[8] = {0};
        uint8_t out_dlc;

        if (to_vcu_table[i].xlat != NULL)
        {
            out_dlc = to_vcu_table[i].xlat(data, dlc, out);
            if (out_dlc == 0u || out_dlc > 8u)
                return false;
        }
        else
        {
            memcpy(out, data, dlc);
            out_dlc = dlc;
        }

        to_vcu_last[i] = now;

        if (CAN_Receiver_Transmit(to_vcu_table[i].vcu_id, out, out_dlc) != HAL_OK)
        {
            VcuLcd_ReportError(&err_lim_to_vcu, (uint8_t)ERR_CAN_TX_FAIL,
                               0xB0000000u | to_vcu_table[i].vcu_id);
            return false;
        }
        return true;
    }

    return false;
}

void VcuLcd_Thread_Entry(ULONG thread_input)
{
    (void)thread_input;

    VcuLcd_Init();

    for (;;)
    {
        tx_thread_sleep(VCULCD_TICK_MS);

#if PHASE6_SAFETY
        Telemetry_AliveSet(ALIVE_BIT_VCULCD);
#endif

        uint32_t now = HAL_GetTick();

        uint32_t due[TO_LCD_COUNT];
        uint32_t n_due = 0u;

        for (uint32_t i = 0; i < TO_LCD_COUNT; i++)
        {
            if (to_lcd_table[i].lcd_id == 0u || to_lcd_table[i].period_ms == 0u)
                continue;
            if ((now - to_lcd_last[i]) < to_lcd_table[i].period_ms)
                continue;
            due[n_due++] = i;
        }

        for (uint32_t k = 0; k < n_due; k++)
        {
            uint32_t i = due[k];

            {
                uint32_t gap = now - to_lcd_last[i];
                if (g_vcu_lcd_gap_min == 0u || gap < g_vcu_lcd_gap_min)
                    g_vcu_lcd_gap_min = gap;
                if (gap > g_vcu_lcd_gap_max)
                    g_vcu_lcd_gap_max = gap;
                g_vcu_lcd_gap_last = gap;
            }
            to_lcd_last[i] = now;

            uint8_t payload[8] = {0};
            if (to_lcd_table[i].pack != NULL)
                to_lcd_table[i].pack(&VehicleData, payload);

            g_lcd_last_payload_lo = ((uint32_t)payload[3] << 24) | ((uint32_t)payload[2] << 16)
                                  | ((uint32_t)payload[1] <<  8) |  (uint32_t)payload[0];
            g_lcd_last_payload_hi = ((uint32_t)payload[7] << 24) | ((uint32_t)payload[6] << 16)
                                  | ((uint32_t)payload[5] <<  8) |  (uint32_t)payload[4];

            if (ExtCAN_TransmitFrame(to_lcd_table[i].lcd_id, payload,
                                     to_lcd_table[i].dlc) != HAL_OK)
            {
                g_vcu_lcd_tx_fail++;
                VcuLcd_ReportError(&err_lim_to_lcd, (uint8_t)ERR_CAN_TX_FAIL,
                                   0xB1000000u | to_lcd_table[i].lcd_id);
            }
            else
            {
                g_vcu_lcd_tx_ok++;
            }
        }
    }
}

#else

void VcuLcd_Init(void)
{
}

bool VcuLcd_OnLcdFrame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    (void)can_id; (void)data; (void)dlc;
    return false;
}

void VcuLcd_Thread_Entry(ULONG thread_input)
{
    (void)thread_input;
    for (;;)
    {
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
}

#endif
