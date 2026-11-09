/*
 * types.h
 *
 * Modified for STM32U5 & LoRa Telemetry
 */

#ifndef CORE_INC_TYPES_H_
#define CORE_INC_TYPES_H_

#include "main.h" // HAL 핸들 및 기본 정의 포함

/* -------------------------------------------
 * Configuration Constants
 * ------------------------------------------- */
#define ADC_COUNT 2      // 외부 센서 개수 (PC1, PC2)
#define PULSE_CH_COUNT 4

/* Peripherals Handles (STM32U5 CubeMX 설정에 맞게 수정 필요) */
#define UART_TELEMETRY &huart1
/* -------------------------------------------
 * Data Structures & Enums
 * ------------------------------------------- */

/* [추가] 로그 출처 (LOG_SOURCE) 정의 - 빌드 에러 해결 핵심 */
typedef enum {
    SYS = 0,
    CAN = 1,
    ADC = 2,
    IO  = 3,
} LOG_SOURCE;

/* RTC datetime */
typedef struct {
  uint8_t year;
  uint8_t month;
  uint8_t date;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} DATETIME;

/* [변경] LoRa Operation State (기존 ESP Handshake 대체) */
typedef enum {
  LORA_IDLE = 0,      // 대기 상태
  LORA_INITIALIZED,   // 초기화 완료
  LORA_TX_READY,      // 전송 준비 완료
  LORA_TRANSMITTING,  // 전송 중
  LORA_RX_RECEIVED    // 데이터 수신됨 (양방향 시)
} LORA_STATE_FLAG;

/* Timer flags */
typedef enum {
  FLAG_TIMER_100ms = 0,
  FLAG_TIMER_1s,
} TIMER_FLAG;

/* ADC flags */
typedef enum {
  FLAG_ADC_SYS = 0, // CPU Temp, VREF
  FLAG_ADC_AIN,     // 외부 센서
} ADC_FLAG;

/* Pulse input capture channel index */
typedef enum {
  PULSE_CH0 = 0,
  PULSE_CH1,
  PULSE_CH2,
  PULSE_CH3,
  PULSE_CH0_HALF,
  PULSE_CH1_HALF,
  PULSE_CH2_HALF,
  PULSE_CH3_HALF,
  PULSE_ARMED,
  PULSE_SET,
} PULSE_FLAG;

#define PULSE_READY ((1 << PULSE_ARMED) | (1 << PULSE_CH0) | (1 << PULSE_CH1) | (1 << PULSE_CH2) | (1 << PULSE_CH3))

/* Telemetry buffer flag */
typedef enum {
  TELEMETRY_BUFFER_REMAIN = 0,
  TELEMETRY_BUFFER_TRANSMIT,
} TELEMETRY_BUFFER_FLAG;

/* Serial buffer flag */
typedef enum {
  SERIAL_BUFFER_REMAIN = 0,
  SERIAL_BUFFER_TRANSMIT,
} SERIAL_BUFFER_FLAG;

/* NMEA GPRMC message */
typedef struct nmea_gprmc_t {
  uint8_t *id;
  uint8_t *utc_time;
  uint8_t *status;
  uint8_t *lat;
  uint8_t *north;
  uint8_t *lon;
  uint8_t *east;
  uint8_t *speed;
  uint8_t *course;
  uint8_t *utc_date;
  uint8_t *others;
} NMEA_GPRMC;

typedef struct {
  uint32_t lat;
  uint32_t lon;
} GPS_COORD;

typedef struct {
  uint32_t speed;
  uint32_t course;
} GPS_VECTOR;

typedef struct {
  uint32_t utc_date;
  uint32_t utc_time;
} GPS_DATETIME;

/* System state type */
typedef struct {
  uint8_t ERR;
  uint8_t SD;
  uint8_t TELEMETRY; // 여기에 LORA_STATE_FLAG 값 저장
  uint8_t CAN;
} SYSTEM_STATE;

/* System Status */
typedef enum {
  SYS_OK = 0,
  SYS_ERROR
} SYS_STATUS;

typedef enum {
    KEY_ADC_PC1 = 10,
    KEY_ADC_PC2 = 11,
    KEY_ADC_TEMP = 0xA0
} ADC_KEY_MAP;

typedef enum {
    KEY_IO_CHANGE = 0xB0  // 디지털 입력 변화 감지
} GPIO_KEY_MAP;

/* [변경] LoRa Init Fail Reason (기존 ESP Init Fail 대체) */
typedef enum {
  LORA_OK = 0,
  LORA_NOT_FOUND = 1,     // SPI/UART 통신 응답 없음
  LORA_SETTINGS_FAIL,     // 주파수/SF 설정 실패
  LORA_TX_FAIL            // 전송 실패
} LORA_ERROR_CODE;

#endif /* CORE_INC_TYPES_H_ */
