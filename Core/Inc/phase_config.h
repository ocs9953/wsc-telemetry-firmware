/**
  ******************************************************************************
  * @file    phase_config.h
  * @brief   Phase 게이트 — 작업 목록의 Phase를 순차 활성화하기 위한 스위치
  *
  *          모든 잔여 태스크가 미리 구현되어 있고, 기본값은 전부 0(비활성).
  *          작업 목록 체크포인트 순서대로 아래 매크로를 1로 바꿔가며 보드에서
  *          검증한다. 0이면 해당 배선(스레드/큐 생성, 콜백, 시작 호출)이
  *          컴파일에서 제외되어 현재 동작(Phase 2: CAN RX만)과 동일하다.
  *
  *          ⚠ 순수 로직 모듈(cobs, packet_builder 헬퍼, rtc_manager 함수)은
  *          호출자가 없으면 실행되지 않으므로 게이트 없이 항상 컴파일된다.
  ******************************************************************************
  */
#ifndef __PHASE_CONFIG_H
#define __PHASE_CONFIG_H

/* ── Phase 2.5 (task 6A): MCP2518FD CAN TX + SPI 뮤텍스 ─────────────────── */
#define PHASE25_CAN_TX          0

/* ── Phase 4 (task 10~12): TIM5 스케줄러 + UART TX 스레드 + 큐 생성 ─────── */
#define PHASE4_UART_PIPELINE    1

/* ── Phase 4.5 (task 13A): UART RX 수신 → MCP CAN TX (지상국→LCD) ───────── */
#define PHASE45_UART_RX_CAN_TX  0

/* ── Phase 5 (task 14~16): SD 로깅 + FileX 배선 + RTC 백업레지스터 ──────── */
#define PHASE5_SD_LOGGING       1

/* ── Phase 6 (task 18): Alive-Flag IWDG + 전원강하 감지 ─────────────────── */
#define PHASE6_SAFETY           1

/* ── 임시 (작업 목록 알려진 이슈): Phase 2~3 보드 테스트용 IWDG 리프레시 ──
 *    IWDG(4초)는 부팅부터 활성인데 리프레시는 Phase 4+에서만 수행되므로,
 *    Phase 4 활성 전 보드 테스트 시 이 스위치를 1로 켠다(MCP_RX_Thread의
 *    100ms 폴링 루프에서 리프레시). PHASE4 이상 켜면 다시 0으로. */
#define TEMP_IWDG_REFRESH       0

#define TEMP_S3_TIMEOUT_TEST    0

#define PHASE7_VCU_LCD_BRIDGE   0

#define PHASE8_GPS              1

#define PHASE9_ADC_SENSOR       1

#define TEST_DUMMY_DATA         1


/* ── 게이트 의존성 검사 ─────────────────────────────────────────────────── */
#if PHASE45_UART_RX_CAN_TX && !PHASE25_CAN_TX
#error "PHASE45(UART RX->CAN TX)는 PHASE25(CAN TX)가 필요합니다"
#endif
#if PHASE45_UART_RX_CAN_TX && !PHASE4_UART_PIPELINE
#error "PHASE45는 PHASE4(UART 파이프라인)가 필요합니다"
#endif
#if PHASE5_SD_LOGGING && !PHASE4_UART_PIPELINE
#error "PHASE5(SD 로깅)는 PHASE4(큐/스레드 생성)가 필요합니다"
#endif
#if PHASE6_SAFETY && !PHASE4_UART_PIPELINE
#error "PHASE6(Alive-Flag IWDG)는 PHASE4(스케줄러)가 필요합니다"
#endif
#if PHASE6_SAFETY && TEMP_IWDG_REFRESH
#error "PHASE6 활성 시 TEMP_IWDG_REFRESH는 꺼야 합니다 (Alive-Flag가 IWDG 전담)"
#endif

#if PHASE7_VCU_LCD_BRIDGE && !PHASE25_CAN_TX
#error "PHASE7(VCU<->LCD 중계)은 PHASE25(Bus B CAN TX)가 필요합니다"
#endif

#if PHASE8_GPS && !PHASE4_UART_PIPELINE
#error "PHASE8(GPS)는 PHASE4가 필요합니다 — 수집한 값을 내보낼 스케줄러/큐가 없습니다"
#endif

#endif /* __PHASE_CONFIG_H */
