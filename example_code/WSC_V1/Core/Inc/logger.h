/*
 * logger.h
 *
 * Modified for STM32U5 Series (LoRa, No GPS/ACC)
 */

#ifndef CORE_INC_LOGGER_H_
#define CORE_INC_LOGGER_H_

#include <stdint.h> // uint8_t, uint32_t 사용을 위해 필요
#include "main.h"
#include "types.h"
/* system log structure */
typedef struct {
    uint32_t timestamp;
    uint8_t level;
    uint8_t source;
    uint8_t key;
    uint8_t value[8];
    uint8_t checksum;
} LOG;

/* log level type */
typedef enum {
    LOG_FATAL = 0,
    LOG_ERROR,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG,
} LOG_LEVEL;

/* log key types per sources */
typedef enum {
    SYS_SD_INIT = 0,
    SYS_CORE_INIT,
    SYS_SERIAL_INIT,      // LoRa 초기화에 사용
    SYS_TELEMETRY_REMOTE, // 필요 없다면 제거 가능 (기존 코드 호환 위해 유지)
    SYS_TELEMETRY_INIT,
    CAN_INIT,
    DIGITAL_INIT,
    ANALOG_INIT,
    PULSE_INIT,
    SYS_READY,
    SYS_RTC_FIX,          // GPS 보정이 없으므로 실제론 안 쓰임
    SYS_SD_FAIL,
    CAN_ERR_CANERR,
    CAN_ERR_RXMSGFAIL,
    CAN_ERR_FIFOFULL
} LOG_KEY_SYS;

typedef enum {
    DIGITAL_DATA = 0
} LOG_KEY_DIGITAL;

typedef enum {
    ANALOG_SYS = 0,
    ANALOG_DATA
} LOG_KEY_ANALOG;

typedef enum {
    PULSE_DATA = 0,
} LOG_KEY_PULSE;

/* ACCELEROMETER, GPS 관련 키 정의 제거됨
   (LOG_KEY_ACCELEROMETER, LOG_KEY_GPS 등)
*/

/* Prototypes */
int SYS_LOG(LOG_LEVEL level, LOG_SOURCE source, int key, void *payload);

/* macro for debug print */
#ifdef DEBUG_MODE
#include <stdio.h>
#define DEBUG_MSG(f, ...) printf((f), ##__VA_ARGS__)
#else
#define DEBUG_MSG(f, ...)
#endif

#endif /* CORE_INC_LOGGER_H_ */
