
#include "dummy_data.h"

#if TEST_DUMMY_DATA

#include "types.h"
#include "freshness.h"

extern VehicleData_t VehicleData;

static uint32_t tk = 0;

static uint32_t tri(uint32_t t, uint32_t period, uint32_t span)
{
    if (period == 0u) return 0u;
    uint32_t half = period / 2u;
    if (half == 0u) return 0u;
    uint32_t phase = t % period;
    uint32_t up = (phase < half) ? phase : (period - phase);
    return (up * span) / half;
}

void Dummy_Update(void)
{
    tk++;

    const uint32_t now = HAL_GetTick();

    {
        BmsData_t *b = &VehicleData.bms;
        uint32_t s = tri(tk, 600u, 800u);
        b->pack_v_cv     = (uint16_t)(11200u + s * 3u);
        b->pack_current  = (int16_t)((int32_t)s * 5 - 2000);
        b->min_cell_mv   = (uint16_t)(3500u + s * 3u / 4u);
        b->max_cell_mv   = (uint16_t)(b->min_cell_mv + 20u);
        b->soc_permille  = (uint16_t)(200u + s);
        b->min_cell_temp = (int16_t)(250 + (int32_t)tri(tk, 900u, 150u));
        b->max_cell_temp = (int16_t)(b->min_cell_temp + 30);
        b->ext_status    = 0u;
        b->contactor_status = 0x1Cu;
        b->precharge_state  = 4u;
        b->status_flags  = 0u;
        b->cmu_count     = BMS_CMU_COUNT;
        b->pack_capacity_ah = 30u;
        b->soc_ah_cah    = (uint16_t)((1000u - (200u + s)) * 3u);
        b->bms_last_tick = now;

        for (uint8_t n = 0u; n < BMS_CMU_COUNT; n++) {
            BmsCmu_t *c = &b->cmu[n];
            uint32_t cs = tri(tk, 700u + (uint32_t)n * 100u, 600u);
            c->serial    = 0x1000u + n;
            c->pcb_temp  = (int16_t)(300 + (int32_t)(cs / 20u));
            c->cell_temp = (int16_t)(250 + (int32_t)(cs / 4u));
            for (uint8_t k = 0u; k < BMS_CELLS_PER_CMU; k++)
                c->cell_mv[k] = (int16_t)(3500u + cs + k);
            c->last_tick = now;
        }
    }

    {
        BmsData_t *lv = &VehicleData.bms_lv;
        uint32_t s = tri(tk, 500u, 500u);
        lv->min_cell_mv   = (uint16_t)(3000u + s);
        lv->max_cell_mv   = (uint16_t)(lv->min_cell_mv + 15u);
        lv->pack_v_cv     = (uint16_t)((lv->min_cell_mv * BMS_LV_CELLS + 5u) / 10u);
        lv->pack_current  = (int16_t)((int32_t)s - 200);
        lv->soc_permille  = (uint16_t)(500u + s);
        lv->min_cell_temp = (int16_t)(250 + (int32_t)tri(tk, 800u, 100u));
        lv->max_cell_temp = (int16_t)(lv->min_cell_temp + 20);
        lv->cmu_count     = BMS_LV_CMU_COUNT;
        lv->bms_last_tick = now;

        BmsCmu_t *c = &lv->cmu[0];
        c->serial    = 0x4C560001u;
        c->pcb_temp  = (int16_t)(300 + (int32_t)(s / 20u));
        c->cell_temp = lv->min_cell_temp;
        for (uint8_t k = 0u; k < BMS_CELLS_PER_CMU; k++)
            c->cell_mv[k] = (k < BMS_LV_CELLS) ? (int16_t)(lv->min_cell_mv + k * 5u)
                                               : (int16_t)-32768;
        c->last_tick = now;
    }

    for (uint8_t i = 0u; i < MPPT_UNIT_COUNT; i++) {
        MpptData_t *m = &VehicleData.mppt[i];
        uint32_t period = 400u + (uint32_t)i * 100u;
        uint32_t s = tri(tk, period, 1000u);

        m->in_v        = (uint16_t)(6000u + s * 4u);
        m->in_i        = (uint16_t)(s * 2u);
        m->out_v       = (uint16_t)(11000u + s * 2u);
        m->out_i       = (uint16_t)(s + 100u);
        m->mosfet_temp = (int16_t)(300 + (int32_t)(s / 10u));
        m->ctrl_temp   = (int16_t)(280 + (int32_t)(s / 12u));
        m->error_flags = 0u;
        m->mode        = 1u;
        m->aux_12v     = 1200u;
        m->aux_3v      = 3300u;
        m->max_out_v   = 13000u;
        m->max_in_i    = 2000u;
        m->powerconn_v = m->out_v;
        m->powerconn_temp = m->ctrl_temp;
        m->mppt_last_tick = now;
    }

    {
        MotorData_t *mo = &VehicleData.motor;
        uint32_t period = 300u;
        uint32_t phase  = tk % period;
        uint32_t s      = tri(tk, period, 2500u);
        int32_t  rising = (phase < period / 2u) ? 1 : -1;

        mo->veh_speed     = (uint16_t)s;
        mo->motor_rpm     = (int16_t)((int32_t)s * 2);
        mo->bus_v         = (uint16_t)(11000u + tri(tk, 700u, 2000u));
        mo->bus_i         = (int16_t)(rising * (int32_t)(s / 5u));
        mo->motor_temp    = (int16_t)(350 + (int32_t)(s / 25u));
        mo->heatsink_temp = (int16_t)(300 + (int32_t)(s / 30u));
        mo->dsp_temp      = 400;
        mo->error_flags   = 0u;
        mo->limit_flags   = 0u;
        mo->active_motor  = 0u;
        mo->rail_15v      = 1500u;
        mo->rail_3v3      =  330u;
        mo->rail_1v9      =  190u;
        mo->motor_last_tick = now;
    }

    VehicleData.distance_traveled          += (float)VehicleData.motor.veh_speed * 0.01f * 0.1f;
    VehicleData.battery_accumulated_energy += 0.001f;
    VehicleData.odo_last_tick = now;

    for (uint32_t f = 0u; f < FRESH_FR_ADC_FIRST; f++)
        Fresh_Note(f, now);
    for (uint32_t f = FRESH_FR_BMSLV_SOC; f < FRESH_FR_COUNT; f++)
        Fresh_Note(f, now);

#if !PHASE9_ADC_SENSOR
    for (uint8_t n = 0u; n < ADC_POT_CHANNELS; n++) {
        AdcPotAcc_t *a = &VehicleData.adc_pot[n];
        uint16_t raw = (uint16_t)(2000u + tri(tk, 80u + (uint32_t)n * 40u, 12000u));
        if (a->count == 0u) { a->min = raw; a->max = raw; }
        if (raw < a->min) a->min = raw;
        if (raw > a->max) a->max = raw;
        if (a->count < 0xFFFFu) {
            a->sum += raw;
            a->count++;
        }
        a->last  = raw;
        Fresh_Note(FRESH_FR_ADC_FIRST + n, now);
    }
#endif
}

#endif
