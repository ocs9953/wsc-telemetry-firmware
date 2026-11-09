/*
 * types.h
 *
 * WSC Telemetry Firmware - Shared Type Definitions
 * Based on WSC_V1 example code style
 */

#ifndef CORE_INC_TYPES_H_
#define CORE_INC_TYPES_H_

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------
 * LOG Level
 * ------------------------------------------- */
typedef enum {
  LOG_INFO = 0,
  LOG_WARNING,
  LOG_ERROR,
  LOG_CRITICAL,
} LOG_LEVEL;

typedef enum {
  /* ── 데이터 소스 ── */
  SRC_SYS = 0,        /* 시스템 전반 (RTC, 초기화, Watchdog 등)        */
  SRC_RETIRED_BMS_BOT,
  SRC_RETIRED_BMS_TOP,
  SRC_MPPT1,
  SRC_MPPT2,
  SRC_MPPT3,
  SRC_MPPT4,
  SRC_MOTOR,          /* 🟠 잠정 — VCU 경유, WaveSculptor22 virtual 매핑 */
  SRC_BMS_LV,
  SRC_GPS,
  SRC_BMS,
  /* ── 에러 소스 ── */
  SRC_CAN_ERR,        /* FDCAN1 버스/수신 오류                        */
  SRC_SPI_ERR,        /* MCP2518FD SPI 통신 오류                      */
  SRC_UART_ERR,       /* USART2 / RFD900x 오류                        */
  SRC_SD_ERR,         /* SD 카드 / FileX 오류                         */
  SRC_ADC,
} LOG_SOURCE;

/* -------------------------------------------
 * LOG Key (INFO level data keys)
 * ------------------------------------------- */
typedef enum {
  KEY_SYS_BOOT = 0,
  KEY_SYS_SD_INIT,
  KEY_SYS_CAN_INIT,
  KEY_SYS_UART_INIT,
  KEY_SYS_RTC_TIME,
  KEY_SYS_TIMEOUT,
  KEY_SYS_SD_FAIL,
  KEY_SYS_CAN_ERR,
  KEY_SYS_UART_ERR,
  KEY_SYS_SPI_ERR,
  KEY_SYS_RTC_ERR,
  KEY_SYS_MCP_ERR,
  KEY_SYS_WATCHDOG,
  KEY_SYS_POWER,
  KEY_SYS_READY,
  KEY_SYS_RESERVED15,
  KEY_SYS_FRESHNESS,
  KEY_SYS_VBAT,
} LOG_KEY;

typedef enum {
  FRESH_GRP_BMS_PACK_VI = 0,
  FRESH_GRP_BMS_CELL_V,
  FRESH_GRP_BMS_CHARGER,
  FRESH_GRP_BMS_SOC,
  FRESH_GRP_BMS_CELL_T,
  FRESH_GRP_BMS_STATUS,
  FRESH_GRP_BMS_PRECHARGE,
  FRESH_GRP_BMS_BAL_FAN,
  FRESH_GRP_CMU1,
  FRESH_GRP_CMU2,
  FRESH_GRP_CMU3,
  FRESH_GRP_CMU4,
  FRESH_GRP_MPPT1_POWER,
  FRESH_GRP_MPPT2_POWER,
  FRESH_GRP_MPPT3_POWER,
  FRESH_GRP_MPPT4_POWER,
  FRESH_GRP_MPPT1_STATUS,
  FRESH_GRP_MPPT2_STATUS,
  FRESH_GRP_MPPT3_STATUS,
  FRESH_GRP_MPPT4_STATUS,
  FRESH_GRP_MOTOR_BUS,
  FRESH_GRP_MOTOR_TEMP,
  FRESH_GRP_MOTOR_STATUS,
  FRESH_GRP_MOTOR_ODO,
  FRESH_GRP_ADC1,
  FRESH_GRP_ADC2,
  FRESH_GRP_BMSLV_PACK,
  FRESH_GRP_BMSLV_SOC,
  FRESH_GRP_COUNT
} FRESH_GROUP;
_Static_assert(FRESH_GRP_COUNT <= 32, "freshness bitmap is 32 bits");

