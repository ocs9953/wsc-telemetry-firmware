#include "main.h"
#include "bms_dummy.h"
#include "wsc_dummy_common.h"

#define PERIOD_FAST_MS   100u
#define PERIOD_SLOW_MS  1000u

#define CELLS_PER_CMU      8u

static CAN_HandleTypeDef *s_hcan;
static uint32_t t_fast, t_slow;
static uint32_t seq;

typedef struct {
    uint16_t base;
    uint8_t  cmu_count;
    uint8_t  cells;
    int32_t  cell_lo_mv, cell_hi_mv;
    int32_t  i_lo_ma, i_hi_ma;
    float    capacity_ah;
    uint32_t serial;
    int32_t  temp_bias;
} bms_cfg_t;

static const bms_cfg_t HV = {
    ID_BMSHV_BASE, 4u, 32u, 3000, 4150, -15000, 30000, 30.0f, 0x48560000u,  0
};
static const bms_cfg_t LV = {
    ID_BMSLV_BASE, 1u,  4u, 3000, 3500,  -3000,  8000,  7.0f, 0x4C560000u, 20
};

void BMS_Dummy_Init(CAN_HandleTypeDef *hcan)
{
    s_hcan = hcan;
    t_fast = t_slow = HAL_GetTick();
    seq = 0u;
}

static inline uint16_t id_of(const bms_cfg_t *c, uint8_t ofs) { return (uint16_t)(c->base + ofs); }

static int16_t cell_mv(const bms_cfg_t *c, uint32_t idx)
{
    if (idx >= c->cells) return BMS_CELL_ABSENT;
    return (int16_t)tri_wave(seq + idx * 23u, 700u, c->cell_lo_mv, c->cell_hi_mv);
}

static int16_t cmu_cell_temp(const bms_cfg_t *c, uint32_t n)
{
    return (int16_t)(tri_wave(seq + n * 37u, 900u, 250, 420) + c->temp_bias);
}

static int32_t pack_ma(const bms_cfg_t *c)
{
    return tri_wave(seq, 500u, c->i_lo_ma, c->i_hi_ma);
}

static void send_fast(const bms_cfg_t *c)
{
    uint8_t d[8];

    memset(d, 0, 8);
    lei16(d, 0, (int16_t)tri_wave(seq, 300u, -50, 50));
    lei16(d, 2, (int16_t)tri_wave(seq, 900u, 50, 200));
    lei16(d, 4, (int16_t)tri_wave(seq, 300u, -50, 50));
    le16(d, 6, (uint16_t)c->capacity_ah);
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_CHARGER), d);

    int16_t lo = 0x7FFF, hi = -0x7FFF;
    uint8_t lo_i = 0u, hi_i = 0u;
    for (uint8_t i = 0u; i < c->cells; i++)
    {
        int16_t v = cell_mv(c, i);
        if (v < lo) { lo = v; lo_i = i; }
        if (v > hi) { hi = v; hi_i = i; }
    }
    memset(d, 0, 8);
    le16(d, 0, (uint16_t)lo);
    le16(d, 2, (uint16_t)hi);
    d[4] = (uint8_t)(lo_i / CELLS_PER_CMU); d[5] = (uint8_t)(lo_i % CELLS_PER_CMU);
    d[6] = (uint8_t)(hi_i / CELLS_PER_CMU); d[7] = (uint8_t)(hi_i % CELLS_PER_CMU);
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_CELL_V), d);

    uint32_t sum = 0u;
    for (uint8_t i = 0u; i < c->cells; i++) sum += (uint32_t)cell_mv(c, i);
    memset(d, 0, 8);
    leu32(d, 0, sum);
    leu32(d, 4, (uint32_t)pack_ma(c));
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_PACK_VI), d);
}

