#ifndef __FRESHNESS_H
#define __FRESHNESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "types.h"

#define FRESH_MPPT_SLOTS  6u

enum {
  FRESH_FR_BMS_SOC = 0,
  FRESH_FR_BMS_BAL_SOC,
  FRESH_FR_BMS_CHARGER,
  FRESH_FR_BMS_PRECHARGE,
  FRESH_FR_BMS_CELL_V,
  FRESH_FR_BMS_CELL_T,
  FRESH_FR_BMS_PACK_VI,
  FRESH_FR_BMS_STATUS,
  FRESH_FR_BMS_FAN,
  FRESH_FR_BMS_EXT_STATUS,
  FRESH_FR_CMU_FIRST,
  FRESH_FR_MPPT_FIRST = FRESH_FR_CMU_FIRST + 3 * BMS_CMU_COUNT,
  FRESH_FR_MOTOR_STATUS = FRESH_FR_MPPT_FIRST + FRESH_MPPT_SLOTS * MPPT_UNIT_COUNT,
  FRESH_FR_MOTOR_BUS,
  FRESH_FR_MOTOR_VEL,
  FRESH_FR_MOTOR_TEMP,
  FRESH_FR_MOTOR_DSP_TEMP,
  FRESH_FR_MOTOR_ODO,
  FRESH_FR_ADC_FIRST,
  FRESH_FR_BMSLV_SOC = FRESH_FR_ADC_FIRST + ADC_POT_CHANNELS,
  FRESH_FR_BMSLV_CELL_V,
  FRESH_FR_BMSLV_CELL_T,
  FRESH_FR_BMSLV_PACK_VI,
  FRESH_FR_BMSLV_STATUS,
  FRESH_FR_COUNT
};


void Fresh_Note(uint32_t frame, uint32_t tick);

extern volatile uint32_t g_fresh_count[FRESH_FR_COUNT];
extern volatile uint16_t g_fresh_gap_max[FRESH_FR_COUNT];


void Fresh_Evaluate(uint32_t now, uint32_t *stale, uint32_t *never);

#ifdef __cplusplus
}
#endif

#endif
