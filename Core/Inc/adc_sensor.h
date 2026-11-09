
#ifndef __ADC_SENSOR_H
#define __ADC_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32u5xx_hal.h"
#include "tx_api.h"
#include "phase_config.h"
#include "types.h"

void ADC_Sensor_Thread_Entry(ULONG arg);

void ADC_Sensor_PackSummary(const AdcPotAcc_t *acc, uint8_t *out);

extern volatile uint16_t g_vbat_mv;
extern volatile uint16_t g_vbat_raw;
extern volatile uint32_t g_vbat_seq;
extern volatile uint32_t g_adc_vdda_mv;

#ifdef __cplusplus
}
#endif

#endif