static void send_slow(const bms_cfg_t *c)
{
    uint8_t d[8];

    memset(d, 0, 8);
    leu32(d, 0, 0x00001000u);
    leu32(d, 4, c->serial);
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_HEARTBEAT), d);

    for (uint8_t n = 0u; n < c->cmu_count; n++)
    {
        uint8_t first = (uint8_t)(BMS_OFS_CMU_FIRST + 3u * n);

        memset(d, 0, 8);
        leu32(d, 0, c->serial + n + 1u);
        lei16(d, 4, (int16_t)(tri_wave(seq + n * 11u, 900u, 280, 450) + c->temp_bias));
        lei16(d, 6, cmu_cell_temp(c, n));
        wsc_can_send(s_hcan, id_of(c, first), d);

        for (uint8_t half = 0u; half < 2u; half++)
        {
            memset(d, 0, 8);
            for (uint8_t k = 0u; k < 4u; k++)
                lei16(d, (uint8_t)(k * 2u), cell_mv(c, n * CELLS_PER_CMU + half * 4u + k));
            wsc_can_send(s_hcan, id_of(c, (uint8_t)(first + 1u + half)), d);
        }
    }

    float pct = tri_wave_f(seq, 1500u, 100.0f, 55.0f);
    memset(d, 0, 8);
    lef(d, 0, c->capacity_ah * (100.0f - pct) / 100.0f);
    lef(d, 4, pct);
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_SOC), d);

    memset(d, 0, 8);
    lef(d, 0, 0.1f);
    lef(d, 4, pct > 99.0f ? 100.0f : pct + 1.0f);
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_BAL_SOC), d);

    memset(d, 0, 8);
    d[0] = 0x04u | 0x08u | 0x10u;
    d[1] = 4u;
    le16(d, 2, 12400u);
    d[6] = 1u;
    d[7] = 50u;
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_PRECHARGE), d);

    int16_t tlo = 0x7FFF, thi = -0x7FFF;
    uint8_t tlo_n = 0u, thi_n = 0u;
    for (uint8_t n = 0u; n < c->cmu_count; n++)
    {
        int16_t t = cmu_cell_temp(c, n);
        if (t < tlo) { tlo = t; tlo_n = n; }
        if (t > thi) { thi = t; thi_n = n; }
    }
    memset(d, 0, 8);
    lei16(d, 0, tlo);
    lei16(d, 2, thi);
    d[4] = tlo_n;
    d[6] = thi_n;
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_CELL_T), d);

    memset(d, 0, 8);
    le16(d, 0, (uint16_t)(c->cell_hi_mv - 50));
    le16(d, 2, (uint16_t)(c->cell_hi_mv - 100));
    d[4] = 0u;
    d[5] = c->cmu_count;
    le16(d, 6, 1234u);
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_STATUS), d);

    memset(d, 0, 8);
    le16(d, 0, (uint16_t)tri_wave(seq, 600u, 0, 3000));
    le16(d, 2, (uint16_t)tri_wave(seq + 150u, 600u, 0, 3000));
    le16(d, 4, 180u);
    le16(d, 6, (uint16_t)(40u * c->cmu_count));
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_FAN), d);

    memset(d, 0, 8);
    d[4] = 5u;
    d[5] = 1u;
    wsc_can_send(s_hcan, id_of(c, BMS_OFS_EXT_STATUS), d);
}

#define DUMMY_HEARTBEAT_ID        0
#define DUMMY_HEARTBEAT_PERIOD_MS 100u

void BMS_Dummy_Task(void)
{
    wsc_heartbeat_tick(s_hcan, DUMMY_HEARTBEAT_ID, DUMMY_HEARTBEAT_PERIOD_MS);

    uint32_t now = HAL_GetTick();

    if ((now - t_fast) >= PERIOD_FAST_MS)
    {
        t_fast += PERIOD_FAST_MS;
        seq++;
        send_fast(&HV);
        send_fast(&LV);
    }

    if ((now - t_slow) >= PERIOD_SLOW_MS)
    {
        t_slow += PERIOD_SLOW_MS;
        send_slow(&HV);
        send_slow(&LV);
    }
}
