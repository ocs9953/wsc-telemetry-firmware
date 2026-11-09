#include "freshness.h"

static uint32_t fresh_tick[FRESH_FR_COUNT];

volatile uint32_t g_fresh_count[FRESH_FR_COUNT];
volatile uint16_t g_fresh_gap_max[FRESH_FR_COUNT];


typedef struct {
    uint8_t  frame[4];
    uint8_t  count;
    uint16_t limit_ms;
} FreshGroupDef;

#define G1(a, ms)           { { (a), 0, 0, 0 },     1, (ms) }
#define G2(a, b, ms)        { { (a), (b), 0, 0 },   2, (ms) }
#define G3(a, b, c, ms)     { { (a), (b), (c), 0 }, 3, (ms) }
#define G4(a, b, c, d, ms)  { { (a), (b), (c), (d) }, 4, (ms) }

#define CMU_G(n)         G3(FRESH_FR_CMU_FIRST + 3 * (n), FRESH_FR_CMU_FIRST + 3 * (n) + 1, \
                            FRESH_FR_CMU_FIRST + 3 * (n) + 2, 3000)
#define MPPT_FR(u, s)    (FRESH_FR_MPPT_FIRST + FRESH_MPPT_SLOTS * (u) + (s))
#define MPPT_PWR_G(u)    G2(MPPT_FR(u, 0), MPPT_FR(u, 1), 3000)
#define MPPT_STS_G(u)    G4(MPPT_FR(u, 2), MPPT_FR(u, 3), MPPT_FR(u, 4), MPPT_FR(u, 5), 3000)

static const FreshGroupDef fresh_groups[FRESH_GRP_COUNT] = {
    [FRESH_GRP_BMS_PACK_VI]   = G1(FRESH_FR_BMS_PACK_VI,   1000),
    [FRESH_GRP_BMS_CELL_V]    = G1(FRESH_FR_BMS_CELL_V,    1000),
    [FRESH_GRP_BMS_CHARGER]   = G1(FRESH_FR_BMS_CHARGER,   1000),
    [FRESH_GRP_BMS_SOC]       = G1(FRESH_FR_BMS_SOC,       3000),
    [FRESH_GRP_BMS_CELL_T]    = G1(FRESH_FR_BMS_CELL_T,    3000),
    [FRESH_GRP_BMS_STATUS]    = G2(FRESH_FR_BMS_STATUS, FRESH_FR_BMS_EXT_STATUS, 3000),
    [FRESH_GRP_BMS_PRECHARGE] = G1(FRESH_FR_BMS_PRECHARGE, 3000),
    [FRESH_GRP_BMS_BAL_FAN]   = G2(FRESH_FR_BMS_BAL_SOC, FRESH_FR_BMS_FAN, 3000),
    [FRESH_GRP_CMU1]          = CMU_G(0),
    [FRESH_GRP_CMU2]          = CMU_G(1),
    [FRESH_GRP_CMU3]          = CMU_G(2),
    [FRESH_GRP_CMU4]          = CMU_G(3),
    [FRESH_GRP_MPPT1_POWER]   = MPPT_PWR_G(0),
    [FRESH_GRP_MPPT2_POWER]   = MPPT_PWR_G(1),
    [FRESH_GRP_MPPT3_POWER]   = MPPT_PWR_G(2),
    [FRESH_GRP_MPPT4_POWER]   = MPPT_PWR_G(3),
    [FRESH_GRP_MPPT1_STATUS]  = MPPT_STS_G(0),
    [FRESH_GRP_MPPT2_STATUS]  = MPPT_STS_G(1),
    [FRESH_GRP_MPPT3_STATUS]  = MPPT_STS_G(2),
    [FRESH_GRP_MPPT4_STATUS]  = MPPT_STS_G(3),
    [FRESH_GRP_MOTOR_BUS]     = G2(FRESH_FR_MOTOR_BUS, FRESH_FR_MOTOR_VEL, 1000),
    [FRESH_GRP_MOTOR_TEMP]    = G2(FRESH_FR_MOTOR_TEMP, FRESH_FR_MOTOR_DSP_TEMP, 3000),
    [FRESH_GRP_MOTOR_STATUS]  = G1(FRESH_FR_MOTOR_STATUS, 1000),
    [FRESH_GRP_MOTOR_ODO]     = G1(FRESH_FR_MOTOR_ODO, 3000),
    [FRESH_GRP_ADC1]          = G1(FRESH_FR_ADC_FIRST,     1000),
    [FRESH_GRP_ADC2]          = G1(FRESH_FR_ADC_FIRST + 1, 1000),
    [FRESH_GRP_BMSLV_PACK]    = G2(FRESH_FR_BMSLV_PACK_VI, FRESH_FR_BMSLV_CELL_V, 1000),
    [FRESH_GRP_BMSLV_SOC]     = G3(FRESH_FR_BMSLV_SOC, FRESH_FR_BMSLV_CELL_T,
                                   FRESH_FR_BMSLV_STATUS, 3000),
};

_Static_assert(BMS_CMU_COUNT == 4 && MPPT_UNIT_COUNT == 4 && ADC_POT_CHANNELS == 2,
               "freshness group table assumes 4 CMU, 4 MPPT, 2 ADC channels");
_Static_assert(FRESH_FR_COUNT <= 255, "frame index must fit in uint8_t");

void Fresh_Note(uint32_t frame, uint32_t tick)
{
    uint32_t prev;
    uint32_t now;

    if (frame >= FRESH_FR_COUNT)
        return;

    now  = (tick == 0u) ? 1u : tick;
    prev = fresh_tick[frame];

    if (prev != 0u)
    {
        uint32_t gap = now - prev;
        if (gap > 0xFFFFu) gap = 0xFFFFu;
        if ((uint16_t)gap > g_fresh_gap_max[frame])
            g_fresh_gap_max[frame] = (uint16_t)gap;
    }

    fresh_tick[frame] = now;
    g_fresh_count[frame]++;
}

void Fresh_Evaluate(uint32_t now, uint32_t *stale, uint32_t *never)
{
    uint32_t s = 0u, nv = 0u;

    for (uint32_t g = 0; g < FRESH_GRP_COUNT; g++)
    {
        const FreshGroupDef *d = &fresh_groups[g];
        for (uint8_t i = 0; i < d->count; i++)
        {
            uint32_t t = fresh_tick[d->frame[i]];
            if (t == 0u)
            {
                nv |= (1u << g);
                s  |= (1u << g);
            }
            else
            {
                uint32_t age = now - t;
                if (age > d->limit_ms && age < 0x80000000u)
                    s |= (1u << g);
            }
        }
    }

    *stale = s;
    *never = nv;
}