/* -------------------------------------------
 * 데이터 패킷 키 — 소스별 128-key 네임스페이스 (0x00~0x7F)
 *
 * 데이터 패킷의 key는 source에 따라 해석된다 (예: key=0x00은
 * SRC_BMS이면 PACK_VI, SRC_MPPT1이면 IV_IN_OUT).
 * 0x80 이상은 ERR_KEY 전용.
 * ⚠ 슬롯 순서·번호는 지상국 디코더와 동기 필요 (스펙 잔여 항목).
 *   패킷 정의: can-signal-summary.md §5
 *
 * 아래 주석의 PRIMARY / DETAIL / SD_ONLY는 "발행 티어"만 나타낸다.
 * 티어별 실제 주기는 timer_scheduler.c의 SCHED_*_TICKS 상수가 유일한 기준이다
 * (2026-08 현재 PRIMARY=1s, DETAIL=5s). 신호마다 주기 숫자를 중복 기재하면
 * 주기 개정 때마다 통째로 낡으므로(2026-08 감속 개정에서 실제로 33곳이 낡았다)
 * 여기에는 티어 이름만 적는다.
 * ------------------------------------------- */
typedef enum {
  KEY_BMS_PACK_VI    = 0x00,
  KEY_BMS_SOC_TEMP   = 0x01,
  KEY_BMS_STATUS     = 0x02,
  KEY_BMS_CELL_LOC   = 0x10,
  KEY_BMS_SOC_DETAIL = 0x11,
  KEY_BMS_CHARGER    = 0x12,
  KEY_BMS_FAN_12V    = 0x13,
  KEY_BMS_CMU_CELL_A = 0x14,
  KEY_BMS_CMU_CELL_B = 0x18,
  KEY_BMS_CMU_TEMP   = 0x1C,
  KEY_BMS_BAL_PRECHG = 0x20,
  KEY_BMS_CMU_SERIAL = 0x24,
} BMS_KEY;

typedef enum {
  KEY_ADC_POT        = 0x10,
} ADC_KEY;

typedef enum {
  KEY_MPPT_IV_IN_OUT  = 0x00,
  KEY_MPPT_TEMP_FLAGS = 0x10,
  KEY_MPPT_MAXLIM     = 0x11,
  KEY_MPPT_AUX_2      = 0x12,
  KEY_MPPT_AUX_1      = 0x20,
} MPPT_KEY;

typedef enum {                       /* SRC_MOTOR (🟠 잠정) */
  KEY_MOTOR_BUS_VEL = 0x00,
  KEY_MOTOR_TEMP    = 0x01,
  KEY_MOTOR_BEMF    = 0x02,
  KEY_MOTOR_ODO     = 0x03,
  KEY_MOTOR_DSP_TEMP = 0x10,
  KEY_MOTOR_STATUS   = 0x11,
  KEY_MOTOR_PHASE_I  = 0x12,
  KEY_MOTOR_V_VECTOR = 0x20,
  KEY_MOTOR_I_VECTOR = 0x21,
  KEY_MOTOR_RAILS    = 0x22,
  KEY_MOTOR_SLIP     = 0x23,
} MOTOR_KEY;

typedef enum {
  KEY_GPS_POS    = 0x00,
  KEY_GPS_MOTION = 0x01,
  KEY_GPS_TIME   = 0x02,
} GPS_KEY;

/* -------------------------------------------
 * Error Keys (WARNING/ERROR/CRITICAL level)
 * ------------------------------------------- */
