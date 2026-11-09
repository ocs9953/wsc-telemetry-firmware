#include "main.h"
#include "mppt_dummy.h"
#include "wsc_dummy_common.h"

#define PERIOD_FAST_MS    200u
#define PERIOD_SLOW_MS   1000u

static CAN_HandleTypeDef *s_hcan;
static uint32_t t_fast, t_slow, seq;

static const uint16_t MPPT_BASE[MPPT_UNIT_COUNT] = {
    ID_MPPT1_BASE, ID_MPPT2_BASE, ID_MPPT3_BASE, ID_MPPT4_BASE
};

void MPPT_Dummy_Init(CAN_HandleTypeDef *hcan)
{
    s_hcan = hcan;
    t_fast = t_slow = HAL_GetTick();
    seq = 0u;
}

static uint32_t phase_of(uint8_t unit) { return seq + (uint32_t)unit * 37u; }

static void send_fast(uint8_t unit)
{
    uint8_t d[8];
    uint16_t base = MPPT_BASE[unit];
    uint32_t ph = phase_of(unit);

    float in_v  = tri_wave_f(ph, 300u, 60.0f, 95.0f);
    float in_i  = tri_wave_f(ph, 220u,  0.2f,  9.0f);
    memset(d, 0, 8);
    lef(d, 0, in_v);
    lef(d, 4, in_i);
    wsc_can_send(s_hcan, (uint16_t)(base + 0u), d);

    float out_v = tri_wave_f(ph, 600u, 340.0f, 400.0f);
    float out_i = (out_v > 1.0f) ? (in_v * in_i * 0.97f / out_v) : 0.0f;
    memset(d, 0, 8);
    lef(d, 0, out_v);
    lef(d, 4, out_i);
    wsc_can_send(s_hcan, (uint16_t)(base + 1u), d);
}

static void send_slow(uint8_t unit)
{
    uint8_t d[8];
    uint16_t base = MPPT_BASE[unit];
    uint32_t ph = phase_of(unit);

    memset(d, 0, 8);
    lef(d, 0, tri_wave_f(ph, 400u, 30.0f, 72.0f));
    lef(d, 4, tri_wave_f(ph, 400u, 28.0f, 58.0f));
    wsc_can_send(s_hcan, (uint16_t)(base + 2u), d);

    memset(d, 0, 8);
    lef(d, 0, tri_wave_f(ph, 900u, 11.8f, 12.2f));
    lef(d, 4, tri_wave_f(ph, 900u,  3.28f, 3.32f));
    wsc_can_send(s_hcan, (uint16_t)(base + 3u), d);

    memset(d, 0, 8);
    lef(d, 0, 150.0f);
    lef(d, 4,  10.0f);
    wsc_can_send(s_hcan, (uint16_t)(base + 4u), d);

    memset(d, 0, 8);
    d[0] = 0u;
    d[1] = 0u;
    d[2] = 0u;
    d[3] = ((ph % 60u) == 59u) ? 0x40u : 0x00u;
    d[4] = (uint8_t)((ph % 20u) < 10u ? 0x02u : 0x01u);
    d[5] = 1u;
    d[6] = 0u;
    d[7] = (uint8_t)(ph & 0xFFu);
    wsc_can_send(s_hcan, (uint16_t)(base + 5u), d);

    memset(d, 0, 8);
    lef(d, 0, tri_wave_f(ph, 600u, 338.0f, 398.0f));
    lef(d, 4, tri_wave_f(ph, 400u,  25.0f,  55.0f));
    wsc_can_send(s_hcan, (uint16_t)(base + 6u), d);
}

#define MPPT_BURST_MODE   0

#define DUMMY_HEARTBEAT_ID        ID_TEST_HEARTBEAT_BUS_A
#define DUMMY_HEARTBEAT_PERIOD_MS 100u

void MPPT_Dummy_Task(void)
{
    wsc_heartbeat_tick(s_hcan, DUMMY_HEARTBEAT_ID, DUMMY_HEARTBEAT_PERIOD_MS);

    uint32_t now = HAL_GetTick();

#if MPPT_BURST_MODE
    (void)now;
    seq++;
    for (uint8_t u = 0u; u < MPPT_UNIT_COUNT; u++) send_fast(u);
    return;
#endif

    if ((now - t_fast) >= PERIOD_FAST_MS)
    {
        t_fast += PERIOD_FAST_MS;
        seq++;
        for (uint8_t u = 0u; u < MPPT_UNIT_COUNT; u++) send_fast(u);
    }

    if ((now - t_slow) >= PERIOD_SLOW_MS)
    {
        t_slow += PERIOD_SLOW_MS;
        for (uint8_t u = 0u; u < MPPT_UNIT_COUNT; u++) send_slow(u);
    }
}
