#include "main.h"
#include "motor_dummy.h"
#include "wsc_dummy_common.h"

#define PERIOD_FAST_MS    100u
#define PERIOD_MID_MS     200u
#define PERIOD_SLOW_MS   1000u

#define WHEEL_CIRCUM_M     1.759f
#define VEHICLE_MASS_KG    250.0f
#define ROLL_RESIST_N       12.0f
#define AERO_COEF           0.200f
#define DRIVE_EFF           0.92f
#define SPEED_MAX_MPS       25.0f
#define SPEED_PERIOD_TICKS   600u

static CAN_HandleTypeDef *s_hcan;
static uint32_t t_fast, t_mid, t_slow, seq;

static float speed_mps;
static float speed_prev;
static float bus_v;
static float bus_i;
static float motor_rpm;
static float bemf_q;
static float temp_motor;
static float temp_heatsink;
static float temp_dsp;
static float odo_m;
static float bus_ah;

static float f_abs(float v) { return (v < 0.0f) ? -v : v; }

void Motor_Dummy_Init(CAN_HandleTypeDef *hcan)
{
    s_hcan = hcan;
    t_fast = t_mid = t_slow = HAL_GetTick();
    seq    = 0u;

    speed_mps = 0.0f;
    speed_prev = 0.0f;
    bus_v = 120.0f;
    bus_i = 0.0f;
    motor_rpm = 0.0f;
    bemf_q = 0.0f;
    temp_motor    = 25.0f;
    temp_heatsink = 25.0f;
    temp_dsp      = 25.0f;
    odo_m  = 0.0f;
    bus_ah = 0.0f;
}

static void physics_step(void)
{
    const float dt = (float)PERIOD_FAST_MS / 1000.0f;

    speed_prev = speed_mps;
    speed_mps  = tri_wave_f(seq, SPEED_PERIOD_TICKS, 0.0f, SPEED_MAX_MPS);

    float accel  = (speed_mps - speed_prev) / dt;
    float f_res  = ROLL_RESIST_N + AERO_COEF * speed_mps * speed_mps;
    float f_tot  = f_res + VEHICLE_MASS_KG * accel;
    float p_mech = f_tot * speed_mps;

    bus_v = tri_wave_f(seq, 3000u, 108.0f, 126.0f);

    float p_elec = (p_mech >= 0.0f) ? (p_mech / DRIVE_EFF)
                                    : (p_mech * DRIVE_EFF);
    bus_i = (bus_v > 1.0f) ? (p_elec / bus_v) : 0.0f;

    motor_rpm = speed_mps / WHEEL_CIRCUM_M * 60.0f;
    bemf_q    = motor_rpm * 0.115f;

    odo_m  += speed_mps * dt;
    bus_ah += bus_i * dt / 3600.0f;

    float load = f_abs(bus_i);
    temp_motor    += ((30.0f + load * 0.85f) - temp_motor)    * 0.010f;
    temp_heatsink += ((28.0f + load * 0.55f) - temp_heatsink) * 0.008f;
    temp_dsp      += ((38.0f + load * 0.10f) - temp_dsp)      * 0.005f;
}

static void send_fast(void)
{
    uint8_t d[8];

    memset(d, 0, 8);
    lef(d, 0, bus_v);
    lef(d, 4, bus_i);
    wsc_can_send(s_hcan, ID_MOTOR_BUS, d);

    memset(d, 0, 8);
    lef(d, 0, motor_rpm);
    lef(d, 4, speed_mps);
    wsc_can_send(s_hcan, ID_MOTOR_VEL, d);
}

static void send_mid(void)
{
    uint8_t d[8];

    float i_mag = f_abs(bus_i) * 1.35f;
    float ph_b  = i_mag * tri_wave_f(seq,      7u, -1.0f, 1.0f);
    float ph_c  = i_mag * tri_wave_f(seq + 5u, 7u, -1.0f, 1.0f);

    memset(d, 0, 8);
    lef(d, 0, ph_b);
    lef(d, 4, ph_c);
    wsc_can_send(s_hcan, ID_MOTOR_PHASE_I, d);

    memset(d, 0, 8);
    lef(d, 0, bemf_q * 1.05f);
    lef(d, 4, tri_wave_f(seq, 130u, -4.0f, 4.0f));
    wsc_can_send(s_hcan, ID_MOTOR_V_VECTOR, d);

    memset(d, 0, 8);
    lef(d, 0, bus_i * 1.35f);
    lef(d, 4, tri_wave_f(seq, 170u, -2.0f, 0.5f));
    wsc_can_send(s_hcan, ID_MOTOR_I_VECTOR, d);

    memset(d, 0, 8);
    lef(d, 0, bemf_q);
    lef(d, 4, 0.0f);
    wsc_can_send(s_hcan, ID_MOTOR_BEMF, d);

    memset(d, 0, 8);
    lef(d, 0, temp_motor);
    lef(d, 4, temp_heatsink);
    wsc_can_send(s_hcan, ID_MOTOR_TEMP, d);

    memset(d, 0, 8);
    lef(d, 0, temp_dsp);
    wsc_can_send(s_hcan, ID_MOTOR_DSP_TEMP, d);
}

static void send_slow(void)
{
    uint8_t d[8];

    uint16_t limit = (speed_mps > SPEED_MAX_MPS * 0.9f) ? 0x0001u : 0x0000u;
    uint16_t error = ((seq % 300u) < 10u) ? 0x0004u : 0x0000u;

    memset(d, 0, 8);
    le16(d, 0, limit);
    le16(d, 2, error);
    le16(d, 4, 0x0001u);
    d[6] = 0u;
    d[7] = 0u;
    wsc_can_send(s_hcan, ID_MOTOR_FLAGS, d);

    memset(d, 0, 8);
    lef(d, 4, tri_wave_f(seq, 2600u, 14.85f, 15.15f));
    wsc_can_send(s_hcan, ID_MOTOR_RAIL_A, d);

    memset(d, 0, 8);
    lef(d, 0, tri_wave_f(seq, 2900u, 1.888f, 1.912f));
    lef(d, 4, tri_wave_f(seq, 3100u, 3.288f, 3.312f));
    wsc_can_send(s_hcan, ID_MOTOR_RAIL_B, d);

    memset(d, 0, 8);
    lef(d, 0, odo_m);
    lef(d, 4, bus_ah);
    wsc_can_send(s_hcan, ID_MOTOR_ODO, d);

    memset(d, 0, 8);
    lef(d, 4, tri_wave_f(seq, 800u, 0.0f, 2.5f));
    wsc_can_send(s_hcan, ID_MOTOR_SLIP, d);
}

#define DUMMY_HEARTBEAT_ID        ID_TEST_HEARTBEAT_BUS_B
#define DUMMY_HEARTBEAT_PERIOD_MS 100u

void Motor_Dummy_Task(void)
{
    wsc_heartbeat_tick(s_hcan, DUMMY_HEARTBEAT_ID, DUMMY_HEARTBEAT_PERIOD_MS);

    uint32_t now = HAL_GetTick();

    if ((now - t_fast) >= PERIOD_FAST_MS)
    {
        t_fast += PERIOD_FAST_MS;
        seq++;
        physics_step();
        send_fast();
    }

    if ((now - t_mid) >= PERIOD_MID_MS)
    {
        t_mid += PERIOD_MID_MS;
        send_mid();
    }

    if ((now - t_slow) >= PERIOD_SLOW_MS)
    {
        t_slow += PERIOD_SLOW_MS;
        send_slow();
    }
}