typedef enum {
  ERR_CAN_BUS_OFF = 0x80,
  ERR_CAN_RX_OVERFLOW,
  ERR_CAN_ERROR_PASSIVE,
  ERR_CAN_ERROR_WARNING,
  ERR_CAN_TX_FAIL,
  ERR_SPI_TIMEOUT,
  ERR_SPI_TRANSFER,
  ERR_UART_DMA,
  ERR_UART_QUEUE_FULL,
  ERR_SD_INIT,
  ERR_SD_WRITE,
  ERR_SD_NOT_DETECTED,
  ERR_SD_QUEUE_FULL,
  ERR_RTC_READ,
  ERR_INIT_FAIL,
  ERR_TIMEOUT,
  ERR_POWER_DOWN,
  ERR_WATCHDOG,
  ERR_DUMMY_DATA,
  ERR_RTC_SYNCED,
  ERR_STACK_OVERFLOW,
  ERR_BOOT_REPORT,
} ERR_KEY;

#define BOOT_THREAD_UNKNOWN   0u
#define BOOT_THREAD_MCP_RX    1u
#define BOOT_THREAD_UART      2u
#define BOOT_THREAD_UART_RX   3u
#define BOOT_THREAD_SD_LOGGER 4u
#define BOOT_THREAD_VCU_LCD   5u
#define BOOT_THREAD_GPS       6u
#define BOOT_THREAD_ADC       7u
#define BOOT_THREAD_OTHER     8u

/* -------------------------------------------
 * LOG Packet Structure (16 bytes, packed)
 * ------------------------------------------- */
typedef struct __attribute__((packed)) {
  uint32_t timestamp;   /* HAL_GetTick() ms                       */
  uint8_t  level;       /* LOG_LEVEL                              */
  uint8_t  source;      /* LOG_SOURCE                             */
  uint8_t  key;         /* LOG_KEY or ERR_KEY                     */
  uint8_t  value[8];    /* payload (float x2, DATETIME, err, etc) */
  uint8_t  checksum;    /* CRC-8 (poly 0x07) of bytes[0..14]      */
} LOG;
_Static_assert(sizeof(LOG) == 16, "LOG must be 16 bytes");

#define LOG_QUEUE_MSG_WORDS  4
_Static_assert(sizeof(LOG) == LOG_QUEUE_MSG_WORDS * sizeof(uint32_t),
               "queue message size must equal sizeof(LOG)");

/* -------------------------------------------
 * RTC Datetime
 * ------------------------------------------- */
typedef struct {
  uint8_t year;
  uint8_t month;
  uint8_t date;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} DATETIME;

/* -------------------------------------------
 * Vehicle Data — 전체 정수, 중첩 구조체
 *
 * 설계 원칙: BMS는 정수 원본(리틀엔디안, 그대로 memcpy),
 * MPPT/Motor/BMS SOC 는 float 원본을 CAN RX 시점에 scaled int로 변환하여 저장.
 * 자세한 매핑·스케일은 can-signal-summary.md §7 참조(SSOT).
 * ------------------------------------------- */

#define BMS_CMU_COUNT      4

#define BMS_LV_CMU_COUNT   1
#define BMS_LV_CELLS       4
#define BMS_CELLS_PER_CMU  8

#define BMS_CONTACTOR_ERR_MASK     0x23u
#define BMS_SEEN_PRECHARGE_ERROR   0x80000000u

typedef struct {
  uint32_t serial;
  int16_t  pcb_temp;
  int16_t  cell_temp;
  int16_t  cell_mv[BMS_CELLS_PER_CMU];
  uint32_t last_tick;
} BmsCmu_t;

