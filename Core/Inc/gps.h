
#ifndef __GPS_H
#define __GPS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32u5xx_hal.h"
#include "phase_config.h"

typedef struct {
    int32_t  lat_1e7;
    int32_t  lon_1e7;
    uint16_t speed_cms;
    uint16_t course_cdeg;
    int16_t  alt_dm;
    uint8_t  sats;
    uint8_t  fix;
    uint32_t utc_time;
    uint32_t utc_date;
    uint32_t last_tick;
    uint32_t sentences;
    uint32_t csum_err;
} GPS_DATA;

void GPS_Init(void);

void GPS_RxCpltCallback(UART_HandleTypeDef *huart);

void GPS_ErrorCallback(UART_HandleTypeDef *huart);

void GPS_Thread_Entry(unsigned long thread_input);

void GPS_GetSnapshot(GPS_DATA *out);

#ifdef __cplusplus
}
#endif

#endif