typedef struct {
  uint16_t pack_v_cv;
  int16_t  pack_current;
  uint16_t min_cell_mv;
  uint16_t max_cell_mv;
  uint16_t soc_permille;
  int16_t  min_cell_temp;
  int16_t  max_cell_temp;
  uint32_t ext_status;
  uint8_t  contactor_status;
  uint8_t  precharge_state;
  uint8_t  status_flags;
  uint8_t  cmu_count;

  uint32_t ext_status_seen;
  uint8_t  contactor_err_seen;

  uint8_t  min_cell_mv_cmu, min_cell_mv_cell;
  uint8_t  max_cell_mv_cmu, max_cell_mv_cell;
  uint8_t  min_cell_temp_cmu, max_cell_temp_cmu;
  uint16_t soc_ah_cah;
  uint16_t bal_soc_ah_cah;
  uint16_t bal_soc_permille;
  int16_t  chg_cell_v_err_mv;
  int16_t  cell_temp_margin;
  int16_t  dchg_cell_v_err_mv;
  uint16_t pack_capacity_ah;
  uint16_t fan0_rpm, fan1_rpm;
  uint16_t i12v_fan_ma;
  uint16_t i12v_cmu_ma;

  uint16_t bal_rise_mv;
  uint16_t bal_fall_mv;
  uint8_t  precharge_timer_elapsed;
  uint8_t  precharge_timer_count;

  uint32_t bms_last_tick;

  BmsCmu_t cmu[BMS_CMU_COUNT];
} BmsData_t;

_Static_assert(KEY_BMS_CMU_CELL_A + BMS_CMU_COUNT <= KEY_BMS_CMU_CELL_B &&
               KEY_BMS_CMU_CELL_B + BMS_CMU_COUNT <= KEY_BMS_CMU_TEMP &&
               KEY_BMS_CMU_TEMP   + BMS_CMU_COUNT <= KEY_BMS_BAL_PRECHG &&
               KEY_BMS_CMU_SERIAL + BMS_CMU_COUNT <= 0x80,
               "BMS CMU key blocks overlap - re-plan BMS_KEY for more CMUs");

#define MPPT_UNIT_COUNT  4

_Static_assert(SRC_MPPT4 - SRC_MPPT1 == MPPT_UNIT_COUNT - 1,
               "SRC_MPPT1..SRC_MPPT4 must stay contiguous (SRC_MPPT1 + i arithmetic)");

typedef struct {
  uint16_t in_v;           /* Input Voltage  (float×100 → u16)           */
  uint16_t in_i;           /* Input Current  (float×100 → u16)           */
  uint16_t out_v;          /* Output Voltage (float×100 → u16)           */
  uint16_t out_i;          /* Output Current (float×100 → u16)           */
  int16_t  mosfet_temp;    /* Mosfet Temp    (float×10  → i16)           */
  int16_t  ctrl_temp;      /* Controller Temp(float×10  → i16)           */
  uint8_t  error_flags;    /* Error_Flags 비트                            */
  uint8_t  mode;           /* Mode                                        */

  uint16_t aux_12v;        /* 12V_Aux          (float×100  → u16, 0.01V) */
  uint16_t aux_3v;         /* 3V_Aux           (float×1000 → u16, 0.001V)*/
  uint16_t max_out_v;      /* MaxOutput_Voltage(float×100  → u16, 0.01V) */
  uint16_t max_in_i;       /* MaxInput_Current (float×100  → u16, 0.01A) */
  uint16_t powerconn_v;    /* PowerConn_Voltage(float×100  → u16, 0.01V) */
  int16_t  powerconn_temp; /* PowerConn_Temp   (float×10   → i16, 0.1℃) */

  uint32_t mppt_last_tick; /* 타임아웃 워치독                             */
} MpptData_t;

/* 🟠 잠정(virtual) — WaveSculptor22 매뉴얼 기반. 실제 경로는 모터→VCU→보드이며
   VCU 미확정이므로 placeholder. VCU 출력 표 확정 시 필드·스케일 교체. */
typedef struct {
  uint16_t bus_v;          /* Bus Voltage    (float×100 → u16, 0.01V)     */
  int16_t  bus_i;          /* Bus Current    (float×10  → i16, regen 음수)*/
  int16_t  motor_rpm;      /* Motor Velocity (float 반올림 → i16, 역방향 음수) */
  uint16_t veh_speed;      /* Vehicle Velocity(float×100 → u16, 0.01 m/s) */
  int16_t  heatsink_temp;  /* Heat-sink Temp (float×10  → i16, 0.1℃)     */
  int16_t  motor_temp;     /* Motor Temp     (float×10  → i16, 0.1℃)     */
  uint16_t error_flags;    /* Status ErrorFlags (u16 비트)                */
  uint16_t limit_flags;    /* Status LimitFlags (u16 비트)                */

  uint8_t  can_rx_err_count; /* DSP CAN 수신 에러 카운터 (CAN 2.0)         */
  uint8_t  can_tx_err_count; /* DSP CAN 송신 에러 카운터 (CAN 2.0)         */
  uint16_t active_motor;     /* 현재 활성 모터 인덱스 (0~9)                */

  int16_t  dsp_temp;       /* DSP_Board_Temp (float×10 → i16, 0.1℃)      */

  /* SD_ONLY 진단(무선 미전송) — 0x404~0x409/0x417, 🟠 잠정(스케일 동일 컨벤션 적용) */
  int16_t  phase_b_i;      /* Phase B Current  (float×10  → i16, 0.1A)   */
  int16_t  phase_c_i;      /* Phase C Current  (float×10  → i16, 0.1A)   */
  int16_t  v_vector_d;     /* Motor V Vector Vd(float×100 → i16, 0.01V)  */
  int16_t  v_vector_q;     /* Motor V Vector Vq(float×100 → i16, 0.01V)  */
  int16_t  i_vector_d;     /* Motor I Vector Id(float×10  → i16, 0.1A)   */
  int16_t  i_vector_q;     /* Motor I Vector Iq(float×10  → i16, 0.1A)   */
  int16_t  bemf_d;         /* BackEMF BEMFd    (float×100 → i16, 0.01V)  */
  int16_t  bemf_q;         /* BackEMF BEMFq    (float×100 → i16, 0.01V)  */
  uint16_t rail_15v;       /* 15V 레일  (float×100 → u16, 0.01V)         */
  uint16_t rail_3v3;       /* 3.3V 레일 (float×100 → u16, 0.01V)         */
  uint16_t rail_1v9;       /* 1.9V 레일 (float×100 → u16, 0.01V)         */
  int16_t  slip_speed;     /* Slip Speed(float×10  → i16, 0.1Hz)         */

  uint32_t motor_last_tick;/* 타임아웃 워치독                             */
} MotorData_t;

#define ADC_POT_CHANNELS  2
_Static_assert(KEY_ADC_POT + ADC_POT_CHANNELS <= 0x20,
               "ADC channel keys must stay inside the DETAIL block (0x10~0x1F)");

typedef struct {
  uint32_t sum;
  uint16_t count;
  uint16_t min;
  uint16_t max;
  uint16_t last;
} AdcPotAcc_t;

typedef struct {
  BmsData_t   bms;
  BmsData_t   bms_lv;
  MpptData_t  mppt[MPPT_UNIT_COUNT];
  MotorData_t motor;          /* 🟠 잠정(virtual) — VCU 확정 시 교체 */

  float    distance_traveled;          /* 이동 거리 (m, Motor Odometer)        */
  float    battery_accumulated_energy;
  uint32_t odo_last_tick;

  AdcPotAcc_t adc_pot[ADC_POT_CHANNELS];
} VehicleData_t;

/* -------------------------------------------
 * System State
 * ------------------------------------------- */
typedef struct {
  volatile uint8_t sd_ok;
  volatile uint8_t can_ok;
  volatile uint8_t uart_ok;
  volatile uint8_t mcp_ok;
  volatile uint8_t error;
} SYSTEM_STATE;

#endif /* CORE_INC_TYPES_H_ */
