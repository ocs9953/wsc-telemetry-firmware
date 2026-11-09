# 구현 계획: WSC 텔레메트리 펌웨어

## 개요

STM32U575xx + ThreadX 기반 텔레메트리 펌웨어를 **기능별 점진적 검증** 방식으로 구현한다. 각 Phase 완료 시 실제 보드에서 동작을 확인한 후 다음 Phase로 진행한다.

## ⚡ 구현 상태: Phase 게이트 방식 (2026-07)

**잔여 태스크(6A, 7, 8, 10~16, 18)의 코드가 전부 미리 구현되어 있다.** 다만 tasks.md의 Phase 순서대로 보드에서 순차 검증할 수 있도록, 활성화 지점(스레드/큐 생성, 콜백, 시작 호출)이 **`Core/Inc/phase_config.h`의 게이트 매크로**로 비활성화(0)되어 있다 — 주석 처리와 동일한 효과지만 컴파일러로 켜짐/꺼짐 양쪽을 검증할 수 있다.

| 게이트 (기본 0) | 해당 태스크 | 켜면 활성화되는 것 |
|---|---|---|
| `PHASE25_CAN_TX` | 6A | MCP TX FIFO + `ExtCAN_Transmit()` + `mcp_spi_mutex` |
| `PHASE4_UART_PIPELINE` | 10~12 | UART/SD 큐 생성, UART·UART_RX·SD 스레드 생성, TIM5 스케줄러 시작, UART 콜백 |
| `PHASE45_UART_RX_CAN_TX` | 13A | UART RX DMA+IDLE 수신 → COBS 디코드 → CAN TX |
| `PHASE5_SD_LOGGING` | 14~16 | SD_Logger_Init 배선, app_filex 오류경로 수정, RTC 백업레지스터 |
| `PHASE6_SAFETY` | 18 | Alive-Flag IWDG, 전원강하 감지+긴급 Flush |
| `TEMP_IWDG_REFRESH` | (임시) | Phase 4 전 보드 테스트용 IWDG 리프레시 (MCP_RX_Thread 100ms 루프) |

- 순수 로직(cobs, packet_builder 헬퍼, rtc_manager)은 호출자가 없으면 실행되지 않으므로 게이트 없이 항상 컴파일된다.
- 게이트 전부 0 = **현재 Phase 2 동작과 완전 동일** (빌드 크기 동일 확인). 전부 1 = 전체 파이프라인 (빌드 검증 완료, 경고 0).
- 아래 체크박스는 여전히 **보드 체크포인트 검증 완료** 기준으로 관리한다 (코드 구현 ≠ 검증).

**구현 시 확정한 사항 (md와의 편차/가정 — 보드 테스트 시 확인):**
1. **스레드 IWDG 리프레시는 '타임아웃 시'가 아니라 매 루프** (Phase 6 전): md대로 타임아웃 분기에서만 리프레시하면 데이터가 계속 흐르는 동안 그 분기를 영영 안 타서 IWDG(4초)가 만료된다. Phase 6 활성화 시 Alive-Flag 방식으로 자동 전환(`#if`).
2. **데이터 패킷 KEY enum 신설** (`types.h`): `KEY_BMS_VOLT_STACK(0x00)/CURR_TEMP/FAULT/CELL_00_03(0x10)~`, `KEY_MPPT_IV_IN_OUT/TEMP_FLAGS`, `KEY_MOTOR_BUS_VEL/TEMP_FLAGS` — 소스별 128-key 네임스페이스(0x00~0x7F), 0x80+는 ERR_KEY. ⚠ **지상국 디코더와 번호 동기 필요** (스펙 잔여 항목).
3. **LCD TX CAN ID = `0x500` 확정** (2026-08-21, `ext_can_controller.c`) — Bus B는 우리 보드와 LCD 보드만 있는 독립 절연 버스라 ID를 우리 팀이 정한다. 0x5xx는 Bus A 사용 대역(0x040~0x07F, 0x123/0x223, 0x310~0x346, 0x401~0x417)과 겹치지 않고 FDCAN1 필터 상한(0x4FF) 바깥이라, LCD를 Bus A에 잘못 물려도 조용히 반쯤 동작하지 않고 확실히 무시된다. 예약: 0x501~0x50F(LCD 방향), 0x510~0x51F(LCD→본 보드 방향).
4. **핀 극성 가정 2건** (보드 실측 후 매크로/비교값만 반전): SD_DETECT(PD0) = RESET이면 카드 있음(`SD_DETECT_ABSENT_LEVEL=SET`); Power_check(PD6) = RESET이면 전원 강하.
5. **UART_RX_Thread는 task 12.1대로 Phase 4에서 생성**되지만 Phase 4.5 게이트 전까지 엔트리가 슬립만 한다.
6. ~~**규정 패킷 순서 규약**~~ — **2026-08 폐기**. WSC 규정의 10초 주기 기록 조항이 없어져 `KEY_REG_DATA` 패킷 자체를 제거했다(아래 해당 작업 항목 참조). 누적값은 PRIMARY `KEY_MOTOR_ODO`로 1초마다 나간다.
7. **큐 스토리지는 정적 배열** (UART 20×32B=640B, SD 50×32B=1600B) — byte pool 대신 정적 할당으로 실패 경로 제거.

## Tasks

### Phase 1: 프로젝트 기반 — 타입 정의 + 빌드 설정

- [x] 1. 타입 정의 및 프로젝트 구조 설정
  - [x] 1.1 `Core/Inc/types.h` 생성 — 모든 공유 타입 정의
    - `LOG` 구조체 (16바이트, `__attribute__((packed))`), `_Static_assert(sizeof(LOG) == 16)`
    - `LOG_LEVEL` 열거형 (INFO=0, WARNING, ERROR, CRITICAL)
    - `LOG_SOURCE` 열거형 — **2-티어 통합** (✅ 2025-06 확정):
      - 데이터 소스: `SRC_SYS=0, SRC_BMS_BOT, SRC_BMS_TOP, SRC_MPPT1~3, SRC_MOTOR, SRC_RESERVED1/2`
      - 에러 소스: `SRC_CAN_ERR, SRC_SPI_ERR, SRC_UART_ERR, SRC_SD_ERR`
    - `LOG_KEY` 열거형 (KEY_SYS_BOOT, KEY_SYS_SD_INIT, ..., KEY_SYS_READY)
    - 에러 키 열거형 (ERR_CAN_BUS_OFF, ERR_CAN_RX_OVERFLOW, ERR_SPI_TIMEOUT, ERR_SPI_TRANSFER, ERR_UART_DMA, ERR_UART_QUEUE_FULL, ERR_SD_INIT, ERR_SD_WRITE, ERR_SD_NOT_DETECTED, ERR_SD_QUEUE_FULL, ERR_RTC_READ, ERR_INIT_FAIL, ERR_TIMEOUT, ERR_POWER_DOWN, ERR_WATCHDOG)
    - `DATETIME` 구조체 (year, month, date, hour, minute, second — 각 1바이트)
    - `VehicleData_t` 중첩 구조체 (✅ 2025-06 확정): `BmsData_t bms_bot/bms_top`, `MpptData_t mppt[3]`, `MotorData_t motor`(🟠 잠정 virtual), Reg 예약. **전체 정수** — 규정 데이터 제외 float 필드 없음. 필드 목록은 `can-signal-summary.md` §7 참조.
    - `SYSTEM_STATE` 구조체 (sd_ok, can_ok, uart_ok, mcp_ok, error)
    - _Requirements: 2.1, 2.2, 2.3, 5.1, 5.5, 5.6, 5.7, 10.4, 12.1_
  - [x] 1.2 모듈 헤더 파일 스켈레톤 생성
    - `Core/Inc/packet_builder.h`, `Core/Inc/cobs.h`, `Core/Inc/can_receiver.h`, `Core/Inc/ext_can_controller.h`, `Core/Inc/uart_transmitter.h`, `Core/Inc/sd_logger.h`, `Core/Inc/rtc_manager.h`, `Core/Inc/timer_scheduler.h`
    - 설계 문서의 각 모듈 인터페이스(함수 선언)를 그대로 반영
    - `Core/Inc/Conf_MCP251XFD.h` 생성 (MCP251XFD_TRANS_BUF_SIZE=16)
    - _Requirements: 10.1, 10.4, 4.1_
  - [x] 1.3 Makefile 및 빌드 설정 업데이트
    - C_SOURCES에 모든 새 소스 파일 경로 추가 (packet_builder.c, cobs.c, can_receiver.c, ext_can_controller.c, uart_transmitter.c, sd_logger.c, rtc_manager.c, timer_scheduler.c)
    - MCP251XFD 드라이버 소스 경로 추가 (Lib/MCP251XFD/)
    - C_INCLUDES에 `Lib/MCP251XFD/` 경로 추가
    - example_code 폴더가 빌드에서 제외되어 있는지 확인
    - _Requirements: 10.1, 10.2_

- [x] 1.4 **`Core/Inc/types.h` 재작업 — 확정된 스펙으로** (1.1 재오픈 — ✅ 2026-06 적용 완료, 전체 빌드 통과)
  - `LOG_SOURCE`: 기존 `SYS/CAN_SRC/SPI_SRC/UART_SRC/SD_SRC/ADC_SRC`를 **통합 enum**으로 교체: 데이터 소스(`SRC_SYS, SRC_BMS_BOT, SRC_BMS_TOP, SRC_MPPT1~3, SRC_MOTOR, SRC_RESERVED1/2`) + 에러 소스(`SRC_CAN_ERR, SRC_SPI_ERR, SRC_UART_ERR, SRC_SD_ERR`)
  - `VehicleData_t`: 평면 float 구조체를 **중첩 정수 구조체** `BmsData_t bms_bot/bms_top`, `MpptData_t mppt[3]`, `MotorData_t motor`(🟠 잠정 virtual, Reg 예약)으로 교체. `design.md` §types.h·`can-signal-summary.md` §7 참조.
  - ⚠️ **연쇄 수정 필수 (함께 수정하지 않으면 빌드 깨짐)**: `can_receiver.c`·`ext_can_controller.c`가 현재 `battery_voltage`/`mppt_voltage` 등에 기록하고 `source=CAN_SRC/SPI_SRC`를 사용 중. CAN switch-case를 중첩 정수 필드(엔디안 `__REV`/`__REV16`, mV→cV ÷10, MPPT float→×100/×10 스케일링 수행) + 새 에러 소스로 재작성. Phase 2 CAN 매핑과 묶어 진행.
  - `StdFiltersNbr`: **2 유지** — 논리 ID 묶음 4개를 연속 구간 범위 필터 2개(0x100~0x2FF, 0x400~0x6FF)로 커버. `CAN_Receiver_Init()`의 `FilterID1/ID2` 값만 수정(생성코드 `fdcan.c` 불변). (req 1.5)
  - _Requirements: 2.1, 2.3, 5.5, 1.5_

- [x] 2. Checkpoint — Phase 1 빌드 검증
  - 전체 프로젝트 빌드 확인 (타입 정의 + 빈 모듈 스켈레톤), 질문이 있으면 사용자에게 문의.

### Phase 2: CAN 데이터 수신 검증 — "데이터가 들어오는가?"

- [x] 3. CAN 수신 모듈 구현 (FDCAN1)
  - [x] 3.1 `Core/Src/can_receiver.c` 구현
    - `CAN_Receiver_Init()`: FDCAN1 하드웨어 필터 설정(**StdFiltersNbr=2**, 범위 필터 2개: 0x100~0x2FF, 0x400~0x6FF), 인터럽트 활성화, HAL_FDCAN_Start()
    - Classic CAN 500kbps (NominalPrescaler=10, TimeSeg1=12, TimeSeg2=3)
    - `CAN_Receiver_RxCallback()`: HAL_FDCAN_RxFifo0Callback에서 호출, CAN ID switch-case로 VehicleData 갱신, last_tick 업데이트
    - 버스 오류 시 ERR_CAN_BUS_OFF LOG 생성 → UART_Queue + SD_Queue 전송, LED_CAN(PE14) 점등
    - Rx FIFO 오버플로우 시 ERR_CAN_RX_OVERFLOW WARNING LOG 생성
    - CAN ID 목록은 플레이스홀더로 구현 (확정 후 업데이트)
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 1.5_
  - [x] 3.2 ✅ **BMS DETAIL(상세온도·쿨롱카운터)·MPPT aux·Motor DETAIL/SD_ONLY 파싱 추가** (2026-07)
    - `CAN_StoreBmsDetail()` 신설: 0x113/0x114(상세온도 A/B), 0x130~0x132(쿨롱카운터) + TOP 0x213/0x214/0x230~0x232. 오프셋은 `CAN DATA/BMS_CAN_ID_명세.xlsx` TEST_data 시트에서 직접 추출(기존 md 요약은 "FET_Temp, Internal_Temp, Avg_Temp 등"으로 뭉뚱그려져 있었음 — 정밀화).
    - `CAN_StoreMppt()`: +3/+4/+6(aux 12V/3V/MaxOut/MaxIn/PowerConn) 케이스 추가.
    - `CAN_StoreMotor()`: 0x40C(DSP온도, DETAIL 무선) + 0x404~0x409/0x417(SD_ONLY 진단 6종, 🟠 잠정 스케일: 기존 Motor 컨벤션 적용) 케이스 추가.
    - `types.h`: `BmsData_t`/`MpptData_t`/`MotorData_t`에 대응 필드 + `BMS_KEY`/`MPPT_KEY`/`MOTOR_KEY`에 대응 KEY 신설.
    - `timer_scheduler.c`: `Sched_EmitDetail()`에 신규 DETAIL 패킷 17개(무선) 추가, `Sched_EmitSdOnly()` 신설 + Motor SD_ONLY 진단 6개 패킷(SD_Queue 전용, 무선 미전송).
    - 무선 대역폭 재계산: PRIMARY 70 + DETAIL 25(기존 8 + 신규 17) = **95 pkt/s(56%)** — 기존 설계 추정치와 정확히 일치(설계 당시 이미 이 신호들을 예산에 반영했었음을 확인). (task 3.4에서 CAN 헬스 패킷 1개 추가로 96 pkt/s로 갱신됨)
    - ✅ **크로스팀 이슈 해결**: xlsx 개요 시트의 BMS 보드(STM32U545) 비트타이밍 경고(1.667Mbit/s 오계산)는 타 팀과 통신 속도·설정 합의 완료로 해소됨(500kbps 통일). 상세: `can-signal-summary.md` 상단.
    - BMS 쿨롱카운터는 전부 raw 값(물리 단위 환산은 BQ76972 데이터시트 확인 후 별도 작업).
    - _Requirements: 3.1, 3.2 (연장)_
  - [x] 3.3 ✅ **WaveSculptor22 공식 매뉴얼 확보 후 Motor SD_ONLY 바이트 순서 오류 5건 정정** (2026-07)
    - `CAN DATA/TritiumWaveSculptor22_Manual.pdf`(사용자 추가) Appendix C(CAN Comms Protocol)를 직접 추출·대조.
    - 매뉴얼 규칙: 표에 먼저 나열된 변수 = bits 63..32(byte 4~7), 나중 변수 = bits 31..0(byte 0~3). task 3.2에서 이 순서를 반대로 가정해 아래 5곳이 바이트 순서 오류였음:
      - `CAN_ID_MOTOR_V_VECTOR`(0x405): Vd/Vq 바이트 순서 반대 → 수정
      - `CAN_ID_MOTOR_I_VECTOR`(0x406): Id/Iq 바이트 순서 반대 → 수정
      - `CAN_ID_MOTOR_BEMF`(0x407): BEMFd/BEMFq 바이트 순서 반대 → 수정
      - `CAN_ID_MOTOR_RAIL_A`(0x408): **구조 자체가 잘못됨** — 이 프레임엔 15V만 있고(byte4-7), byte0-3은 Reserved인데 3.3V로 잘못 읽고 있었음. 3.3V는 별도 프레임 0x409(byte4-7)에 있음(1.9V는 0x409 byte0-3)
      - `CAN_ID_MOTOR_SLIP`(0x417): byte0-3(Reserved)에서 읽고 있었음 → byte4-7로 수정
    - PRIMARY/DETAIL 신호(Bus V/I, Velocity, Heat-sink/Motor Temp, DSP Temp, Odometer/AmpHours, Flags)는 매뉴얼 대조 결과 전부 기존 구현이 정확했음(우연이 아니라 일관된 컨벤션 적용 덕분).
    - `can-signal-summary.md`·`telemetry-signals.md` §4/§7 갱신: 매뉴얼 원문 출처 명시 + 정정된 오프셋 반영.
    - _Requirements: 3.1, 3.2 (연장)_
  - [x] 3.4 ✅ **Status Information(0x401) 확장 필드 추가 — Active Motor·TX/RX 에러 카운터** (2026-07, 사용자 요청)
    - `CAN_StoreMotor()`의 `CAN_ID_MOTOR_FLAGS`(0x401) 케이스 확장: 기존 Limit/Error Flags(byte0-3)에 더해 Active Motor(byte4-5, u16), Transmit Error Count(byte6, u8), Receive Error Count(byte7, u8) 파싱 추가.
    - `types.h`: `MotorData_t`에 `can_rx_err_count`/`can_tx_err_count`/`active_motor` 필드, `MOTOR_KEY`에 `KEY_MOTOR_CAN_HEALTH`(0x11) 추가.
    - `timer_scheduler.c`: 신규 DETAIL 무선 패킷 1개(`KEY_MOTOR_CAN_HEALTH`) 추가 — Limit/Error Flags(#14, 이미 4슬롯 꽉 참)와 별도 패킷.
    - Identification Information(0x400, Serial Number/Tritium ID)은 사용자 요청으로 미구현 유지.
    - 무선 대역폭: DETAIL 25→26패킷, 합계 **96 pkt/s(56%)** — 여유 내.
    - _Requirements: 3.1, 3.2 (연장)_
  - [x] 3.5 ✅ **BMS 셀 전압 + SD_ONLY 상세(CUV/COV/CB밸런싱/Safety·PF) 파싱·송출 추가** (2026-07, 전체 리뷰 발견 갭 해소)
    - 리뷰 발견: 스케줄러가 `cell_mv[16]`을 8패킷으로 송출 중이었으나 **정작 채우는 파싱(0x101~0x104/0x201~0x204)이 없어 0만 전송**되고 있었음 → `CAN_StoreBmsDetail()`에 셀 전압 케이스 추가로 해소.
    - 추가 구현(xlsx 정밀 오프셋 대조): CUV 스냅샷(0x140~0x143/0x240~0x243), COV 스냅샷(0x150~0x153/0x250~0x253), CB_Status1/2/3 밸런싱 비트맵(0x123·0x160~0x163·0x170~0x173 + TOP), Safety/PF Alert 상세(0x121/0x122 + TOP) — 전부 **SD_ONLY**(무선 미전송).
    - `types.h`: `BmsData_t`에 `cuv_mv[16]`/`cov_mv[16]`/`cb_status1~3`/Safety·PF u8×16 필드, `BMS_KEY`에 0x20~0x32 SD_ONLY 키 신설.
    - `can_receiver.c`: BMS TEST 디스패치를 명시적 case 목록에서 **범위 기반**((id&0xF00)==0x100→BOT, 0x200→TOP)으로 재구성 — 미정의 sub_id는 `CAN_StoreBmsDetail()` default에서 무시.
    - `timer_scheduler.c`: 보드당 SD_ONLY 19패킷(CUV4+COV4+CB9+Safety/PF2) × 2보드 = 38패킷/s SD 큐 추가 (무선 대역폭 무영향, 96 pkt/s 유지).
    - `can-rx-data-current.md` 신설: 현재 코드가 실제 수신·파싱하는 CAN ID 전체 목록(BMS 62 + MPPT 21 + Motor 13). (2026-07-31 문서 정리에서 `can-signal-summary.md` §1.5로 흡수·통합되고 파일 자체는 삭제됨 — 아래 "문서 구조 정리" 참조.)
    - _Requirements: 3.1, 3.2, 8 (연장)_
  - [x] 3.6 ✅ **무선 티어 재배치 + BMS(LV) 신규 + HV BMS TOP RUN ID 이전** (2026-07-13, `CAN DATA/CAN_수신_데이터_정리본.xlsx` + Prohelion D1000 Gen1 데이터시트 기준)
    - **티어 재배치**(xlsx 기준): MPPT 0x612/0x615(온도·flags) PRIMARY→DETAIL, 0x613(12V/3V aux) DETAIL→SD_ONLY(신규 KEY_MPPT_MAXLIM로 0x614 분리); Motor 0x401(전체) DETAIL 통합(`KEY_MOTOR_CAN_HEALTH`→`KEY_MOTOR_STATUS`로 개명, Limit/Error까지 포함), 0x404 SD_ONLY→DETAIL, 0x407(BEMF) SD_ONLY→PRIMARY, 0x40E(Odo/Ah) PRIMARY 신규 추가(규정 10s 패킷과 병행).
    - **BMS(LV) 신규**(Prohelion BMS D1000 Gen1, 리틀엔디안): `types.h`에 `SRC_BMS_LV`·`BmsLvData_t`·`BMS_LV_KEY`(고정 키는 `0x10+3×LV_MAX_CMU`에서 자동 파생 → CMU 증가 시 충돌 방지)·`LV_MAX_CMU`/`LV_MAX_CELLS`; `can_receiver.c`에 `CAN_StoreBmsLv()`(0x600 base, heartbeat/CMU/SoC/PackV·I/충전기/프리차지/MinMax/상태/팬/확장) + 디스패치 연결; `timer_scheduler.c`에 PRIMARY(SoC·PackV·I) + DETAIL(CMU·나머지) + SD_ONLY(확장·heartbeat) 송출 + 타임아웃 워치독. base 0x600·5S2P(LV_MAX_CMU=1) 가정 — 🟠 잠정.
    - **HV BMS TOP RUN ID 이전**: 0x600~0x602 → **0x503~0x505** (LV base 0x600 충돌 회피). `CAN_ID_BMS_TOP_*` 상수 + BOT/TOP 프레임 인덱스 추출(`id-FAULT`)·디스패치 수정. ⚠ **실물 HV BMS 보드 재설정 필요**.
    - 무선 대역폭 재계산: PRIMARY 15 + DETAIL 40 = **115 pkt/s(68%)** (이전 96/56%에서 증가, 여유 내).
    - 문서 갱신: `can-signal-summary.md`(§1 필터·§2 BMS TOP ID·§3 MPPT·§4 Motor·§4.5 BMS(LV) 신설·§5 PRIMARY 목록·§8 대역폭·§9 잔여), `telemetry-signals.md`(상단 supersede 배너 + CAN ID 맵·티어 요약 정정). **지상국 디코더 재동기 필수.**
    - _Requirements: 3.1, 3.2 (연장)_
  - [x] 3.7 ✅ **BMS(HV) CAN ID 전면 재배치** (2026-07-24, `CAN DATA/BMS_CAN_ID_명세_최신_PR31.xlsx` PR#31 기준 — 위 3.5/3.6의 0x500대·0x1xx/0x2xx·0x503~0x505 체계를 완전히 대체)
    - BMS(HV) 전체를 우선순위(PRIMARY/DETAIL/SD_ONLY) 기반 연속 대역 **0x040~0x07F**로 재배치. PRIMARY BOT 0x040~0x043(Fault/Volt/Curr/SOC)·TOP 0x044~0x047, DETAIL BOT 0x048~0x050·TOP 0x051~0x059(9개씩), SD_ONLY BOT 0x05A~0x06C·TOP 0x06D~0x07F(19개씩).
    - **경위**: 처음엔 SOC 없이 0x040~0x07D(62개)로 1차 배치 → SOC가 LEGACY(0x503/0x603→0x504)로 남아 MPPT·Motor보다 버스 우선순위가 낮은 문제 발견 → SOC를 PRIMARY로 편입 요청 → 팀이 RUN을 0x040~0x047 연속 8개로 재구성, TEST 전체 +2 이동(0x040~0x07F, 64개)해 최종 확정.
    - LEGACY 프레임(0x123/0x223 FET_Status/CHG/DSG/PDSG)만 파싱, CB_Status1은 여기서 분리되어 SD_ONLY 0x05C/0x06F로 이전. 그 외 0x111/112/120/133 계열(및 TOP 0x2xx)은 매칭 항목 없어 미파싱 유지.
    - MPPT: general_id 공장설정 0x600→0x300 발주 반영, base 0x610/0x620/0x630 → **0x310/0x320/0x330**(로터리 스위치 1/2/3 유지).
    - FDCAN1 필터 재구성: 필터0 **0x040~0x33F**(BMS(HV) 신규대역+레거시+MPPT), 필터1 0x400~0x6FF(Motor+BMS(LV), 변경 없음).
    - `can_receiver.c`: BMS(HV) DETAIL/SD_ONLY 디스패치를 옛 `(id&0xF00)` 니블 마스크 방식에서 **상대 오프셋 범위 체크**(`CAN_ID_BMS_{BOT,TOP}_{DETAIL,SDONLY}_BASE` + `CAN_StoreBmsDetail`/`CAN_StoreBmsSdOnly`)로 재작성. `types.h`에 `soc_permille`·`fet_status/chg/dsg/pdsg` 필드 추가, `ld_voltage_raw` 제거(DLC 8→6). `BMS_KEY`에 `KEY_BMS_SOC`(0x03)·`KEY_BMS_FET_STATUS`(0x33) 추가.
    - 무선 대역폭 재계산: PRIMARY 17(+2, SOC) + DETAIL 40 = **125 pkt/s(74%)** (이전 115/68%에서 증가, 여유 내).
    - 문서 갱신: `can-signal-summary.md`·`can-rx-data-current.md`·`design.md`(en 포함)·`requirements.md`(en 포함)·`telemetry-signals.md`(en 포함) 전체 CAN ID 동기화. **지상국 디코더 재동기 필수**(SOC #16/#17 신규 패킷 포함). 실물 HV BMS/MPPT 보드가 새 ID로 재플래시됐는지 배포 전 확인 필요.
    - _Requirements: 3.1, 3.2 (연장)_

  - [x] 3.8 ✅ **스케줄러 버스트 해소 + ISR 경량화** (2026-07-31, 전체 시스템 리뷰(코드 구조·최적화) 발견)
    - **발견한 결함**: `Timer_Scheduler_Tick()`이 `tick%10==0`인 틱에서 `tick%2==0`도 참이라 `Sched_EmitPrimary()`(17패킷)와 `Sched_EmitDetail()`(89패킷 = EmitBoth 40 + EmitSdOnly 49)을 **한 ISR 안에서 연속 실행**. UART 57건/SD 106건이 깊이 20/50 큐에 몰리는데, 소비 스레드(pri 5/10)는 ISR(pri 1) 중 드레인 불가 → **UART 65%·SD 53%가 매초 드롭**. `uart_full_latch`/`sd_full_latch` 때문에 경고는 초당 1회만 나가 로그에서 심각도가 가려져 있었음. `PHASE4_UART_PIPELINE=0`이라 실기판에서 미발현, 게이트를 켜는 즉시 발현하는 상태였음.
    - **수정 ① DETAIL 10 서브틱 분산**: 240줄짜리 단일 `Sched_EmitDetail()`을 논리 그룹 7개 함수로 분해(`Sched_EmitBmsDetailWireless`/`BmsCuvCov`/`BmsCbSafety`/`MpptDetail`/`MpptSdAndMotor`/`LvA`/`LvB`) 후 `Sched_EmitDetailStep(tick%10)`으로 100ms마다 하나씩 발행. **그룹별 1초 주기는 유지**되고 ISR당 최대 발행량만 89→12로 감소. PRIMARY(짝수 틱)와 겹치지 않게 큰 그룹을 홀수 스텝에 배치. 타임아웃 검사는 부하 최저인 step 5로 이동. 발행 총량은 분해 전과 동일(EmitBoth 40 / EmitSdOnly 49)함을 확인.
    - **수정 ② 큐 메시지 32B→16B**: `LOG`가 16B인데 메시지가 8 ULONG(32B)이라 절반이 패딩이었음. `types.h`에 `LOG_QUEUE_MSG_WORDS`(=4) + `_Static_assert` 추가, 스토리지 총량은 그대로 두고 **깊이만 2배**(UART 20→40, SD 50→100). `ULONG msg[8]` 지역변수 10곳 일괄 교체.
    - → 결과: **피크 26 vs 깊이 40/100** (여유 1.5×~3.8×). 평균 대역폭 125 pkt/s(74%)는 불변.
    - **수정 ③ crc8 테이블화**: 비트 단위 루프(15B = 120 iter)를 256엔트리 룩업(15 iter)으로 교체, 약 8배 단축. FDCAN1과 TIM5는 preemption priority가 같아(무락 `VehicleData` 설계 전제) 서로 선점 못 하므로 TIM5 ISR 길이가 곧 CAN RX 유실 마진인데, STM32U5의 RX FIFO0는 HAL 하드코딩 **3칸 고정**(`SRAMCAN_RF0_NBR`)이라 여유가 없었음. **출력값은 기존과 완전 동일**(테이블은 기계 생성, 비트 단위 구현과 전수 대조 검증) → 지상국 디코더 변경 불필요.
    - **수정 ④ UART DMA 에러 오인 수정**: `UART_Transmitter_ErrorCallback()`이 대기 해제용으로 `uart_tx_complete_sem`을 put하면 `UART_Thread_Entry`가 이를 **전송 완료로 오인**해 LED 토글·`uart_ok=1` 복구·`dma_err_latch` 해제까지 수행(=에러가 지워짐). `uart_tx_dma_error` 플래그로 구분.
    - **수정 ⑤ 소소**: `f_to_u16`/`f_to_i16`에 NaN 가드(`v != v`) — 고장 노드의 NaN이 Cortex-M33 VCVT를 거쳐 조용히 0이 되어 정상 0 판독과 구분 불가였음. `CAN_StoreMotor`/`CAN_StoreBmsLv`가 전역 `VehicleData`를 직접 참조하던 것을 포인터 인자로 변경(`CAN_StoreBms`/`CAN_StoreMppt`와 일관성 + 테스트 가능). 미사용 `SD_PKTS_PER_BUF` 제거.
    - **검증**: `.\build.ps1 -Clean` — `PHASE4_UART_PIPELINE` 0/1 양쪽 빌드 성공, `Core/Src` 경고 0(잔존 5건은 전부 서드파티 `Lib/MCP251XFD` 4 + `Middlewares/ST/filex` 1). crc8 테이블 등가성은 알려진 답(`CRC-8('123456789')==0xF4`) + 256 전수 + 랜덤 15바이트 5,000회로 비트 단위 구현과 대조 확인. `tests/host/test_main.c`에 동일 대조를 `test_crc8_table_equivalence()`로 상시 추가(단, 이 PC엔 네이티브 gcc가 없어 호스트 테스트 실행은 미수행 — 툴체인 있는 환경에서 실행 필요).
    - **2차 점검 후속 수정** (같은 날, 1차 수정 결과를 재검토하며 발견):
      - **10초 패킷 위상 충돌**: `Sched_Emit10s()`(RTC+규정 3패킷)가 `tick%100==0` → **phase 0**에서 실행되어, 가장 무거운 스텝(PRIMARY 17 + step0 9)과 겹쳐 10초마다 피크가 26→**29**로 튀었다. `tick%100==7`(무선 발행 최소 스텝)로 이동 — 주기는 10초 그대로. 1차 수정 때 문서에 적은 "피크 26"이 실제로는 29였던 것을 정정.
      - **`FileX/App/app_filex.c`의 `ULONG msg[8]` 2곳 누락**: 1차 스윕을 `Core/Src/`로만 돌려 놓쳤다. `tx_queue_send`가 `tx_queue_message_size`(4 ULONG)만 복사하므로 동작상 버그는 아니었으나 일관성을 위해 교체. (`PHASE5_SD_LOGGING` 게이트라 평소 미컴파일 → 게이트를 켜고 빌드 검증함)
      - **`design.md` 메모리 예산 오류 정정**: UART_Queue/SD_Queue 스토리지가 `tx_app_byte_pool` 사용처로 적혀 있었으나 실제로는 **BSS 정적 배열**(`static ULONG uart_queue_storage[]`)이다. byte pool에서 할당되는 건 스레드 스택뿐. 합계도 ~8.4KB → ~5.4KB로 정정. 아울러 전체 RAM 사용률이 768KB 중 7% 수준이라 **큐 깊이 확장에 RAM 제약이 없음**을 명시.
    - _Requirements: 6.1~6.8, 7.5, 7.7, 9.7 (연장)_

  - [x] 3.9 ✅ **스펙 문서 구조 정리 — 8개 파일의 CAN 스펙 3중 중복 해소** (2026-07-31, 사용자 지적)
    - **발견한 문제**: CAN ID·신호 상세가 `can-signal-summary.md`·`can-rx-data-current.md`·`telemetry-signals.md` 3개 문서에 각각 다른 형식으로 중복되어 있었고, `telemetry-signals.md`는 스스로 "§5~7은 구버전, 정본은 can-signal-summary.md"라고 선언하면서도 그 구버전 내용을 그대로 남겨두고 있었음. 코드 주석도 SSOT를 파일마다 다르게 지목(`can_receiver.c`는 design.md, `timer_scheduler.c`는 can-signal-summary.md+telemetry-signals.md). CAN ID가 바뀔 때마다 최대 7개 파일을 손으로 맞춰야 했음(이번 세션의 스케줄러 수정 1건에도 7개 파일 수정).
    - **원칙**: `requirements.md`(요구사항 정의)/`design.md`(아키텍처 설계)/`tasks.md`(수행 이력)의 3분할 역할은 유지. 이 셋에 새어 들어와 있던 CAN ID 상세만 제거하고, 그 상세를 담당하는 **별도 문서군**(can-signal-summary/can-rx-data-current/telemetry-signals)을 하나로 통합.
    - **`can-rx-data-current.md` 삭제**: 내용(대분류별 CAN ID 빠른조회 표 + 합계 66+21+13+14=114)을 `can-signal-summary.md` 신설 §1.5로 흡수. 이제 CAN ID 상세의 **유일한 SSOT**.
    - **`telemetry-signals.md` 433→약 200줄로 축소**: 스스로 구버전이라 선언한 신호 표 §5~7·VehicleData_t 초안(§9)·스테일해진 무선예산 표(§2, 56% vs 실제 74%)를 삭제. 설계 원칙(§0 확장 대응 원칙, §0.5 파이프라인, 패킷 인코딩 원칙, telemetry_map 테이블 지향점 등)만 남기고 전부 can-signal-summary.md 참조로 교체.
    - **`design.md`**: `types.h` 전체 구조체 스냅샷(3번의 CAN 개정 동안 실제 코드와 계속 어긋나 있었음 — 예: BmsLvData_t 자체가 누락)을 "전략 설명 + types.h/can-signal-summary.md §7 참조"로 교체. 파일 내부에서 LOG 구조체가 두 번 중복 정의되어 있던 것 정리. `ExtCAN_Transmit()` 함수 시그니처 등 실제 코드와 어긋나 있던 소소한 드리프트 정정.
    - _Requirements: 10 (모듈화 코드 구조, 연장)_

- [x] 4. 외부 CAN 컨트롤러 모듈 구현 (MCP2518FD)
  - [x] 4.1 `Core/Src/ext_can_controller.c` 구현
    - 정적 객체: MCP251XFD 디바이스 인스턴스, SPI 버퍼, RX 메시지/페이로드 버퍼
    - HAL SPI 콜백 구현: `MCP_SPIInit()`, `MCP_SPITransfer()` (Software GPIO NSS), `MCP_GetCurrentms()`
    - `ExtCAN_Init()`: 디바이스 구조체 설정 → Init_MCP251XFD() → ConfigureFIFO() → ConfigureFilter() → StartCAN20()
    - 수신 deferred 구조: EXTI는 `ExtCAN_NotifyInterrupt()`로 `mcp_rx_semaphore` set, `MCP_RX_Thread`(`ExtCAN_RxThread_Entry`)가 `ExtCAN_DrainRxFIFO()`로 FIFO 드레인 → `ExtCAN_ProcessMessage()`로 VehicleData 갱신(`TX_DISABLE/TX_RESTORE` 보호)
    - `ExtCAN_IsReady()`: 초기화 완료 플래그 반환
    - SPI 타임아웃/전송 실패 시 ERR_SPI_TIMEOUT/ERR_SPI_TRANSFER LOG 생성, LED_ERR(PE11) 점등
    - CAN ID 목록은 플레이스홀더로 구현 (확정 후 업데이트)
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 1.6_

- [x] 5. CAN 수신 와이어링 — main.c 초기화 + 인터럽트 콜백 연결
  - [x] 5.1 `Core/Src/main.c` — USER CODE BEGIN 2에서 CAN_Receiver_Init(), ExtCAN_Init() 호출
    - 초기화 실패 시 LED_ERR(PE11) 점등
    - _Requirements: 1.1, 1.5, 1.6_
  - [x] 5.2 `Core/Src/main.c` — USER CODE BEGIN 4에서 CAN 콜백 연결
    - `HAL_FDCAN_RxFifo0Callback()`: CAN_Receiver_RxCallback() 호출
    - `HAL_GPIO_EXTI_Falling_Callback()`: CAN_INT(PB0)/CAN_RX_INT(PE9) → ExtCAN_NotifyInterrupt() 호출 (세마포어 set; MCP INT active-low → falling 엣지)
    - _Requirements: 3.1, 4.4_

- [x] 6. Checkpoint — Phase 2 CAN 수신 검증 **(2026-08-31 완료)**
    - 더미 3종(BMS/MPPT/Motor) 동시 수신 확인. 버스 `ECR=0` — 부팅 후 프로토콜 에러 0건.
    - 시나리오 S1(짧은 DLC)·S2(순간 고장 비트)·S5(필터 상한 0x34F) 통과.
    - S3(수신 두절)은 통과했으나 타임아웃 검사가 TIM5 스케줄러(PHASE4)에 있어
      **Phase 4 기능**임을 확인. S4(FIFO 오버플로우)는 버스 포화에서도 유실이
      0 이라 재현되지 않아 **Phase 4 로 이관**.
    - IWDG 주의사항은 `TEMP_IWDG_REFRESH` 로 해소된 상태로 진행함.
  - 빌드 확인, 보드에 플래시 후 CAN 버스에서 데이터 수신 확인 (디버거로 VehicleData 값 확인 — LED_CAN은 에러 표시 전용이라 수신 시 점등하지 않음), 질문이 있으면 사용자에게 문의.
  - ⚠️ **IWDG 리셋 주의**: IWDG가 부팅부터 활성(~4초)인데 리프레시 로직은 task 18.3(Phase 6)에나 추가됨 → 이 단계 실기 테스트 시 보드가 ~4초마다 리셋됨. 아래 '알려진 이슈'의 IWDG 항목 참조(임시 비활성 또는 임시 리프레시).

### Phase 2.5: CAN TX — "지상국 메시지가 LCD에 전달되는가?"

- [ ] 6A. MCP2518FD CAN TX 기능 구현
  - [ ] 6A.1 `Core/Src/ext_can_controller.c` — TX FIFO 설정 추가
    - ExtCAN_Init()에 TX FIFO(MCP251XFD_FIFO2) 설정 추가: Direction=MCP251XFD_TRANSMIT_FIFO, Payload=8BYTE
    - _Requirements: 13.3_
  - [ ] 6A.2 `Core/Src/ext_can_controller.c` — ExtCAN_Transmit() 구현
    - MCP251XFD_TransmitMessageToFIFO()를 사용하여 CAN 메시지 전송
    - CAN ID는 LCD 보드용 플레이스홀더 (확정 후 업데이트) [^3]
    - 8바이트 CAN 프레임의 첫 1바이트는 제어 바이트(bit7=마지막 프레임, bit6~0=시퀀스 번호), 나머지 7바이트에 데이터
    - 7바이트 초과 시 복수 CAN 프레임으로 분할 전송
    - 전송 실패 시 ERR_CAN_TX_FAIL ERROR LOG 생성 → UART_Queue + SD_Queue 전송
    - **SPI1 동시성 보호 (리뷰 #1)**: `mcp_spi_mutex`(TX_MUTEX, app_threadx.c에서 생성) 추가. `ExtCAN_Transmit()`의 송신 op와 `ExtCAN_DrainRxFIFO()`의 수신 op를 각각 상위 드라이버 호출 단위로 mutex get/put 감싸 직렬화 (NSS 한 사이클이 아니라 논리적 op 전체). → design.md "SPI1 동시성" 참조.
    - _Requirements: 13.2, 13.4, 13.5, 13.6_
  - [ ] 6A.3 `Core/Inc/ext_can_controller.h` — ExtCAN_Transmit() 함수 선언 추가
    - _Requirements: 13.2_

- [ ] 6C. Checkpoint — Phase 2.5 CAN TX 검증
  - 빌드 확인, ExtCAN_Transmit() 단위 테스트 (디버거에서 직접 호출하여 CAN 버스에 메시지 출력 확인), 질문이 있으면 사용자에게 문의.

### Phase 3: 패킷 빌더 + COBS — "데이터가 올바르게 포장되는가?"

- [x] 7. 패킷 빌더 모듈 구현 (순수 로직)
  - [x] 7.1 `Core/Src/packet_builder.c` 구현
    - `PacketBuilder_Create()`: timestamp(HAL_GetTick), level, source, key, value 복사, checksum(CRC-8 bytes[0..14]) 계산
    - `PacketBuilder_CalcChecksum()`: 15바이트 CRC-8 연산 (다항식 0x07, `crc8()` 정적 헬퍼)
    - `PacketBuilder_VerifyChecksum()`: 체크섬 검증
    - `PacketBuilder_PackFloat2()` / `PacketBuilder_UnpackFloat2()`: memcpy 기반 float 2개 패킹/언패킹 (RTC/규정 전용으로만 유지)
    - **신규** `PacketBuilder_PackU16x4()`, `PackI16x4()`, `PackU32x2()`: 정수 패킹 (all-int 텔레메트리)
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.8, 5.9_
  - [x]* 7.2 호스트 PC용 property test — 패킷 라운드트립 일관성 ✅ **실행 완료(2026-07)**
    - **Property 1: 라운드트립 일관성** — 임의의 LOG 필드 값으로 PacketBuilder_Create() 후 직렬화/역직렬화하면 원본과 동일
    - **Validates: Requirements 5.9**
    - `tests/host/test_main.c` — 네이티브 gcc(MinGW)로 실제 `packet_builder.c` 컴파일해 호스트에서 실행. `sizeof(LOG)==16` 확인 + PASS.
  - [x]* 7.3 호스트 PC용 property test — 체크섬 무결성 ✅ **실행 완료(2026-07)**
    - **Property 2: 체크섬 무결성** — 임의의 LOG 패킷에 대해 CalcChecksum 결과가 VerifyChecksum과 일치하며, 임의 1비트 변경 시 검증 실패
    - **Validates: Requirements 5.3**
    - 패킷 2000개 × 120비트 플립 = 240,000회 전수 검사, 전부 검출 확인. PASS.
  - [x]* 7.4 호스트 PC용 property test — float 패킹 라운드트립 ✅ **실행 완료(2026-07)**
    - **Property 3: float 패킹 라운드트립** — 임의의 float 2개를 PackFloat2 → UnpackFloat2하면 원본과 동일
    - **Validates: Requirements 5.4**
    - 5,000회 라운드트립 PASS. 추가로 `PackU16x4`/`PackI16x4`(INT16_MIN/MAX 경계값 포함)/`PackU32x2` 각 5,000회도 함께 검증.

- [x] 8. COBS 인코더/디코더 모듈 구현 (순수 로직)
  - [x] 8.1 `Core/Src/cobs.c` 구현
    - `COBS_Encode()`: 16바이트 입력 → 최대 17바이트 인코딩 (0x00 제거), 호출자가 0x00 delimiter 추가
    - `COBS_Decode()`: 인코딩된 데이터 → 원본 복원, 오류 시 0 반환
    - _Requirements: 7.2, 7.3_
  - [x]* 8.2 호스트 PC용 property test — COBS 라운드트립 ✅ **실행 완료(2026-07)**
    - **Property 4: COBS 라운드트립** — 임의의 16바이트 데이터를 Encode → Decode하면 원본과 동일
    - **Validates: Requirements 7.3**
    - 20,000회(길이 0~16 랜덤 포함) PASS. 손상 입력(인코딩 스트림에 0x00 포함) 거부도 확인.
  - [x]* 8.3 호스트 PC용 property test — COBS 인코딩 결과에 0x00 없음 ✅ **실행 완료(2026-07)**
    - **Property 5: 0x00 부재** — 임의의 16바이트 입력을 COBS_Encode한 결과에 0x00 바이트가 포함되지 않음
    - **Validates: Requirements 7.3**
    - 위 20,000회 인코딩 결과 전수 검사, 전부 PASS. 실행 방법: `tests/host/README.md`.

- [ ] 9. Checkpoint — Phase 3 패킷 로직 검증
  - 빌드 확인, 순수 로직 모듈 테스트 통과 확인, 질문이 있으면 사용자에게 문의.

### Phase 4: UART 전송 검증 — "패킷이 무선으로 나가는가?"

- [ ] 10. UART 송신 모듈 구현 (UART_Thread)
  - [ ] 10.1 `Core/Src/uart_transmitter.c` 구현
    - `UART_Transmitter_Init()`: uart_tx_complete_sem 세마포어 생성, UART RX DMA 수신 시작
    - `UART_Thread_Entry()`: 무한 루프 — tx_queue_receive(&UART_Queue, msg, 2000) → 성공 시 타입 헤더(0x01) 추가 + COBS 인코딩(최대 19B) → HAL_UART_Transmit_DMA(USART2) → 세마포어 대기(DMA 완료) → LED_RFD900x(PE15) 토글
    - 타임아웃 시 HAL_IWDG_Refresh(), timeout_count 누적, 5회 초과 시 ERR_TIMEOUT WARNING LOG 생성
    - DMA 전송 완료 콜백(HAL_UART_TxCpltCallback)에서 세마포어 put
    - DMA 오류 시 ERR_UART_DMA ERROR LOG → SD_Queue 전송, LED_RFD900x(PE15) 점등
    - USART2 설정: 115200bps, 8N1, HW Flow Control RTS/CTS, GPDMA1_Channel2
    - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 9.1, 9.3, 9.5, 9.7_

- [ ] 11. 타이머 스케줄러 구현 (TIM5 ISR — Producer)
  - [ ] 11.1 `Core/Src/timer_scheduler.c` 구현
    - `Timer_Scheduler_Start()`: HAL_TIM_Base_Start_IT(&htim5)
    - `Timer_Scheduler_Tick()`: HAL_TIM_PeriodElapsedCallback에서 TIM5 분기 시 호출
    - 100ms 주기(Prescaler=15999, ARR=999): VehicleData 읽기 → PacketBuilder_Create(INFO) → tx_queue_send(UART_Queue, TX_NO_WAIT) + tx_queue_send(SD_Queue, TX_NO_WAIT)
    - **멀티레이트 스케줄링** (✅ 2025-06 확정, 2026-07-31 task 3.8로 DETAIL 발행 방식 개정): **PRIMARY 200ms**(phase 짝수)·**DETAIL 1s**(`Sched_EmitDetailStep(tick%10)`으로 10개 서브틱에 분산 — 그룹별 주기는 1s 유지). SD_ONLY 신호 → SD_Queue만. SSOT는 `can-signal-summary.md`(신호별 티어) / 설계 원칙은 `telemetry-signals.md` §2. 무선 여유: 최신 수치는 `can-signal-summary.md` §8 참조(작성 시점 ~96 pkt/s → 현재 125 pkt/s).
    - UART_Queue Full 시 ERR_UART_QUEUE_FULL WARNING LOG → SD_Queue에만 전송, LED_RFD900x(PE15) 점등
    - SD_Queue Full 시 ERR_SD_QUEUE_FULL WARNING LOG → UART_Queue에만 전송, LED_SD(PE13) 점등
    - 타임아웃 검사: 각 핵심 데이터 last_tick이 1000ms 초과 시 ERR_TIMEOUT WARNING LOG 생성 (한 번만 기록, 갱신 시 해제)
    - 10초(10000ms) 주기 RTC 시각 읽기 → RTC_TIME INFO LOG 생성
    - 10초 주기 WSC 규정용 패킷 생성 (key=KEY_REG_DATA): VehicleData에서 distance_traveled, solar_accumulated_energy, battery_accumulated_energy 읽기 → float 2개씩 PackFloat2로 패킹하여 복수 LOG 패킷 생성 → UART_Queue + SD_Queue 전송
    - 소프트웨어 카운터(static 변수)로 100회(=10초) 카운트, 새 타이머 불필요
    - 인터럽트 내 블로킹 함수 호출 금지
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 11.3, 11.4, 9.4, 9.7_

- [ ] 12. RTOS 객체 생성 + UART 와이어링
  - [ ] 12.1 `Core/Src/app_threadx.c` — RTOS 객체 생성 (USER CODE 블록)
    - `tx_application_define()` 내 App_ThreadX_Init()에서:
    - UART_Thread 생성 (Priority 5, Stack 1024B, Auto Start)
    - UART_RX_Thread 생성 (Priority 7, Stack 1024B, Auto Start)
    - SD_Logger_Thread 생성 (Priority 10, Stack 2048B, Auto Start) — 엔트리 함수는 빈 루프로 임시 구현
    - UART_Queue 생성 (Msg 4 ULONG = sizeof(LOG), Depth 40 — 2026-07 task 3.8에서 8 ULONG/Depth 20에서 변경)
    - SD_Queue 생성 (Msg 4 ULONG = sizeof(LOG), Depth 100 — 2026-07 task 3.8에서 8 ULONG/Depth 50에서 변경)
    - uart_tx_complete_sem 세마포어 생성
    - _Requirements: 1.2, 9.2, 9.5_
  - [ ] 12.2 `Core/Src/main.c` — USER CODE BEGIN 2에서 Timer_Scheduler_Start() 추가
    - LED_BOARD_CHECK(PE12) 점등 (정상 부팅 표시)
    - _Requirements: 1.3, 1.4_
  - [ ] 12.3 `Core/Src/main.c` — USER CODE BEGIN Callback 1에서 TIM5 분기 추가
    - `HAL_TIM_PeriodElapsedCallback()`: TIM5 분기 → Timer_Scheduler_Tick() 호출
    - `HAL_UART_TxCpltCallback()`: uart_tx_complete_sem 세마포어 put
    - _Requirements: 6.2, 7.5, 9.7_

- [ ] 13. Checkpoint — Phase 4 UART 전송 검증
  - 빌드 확인, 보드에 플래시 후 지상국 PC에서 UART 데이터 수신 확인 (시리얼 모니터로 COBS 프레임 확인, 100ms 주기 패킷 도착 확인), 질문이 있으면 사용자에게 문의.

### Phase 4.5: UART RX → CAN TX 연동 — "지상국 메시지가 LCD에 전달되는가?"

- [ ] 13A. UART RX 수신 → CAN TX 연동
  - [ ] 13A.1 UART RX 수신 로직 구현 (`Core/Src/uart_transmitter.c`)
    - USART2 DMA + IDLE 인터럽트 기반으로 바이트 수신
    - 0x00 delimiter 감지 시 COBS 프레임 완성 → COBS 디코딩
    - 첫 바이트(타입 헤더) 확인: 0x01=CAN 데이터(미사용), 0x02=텍스트 메시지
    - 타입 0x02이면 나머지 페이로드를 제어 바이트(1B) + 데이터(7B)로 분할하여 ExtCAN_Transmit() 호출
    - 프레임 오류 시 폐기 후 다음 0x00부터 재동기화
    - _Requirements: 7.8, 7.9, 7.10, 7.11, 13.1_

- [ ] 13B. Checkpoint — Phase 4.5 UART RX → CAN TX 검증
  - 빌드 확인, 지상국에서 메시지 전송 → UART RX 수신 → COBS 디코딩 → MCP2518FD CAN TX → LCD 보드 수신 확인, 질문이 있으면 사용자에게 문의.

### Phase 5: SD 카드 로깅 검증 — "데이터가 SD에 저장되는가?"

- [ ] 14. RTC 관리 모듈 구현
  - [ ] 14.1 `Core/Src/rtc_manager.c` 구현
    - `RTC_Manager_GetDateTime()`: HAL_RTC_GetTime/GetDate 호출 → DATETIME 구조체 채움, 실패 시 HAL_ERROR 반환
    - `RTC_Manager_PackToValue()`: DATETIME 6바이트를 LOG value 필드(8B)에 패킹
    - `RTC_Manager_MakeFilename()`: DATETIME → "YYMMDD_HHMMSS.log" 형식 문자열 생성
    - 부팅 시 RTC Backup Register 확인하여 이미 세팅된 경우 초기화 건너뛰기
    - RTC 읽기 실패 시 ERR_RTC_READ ERROR LOG 생성 → UART_Queue + SD_Queue 전송, LED_ERR(PE11) 점등
    - _Requirements: 11.1, 11.2, 11.5, 11.6, 11.7_

- [ ] 15. SD 로거 모듈 구현 (SD_Logger_Thread)
  - [ ] 15.1 `Core/Src/sd_logger.c` 구현
    - 더블 버퍼 선언: `__attribute__((aligned(4))) static uint8_t sd_buf_a[512], sd_buf_b[512];`
    - `SD_Logger_Init()`: FX_MEDIA 포인터 저장, 부팅 시각으로 전체 로그 파일명(`YYMMDD_HHMMSS.log`) + 규정용 파일명(`YYMMDD_HHMMSS_reg.log`) 생성, fx_file_create/open (2개 파일)
    - `SD_Logger_Thread_Entry()`: 무한 루프 — tx_queue_receive(&SD_Queue, msg, 2000) → 활성 버퍼에 LOG 패킷 append → 32패킷(512B) 도달 시 버퍼 스왑 → fx_file_write() → 10초 주기 fx_media_flush()
    - **규정용 파일 분리 기록**: 패킷의 key가 KEY_REG_DATA이면 더블 버퍼를 거치지 않고 즉시 규정용 파일(`_reg.log`)에 fx_file_write() + fx_media_flush() 수행 (10초에 1회, 더블 버퍼 흐름과 독립)
    - 타임아웃 시 HAL_IWDG_Refresh(), timeout_count 누적, 5회 초과 시 ERR_TIMEOUT WARNING LOG 생성
    - SD 미감지(SD_DETECT: PD0) 시 ERR_SD_NOT_DETECTED LOG → UART_Queue 전송, SD 로깅 비활성화, LED_SD(PE13) 점등
    - FileX 마운트 실패 시 ERR_SD_INIT LOG → UART_Queue 전송
    - 기록 오류 시 ERR_SD_WRITE LOG → UART_Queue 전송, LED_SD(PE13) 점등
    - _Requirements: 8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.7, 8.8, 8.9, 8.10, 8.11, 9.1, 9.3_

- [ ] 16. SD 로거 와이어링
  - [ ] 16.1 `Core/Src/app_threadx.c` — SD_Logger_Thread 엔트리 함수를 실제 SD_Logger_Thread_Entry로 교체
    - _Requirements: 1.2_
  - [ ] 16.2 `FileX/App/app_filex.c` — FileX 미디어 초기화 후 SD_Logger_Init() 호출 (USER CODE 블록)
    - fx_app_thread_entry 내에서 fx_media_open 성공 후 SD_Logger_Init(&sdio_disk, &boot_time) 호출
    - 부팅 시 RTC_Manager_GetDateTime()으로 부팅 시각 획득
    - ⚠️ **fx_media_open 실패 경로 데드락 수정 (시스템 점검 발견)**: 현재 생성코드는 `USER CODE BEGIN SD DRIVER get info error` 안이 `while(1);` — SD 미삽입/마운트 실패 시 **무한루프 → (IWDG 활성 시) 리셋 루프**, 요구사항 7("SD 실패해도 무선 텔레메트리는 계속")과 정면 충돌. `while(1)`을 ERR_SD_INIT LOG(→UART_Queue) + LED_SD(PE13) 점등 + SD 로깅 비활성 플래그로 교체하고 스레드는 종료/대기. USER CODE 블록 내부라 규칙 준수하며 수정 가능.
    - _Requirements: 1.2, 8.11, 11.2, 7 (SD 실패 시 무선 지속)_

- [ ] 17. Checkpoint — Phase 5 SD 로깅 검증
  - 빌드 확인, 보드에 플래시 후 SD 카드에 로그 파일 생성 확인 (파일명 형식, 데이터 내용, 더블 버퍼 동작), 질문이 있으면 사용자에게 문의.

### Phase 6: 오류 처리 + 전원 관리 + 워치독 — "시스템이 안전한가?"

- [ ] 18. 오류 처리 및 전원 관리 통합
  - [ ] 18.1 오류 LED 및 SYSTEM_STATE 관리 로직 구현
    - 각 모듈의 오류 발생 시 SYSTEM_STATE 구조체 갱신
    - LED 점등 로직: LED_SD(PE13), LED_CAN(PE14), LED_RFD900x(PE15), LED_ERR(PE11) — source별 매핑
    - LED_BOARD_CHECK(PE12): Power_check(PD6) 3.3V 정상 시 점등
    - _Requirements: 12.1, 12.2, 12.3, 12.4_
  - [ ] 18.2 전원 강하 감지 및 긴급 Flush 구현
    - Power_check(PD6) GPIO 인터럽트 또는 폴링으로 전원 강하 감지
    - CRITICAL LOG 생성 (ERR_POWER_DOWN, value=감지 시점 tick + 미기록 버퍼 잔량)
    - SD_Logger 긴급 Flush 수행 후 무한 루프 진입
    - _Requirements: 12.5, 12.6_
  - [ ] 18.3 IWDG 워치독 Alive Flag 방식 통합
    - IWDG 타임아웃 ~4초 (LSI 32kHz, Prescaler=64, Reload=2000)
    - 각 스레드(UART_Thread, UART_RX_Thread, SD_Logger_Thread)에서 HAL_IWDG_Refresh() 직접 호출 제거, Alive Flag SET으로 변경
    - Timer_Scheduler_Tick()에서 2초 주기로 모든 Alive Flag 확인 → 전부 SET이면 HAL_IWDG_Refresh() + 플래그 초기화
    - 하나라도 미SET이면 리프레시 안 함 → IWDG 타임아웃으로 시스템 자동 리셋
    - CRITICAL 오류 시 LED_ERR 점등 + 인터럽트 비활성화 + 무한 루프
    - _Requirements: 9.6, 9.3, 12.5_

- [ ] 19. Checkpoint — Phase 6 오류 처리 검증
  - 빌드 확인, 오류 시나리오 테스트 (SD 미삽입, CAN 미연결 등), LED 점등 확인, 워치독 리프레시 정상 동작 확인, 질문이 있으면 사용자에게 문의.

### Phase 7: 최종 통합 — "전체 시스템이 동작하는가?"

- [ ] 20. 최종 통합 및 클린업
  - [ ] 20.1 모든 인터럽트 콜백 최종 확인 (main.c USER CODE BEGIN 4)
    - `HAL_TIM_PeriodElapsedCallback()`: TIM5 분기 확인
    - `HAL_FDCAN_RxFifo0Callback()`: CAN_Receiver_RxCallback() 확인
    - `HAL_GPIO_EXTI_Falling_Callback()`: CAN_INT(PB0)/CAN_RX_INT(PE9) → ExtCAN_NotifyInterrupt() 확인
    - `HAL_UART_TxCpltCallback()`: uart_tx_complete_sem 확인
    - _Requirements: 3.1, 4.4, 6.2, 7.5, 9.7_
  - [ ] 20.2 초기화 실패 시 오류 LOG 생성 로직 최종 확인
    - CAN_Receiver_Init(), ExtCAN_Init() 실패 시 ERR_INIT_FAIL ERROR LOG 생성
    - LED_ERR(PE11) 점등
    - _Requirements: 1.4_

- [ ] 21. Final Checkpoint — 최종 빌드 및 통합 완료
  - 전체 프로젝트 클린 빌드 성공 확인, CAN 수신 → 패키징 → UART 전송 + SD 로깅 전체 파이프라인 동작 확인, 질문이 있으면 사용자에게 문의.

## Notes

- `*` 표시된 태스크는 선택 사항이며 빠른 MVP를 위해 건너뛸 수 있음
- Property-based test는 순수 로직 모듈(packet_builder, cobs)에만 적용 — HAL/RTOS 의존 코드는 PBT 대상 제외
- CAN ID: BMS·MPPT 확정 (`can-signal-summary.md` 참조). Motor는 🟠 잠정 (WaveSculptor22 기반 virtual, 0x400~0x4FF — VCU 확정 시 교체). LCD 보드 TX ID는 미정 (플레이스홀더 코드)
- FDCAN1/MCP2518FD 역할: FDCAN1=차량 전체 RX(범위 필터 2개: 0x040~0x33F, 0x400~0x6FF — 2026-07-24 BMS(HV) 재배치 반영). MCP=향후 저우선순위 주변장치 버스로 예약(확정 용도=LCD 메시지 TX, 센서 RX는 미확정)
- ✅ **types.h 재작업 완료** (task 1.4, 2026-06): `Core/Inc/types.h`는 확정 스펙(통합 `LOG_SOURCE` + 중첩 정수 `VehicleData_t`)으로 재작성됨. `can_receiver.c`(실 CAN ID·엔디안·스케일)·`ext_can_controller.c`(파싱 stub) 연쇄 수정 포함, 전체 빌드 통과.
- 모든 커스텀 코드는 `/* USER CODE BEGIN/END */` 블록 내부에만 작성
- 각 태스크는 특정 요구사항을 참조하여 추적 가능
- 각 Phase의 Checkpoint에서 실제 보드 동작을 확인한 후 다음 Phase로 진행

## 확정된 설계 결정 (코드 리뷰 후 반영됨)

- **MCP 오실레이터**: SiT8008BI-12-33E-40.000000 (40MHz) 확정 → `OscFreq=40000000`, `SYSCLK_IS_CLKIN`.
- **EXTI 엣지**: MCP INT는 active-low → `GPIO_MODE_IT_FALLING` + `GPIO_PULLUP`. 콜백은 `HAL_GPIO_EXTI_Falling_Callback`에서 CAN_INT(PB0)/CAN_RX_INT(PE9) 둘 다 처리.
- **NVIC 우선순위** (PRIORITYGROUP_4, preemption): FDCAN1·TIM5=1, EXTI0/9·SPI1·USART2·SDMMC1=2, GPDMA1_Ch1/Ch2=3. SysTick=14, TIM6=15(기본값). → design.md "인터럽트 우선순위 설계" 참조.
  - ⚠️ **GPDMA1_Channel1(USART2_RX)**: `.ioc` **손편집**으로 우선순위를 넣으면 `ForceEnableDMAVector` 때문에 재생성 시 0,0으로 리셋된다. 반드시 **CubeMX GUI(NVIC Configuration 탭)** 에서 Preemption=3으로 설정할 것 — GUI 설정은 재생성에도 유지됨(검증 완료, gpdma.c가 `3,0` 생성). Ch2는 손편집/GUI 모두 정상.
- **LED_CAN(PB15)**: CAN 문제 진행 중=점등, 정상=소등의 실시간 양방향 표시(2026-08 LED 재설계로 갱신 — 아래 항목 참고). 요구사항 3.4/12.3 반영. (2026-08: PE14→PB15로 정정 — 아래 "회로도 대조" 항목 참고)
- **RTC 프리스케일러** (✅ 시스템 점검 수정, 2026-06): RTC 클럭=LSI(32000Hz)인데 프리스케일러가 LSE(32768Hz)용 CubeMX 기본값 127/255여서 캘린더가 0.9766Hz(하루 ~34분 지연, 계통오차)였음. → **`SynchPrediv=249`**(32000÷128÷250=1.000Hz)로 수정. `rtc.c`(즉시 반영) + `.ioc`(`RTC.AsynchPrediv=127`/`SynchPrediv=249` 추가 — 기존엔 항목 없어 재생성 시 기본값 복귀했음) 양쪽 수정. RTC는 `MX_RTC_Init`/`.ioc`에서만 설정 가능(USER CODE 대안 없음). LSI RC 고유 드리프트(±수%)는 잔존 — 절대 정밀 필요 시 LSE 크리스탈 또는 스무스 캘리브레이션.
- **LOG_SOURCE** (✅ task 1.4 적용 완료, 2026-06): *이전* `types.h`는 기존 6종 enum `SYS=0, CAN_SRC=1, SPI_SRC=2, UART_SRC=3, SD_SRC=4, ADC_SRC=5`이었음. **2025-06 확정 → 2026-06 적용**: 데이터 소스(`SRC_SYS, SRC_BMS_BOT, SRC_BMS_TOP, SRC_MPPT1~3, SRC_MOTOR, SRC_RESERVED1/2`) + 에러 소스(`SRC_CAN_ERR, SRC_SPI_ERR, SRC_UART_ERR, SRC_SD_ERR`) 통합 단일 enum으로 변경 완료 (빌드 통과).
- **MCP_SPITransfer**: 수신 전용 경로에서 `mcp_spi_buf`(16B) 크기 단위로 분할 전송하여 송신 버퍼 초과읽기 방지.
- **MCP 수신 deferred 구조** (Issue #1 해결): EXTI ISR은 `ExtCAN_NotifyInterrupt()`로 `mcp_rx_semaphore`만 set. `MCP_RX_Thread`(Priority 6, Stack 1024B, app_threadx.c에서 생성)가 세마포어 대기 → `ExtCAN_DrainRxFIFO()`로 FIFO를 빌 때까지 드레인. 블로킹 SPI가 스레드 컨텍스트라 `HAL_GetTick()` timeout이 정상 동작한다.
- **VehicleData 임계구역**: FDCAN ISR(pri 1)은 선점되지 않아 보호 불필요(`__disable_irq` 제거). MCP_RX_Thread(pri 6)는 FDCAN ISR에 선점되므로 갱신 구간을 `TX_DISABLE/TX_RESTORE`(PRIMASK 저장/복원)로 보호.
- **MCP_RX_Thread 폴백 폴링** (리뷰 #3 해결): `tx_semaphore_get`을 `TX_WAIT_FOREVER` 대신 유한 타임아웃(`MCP_RX_POLL_TICKS` ≈100ms)으로 대기하고, 타임아웃(`TX_NO_INSTANCE`) 시에도 드레인한다. 드레인 중 SPI 일시오류로 빠져나와 INT(active-low)가 물린 채 새 falling 엣지가 없는 'stuck' 상황 및 부팅/경쟁으로 놓친 통지를 주기적으로 복구한다.
- **SPI 에러 LOG 레이트리밋** (리뷰 #5 해결): `ExtCAN_ReportSPIError`는 드라이버 콜백(SPI 전송 1건)마다 호출되므로, 동일 LOG를 `MCP_SPI_ERR_LOG_MIN_INTERVAL_MS`(500ms) 간격으로 제한해 폭주를 막는다. LED/SYSTEM_STATE는 매번 갱신(멱등), retry count는 계속 누적되어 다음 보고에 반영.
- **SPI1 단일소유/뮤텍스** (리뷰 #1, ✅ **task 6A.2 구현 완료, `PHASE25_CAN_TX` 게이트**): `ExtCAN_Transmit()`이 SPI1을 `MCP_RX_Thread`(pri 6, RX)와 공유하므로, 상위 드라이버 호출 단위를 `TX_MUTEX`(`mcp_spi_mutex`, `TX_INHERIT` 우선순위 상속)로 직렬화. TX FIFO full 폴링(최대 20ms) 포함. → design.md "SPI1 동시성" 참조.
- **FileX fault-tolerant 활성화** (✅ 시스템 점검 발견·수정, 2026-07): `fx_user.h`의 `FX_ENABLE_FAULT_TOLERANT`가 켜져 있었으나 `fx_fault_tolerant_enable()`을 아무도 호출하지 않아 실제 보호는 0이었음(플래시만 차지). `FileX/App/app_filex.c`(`PHASE5_SD_LOGGING` 게이트) media_open 성공 직후 4096B 정적 버퍼로 활성화 — 주행 중 전원 컷 시 FAT/디렉터리 손상 방지.
- **VehicleData 무임계구역 불변식의 부팅 시 검증** (✅ 시스템 점검 발견·수정, 2026-07): FDCAN1/TIM5 ISR 무잠금 설계는 "두 IRQ의 preemption priority가 같다"는 CubeMX 설정에만 의존 — 재생성/실수로 어긋나도 컴파일은 통과해 조용히 데이터 경합 발생 가능. `main.c` USER CODE 2에서 `HAL_NVIC_GetPriority()`로 FDCAN1_IT0/IT1과 TIM5 우선순위를 부팅 시 직접 비교, 불일치 시 LED_ERR 점등 + `Error_Handler()`로 즉시 정지(게이트 무관, 항상 실행).
- **회로도(`WSC.kicad_sch`) 대조 — 핀 배정 오류 3건 발견·수정 (2026-08)**: PCB 회로도를 `kicad-cli sch export netlist`로 넷리스트 추출해 STM32 핀 배정 전수 대조한 결과, 발견 즉시 수정.
  - **LED_SD/LED_CAN 핀 완전 오배정** — 코드는 LED_SD를 PE13, LED_CAN을 PE14로 구동했으나, 회로도상 두 LED(각각 R36/R35 저항 경유 정식 회로)는 **PD8/PB15**에 배선되어 있고 PE13/PE14는 **미접속**이었음. 실기판에서는 SD/CAN 에러 LED가 절대 켜지지 않는 상태였음. `main.h`(`LED_SD_Pin/GPIO_Port`, `LED_CAN_Pin/GPIO_Port`), `gpio.c`(두 핀을 기존 GPIOE 일괄 초기화에서 분리해 PD8/PB15 개별 `HAL_GPIO_Init`), `.ioc`(`Mcu.Pin13/14`, `PD8.*`/`PB15.*` 항목) 전부 정정. 코드상 다른 로직(LED 점등 조건 등)은 변경 없음 — 핀 번호만 수정.
  - **`Power_check`(PB1) 핀 미접속 확인** — 회로도상 PB1은 미접속 상태. 이 핀을 읽어 "전원 정상 LED" 로직을 만들려던 계획(requirements.md 구 Req 12.2)은 **폐기**하고, **패시브 하드웨어 LED(3.3V 직결, GPIO/펌웨어 무관)**로 단순화하기로 결정. `LED_BOARD_CHECK`(PE12)는 이후 별도로 실시간 반영 방식으로 재설계됨(아래 "LED 역할 구체화" 항목 참고) — 이걸로 Req 1.3과 Req 12.2가 서로 다른 의미를 주장하던 모순도 함께 해소됨.
    - ⚠️ **PHASE6_SAFETY(task 18.2, 전원 강하 감지)는 여전히 PB1을 GPIO_Input으로 읽어 사용** — 사용자 결정(2026-08): **PB1을 3.3V 레일에 직결 배선 예정**(현재 `PHASE6_SAFETY=0`이라 비활성 상태라 당장 영향 없음, 배선 완료 시 코드 수정 없이 그대로 동작).
      **트레이드오프 인지 필요**: PB1이 감지하는 3.3V가 MCU 자신을 구동하는 레일과 동일하면, 외부 전원 차단 시 PB1과 MCU 코어가 거의 동시에 죽는다. task 18.2의 설계 의도("완전히 죽기 전에 감지해 SD 긴급 flush를 마칠 시간 확보")가 주는 조기경보 여유는 결국 **해당 3.3V 레일의 벌크 캐패시턴스가 무너지는 속도**에 좌우된다 — 캐패시터가 충분히 크면 수 ms 여유가 생기고, 작으면 사실상 조기경보 없이 함께 다운된다. 회로 설계 영역이라 여기서는 트레이드오프만 기록.
  - **GPS UART(USART1, PB6/PB7) 신설 배선 발견 — 활성화 완료**: 회로도에 커넥터(J11)로 배선된 `/GPS_TX`(PB6)·`/GPS_RX`(PB7) net을 확인, GPS 센서 연동용으로 신설된 배선임을 확인. `USART1`을 CubeMX 패턴에 맞춰 활성화(`usart.c`/`usart.h`/`main.c`/`.ioc`) — **보레이트 등 세부 설정은 GPS 센서 기종 선정 후 확정 예정**(현재 임시값으로 컴파일만 통과하는 상태, 실사용 전 재설정 필수). 파싱 로직은 아직 없음.
  - **PB3/PB4/PB5 — 라벨 없는 예비 GPIO 확인**: 회로도상 R43/R28/R29에 각각 연결되어 있으나 용도 미정. 사용자 확인: 예비로 남겨둔 핀이며 필요 시 추후 활성화 예정 — 현재 조치 없음.
- **4차 직접 점검 — 3차 수정의 자체 회귀 2건 + 비일관 2건 (2026-08)**: 리뷰 도구 대신 상태 기계 전체 흐름(각 `_ok` 플래그의 SET/CLEAR 지점 전수, 홀드오프 상호작용)을 직접 추적해 발견.
  1. **`!SystemState.error` 조건이 요구사항을 깨뜨림 — 3차 수정 자체 철회** (`main.c`): 3차에서 리뷰 지적("LED_ERR 켜졌는데 LED_BOARD_CHECK도 정상 표시")을 받아 건강 판정에 `!error`를 넣었는데, **`error`는 6개 지점에서 SET만 되고 어디서도 0으로 돌아가지 않는다**. 그래서 회복 가능한 일시 오류(CAN bus-off 1회, SD 쓰기 재시도, SPI 글리치) **한 번**에 보드 LED가 영구 소등되어, 사용자가 지정한 "문제 생기면 꺼지고 해소되면 다시 켜진다"(Req 12.2)가 깨졌다. → **되돌림**. LED_ERR(치명 오류 **상시 래치**)와 LED_BOARD_CHECK(**실시간** 건강)는 의미가 다른 것이 설계 의도이며, 둘이 동시에 켜진 상태는 "과거에 오류가 있었고 지금은 회복됨"으로 읽는 것이 정상이다 — 해당 리뷰 지적은 이 의도를 모르는 오탐이었다.
  2. **`uart_ok`만 초기 SET 누락** (`uart_transmitter.c`): `can_ok`(CAN_Receiver_Init)·`mcp_ok`(ExtCAN_Init)·`sd_ok`(SD_Logger_Init)는 전부 init 성공 시 1로 출발하는데 `uart_ok`만 0에서 출발했다. PHASE4 활성 시 LED_BOARD_CHECK가 **부팅 후 첫 송신 성공까지 꺼져 있다**. → `UART_Transmitter_Init()`에서 낙관적으로 1 설정(USART2 HW 초기화는 이미 끝났고 실패 시 Error_Handler로 정지하므로 안전).
  3. **큐풀 래치 해제 순서** (`timer_scheduler.c`): 래치 해제가 `Sched_EmitPrimary()` **뒤**에 있어, 이번 틱 PRIMARY가 세운 래치가 곧바로 지워져 같은 1초 안에 DETAIL 틱이 한 번 더 보고할 수 있었다(주석의 "1초당 1회"와 불일치). → 해제를 발행 **앞**으로 이동.
  4. 확인만 하고 **바꾸지 않은 것**: `uart_tx_in_flight`를 `HAL_UART_Transmit_DMA` 성공 확인 뒤에 세우는 현재 위치에는 명령어 2~3개 폭의 잔여 창이 남아, 그 사이 RX측 DMA 에러가 오면 최대 1초 스톨이 가능하다. 다만 호출 **앞**에 두면 창이 HAL 함수 전체 길이로 넓어지고 결과도 더 나쁘므로(정상 전송 중단 + 패킷 손실), 현재 배치가 최선이라 판단해 유지하고 잔여 위험만 기록한다.
  - 검증: 상태 기계 전수 추적(각 플래그의 초기 SET·회복 경로·클리어 지점 존재 확인), `SYSTEM_STATE`가 필드 단위로만 접근되어 volatile 전환이 안전함 확인, `fault_tick[]`이 단일 32비트 read/write뿐이라 ISR/스레드 공유에 원자적임 확인, 타임아웃 감시 배열 9원소 = `SCHED_TIMEOUT_SOURCES` 일치 확인. 빌드는 게이트 전부 0 / 전부 1 양쪽 경고 0.
- **3차 코드 리뷰(max 강도) 후속 수정 (2026-08)**: 전 범위 재점검에서 15건 지적. 아래 순서로 반영.
  - **A. 프레임 손상 (최우선)** — `uart_transmitter.c`: **잔여 세마포어 토큰**이 프레임을 깨뜨렸다. 직전 전송이 타임아웃으로 끝난 직후 늦게 도착한 TxCplt/Error가 `ceiling_put`을 하면 대기자가 없어 카운트 1이 남고, 다음 송신에서 `tx_semaphore_get`이 **즉시 성공**해(방금 `uart_tx_dma_error=0`으로 클리어됨) 갓 시작한 DMA를 '완료'로 오인 → **DMA가 읽는 중인 공유 버퍼 `uart_tx_frame[]`에 다음 패킷을 덮어써** 깨진 프레임이 무선으로 나간다. → 송신 시작 직전 `TX_NO_WAIT` 루프로 **잔여 토큰 배수(drain)** 추가.
  - **B. 잘못된 데이터 저장** — `can_receiver.c`: `rx_data[8]`을 드레인 루프가 재사용하는데 `HAL_FDCAN_GetRxMessage`는 **DLC 바이트만** 복사한다. 파서들은 8바이트를 무조건 읽으므로(예: `CAN_StoreBmsSdOnly` case 0/1이 d[0..7], LV `MINMAX_V`·`STATUS`가 d[4..7]) **DLC<8 프레임에서 직전 프레임의 잔여 바이트가 유효 데이터로 저장**됐다. 실제로 BMS DETAIL 오프셋 8은 DLC 6이다. → 루프를 `for(;;)`로 바꿔 **매 프레임 읽기 직전 `memset`**.
  - **C. 2차 수정이 만든 회귀 2건** — ① `uart_tx_in_flight`를 `HAL_UART_Transmit_DMA` **호출 전**에 세워서, 그 사이 도착한 RX측 DMA 에러가 "송신 중"으로 오인되어 방금 정상 시작된 전송을 중단시켰다 → 전송 성공 확인 **후**로 이동. ② sticky 클리어 조건을 "양쪽 큐 모두 성공"으로 잡아서, **SD 카드가 느려져 SD_Queue만 차면 무선은 멀쩡한데도 클리어가 영영 막혀** 비트맵이 단조 증가만 하고 0으로 못 돌아갔다(지상국이 고장 해제를 볼 수 없음) → `Sched_Send`가 **주 목적지(무선)** 기준으로 반환하도록 변경.
  - **D. 큐 오류 오집계** — `Sched_Send`가 `== TX_QUEUE_FULL`만 실패로 봐서, 큐 생성 전(`main.c`가 `Timer_Scheduler_Start()`를 `MX_ThreadX_Init()`보다 먼저 호출하는 구간)의 `TX_QUEUE_ERROR`가 **성공으로 집계**됐다 → 드롭된 패킷의 sticky 비트가 지워지고 경고도 안 나감. → 판정을 `== TX_SUCCESS` 기준으로 교체하고, 경고/LED는 `TX_QUEUE_FULL`일 때만 발생하도록 분리. UART 분기의 비대칭(`all_queued` 재사용)도 SD 분기와 동일 형태로 정리.
  - **E. LED 표시 일관성 2건** — ① `SystemState_RefreshBoardLed()`가 `SystemState.error`(치명 오류 상시 래치)를 무시해, **LED_ERR가 켜져 있는데 LED_BOARD_CHECK도 "정상"을 표시**하는 모순이 있었다 → 건강 판정에 `!error` 포함. ② UART/SD 큐 Full 표시가 **홀드오프 없이** `uart_ok`/`sd_ok`를 내려서, 큐 Full은 소비 스레드가 정상 동작 중이라는 뜻이라 **다음 성공(~1.7ms)이 곧바로 LED를 꺼** 사람 눈에 안 보였다(1차에서 CAN에만 홀드오프를 넣고 UART/SD는 빠뜨림) → **공통 홀드오프 API**(`SystemState_MarkFault` / `SystemState_FaultHoldExpired`, `SUBSYS_FAULT_HOLD_MS`=3초, `FAULT_IDX_CAN/UART/SD`)를 `main.h`/`main.c`에 신설하고 CAN·UART·SD 전부 이관(can_receiver의 로컬 홀드오프 변수 제거).
  - **F. `SYSTEM_STATE` volatile 화** — 이 플래그들은 ISR(FDCAN1/TIM5)과 스레드(UART/SD/MCP_RX)가 공유하는데 회복 경로가 전부 `if (!SystemState.uart_ok)` 같은 가드로 묶여 있다. volatile이 없으면 **-O2(릴리스 빌드)에서 이 읽기가 호이스팅·캐싱**되어 ISR이 플래그를 내려도 스레드가 낡은 값만 보고 LED를 영영 갱신하지 않을 수 있다 → 전 필드 `volatile`.
  - **G. 유지보수성/문서** — `SD_PKTS_PER_BUF` 제거로 사라진 "SD_BUF_SIZE는 sizeof(LOG) 배수" 불변식을 `_Static_assert`로 복원(append가 **쓰고 나서 검사**하는 구조라 배수가 아니면 버퍼 오버런) / `app_threadx.c`의 큐 사이징 근거를 옛 수치(26패킷)에서 현재 값(피크 18)으로 정정하고 SSOT를 timer_scheduler.c로 명시 / NaN 가드 주석의 "`-ffast-math`에 영향받지 않는다"는 **틀린 서술** 정정(`-ffinite-math-only`가 `v != v`를 상수 false로 접어 가드를 삭제하므로 `-Ofast` 금지 명시) / `test_main.c`가 스위트 중간에 `srand(1234)`로 전역 시드를 바꿔 뒤따르는 `test_packing`의 입력이 통째로 달라지던 문제 제거 / `.vscode/settings.json`의 `cortex-debug.*` 경로가 다른 사용자(ocs99) 홈의 구 툴체인을 가리키던 것을 현재 ST 설치 경로로 갱신.
  - **판단 보류 1건**: `MX_USART1_UART_Init`의 `Error_Handler()` 호출(초기화 실패 시 전 시스템 정지). GPS는 아직 소비자가 없는 미사용 주변장치라 부팅을 막을 이유가 없다는 지적은 타당하나, 이는 **CubeMX 생성 코드 규약**이라 손대면 재생성 시 되돌아간다. 현 상태 유지하고 이슈로만 기록 — GPS 센서 확정 후 USER CODE 블록으로 옮길 때 함께 정리.
  - 검증: PHASE 게이트 전부 0 / 전부 1 양쪽 빌드 경고 0. 호스트 테스트는 이 PC에 네이티브 gcc가 없어(README에 기록된 알려진 조건) 문법 검사만 수행 — 실제 실행은 CI(`.github/workflows/ci.yml`)가 담당.
- **2차 코드 리뷰 후속 수정 3건 (2026-08)**: 1차 수정본을 다시 점검해 발견. **1·2번은 1차 수정이 만든 새 회귀**다.
  1. **`can_ok`가 1로 서지 않아 LED_BOARD_CHECK가 영영 안 켜짐 (Req 1.3 회귀)** (`can_receiver.c`) — 1차에서 CAN 회복에 `can_err_seen` 홀드오프 가드를 넣었는데, `SystemState={0}`이라 `can_ok`는 0으로 시작하고 `CAN_Receiver_Init()`은 이를 세우지 않았다. 결과적으로 **오류가 한 번도 없던 정상 보드에서는 회복 경로가 아예 실행되지 않아 `can_ok`가 영원히 0** → 건강 LED가 절대 안 켜지고, 역설적으로 "CAN이 한 번 죽었다가 3초 살아나야" 비로소 켜지는 상태였다. → `CAN_Receiver_Init()` 성공 말미에 `SystemState.can_ok = 1` 추가(낙관적 초기화). `mcp_ok`(ExtCAN_Init)·`sd_ok`(SD_Logger_Init)와 동일 패턴.
  2. **RX측 DMA 에러가 UART 스레드를 1초 스톨** (`uart_transmitter.c`) — 1차 수정에서 `gState == HAL_UART_STATE_BUSY_TX`로 TX 여부를 판별했으나, **HAL `UART_DMAError()`는 에러 콜백을 부르기 전에 `UART_EndTxTransfer()`로 gState를 READY로 되돌린다** → 이 조건은 **죽은 코드**. 게다가 HAL은 **RX 채널 에러여도 진행 중이던 TX를 함께 종료**시키므로, `hdmatx->ErrorCode`가 NONE이라 세마포어를 안 풀면 스레드가 `UART_DMA_DONE_TIMEOUT`(1초)만큼 통째로 멈췄다(수정 전 코드는 즉시 해제됐음 — 순수 회귀). → HAL 내부 상태에 의존하지 않는 **`uart_tx_in_flight` 플래그를 직접 관리**(송신 시작 직전 1, TxCplt·대기 종료·시작 실패 시 0)하고, 송신 중이었으면 **원인이 TX든 RX든 무조건 대기 해제**. LED/`uart_ok` 하향은 깨어난 스레드의 오류 경로가 담당. LOG value[4]=TX채널 결함 여부, value[5]=송신중 여부로 진단 정보 보강.
  3. **큐 Full로 드롭된 패킷의 sticky 비트까지 클리어** (`timer_scheduler.c`) — `Sched_Send`가 `TX_QUEUE_FULL`을 삼키는데 클리어는 무조건 수행돼, 큐가 찬 틱에서는 **5초치 고장 비트 합집합이 그대로 소실**됐다(sticky 도입 목적 자체가 무효화). → `Sched_Send`/`Sched_EmitBoth`가 **"요청한 큐에 전부 들어갔는지"를 반환**하도록 하고, **성공했을 때만** 누적을 리셋. 실패 시 누적을 유지해 다음 주기에 재시도(중복 보고는 "최근 N초 내 발생" 의미상 무해).
  - 2차 리뷰가 **정상 확인**해준 것: CRC-8 테이블 참조 구현 일치(신규 호스트 테스트가 고정) / 큐 사이징 일관성 / BMS 0x040~0x07F 블록 산술·MPPT 4유닛 디스패치·LV CMU 인덱싱·필터 0x34F 확장 모두 경계 문제 없음 / PD6·PD8·PB15 GPIO 클럭 활성 및 `MX_USART1_UART_Init`이 `MX_GPIO_Init` 뒤에 위치 / PB6·PB7 충돌 없음 / 스케줄러 위상 실제 무충돌 및 스텝별 패킷 수가 주석과 일치 / `build.ps1 -Release`가 makefile 규약과 일치 / Motor 0x40B·0x40E 바이트 오더가 SSOT와 일치.
- **코드 리뷰 후속 수정 5건 (2026-08)**: 위 LED 재설계·주기 감속 직후 전체 점검에서 발견. 1~3번은 LED 재설계가 넣은 회귀, 4번은 감속의 부작용, 5번은 기존 오해.
  1. **LED_CAN 회복이 오류 표시를 즉시 지움** (`can_receiver.c`) — 회복 로직을 RX 드레인 루프 **안**에 넣었더니, ⚠ **RX FIFO 오버플로우·프로토콜 오류는 정확히 "프레임이 계속 쏟아지는" 상황에서 발생**하므로 오류로 켠 LED를 같은 루프의 다음 프레임이 마이크로초 안에 꺼버렸다(`SystemState.error`는 1인데 LED만 정상인 모순). → 회복 판정을 **루프 밖으로 이동(콜백당 1회)** 하고 `CAN_ERR_RECOVERY_HOLD_MS`(3초) 홀드오프 도입: 마지막 오류 후 3초간 무오류 수신이 지속돼야 소등. 모든 오류 지점(FIFO Full/메시지 유실/프로토콜/Bus-Off)에서 `CAN_MarkError()`로 기준 시각 갱신.
     - 같이 정정: **메시지 유실**이 FIFO Full과 같은 `ERR_CAN_RX_OVERFLOW`인데도 LED를 안 켜던 불일치 해소(유실은 "곧 넘칠 것"이 아니라 "실제로 잃었다"라 더 심각).
  2. **`mcp_ok`가 초기화 실패를 가림** (`ext_can_controller.c`) — `MCP_SPITransfer` 회복 경로가 **`ExtCAN_Init()` 내부의 첫 SPI 전송에서 이미 발동**해, 이후 ConfigureFIFO/Filter/StartCAN20이 실패해도 `mcp_ok`가 1로 남아 MCP2518FD가 죽은 채 LED_BOARD_CHECK가 "정상"을 표시했다. → 회복 경로에 **`mcp_initialized` 가드** 추가. 초기화 성공 시의 `mcp_ok=1`은 `ExtCAN_Init()` 말미가 전담(+ `SystemState_RefreshBoardLed()` 호출 추가).
  3. **`SystemState_RefreshBoardLed()` 경합** (`main.c`) — ISR·스레드 양쪽에서 호출되는데 "플래그 읽기 → GPIO 쓰기"가 비원자적이라, 스레드가 낡은 값으로 ISR의 최신 결과를 덮어쓸 수 있었다. 호출부가 전부 `if (!flag)` 가드라 **재보정도 영영 일어나지 않아** LED가 잘못된 상태로 고착. → 계산+기록을 **PRIMASK 저장/복원 임계구역**으로 보호(ISR·스레드 양쪽 안전, 상태 전이 시점에만 호출되므로 차단 구간 무해).
  4. **순간 고장 플래그 유실 → sticky OR 채택** (`can_receiver.c` + `timer_scheduler.c`) — Motor 0x401 Limit/Error와 MPPT Error_Flags가 DETAIL(5초)로 밀리면서, 잠깐 떴다 사라지는 비트가 샘플링 전에 덮어써져 **한 번도 관측되지 않았다**. (감속 논의 때 "Fault는 래치되니 괜찮다"고 했으나 그건 **BMS Fault에만** 해당 — Motor/MPPT는 기기 측 래치 보장 없음.) → CAN RX에서 **OR로 누적**하고 스케줄러가 패킷에 실은 직후 클리어. **값의 의미가 "현재 떠 있는 비트" → "직전 주기 동안 한 번이라도 떴던 비트의 합집합"으로 바뀌므로 지상국 표기도 "최근 5초 내 발생"으로 수정 필요**(can-signal-summary.md §5에 명시).
     - 임계구역 불필요: 누적은 FDCAN1 ISR, 클리어는 TIM5 ISR인데 **두 IRQ의 preemption priority가 같아 서로 선점하지 못한다**(main.c 부팅 시 불변식 검증). MCP 경로는 이 필드를 쓰지 않음(확인 완료).
     - Active_Motor·TX/RX ErrCnt·MPPT Mode는 상태·카운터값이라 최신값 덮어쓰기 유지.
  5. **RX DMA 에러가 정상 송신을 중단** (`uart_transmitter.c`) — `HAL_UART_ERROR_DMA`가 TX 전용 비트라고 주석에 단정했으나 **HAL은 RX DMA 에러에도 같은 비트를 세운다.** 그래서 수신측 결함이 진행 중이던 정상 송신을 중단시키고 패킷을 버렸다. → `huart->hdmatx->ErrorCode`와 `gState == HAL_UART_STATE_BUSY_TX`로 **방향을 실제 판별**해 TX측 결함일 때만 송신 중단 처리. RX측이면 LOG만 남기고 송신을 건드리지 않는다(재무장은 기존 PHASE45 블록 담당). LOG value[4]에 방향 플래그를 실어 지상국에서도 구분 가능.
  - 리뷰가 **정상 확인**해준 것: CRC-8 테이블이 기존 비트연산 구현과 256엔트리 전수 일치 / BMS(HV) DETAIL·SD_ONLY 상대 오프셋이 0x040~0x07F를 빈틈·중복 없이 타일링 / 스케줄러 위상(0·2,7·5·4·3 mod 10)이 실제로 충돌 없음 / 필터0이 MPPT4(0x340~0x346) 커버 / 이동한 LED 핀의 GPIO 클럭·핀 충돌 없음 / 큐 메시지 8→4 ULONG 산술이 총 바이트 보존하며 깊이 2배 / 제거된 심볼 잔여 참조 없음.
- **발행 주기 감속 — PRIMARY 200ms→1s, DETAIL 1s→5s (2026-08)**: 사용자 판단("데이터 내용 보면 이렇게 빠르게 받을 필요가 없다")에서 출발해 신호별 실제 변화 시간상수를 재검토한 결과, 기존 주기가 과했음을 확인하고 감속.
  - **근거(시간상수)**: 모터 RPM·차속은 차체 관성(200~300kg, 순항 1~2kW)으로 **초 단위**로만 변함 / 버스 전류·셀전압 강하(V=I×R)는 스로틀·램프 속도인 **100ms~1s** / 가장 빠른 축인 MPPT 입력(구름 그림자가 어레이 통과)도 **약 170ms** / 온도류 수십 초~분 / Fault·Alarm은 BMS 래치라 폴링 시점 무관. **5Hz가 필요한 신호는 하나도 없었다.**
  - **결정적 근거**: 누적 에너지(Odometer, DC_Bus_AmpHours, 쿨롱카운터, 규정 누적값)는 **기기가 자체 적분해 보내므로 샘플링 주기와 무관하게 정확**하다. 우리가 전류를 적분하지 않으므로 순간값 샘플링을 늦춰도 에너지 전략 정확도가 안 깎인다.
  - **실익은 대역폭 절감보다 링크 마진**: 170 pkt/s는 64kbps·듀티 50% **이론값**이고 아웃백 장거리·이동 중에는 재전송·페이딩으로 실효 처리량이 크게 떨어진다. 78% → **16%**(133 → 27 pkt/s)로 낮춰 통신 거리·유실 내성 확보. SD 기록량도 2.9 → 0.6 KB/s(8시간 84MB → 17MB).
  - **위상 분리로 피크 30 → 18**: 주기가 느려지면서 각 작업을 **서로 다른 mod 10 잔여류**에 배치할 여유가 생겼다 — PRIMARY `tick%10==0`, DETAIL 그룹 `tick%5==2`(≡2·7 mod 10), 타임아웃 `tick%10==5`, 규정 `tick%100==4`, IWDG `tick%20==13`(≡3). **어떤 틱도 두 작업을 동시에 수행하지 않으므로** 피크 = 가장 큰 단일 작업 = PRIMARY 18패킷. UART 깊이 40 대비 여유 1.33× → **2.2×**. 이제 MPPT를 더 늘려도 DETAIL(step 6)이 아니라 PRIMARY가 유닛당 +1로 자란다.
  - ⚠ **타임아웃 검사를 DETAIL 스텝에서 분리**(`Sched_CheckTimeouts()`를 step 5에서 떼어내 자체 `tick%10==5` 분주로). 안 그러면 DETAIL이 5초 주기가 되면서 CAN 두절 검출 지연이 최대 6초(임계값 1s + DETAIL 1주기 5s)로 늘어난다. 분리로 종전(최대 ~2초) 유지.
  - 주기·위상을 전부 `SCHED_*_TICKS` / `SCHED_PHASE_*` 상수로 추출 — **Phase 4 보드 검증 중에는 일시적으로 줄여 관측을 촘촘히 할 수 있다**(예: `SCHED_PRIMARY_TICKS 2` = 200ms). 검증 후 원복 필요.
  - 검증: awk 시뮬레이션(1000틱=100초)으로 **작업 겹침 0회**, UART/SD 최대 유입 18, PRIMARY 1.0s·DETAIL 그룹 5.0s·타임아웃 1.0s·규정 10.0s 주기, DETAIL 합계 EmitBoth 43/EmitSdOnly 50 보존 확인. 빌드는 PHASE 게이트 전부 0 / 전부 1 양쪽 경고 0.
  - 검토했으나 **채택하지 않은 안**: "무선만 감속하고 SD는 기존 해상도 유지"(옵션 B). 포렌식 근거로 들었던 CUV/COV 스냅샷·Safety/PF·CB_Status는 **BMS가 이벤트 시점에 캡처해둔 상태 레지스터**라 5초에 회수해도 내용이 동일해, 코드 복잡도를 늘릴 만한 실익이 없다고 판단.
- **LV BMS 제거 (2026-08, 팀 결정)**: 저전압 보조배터리 BMS(Prohelion D1000 Gen1, base 0x600)를 차량 구성에서 제외 → 수신·파싱·발행·필터를 전부 삭제.
  - `types.h`: `BmsLvData_t`·`BMS_LV_KEY`·`LV_MAX_CMU`/`LV_MAX_CELLS`·`VehicleData_t.bms_lv` 삭제. **`SRC_BMS_LV`(값 8)는 지우지 않고 `SRC_RESERVED3`로 남겼다** — 지우면 `SRC_CAN_ERR`(11)~`SRC_SD_ERR`(14)가 전부 당겨져 지상국 디코더와 무성(無聲) 불일치가 난다.
  - `can_receiver.c`: LV ID 정의·`CAN_StoreBmsLv()`·디스패치 삭제. **FDCAN1 필터1 `0x6FF → 0x4FF` 축소**(Motor 최대 ID 0x417) — LV 대역이 하드웨어에서 차단된다.
  - `timer_scheduler.c`: PRIMARY LV 2패킷·`Sched_EmitLvA/B` 삭제, 타임아웃 감시 9→8소스. **DETAIL step 8·9는 비워두되 `SCHED_DETAIL_STEPS`는 10 유지** — 8로 줄이면 DETAIL 주기가 5s→4s로 조용히 바뀐다.
  - 패킷 수: PRIMARY **18→16**, DETAIL **43→33**, SD_ONLY **50→48**. ISR 피크 18→16.
  - 지상국(ArrowPoint-Telemetry): `LogSource.SRC_RESERVED3`, 디코더 `applyBmsLv` 제거, `VehicleSnapshot.BmsLvUnit` 제거, 시뮬레이터 LV emit 제거, `bms-lv.html`·라우팅·메뉴·`renderBmsLv` 제거, 메인 대시보드 LV 타일 제거.
  - 빌드: 게이트 전부 0 / 전부 1 양쪽 경고 0. 지상국 `mvn compile` 통과.
- **MPPT 3대 → 4대 증설 (2026-08)**: MPPT #4를 로터리 스위치 4(base **0x340**, 0x340~0x346)로 추가. **기존 #1~#3의 CAN ID(0x310/0x320/0x330)와 BMS·Motor·BMS_LV의 모든 CAN ID는 변경 없음.**
  - **FDCAN1 필터0 상한 `0x33F → 0x34F` 확장** (`can_receiver.c` `CAN_FILTER0_ID2`) — ⚠ 이걸 빠뜨리면 MPPT4 프레임이 하드웨어 필터에서 전량 거부되어 SW까지 도달조차 못 한다. 필터는 수신 보드 내부 설정이라 타 보드 영향 없음.
  - **`LOG_SOURCE`에 `SRC_MPPT4`를 `SRC_MPPT3` 바로 뒤(=6)에 삽입** → 사용자 결정(2026-08, MPPT를 연속 배치). 그 결과 **이후 소스 값이 전부 +1 시프트**: `SRC_MOTOR 6→7`, `SRC_BMS_LV 7→8`, `SRC_RESERVED1/2 8·9→9·10`, `SRC_CAN_ERR 10→11`, `SRC_SPI_ERR 11→12`, `SRC_UART_ERR 12→13`, `SRC_SD_ERR 13→14`. (`SRC_SYS`~`SRC_MPPT3` 0~5는 불변)
    - 🚨 **이 enum 값 = LOG 패킷 `source` 바이트(전송 포맷)** → **지상국 디코더의 source 매핑 테이블을 반드시 함께 갱신해야 한다.** 갱신하지 않으면 Motor·BMS_LV·모든 에러 패킷을 조용히 오독한다(빌드는 통과하므로 드러나지 않음).
    - 대안이었던 "빈 `SRC_RESERVED1`(=8) 슬롯 재사용"(기존 값 전부 보존, 단 MPPT가 3·4·5·8로 비연속)은 채택하지 않음.
  - `types.h`에 **`MPPT_UNIT_COUNT`(=4)** 신설 — `MpptData_t mppt[MPPT_UNIT_COUNT]`와 `timer_scheduler.c`의 MPPT 루프 3곳이 이 상수를 참조하므로, 다음 증설 시 상수 + CAN base define + 디스패치 분기만 손대면 된다.
  - 타임아웃 감시 대상 `SCHED_TIMEOUT_SOURCES` 8→9 (MPPT4 워치독 추가).
  - **예산 재계산**: PRIMARY 17→18, DETAIL 40→43, SD_ONLY 49→50. 무선 125→**133 pkt/s(74%→78%)**, ISR 피크 26→**30** vs UART 깊이 40(여유 1.54×→**1.33×**). 피크 스텝은 **step 6(MPPT DETAIL)** 로 고정 — MPPT를 더 늘리면 이 스텝이 먼저 한계에 닿아 **7대(42 > 40)부터 초과**하므로, 그 전에 MPPT DETAIL을 두 스텝으로 분할해야 한다.
  - 패킷 번호 재부여: MPPT4가 PRIMARY **#10**으로 들어가며 Motor #10~#13→**#11~#14**, BMS_LV #14~#15→**#15~#16**, BMS SOC #16~#17→**#17~#18**.
  - 빌드 검증: PHASE 게이트 전부 0 / 전부 1 양쪽에서 `Core/Src`·`Core/Inc` 경고 0 확인 후 게이트 원복.
  - ❌ **미확인 잔여**: 실제 MPPT #4 유닛의 **로터리 스위치를 4로 설정했는지 현장 확인 필요** (0이면 base=general_id 0x300이라 다른 대역과 충돌 가능). `can-signal-summary.md` §9 체크리스트에 등록됨.
- **LED 역할 구체화 — 실시간 상태 표시로 재설계 (2026-08)**: 기존 LED는 "오류 시 점등"만 하고 자동 회복 경로가 없어(일부는 리셋 전까지 영구 래치, 일부는 매 이벤트 토글) 육안으로 "지금 문제가 있는지"를 바로 판별할 수 없었다. `SystemState_RefreshBoardLed()`(`main.h`/`main.c`)를 신설해 아래처럼 재설계:
  - **LED_BOARD_CHECK(PE12)**: "상시 점등"에서 "현재 활성화된 서브시스템 전부 정상이면 점등, 하나라도 비정상이면 소등"으로 변경. can_ok/mcp_ok는 항상, uart_ok/sd_ok는 각각 `PHASE4_UART_PIPELINE`/`PHASE5_SD_LOGGING` 게이트가 켜져 있을 때만 평가(꺼진 파이프라인 때문에 오판하지 않도록). can_ok/mcp_ok/uart_ok/sd_ok가 바뀌는 모든 지점(`can_receiver.c`, `sd_logger.c`, `uart_transmitter.c`, `ext_can_controller.c`, `timer_scheduler.c`)에서 재호출.
  - **LED_SD(PD8)/LED_CAN(PB15)**: 오류 시 점등은 유지하되, 다음 정상 이벤트(SD 기록 성공/CAN 메시지 수신 성공)에서 자동 소등하도록 회복 경로 추가. 큐 Full 경고(`ERR_UART_QUEUE_FULL`/`ERR_SD_QUEUE_FULL`, `timer_scheduler.c`)도 각각 `uart_ok`/`sd_ok`를 0으로 내려 동일한 회복 경로를 타게 함(그렇지 않으면 큐 Full 시의 점등이 다음 성공 이벤트에서도 지워지지 않고 영구 래치로 남는 문제가 있었음).
  - **LED_RFD900x(PE15)**: 매 전송마다 짧게 점등(토글)하던 방식을 폐기하고 "송신 실패=점등, 정상 송신=소등"으로 변경.
  - **LED_ERR(PE11)**: 변경 없음 — 여러 치명적 오류가 공유하는 상시 래치 유지(개별 회복으로 지울 수 없음이 원래 의도). MCP/SPI 오류(`ext_can_controller.c`)의 `mcp_ok`는 SPI 전송 성공 시 실시간 회복되어 LED_BOARD_CHECK 집계에는 반영되지만, LED_ERR 자체는 건드리지 않음(SPI 핫패스이므로 `if (!mcp_ok)` 가드로 불필요한 재기록 방지).
  - 부팅 시 CAN/MCP 초기화 직후 `SystemState_RefreshBoardLed()` 1회 호출로 초기 상태 즉시 반영. 빌드 검증: `PHASE25_CAN_TX`/`PHASE4_UART_PIPELINE`/`PHASE45_UART_RX_CAN_TX`/`PHASE5_SD_LOGGING`/`PHASE6_SAFETY` 전부 1로 켠 상태와 전부 0인 기본 상태 양쪽에서 `Core/Src` 경고 0 확인 후 게이트 원복.
  - requirements.md Req 1.3, 3.4, 7.6, 8.6, 12.2, 12.3 갱신(번호 유지, 문구만 정정 — task 12.x 등 타 문서의 "요구사항 12.x" 참조 안 깨짐).

## 알려진 이슈 / 유보 작업 (후속 Phase에서 처리)

- [x] **ISR 내 블로킹 SPI → deferred 처리** (요구사항 9.7) — **해결됨**: EXTI→세마포어→`MCP_RX_Thread`에서 SPI 수신. `HAL_GetTick()` timeout 정지 함정 제거. (위 "확정된 설계 결정" 참조)
- [x] **MCP 드레인 stuck / SPI 에러 LOG 폭주** (리뷰 #3, #5) — **해결됨**: 유한 타임아웃 폴백 폴링 + 에러 LOG 레이트리밋. (위 "확정된 설계 결정" 참조)
- [x] **SPI1 동시성 (RX 드레인 ↔ CAN TX)** (리뷰 #1) — **해결됨**: task 6A.2에서 `mcp_spi_mutex`로 상위 op 직렬화 구현 완료(`PHASE25_CAN_TX` 게이트). (위 "확정된 설계 결정" 참조)
- [ ] **초기화 순서** (리뷰 #2): `CAN_Receiver_Init()`/`ExtCAN_Init()`이 `MX_ThreadX_Init()` 이전(USER CODE BEGIN 2)에서 인터럽트를 활성화하나 UART_Queue/SD_Queue는 아직 미생성. 부팅 초기 로그가 드롭될 수 있음. → **구체안**: `CAN_Receiver_Init()`을 필터 설정만 하는 부분과 알림 활성화+`HAL_FDCAN_Start()`를 하는 `CAN_Receiver_Start()`로 분할하고, Phase 4(task 12)에서 큐 생성 **이후** Start를 호출. (현재는 ThreadX queue-id 검사로 하드폴트는 없음)
- [ ] **CAN→VehicleData 매핑 중복** — **해소됨(부분)**: task 1.4에서 `ext_can_controller.c`의 중복 switch-case를 stub 처리(MCP는 향후 저우선순위 주변장치 버스로 예약, 차량 센서 RX는 FDCAN1 전담). `can_receiver.c`만 실 매핑 보유. 공용 헬퍼 통합은 미래 센서 RX 확정 시 재검토.
- [ ] **IWDG 부팅부터 활성 (~4초) — Phase 2~5 보드 테스트 중 리셋 루프** (시스템 점검 발견): `MX_IWDG_Init()`이 main에서 IWDG를 즉시 시작(LSI 32kHz, Prescaler=64, Reload=2000 → ~4s)하나 리프레시는 task 18.3(Phase 6)에서야 추가됨. → Phase 2~5 체크포인트(task 6/13/17) 실기 테스트 시 보드가 ~4초마다 리셋. **대응 옵션**: (a) Phase 6 전까지 CubeMX GUI에서 IWDG 임시 비활성, 또는 (b) 임시 `HAL_IWDG_Refresh()`를 어느 스레드/루프에 추가. task 18.3에서 Alive Flag 방식으로 최종 전환하며 임시 코드 제거.
- [x] **SD 실패 시 부팅 데드락** (시스템 점검 발견) — **해결됨**: `FileX/App/app_filex.c` `fx_app_thread_entry`의 `fx_media_open` 실패 분기 `while(1);`을 ERR_SD_INIT LOG + LED_SD + 로깅 비활성(`return`)으로 교체 완료(`PHASE5_SD_LOGGING` 게이트, task 16.2).
- [ ] **`HAL_FDCAN_ErrorCallback`의 ERR_CAN_BUS_OFF**: 일반 에러에도 BUS_OFF로 보고(의미 중복). 현재 활성 알림 구성에선 호출되지 않는 방어 코드라 보류.

- [ ] **GPS UART(USART1) 실사용 설정 — GPS 센서 기종 선정 대기** (2026-08 회로도 대조로 발견·활성화): 하드웨어(PB6/PB7, J11 커넥터)는 배선되어 있고 `MX_USART1_UART_Init()`도 활성화했으나, 보레이트·프로토콜(NMEA vs UBX 등)·파싱 로직은 전부 미정. GPS 센서 기종이 정해지면 `usart.c`의 `huart1.Init.BaudRate` 등 재설정 + 수신 파이프라인(파서) 신규 구현 필요.
- [ ] **`MX_USART1_UART_Init` 실패가 전 시스템을 정지시킴** (2026-08 3차 리뷰 지적, 판단 보류): GPS UART는 아직 보레이트도 임시값이고 수신·파싱 로직도 없는 **미사용 주변장치**인데, 초기화 실패 시 `Error_Handler()` 무한루프로 CAN 수신·무선 송신·SD 로깅이 전부 멈춘다. 부가 기능이 본체를 막는 구조는 옳지 않으나, 해당 코드는 **CubeMX 생성 규약**이라 지금 고치면 `.ioc` 재생성 시 되돌아간다. → GPS 센서 확정 후 초기화를 USER CODE 블록으로 옮길 때 "실패해도 로그만 남기고 계속"으로 함께 정리할 것.
- [x] **Power_check 핀 PB1 → PD6 재배정 완료** (2026-08): 회로도가 갱신되어 **PD6(pin 87)이 `3.3V` 넷에 직결**되고 **PB1(pin 33)은 미접속**으로 바뀐 것을 넷리스트(`kicad-cli sch export netlist`)로 실측 확인 → `main.h`(`Power_check_Pin/GPIO_Port`), `.ioc`(`Mcu.Pin9`, `PD6.*` 항목), 관련 주석·문서 전부 갱신. `gpio.c`는 매크로를 참조하므로 자동 반영되며 GPIOD 클럭은 이미 활성(LED_SD PD8·SD_DETECT PD0 때문). 극성 가정(정상=HIGH)은 그대로 유효.
  - ⚠ **잔존 트레이드오프**: 감지 레일이 MCU 자신의 구동 레일과 같으므로, 전원 차단 시 조기경보 여유는 그 레일의 벌크 캐패시턴스 방전 속도에 좌우됨 — 여유가 거의 없을 수 있다는 점은 배선이 바뀌어도 그대로다(위 "확정된 설계 결정" 참조).
- [ ] **PB3/PB4/PB5 예비 GPIO** (2026-08 회로도 대조로 발견): R43/R28/R29에 연결된 라벨 없는 net. 용도 미정, 필요 시 추후 활성화 예정(현재 조치 없음).

- [ ] **패킷 디스크립터 테이블 리팩터** (2026-07-31 전체 시스템 리뷰 제안, 예상 1~2일 — **Phase 4 보드 검증 후 / 다음 CAN 스펙 개정 전**에 착수 권장)
  - **문제**: 신호 하나를 옮기려면 지금도 세 곳을 동시에 고쳐야 한다 — `can_receiver.c`(파싱·저장) + `types.h`(구조체 필드·KEY enum) + `timer_scheduler.c`(수동 패킹·Emit 호출). `timer_scheduler.c`에는 손으로 쓴 `memcpy(&v[...])`가 44개, `memset(v,0,8)`이 13개 있다. 최근 세 번의 CAN ID 개정(task 3.5/3.6/3.7)에서 주석·문서 노후화가 반복적으로 발생한 **구조적 근본 원인**이며, 매번 grep 스윕으로 사후 수습해 왔다.
  - **제안 방향**: `{src, key, tier, offsetof(VehicleData_t, ...), layout}` 형태의 `const` 디스크립터 테이블을 두고, `Sched_Emit*`를 tier로 필터링하는 루프 하나로 축소한다.
    ```c
    typedef enum { TIER_PRIMARY, TIER_DETAIL, TIER_SD_ONLY } PktTier_t;
    typedef enum { LAY_U16x4, LAY_I16x4, LAY_U32x2, LAY_F32x2, LAY_RAW8 } PktLayout_t;
    typedef struct { LOG_SOURCE src; uint8_t key; PktTier_t tier;
                     uint16_t offset; PktLayout_t layout; } PacketDesc_t;
    ```
  - **기대 효과**: ① 신호 추가 = 테이블 한 줄 ② 테이블이 곧 명세라 `.md` 문서 대조를 스크립트로 자동화 가능(노후화 문제 구조적 해소) ③ 키 중복·offset 범위 등 무결성을 `tests/host`에서 검증 가능 ④ 스텝 분산이 `for (i = step; i < N; i += 10)`으로 자명해짐.
  - **주의**: BMS(LV) `PRECHARGE`/`MINMAX_V`/`MINMAX_T`처럼 u8/u16이 섞인 불규칙 패킷 6개가량은 테이블로 표현하면 오히려 가독성이 나빠진다. `LAY_RAW8` + 함수 포인터 escape hatch로 남길 것.
  - **선행 작업 완료**: task 3.8의 `Sched_EmitDetail()` 그룹 함수 분해가 이 리팩터의 자연스러운 발판이다.

- [ ] **SD_ONLY 유실 우선순위 역전** (2026-07-31 시스템 리뷰 발견, 현재 부하에서는 해소됨 — 감시만)
  - CUV/COV·CB 밸런싱 비트맵·Safety/PF 상세는 **사고 후 가장 필요한 진단 데이터**인데, 발행 순서상 버스트 후미에 몰려 있어 큐가 차면 가장 먼저 버려진다(가치 대비 우선순위 역전).
  - task 3.8의 분산으로 현재 신호 구성에서는 드롭이 발생하지 않으므로 별도 조치 없이 둔다. 다만 **신호가 더 늘어 스텝별 발행량이 큐 깊이에 근접하면**, SD_ONLY를 `SD_Queue` 대신 전용 링버퍼로 우회시키는 안(큐 경합에서 분리)을 검토할 것.
  - 판단 기준: `can-signal-summary.md` §8.1의 스텝별 유입량 표 참조.

- [x] **릴리스 빌드 경로 부재 → `-Release` 스위치 추가** (2026-07-31 3차 점검 발견·수정)
  - `build.ps1`이 `DEBUG=1`을 하드코딩해 **항상 `-Og`(디버깅용 최적화 억제)로만 빌드**되고 있었다. 그대로 대회에 나가면 디버그 최적화 코드로 주행하게 된다.
  - `-Og`는 ISR 체류 시간을 늘리는데, FDCAN1과 TIM5가 같은 NVIC 우선순위라 서로 선점하지 못하고 STM32U5의 CAN RX FIFO0는 3칸 고정이므로 **ISR 시간이 곧 수신 유실 마진**이다. 즉 최적화 레벨이 기능 안정성에 직결된다.
  - `build.ps1 -Release`(= `make DEBUG=0 OPTIMIZATION=-O2`, 산출물 `build/release/`) 추가. 실측 결과 **ROM이 오히려 감소**(67,312B → 64,336B), 프로젝트 코드 경고 0. `-Flash`도 빌드 종류에 맞는 elf를 굽도록 연동.
  - ⚠ 실기 배포 전 **`-Release` 빌드로 Phase 체크포인트를 재검증**할 것. 최적화 레벨이 바뀌면 타이밍이 달라져 `volatile` 누락 같은 잠복 결함이 드러날 수 있다(현재 공유 변수 `alive_flags`/`g_power_down`/`rx_head`/`rx_tail`/`uart_tx_dma_error`는 전부 `volatile` 확인함. `VehicleData`는 비-volatile이지만 FDCAN·TIM5 ISR이 상호 선점 불가라 ISR 1회 실행 중 값이 바뀔 수 없어 안전).

- [x] **MCP2518FD 수신 필터가 모든 ID를 통과시킴** — **2026-08-24 해결** (Bus B ID 맵 확정으로 0x510~0x51F만 통과하도록 축소, 아래 해당 항목 참조). 아래는 발견 당시 기록:
  - `ext_can_controller.c` `ExtCAN_Init()`의 `mcp_filter0.AcceptanceID = 0x000` / `AcceptanceMask = 0x000`. 마스크 0은 "전 비트 don't care" = **표준 ID 전부 수락**이다(주석의 `[미정]` 그대로 남은 상태).
  - 현재는 Bus B에 트래픽이 없고 `ExtCAN_ProcessMessage()`가 stub이라 드레인 후 폐기되므로 무해하다. 다만 **LCD 보드나 센서가 Bus B에 붙는 순간**, MCP_RX_Thread(pri 6)가 모든 프레임마다 깨어나 블로킹 SPI 드레인을 수행하게 된다(불필요한 SPI 점유 + 스레드 스케줄링 부하).
  - → Bus B의 실제 RX 대상 ID가 확정되면 `AcceptanceID`/`AcceptanceMask`를 그 대역으로 좁힐 것. 대상이 끝까지 없다면 필터를 비활성(`EnableFilter = false`)하는 편이 낫다.

- [x] **호스트 테스트 실행 환경 부재** — **2026-08-24 해결** (MinGW-w64 설치 후 전체 실행, ALL TESTS PASSED. 아래 해당 항목 참조). 아래는 발견 당시 기록: (2026-07-31 확인): `tests/host/`는 네이티브 gcc를 요구하는데 현재 개발 PC에는 ARM 툴체인(`arm-none-eabi-gcc`)만 있고 네이티브 gcc가 없어 **실행이 불가**하다. task 3.8에서 추가한 `test_crc8_table_equivalence()` 포함 전체 테스트가 미실행 상태. → MinGW-w64 설치 또는 CI에서 실행되도록 배선 필요. (task 3.8의 crc8 등가성 자체는 PowerShell로 동일 검증을 수행해 확인함)
- [x] **BMS DETAIL(상세온도·쿨롱카운터)·MPPT aux·Motor DETAIL/SD_ONLY 미구현** (전 세션 발견) — **해결됨**: task 3.2에서 xlsx 기준 정밀 파싱+송출 구현 완료. (위 task 3.2 참조)
- [x] **BMS 보드 FDCAN 비트타이밍 불일치 가능성 (크로스팀)** — **해결됨(2026-07)**: `CAN DATA/BMS_CAN_ID_명세.xlsx` 개요 시트의 BMS 보드(STM32U545) 자체 경고(1.667Mbit/s 오계산, Classic CAN 상한 초과 가능성)를 타 팀과 확인, 통신 속도·설정 전부 합의 완료(500kbps 통일). Phase 2 보드 실기 검증(task 6) 시 실측으로 최종 확인 권장.

- [ ] **SD 쓰기 버스트 완화 — `SD_BUF_SIZE` 512 → 2048 검토 (실측 후 반영)** (2026-08-21 소비전력 산출 중 도출)
  - 현재 `sd_logger.c`는 512 B(= LOG 32개) 단위로 `fx_file_write()`를 호출한다. SD 큐 유입이 약 106 pkt/s이므로 **약 300 ms마다 1섹터 쓰기**가 발생한다. 데이터량 자체는 1.7 kB/s(≈146 MB/일)로 미미하다.
  - 문제는 양이 아니라 패턴이다. **512 B 잦은 쓰기는 write amplification이 가장 나쁜 패턴**이라, 카드 내부의 SLC→TLC folding(가비지 컬렉션)이 자주 유발된다. GC 구간은 수십~수백 ms로 길고 전류가 높아 3.3 V 벌크(공칭 74 µF, DC 바이어스 감쇠 후 실효 ≈50 µF)로 버틸 수 없다 — **3.3 V LDO(IC6 TLV70433, ICL 160 mA min)에 남은 유일한 실질 위험**이다. 일반 512 B 쓰기(1~3 ms)는 벌크로 충분히 넘어간다.
  - **제안**: `SD_BUF_SIZE`를 2048(4섹터 = LOG 128개)로 상향 → 쓰기 횟수 1/4(초당 3.3회 → 0.8회). `_Static_assert(SD_BUF_SIZE % sizeof(LOG) == 0)`는 그대로 성립(2048 = 16 × 128). 버퍼가 2개(`sd_buf_a`/`sd_buf_b`)라 **RAM은 1 KB → 4 KB (+3 KB)**.
  - **트레이드오프**: 전원 차단 시 유실 구간이 32패킷(≈0.3 s) → 128패킷(≈1.2 s)으로 늘어난다. PHASE6 긴급 flush(`g_power_down` 경로)가 이를 흡수할 수 있는지 먼저 확인할 것.
  - **반영 조건 (선-측정 후-적용)**: Phase 5 브링업에서 **3.3 V 라인에 션트를 넣어 SD 쓰기 피크와 GC 구간을 실측**한다. ⑴ 피크가 100 mA 이하이고 ⑵ GC로 인한 3.3 V 전압 강하가 관측되지 않으면 현행 512 B를 유지한다. 강하가 관측되면 2048로 올린 뒤 재측정한다.
  - 근거 자료: `C:\Users\user\Desktop\27WSC\WSC_송신보드_소비전력_산출.xlsx` — 레일별 전류·전력 산출, SD 제외(37.25 mA / 정격 23 %) vs 포함(137.25 mA / 86 %) 비교.

- [x] **RFD900x 송신 출력 = 20 dBm (0.1 W) — 2026-09-01 실물 모뎀에서 확인 완료** (설정: 출력 20, 듀티 사이클 50%) (2026-08-21 기록. 이전 RFD900x 테스트 세션에서 결정되었으나 어디에도 기록되지 않아 소비전력 재산출 중 유실이 확인됨)
  - **결정**: 기본값 30 dBm(1 W)이 아니라 **20 dBm(0.1 W)** 로 운용한다.
  - **근거(전력)**: `RFD900x DataSheet.pdf` Figure 4-2(Current vs TX Power Level) 곡선 직독 기준 송신 전류가 **1.2 A → 0.43 A** 로 떨어진다. 수신 대기는 출력과 무관하게 60 mA로 동일하다. 송신 듀티 30 %(텔레메트리 18 kbps ÷ 에어레이트 64 kbps) 기준 5 V_2 평균이 402 → 171 mA, **시스템 총 전력이 3.052 → 1.673 W (−45 %)**, 주행일 9 h 소비가 27.5 → 15.1 Wh가 된다. 5 V_2 벅(IC8 LMR38020, 2 A)의 피크 점유율도 60 % → 22 %로 내려간다.
  - **미반영 상태**: 이 값은 **모뎀 자체 설정(SiK `ATS4=20` / RFD 툴의 Tx Power)** 이라 펌웨어 소스에는 나타나지 않는다. 송수신 양쪽 모뎀 모두 동일하게 설정해야 하며, 설정 후 값을 여기에 기록할 것(현재 실제 모뎀에 반영되어 있는지 미확인).
  - **확인 필요**: 20 dBm에서 차량–지상국 간 실제 통신 거리·링크 마진이 대회 조건에서 충분한지. Phase 4 체크포인트(task 13)의 무선 전송 시험에 **거리별 RSSI/패킷 손실 측정**을 포함할 것. 부족하면 24 dBm(0.60 A, 총 2.001 W) 또는 27 dBm(0.79 A, 총 2.337 W)이 중간 선택지다.
  - 근거 자료: `C:\Users\user\Desktop\27WSC\WSC_송신보드_소비전력_산출.xlsx` C·E 섹션.

- [x] **HSE·LSE 크리스털 활성화 — 내부 RC(MSI/LSI) → 외부 크리스털 전환** (2026-08-21 소비전력 산출 중 발견·수정)
  - **발견**: 보드에 X1(CS07826-16M, 16MHz)·X2(MU00525-32.768K)가 실장되어 있고 `.ioc`의 핀 Mode도 각각 `HSE-External-Oscillator`/`LSE-External-Oscillator`로 잡혀 있었으나, **클럭 트리는 둘 다 쓰지 않고 있었다**. `main.c`가 `OscillatorType = LSI|MSI` / `PLLSource = RCC_PLLSOURCE_MSI`, `rtc.c`가 `RTCClockSelection = RCC_RTCCLKSOURCE_LSI`. CubeMX 신규 프로젝트 기본값이 그대로 남은 것으로 보인다.
  - **왜 문제인가 (CAN)**: FDCAN1 커널 클럭이 `RCC_FDCAN1CLKSOURCE_PLL1`이고 PLL1이 MSI에서 나오므로 **MSI 정확도 = CAN 비트레이트 정확도**였다. MSI range 0 공장 캘리브레이션 후 47.74/48/48.70 MHz(Table 82) = **−0.54% / +1.46%**(상온 초기값, 온도 드리프트 별도). 현재 비트타이밍(`fdcan.c` Prescaler 10, TSeg1 12, TSeg2 3, SJW 1 → NBT 16tq)의 허용 오차는 `SJW / (2×10×NBT)` = **±0.3125%** 로, **허용치의 4.7배 초과**였다. SJW를 3으로 올려도 한계가 0.94%라 부족하다. X1은 ±20ppm(온도 포함) + 에이징 ±3ppm/년 ≈ ±45ppm이라 여유가 약 70배가 된다.
  - **왜 문제인가 (RTC)**: LSI는 RC라 온도 드리프트가 커서 LOG 타임스탬프와 SD 로그 파일명(YYMMDD)이 하루 단위로 틀어진다. X2는 ±20ppm.
  - **수정 내역**
    - `main.c` `SystemClock_Config()`: `OscillatorType = HSE|LSI|LSE`, `HSEState = RCC_HSE_ON`, `LSEState = RCC_LSE_ON`, `PLLSource = RCC_PLLSOURCE_HSE`, `PLLM 3 → 1`(PLL 입력 16MHz 유지), `PLLMBOOST DIV4 → DIV2`(EPOD 부스터 입력 8MHz). 함수 선두에 `HAL_PWR_EnableBkUpAccess()` + `__HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_HIGH)` 추가.
    - `rtc.c`: `RTCClockSelection = RCC_RTCCLKSOURCE_LSE`, `SynchPrediv 249 → 255`(32768Hz 기준 1Hz).
    - `.ioc`: `RCC.PLLM=1`, `RCC.EPOD_VALUE=16000000`, `RCC.PLLSourceVirtual=RCC_PLLSOURCE_HSE`, `RCC.RTCClockSelection=RCC_RTCCLKSOURCE_LSE`, `RTC.SynchPrediv=255` + `RCC.IPParameters`에 두 항목 추가.
  - **하위 클럭 불변 확인**: MSI 48MHz/PLLM 3 = 16MHz, HSE 16MHz/PLLM 1 = 16MHz로 **PLL 입력이 동일**하다. 따라서 VCO 160 / PLLP·Q 80 / PLLR 160, SDMMC 80, FDCAN 80, SYSCLK 160 전부 그대로다. MSIK를 쓰는 주변장치는 없다(SDMMC=PLL1, SPI1=SYSCLK, USART1/2=PCLK). `Core/Src/tx_initialize_low_level.S`의 `SYSTEM_CLOCK = 160000000`도 유효(SysTick 리로드 159999 = 1ms, 디스어셈블리 리터럴 0x270FF로 확인).
  - **LSE 드라이브를 HIGH로 올린 이유**: X2의 ESR 최대 50kΩ(T1 등급) 기준 `gm_crit = 4·ESR·(2πf)²·(C0+CL)² ≈ 0.42 µA/V`. Table 80의 Gmcritmax는 LSEDRV=00에서 0.5 µA/V라 여유가 **1.2배**뿐이다. LSEDRV=11(2.7 µA/V)이면 **6.4배**로 AN2867 권장(≥5)을 만족한다. 추가 소모는 410nA → 700nA로 무시 가능하고, 크리스털 정격 드라이브가 0.5mW라 과구동 우려도 없다. ⚠ **CubeMX 기본값은 LOW이므로 재생성 시 되돌아간다 — 재생성 후 반드시 HIGH로 복구할 것.**
  - **로드 커패시터 적합성 확인**: X1 CL 8pF vs 보드 C22/C23 8.2pF → 실효 ≈7.6pF. X2 CL 6.0pF vs C16/C17 6.8pF + CS_PARA ≈3pF → 실효 ≈6.4pF. 둘 다 수십 ppm 오차로 CAN 요구치(3000ppm) 대비 무해.
  - **전류 영향**: 3.3V에 HSE +1.10mA(Table 79, 8MHz 910µA↔48MHz 1960µA 보간) + LSE +0.0007mA. 평시 19.42 → 20.52mA(ICL 160mA의 12.8%). 시스템 총 전력 1.673 → 1.679W.
  - ⚠ **브링업 주의 2건**: ⑴ HSE/LSE가 기동하지 않으면 `HAL_RCC_OscConfig()` 실패 → `Error_Handler()` 무한루프(LED_ERR 점등)로 부팅 불가. 내부 RC였을 때는 없던 실패 모드다. 필요 시 CSS(Clock Security System)로 폴백을 검토할 것. ⑵ RTC 클럭 소스 변경 시 HAL이 백업 도메인을 리셋하므로(`stm32u5xx_hal_rcc_ex.c`의 `__HAL_RCC_BACKUPRESET_FORCE/RELEASE`) **첫 부팅에서 RTC 시각과 백업 레지스터가 초기화된다.**
  - 빌드: 전 게이트 OFF / `PHASE25_CAN_TX=1` 양쪽 모두 프로젝트 코드 경고 0 (잔존 경고 5건은 전부 서드파티 `Lib/MCP251XFD`·`Middlewares/ST/filex`).

- [x] **`ExtCAN_Wake()` 실패 시 MCP2518FD가 깨어난 채로 영구 고착** (2026-08-21 코드 점검 발견·수정)
  - `MCP251XFD_WakeUp()`이 성공한 뒤 `MCP251XFD_StartCAN20()`이 실패하면 `return false`로 빠지는데, `mcp_asleep = false`가 그 **아래**에 있어 플래그가 `true`로 남았다. 장치는 실제로 깨어 있으므로(OSCDIS 클리어로 Configuration 모드 복귀) 이후:
    - `ExtCAN_Sleep()`의 `if (mcp_asleep) return;` 조기 반환에 걸려 **두 번 다시 재우지 못한다** → 3.3V에서 15mA를 상시 소모(슬립 구현의 목적이 소멸).
    - `ExtCAN_DrainRxFIFO()`의 `if (mcp_asleep) return;`에도 걸려 **RX 드레인이 영구 중단**된다.
  - 조용히 발생하고 스스로 복구되지 않는 상태라 실기에서 원인 추적이 어렵다.
  - **수정**: `mcp_asleep = false`를 `MCP251XFD_WakeUp()` 성공 직후로 올렸다. StartCAN20이 실패해도 플래그는 실제 상태(깨어 있음)와 일치하므로 다음 기회에 정상적으로 다시 잠든다. 아울러 `ExtCAN_Transmit()`의 `if (!ExtCAN_Wake())` 실패 분기에 `ExtCAN_Sleep()`을 추가해 그 자리에서 되돌리도록 했다(이미 자고 있으면 무동작).
  - `PHASE25_CAN_TX=1`로 게이트를 켜서 컴파일 검증 완료(경고 0). 기본 빌드에서는 게이트 OFF라 컴파일되지 않는 경로다.

- [x] **`ExtCAN_DrainRxFIFO()`가 뮤텍스 밖에서 `mcp_asleep`을 읽던 경쟁 조건** (2026-08-21 2차 코드 점검 발견·수정)
  - `if (mcp_asleep) return;`이 `tx_mutex_get()` **이전**에 있어, 드레인 스레드(MCP_RX_Thread, pri 6)가 값을 읽은 뒤 뮤텍스를 기다리는 동안 송신 스레드(UART_RX_Thread, pri 7)가 `ExtCAN_Wake()`/`ExtCAN_Sleep()`으로 상태를 바꿀 수 있었다.
    - 낡은 `false`를 봤으면 → 뮤텍스 획득 후 **자고 있는 칩에 SPI를 친다**(무의미한 레지스터 접근).
    - 낡은 `true`를 봤으면 → 깨어 있는데 드레인을 건너뛴다(100ms 폴백 폴링이 복구하므로 경미).
  - **수정**: `mcp_asleep` 검사를 뮤텍스 획득 **이후**로 옮기고, 자고 있으면 뮤텍스를 반납하고 반환하도록 했다. 게이트 OFF 빌드에서는 뮤텍스 자체가 컴파일되지 않으므로 `#if PHASE25_CAN_TX`로 반납을 감쌌다.
  - 두 게이트 조합(OFF / `PHASE25_CAN_TX=1`) 모두 프로젝트 코드 경고 0으로 빌드 확인.

- [x] **LED 소비 전류 재산출 — VF@20mA가 아니라 실제 동작점 곡선으로** (2026-08-21 LED 데이터시트 입수 후 정정)
  - 실장 부품: **D6(PWR) = APT1608SGC(Super Bright Green)**, **D7~D11(ERR·BOARD_CHECK·SD·CAN·RFD900x) = APT1608SURCK(Hyper Red)**. 전부 직렬 1 kΩ(R34~R39).
  - **이전 오류**: `Vf ≈ 2.0 V`를 가정해 개당 1.3 mA로 계산했다. 데이터시트의 VF(녹 2.2 V / 적 1.95 V)는 **IF = 20 mA 조건**인데, 우리 회로의 동작점은 ~1.5 mA라 그 값을 쓰면 VF를 과대평가해 전류를 과소평가한다.
  - **정정**: 각 데이터시트의 *Forward Current vs. Forward Voltage* 곡선에서 저전류 구간을 읽어 부하선과 교점을 구했다.
    - 녹색 D6: 3.3 V 직결(R39). VF ≈ 1.87 V → (3.3 − 1.87)/1 kΩ = **1.42 mA**
    - 적색 D7~D11: GPIO 푸시풀 구동(VOH ≈ 3.26 V @1.5 mA). VF ≈ 1.73 V → (3.26 − 1.73)/1 kΩ = **1.52 mA**
  - **반영 결과**: 평시(PWR 1개) 1.30 → **1.42 mA**, 피크(PWR + 활동 2개) 3.00 → **4.46 mA**. 3.3 V 레일 합계는 평시 20.52 → **20.64 mA**, 피크 138.35 → **139.81 mA**(ICL 160 mA의 87 %). 시스템 총 전력 1.679 → **1.680 W**.
  - 모든 LED는 DC 정격(적 30 mA / 녹 25 mA)과 PD 75 mW 대비 여유가 20배 이상이라 열 문제는 없다.
  - 산출표: `C:\Users\user\Desktop\27WSC\WSC_송신보드_소비전력_산출.xlsx`

- [x] **ADM3053 IIO(3.3 V측) 정정 — Table 1 값은 VIO = 5 V 조건이었다** (2026-08-21 재검증 발견·수정)
  - `adm3053.pdf` Rev.F Table 1의 `IIO 1.6 typ / 2.5 max mA`를 그대로 3.3 V 레일에 적용했으나, **표 머리말이 "All typical specifications are at TA = 25°C, VCC = 5 V, and VIO = 5 V"** 다. 우리 보드는 넷리스트상 `IC2.6 / IC4.6 → 3.3V`로 VIO가 3.3 V다.
  - 같은 데이터시트 **Figure 7 (Supply Current, IIO vs. Data Rate)** 에 **VIO = 3.3 V 곡선이 별도로 있고**, 100 kbps~1 Mbps 구간에서 거의 평탄하게 **≈0.85 mA**를 가리킨다(VIO = 5 V 곡선은 1.5~1.7 mA로 Table 1과 일치). 즉 기존 값은 약 2배 과대평가였다.
  - **정정**: 소자당 1.60 → **0.85 mA**, 2개 합계 3.20 → **1.70 mA**. 3.3 V 레일 평시 20.64 → **19.14 mA**, 피크 139.81 → **138.31 mA**(ICL 160 mA의 86 %). 시스템 총 전력 1.680 → **1.671 W**.
  - **재확인한 항목(변경 없음)**: ICC(5 V측)는 Table 1의 리세시브 29 typ / 도미넌트 195 typ / 1 Mbps 139 typ이 맞고, 시험조건 `RL = 60 Ω, RS = low`도 보드와 일치한다(넷리스트: `IC2.18 → CAN_GND`, `IC4.18 → CAN_GND_2`로 RS가 절연 GND 직결 = high-speed 모드). 500 kbps 연속 부하의 113 mA는 Figure 5의 VCC = 5 V 곡선 직독값이다.
  - ⚠ VIO = 3.3 V 조건의 **max 규격은 데이터시트에 없다**(Figure 7은 typ 곡선). 0.85 mA는 typ이며, 3.3 V 레일 여유가 큰 구간이라 판정에는 영향이 없다.

- [x] **ThreadX 스택 오버플로 검출 활성화** (2026-08-21 전체 코드 점검)
  - `tx_user.h`의 `TX_ENABLE_STACK_CHECKING`이 주석 처리돼 있어, 스택이 넘쳐도 검출 없이 인접 메모리를 덮어썼다. 스레드 스택이 1024 B(MCP_RX/UART/UART_RX)·2048 B(SD_Logger/FileX)인데 FileX·HAL 호출이 얹혀 여유가 크지 않다.
  - ⚠ **검사만 켜면 무의미하다**: ThreadX의 `_tx_thread_stack_error_handler()`는 애플리케이션 콜백이 등록돼 있지 않으면 인터럽트만 껐다 켜고 그냥 반환한다(소스 확인). 그래서 `app_threadx.c`에 `App_StackErrorHandler()`를 만들고 `tx_thread_stack_error_notify()`로 등록했다.
  - 핸들러는 **최소 동작만** 한다 — `LED_ERR` 점등 + `SystemState.error = 1`. LOG 패킷을 만들지 않는 이유: 이 콜백은 **이미 스택이 손상된 스레드**에 대해 인터럽트가 차단된 상태로 불리므로, `PacketBuilder_Create()`가 손상된 스택에 지역 버퍼를 잡는 것도 `tx_queue_send()`로 차단 구간을 늘리는 것도 부적절하다. 어느 스레드였는지는 디버거에서 `thread_ptr->tx_thread_name`으로 확인한다.
  - 비용: ROM +1,288 B(68,064 → 69,352), RAM +8 B. TX 바이트풀은 16,384 B 중 7,168 B 사용이라 여유 불변.
  - 빌드 검증: 게이트 OFF / `PHASE25=1` / **전 게이트 ON** 3가지 조합 모두 프로젝트 코드 경고 0.

- [x] **`app_filex.c` SD 마운트 실패 시 busy-spin 제거 (PHASE5 OFF 빌드)** (2026-08-21 전체 코드 점검)
  - `fx_app_thread_entry()`의 `fx_media_open` 실패 분기가 `#if PHASE5_SD_LOGGING`에서는 이미 `return`으로 고쳐져 있었으나, `#else`(= 현재 기본 빌드)는 CubeMX 기본값인 `while(1);` 그대로였다.
  - 이 스레드는 **우선순위 10 + `TX_NO_TIME_SLICE`** 라 스핀에 들어가면 양보하지 않는다 → 같은 우선순위인 `SD_Logger_Thread`(Phase 4~)와 그 이하가 통째로 기아 상태가 된다. SD 마운트 실패는 "SD 로깅만 불가"일 뿐 무선 텔레메트리는 계속돼야 하므로 정지시킬 이유가 없다.
  - **수정**: PHASE5 분기와 동일하게 `LED_SD` 점등 + `sd_ok/error` 표시 후 `return`.
  - 부수 수정: `types.h`·`main.h`가 `#if PHASE5_SD_LOGGING` **안**에만 include돼 있어 실패 경로가 `SYSTEM_STATE`·`LED_SD_*`를 볼 수 없었다(PHASE5가 켜지기 전까지 드러나지 않던 잠복 문제). 두 헤더를 게이트 밖으로 옮겼다.

- [ ] **`TICK_INT_PRIORITY = 15` — 변경하지 않음 (브링업 실측 대상)** (2026-08-21 전체 코드 점검, 판단 기록)
  - HAL 타임베이스(TIM6)가 최저 우선순위라, 상위 ISR이 **1 ms를 넘겨 연속 점유하면** 다음 TIM6 인터럽트가 pending에 합쳐져 **틱이 1회 영구 유실**된다. `HAL_GetTick()`은 LOG 타임스탬프·SD 10초 flush 주기·HAL 블로킹 타임아웃의 공통 기준이다.
  - **변경하지 않은 이유**: 최악 ISR 점유를 추정하면 TIM5 PRIMARY 틱(16패킷 × 약 1,000 사이클 ≈ 100 µs) + FDCAN 드레인(≈20 µs) + 나머지(pri 2~3, 짧음)로 **150~200 µs 수준**이라 1 ms까지 약 5배 여유가 있다. 여기서 `TICK_INT_PRIORITY`를 0으로 올리면 TIM6가 FDCAN·TIM5를 선점하게 되어 브링업 직전에 인터럽트 지형을 바꾸는 셈이고, CubeMX 기본값이라 재생성 시 되돌아가 조용한 불일치가 생긴다.
  - **대신 할 일**: Phase 2~4 체크포인트에서 **TIM5 ISR 진입/이탈에 GPIO를 토글해 점유 시간을 실측**한다. 1 ms에 근접하면 그때 `TICK_INT_PRIORITY`를 0으로 올린다(근거를 갖고).

- [x] **정적 분석 강도 확인 — 프로젝트 코드는 `-Wextra -Wconversion`에서도 경고 0** (2026-08-21 전체 코드 점검)
  - 현재 빌드는 `-Wall`만 쓴다. 주요 소스 10개(`can_receiver`, `timer_scheduler`, `ext_can_controller`, `uart_transmitter`, `sd_logger`, `packet_builder`, `cobs`, `rtc_manager`, `app_threadx`, `main`)를 `-Wall -Wextra -Wconversion`으로 별도 컴파일해 본 결과 **전부 경고 0**이었다. 암시적 축소 변환(`-Wconversion`)이 하나도 없다는 뜻이라 정수 폭 관련 잠복 결함 가능성이 낮다.
  - 빌드 플래그 자체는 바꾸지 않았다(서드파티에서 다수 발생하며, `STM32Make.make`는 CubeMX 생성 파일이라 관리 비용이 붙는다). 필요하면 `build.ps1`의 프로젝트 코드 한정 경고 필터와 함께 도입할 수 있다.

- [x] **CRC-8 테이블 독립 재검증** (2026-08-21 전체 코드 점검)
  - `packet_builder.c`의 256엔트리 `crc8_table`을 소스에서 파싱해 poly 0x07 비트단위 정의로 재생성·대조 → **256/256 일치**. 추가로 랜덤 15바이트 패킷 200건에 대해 테이블 룩업과 비트단위 CRC 결과를 대조 → **불일치 0건**.
  - `tests/host`는 네이티브 gcc 부재로 여전히 미실행이지만(별도 이슈 참조), 링크의 유일한 무결성 검사인 CRC는 이 방식으로 확인했다.

- [ ] **`solar_accumulated_energy`가 무선으로 전송되지만 지상국에서 버려진다** (2026-08-21 3차 코드 점검 발견)
  - `Sched_Emit10s()`이 규정 패킷을 `Sched_Send(&reg, 1, 1)`로 **UART(무선) + SD 양쪽**에 보낸다. 그런데 지상국 `WscTelemetryDecoder.applySys()`는 `KEY_SYS_RTC_TIME` 하나만 처리하고 `KEY_REG_DATA`는 `return false`로 떨어뜨린다.
  - 규정 패킷이 담는 값 3개 중 2개는 다른 경로로도 도달한다 — `distance_traveled`와 `battery_accumulated_energy`는 PRIMARY의 `KEY_MOTOR_ODO`(0x03)로도 나가고 지상국이 `updateMotorOdo()`로 정상 반영한다.
  - **`solar_accumulated_energy`만 `KEY_REG_DATA`가 유일한 전송 경로다**(timer_scheduler.c:629). 즉 태양광 누적 발전량은 10초마다 무선으로 나가지만 **대시보드에 절대 표시되지 않는다.** 솔라카 전략 지표로서 값이 큰 데이터라 그냥 두기 아깝다.
  - **선택지 2개**
    - ⒜ 지상국을 고친다 — `VehicleSnapshot`에 필드 추가 + `updateRegData()` + `applySys`에 `KEY_REG_DATA` 분기 + 대시보드 표시. 무선 대역 비용은 10초당 2패킷(전체 125 pkt/s의 0.2 %)이라 무시 가능하고, 실시간 가시성을 얻는다.
    - ⒝ 무선 사본을 끈다 — `Sched_Send(&reg, 0, 1)`로 바꿔 SD 규정 파일에만 남긴다. 대역을 아끼지만 주행 중 태양광 누적량을 볼 수 없다.
  - ⒜를 권장하나 **대시보드 UI 변경이 걸려 있어 결정 대기**. (SD 규정 파일 기록은 어느 쪽이든 정상 동작하므로 WSC 제출용 데이터에는 영향 없음)
  - 참고: `KEY_BMS_COULOMB_1/2/3`도 `Sched_EmitBoth`로 무선 전송되지만 지상국이 의도적으로 버린다(`applyBms` default에 "쿨롱카운터(스케일 미정)" 주석). 이쪽은 BMS 원시 카운트의 스케일이 확정되면 함께 처리할 것.

- [x] **3차 코드 점검 — 무결 확인 항목** (2026-08-21)
  - **인터럽트 벡터**: `HAL_NVIC_EnableIRQ`로 활성화된 11개 IRQ(EXTI0/9, FDCAN1_IT0/IT1, GPDMA1_Ch1/Ch2, SDMMC1, SPI1, TIM5, TIM6, USART2) 전부 `stm32u5xx_it.c`에 핸들러가 존재. 누락 0.
  - **EXTI 라우팅**: `CAN_INT_Pin`·`CAN_RX_INT_Pin` 둘 다 falling + pull-up으로 설정되고 `HAL_GPIO_EXTI_Falling_Callback`에서 `ExtCAN_NotifyInterrupt()`로 연결됨.
  - **주변장치 속도 정합**: SPI1 = SYSCLK 160 MHz ÷ 16 = **10 MHz**(MCP2518FD 상한 20 MHz 내). SDMMC_CK = 커널 80 MHz ÷ (2×ClockDiv 2) = **20 MHz**(SD 기본속도 25 MHz 내). USART2 = 115200(지상국 설정과 일치). USART1 = 9600(GPS 미정, 별도 이슈).
  - **ThreadX 에러체크 레이어 활성**: 코드 곳곳의 "객체 생성 전 호출 시 안전 반환" 전제가 `TX_DISABLE_ERROR_CHECKING` 미정의에 의존하는데, ELF에 `_txe_semaphore_put`·`_txe_queue_send`·`_txe_mutex_get`이 링크된 것을 확인해 전제 성립을 입증했다.
  - **SD 파일명**: `250821_143022.log`(13자) / `..._reg.log`(17자)로 8.3을 넘지만 FileX는 롱파일네임을 기본 지원(`FX_MAX_LONG_NAME_LEN` 기본 256). 버퍼도 `log_name[24]`/`reg_name[32]`로 충분.
  - **DETAIL 스텝 분배**: `Sched_EmitDetailStep()` case 0~7 배치 정합, 빈 스텝 8·9를 남기고 `SCHED_DETAIL_STEPS`를 10으로 유지해 5초 주기를 보존하는 근거도 주석에 명시됨.
  - **키 네임스페이스**: 펌웨어가 발행하는 36개 KEY 중 지상국에 없는 17개를 전수 추적 → 14개는 `Sched_EmitSdOnly`(SD 전용, 무선 미전송)라 정상. 나머지 3개(COULOMB)와 REG_DATA만 위 항목으로 분리.

- [x] **WSC 규정의 10초 주기 기록 조항 폐지 반영 — 규정 전용 패킷·파일 제거, 전체 로그 일원화** (2026-08-21 팀 결정)
  - **배경**: 규정에서 10초 간격 기록 요구가 없어져 "그냥 전체 기록"으로 단순화하기로 했다.
  - **제거해도 잃는 데이터가 없음을 먼저 확인**했다. 규정 패킷이 담던 값 3개를 각각 추적한 결과:
    - `distance_traveled` — PRIMARY `KEY_MOTOR_ODO`(0x03)로 **1초마다 이미 전송 중** (지상국도 `updateMotorOdo()`로 정상 반영)
    - `battery_accumulated_energy` — 동일 패킷으로 이미 전송 중
    - `solar_accumulated_energy` — **대입하는 코드가 프로젝트 전체에 없었다.** 읽기만 하고(timer_scheduler.c:629) 쓰기가 없어 계속 `0.0f`를 보내고 있었다(태양광 적산 미구현). 즉 규정 패킷 자체가 100 % 중복이거나 빈 값이었다.
  - **코드 변경**
    - `timer_scheduler.c`: `Sched_Emit10s()`에서 규정 패킷 2개 발행 삭제(RTC 시각 1개만 남음). `SCHED_REG_TICKS`/`SCHED_PHASE_REG` → `SCHED_RTC_TICKS`/`SCHED_PHASE_RTC` 개명, 위상표·헤더 주석 갱신. 10초 틱의 발행량이 **3 → 1패킷**으로 감소.
    - `sd_logger.c`: `reg_file`·`reg_name`·`*_reg.log` 생성/개방 및 `KEY_REG_DATA` 분기 삭제. 이제 **모든 패킷이 예외 없이** 더블 버퍼를 거쳐 단일 로그 파일로 간다.
    - `types.h`: `KEY_REG_DATA`(값 15, LOG_KEY 마지막) → `KEY_SYS_RESERVED15`로 예약 전환(값 재사용 금지 사유를 주석에 명시 — 기존 SD 로그에 key=15 패킷이 남아 있어 다른 의미로 쓰면 과거 로그를 오독한다). `solar_accumulated_energy` 필드 삭제. `regulation_last_tick` → `odo_last_tick` 개명(실제로는 0x40E 수신 시각이라 이름이 정확해짐).
    - `can_receiver.c`: 0x40E 케이스의 필드명·주석 갱신.
  - **효과**: 무선·SD 양쪽에서 10초마다 2패킷 절감. 파일 핸들 2 → 1로 줄어 `fx_media_flush()`가 갱신할 디렉터리 엔트리 감소(SD 쓰기 버스트 감소 — 3.3 V 여유에 유리). **RAM 512 B 절감**(전 게이트 ON 기준 60,816 → 60,304 B).
  - **부수 확인**: `design.md:633`은 "전체 로그 파일에도 규정 데이터 포함 모든 패킷이 기록된다"고 서술했으나 실제 코드는 `continue`로 빠져 `_reg.log`에**만** 기록하고 있었다 — 문서와 코드가 어긋나 있었고, 이번 변경으로 문서 서술이 실제 동작과 일치하게 됐다.
  - **문서**: `design.md`(SD_Logger 절, 파일명 규칙, 스케줄 표, PackFloat2 주석, flush 근거), `requirements.md`(4, 스케줄 항목, 8.7, 11.3) 갱신. 한/영 구조 대조 일치(헤딩 56/56, 코드펜스 60/60 · 32/32, 0/0).
  - **빌드**: 게이트 OFF(ROM 69,352 / RAM 50,848) · 전 게이트 ON(ROM 108 KB / RAM 60,304) 모두 프로젝트 코드 경고 0.
  - ⚠ **남은 정리 대상**: 이전 항목에서 보고한 "`solar_accumulated_energy`가 지상국에 도달하지 않는다"는 건은 **본 변경으로 소멸**했다(값 자체가 존재하지 않았음). 태양광 누적 발전량이 실제로 필요해지면 MPPT 출력 적산을 펌웨어에 새로 구현해야 하며, 그때 `KEY_SYS_RESERVED15`가 아닌 **새 키(16 이상)** 를 할당할 것.

- [x] **FDCAN1 재동기화 폭 SJW 1 → 3** (2026-08-21 4차 코드 점검)
  - **비트레이트는 바뀌지 않는다.** SJW는 산식 `커널 / (Prescaler × (1 + TSeg1 + TSeg2))`에 들어가지 않는다. 변경 전후 모두 `80 MHz / (10 × 16) = 500 kbps`, NBT 16 tq, 샘플포인트 `(1+12)/16 = 81.25 %`로 **동일**하다. 바뀌는 것은 한 비트당 재동기화로 비트 경계를 옮길 수 있는 폭뿐이다.
  - **타 팀과 재합의 불필요**: SJW는 각 노드의 **수신측 로컬 파라미터**다. 비트레이트·샘플포인트가 그대로이므로 2026-07에 합의한 "500 kbps 통일"과 충돌하지 않는다.
  - **상한 3의 근거**: CAN 규격의 `SJW ≤ min(PhaseSeg1, PhaseSeg2)`. STM32 HAL 표기로 `NominalTimeSeg1(12) = PropSeg + PhaseSeg1`, `NominalTimeSeg2(3) = PhaseSeg2`이므로 PhaseSeg2 = 3이 상한이다(HAL 매크로 `IS_FDCAN_NOMINAL_SJW`는 1~128을 허용하지만 이는 M_CAN 하드웨어 필드 폭일 뿐 규격 제약이 아니다).
  - **올린 이유**: HSE 크리스털 전환으로 SJW의 원래 제약(발진기 오차 허용치 `SJW/(2×10×NBT)` = ±0.3125 %)은 여유 70배로 해소됐다. 그러나 SJW의 다른 역할인 **버스 링잉·과도현상 흡수**는 1 tq = 125 ns뿐이었다. 차량 하네스는 절연 트랜시버(ADM3053, 전파지연 약 145 ns — Rev.F Figure 11) 2단과 수 m 케이블을 거치므로 3 tq(375 ns)로 넓혔다. 대역폭·타이밍 비용은 0이다.
  - **샘플포인트 81.25 %는 그대로 둔다**: CiA 권장은 87.5 %지만, 절연 트랜시버는 전파지연이 커서 PropSeg 요구가 크다. 왕복 소요를 추정하면 `2 × (트랜시버 루프 145 ns + 케이블 약 50 ns) ≈ 390 ns = 3.1 tq`인데 TSeg1 12 tq(1500 ns) 안에 충분히 들어간다. 오히려 낮은 샘플포인트가 전파지연 여유를 더 준다.
  - `fdcan.c`와 `.ioc`(`FDCAN1.NominalSyncJumpWidth=3` + IPParameters 추가) 양쪽 동기화. 디스어셈블리로 `movs r1, #3` 확정 확인.
  - **브링업 확인 항목**: Phase 2 CAN 수신 검증 시 FDCAN 에러 카운터(TEC/REC)와 `ERR_CAN_*` LOG 발생 여부를 관찰한다. SJW를 올린 뒤에도 에러가 보이면 원인은 재동기화가 아니라 종단·배선·샘플포인트 불일치 쪽이다.

- [x] **MCP2518FD 수신 필터를 Bus B 예약 대역(0x510~0x51F)으로 축소** (2026-08-24)
  - 기존 `AcceptanceID = 0x000` / `AcceptanceMask = 0x000`은 마스크 0 = "전 비트 don't care" = **표준 ID 전부 수락**이었다. LCD나 센서가 Bus B에 붙는 순간 모든 프레임마다 EXTI가 떠서 `MCP_RX_Thread`(pri 6)가 깨어나 블로킹 SPI 드레인을 돌게 된다.
  - **유보 사유가 사라져서 지금 해결했다**: "Bus B 수신 대상 ID 미확정"이 막고 있었는데, 같은 날 Bus B ID 맵을 우리 팀이 정의하면서(`EXT_CAN_ID_LCD_TX = 0x500`, LCD→본 보드 방향 0x510~0x51F 예약) 대상이 확정됐다.
  - **설정**: `AcceptanceID = 0x510`, `AcceptanceMask = 0x7F0`. 드라이버 규약상 마스크 '1'=검사 / '0'=don't care이므로 SID bit10~4를 검사한다. 검증: 0x510·0x515·0x51F → 통과 / 0x4FF·0x500·0x50F·0x520·0x700 → 거부.
  - 지금은 `ExtCAN_ProcessMessage()`가 stub이라 드레인 후 폐기되지만, LCD가 실제로 송신을 시작해도 **필터 수정 없이 그대로 받는다.**
  - 빌드: 경고 0, ROM 69,352 → 69,368 B (+16 B).

- [x] **호스트 테스트 실행 환경 확보 — 전체 테스트 실행 및 전부 PASS** (2026-08-24)
  - 장애물이던 "네이티브 gcc 부재"를 `winget install BrechtSanders.WinLibs.POSIX.UCRT`(MinGW-w64 gcc 16.1.0)로 해소했다. ARM 툴체인(`arm-none-eabi-gcc`)만 있어 실행이 불가능하던 상태가 끝났다.
  - `tests/host/README.md`의 명령 그대로 컴파일했고 **`-Wall -Wextra -Wconversion`에서 경고 0**이었다.
  - **실행 결과 — ALL TESTS PASSED (종료코드 0)**
    - `[Layout]` 호스트 컴파일러에서 `sizeof(LOG) == 16`
    - `[COBS]` 왕복 + 0바이트 미포함, 랜덤 20,000건
    - `[CRC-8]` 체크섬 무결성 + 단일 비트반전 검출, 2,000패킷 × 120비트
    - `[CRC-8]` 테이블 룩업 == 비트단위 참조 구현, 256 전수 + 랜덤 20,000 + 기지값 1
    - `[Pack]` float2 / u16x4 / i16x4 / u32x2 왕복, 20,000+건
  - 이로써 그동안 미실행이던 `test_crc8_table_equivalence()`가 실제로 검증됐다(그전까지는 PowerShell 우회 검증만 있었음). COBS 왕복도 코드 리뷰가 아니라 실행으로 확인됐다.
  - 산출물 `tests/host/test_host.exe`는 `.gitignore`에 등록해 커밋되지 않게 했다.

---

## Phase 1 브링업 실기 검증에서 발견된 결함 (2026-08-28)

실물 보드에 처음 전원을 넣고 Phase 1 / 1.5를 수행하며 발견한 **코드 결함**들이다.
셋 다 정적 분석·컴파일러·호스트 테스트로는 잡히지 않았고, 보드에서 실행해야만
드러나는 종류였다. 브링업 계획서 1.3절이 "실기 검증에만 의존하는 항목"을
따로 명시해 둔 이유가 여기에 해당한다.

- [x] **`HAL_GetTick()`이 부팅 수 초 뒤 영구 정지** — **해결됨**
  - **증상**: 보드는 정상 동작(ThreadX 클럭·스레드·LED 모두 정상)하는데 `uwTick`만
    부팅 2~8초 사이 임의 시점에서 멈춘 뒤 다시 증가하지 않음. 정지 시점이 매번 달라
    특정 초기화 단계와 무관.
  - **원인**: `stm32u5xx_hal_conf.h`의 `TICK_INT_PRIORITY`가 **15**로, ThreadX가
    PendSV에 부여하는 우선순위와 **동일**했다. ThreadX Cortex-M 포트는 PendSV 예외
    **안에서** 유휴 대기(`__tx_ts_wait`의 WFI)하므로, PendSV가 활성인 동안 실행
    우선순위가 15로 고정된다. 같은 15인 TIM6(HAL 틱)는 이를 선점할 수 없어 계속
    펜딩 상태로 남는다. SysTick은 ThreadX가 4로 설정하므로 영향이 없었고, 그래서
    "ThreadX만 정상이고 HAL 틱만 죽는" 모습으로 나타났다.
  - **수정**: `TICK_INT_PRIORITY`를 **0**으로 변경. ISR이 `uwTick` 증가 한 줄뿐이라
    최고 우선순위여도 다른 ISR에 영향을 주지 않는다.
  - **검증**: OpenOCD + GDB로 12초 측정 — `uwTick` +12,047 / `_tx_timer_system_clock`
    +12,046으로 1 kHz 동기 확인. 리셋 5회에서도 두 값의 차이가 27~28로 고정.
  - **방치했다면**: `HAL_GetTick()`은 7개 파일 13곳에서 쓰인다. 모든 LOG 패킷의
    `timestamp`가 같은 값이 되고(지상국 시계열 붕괴), `Sched_CheckTimeouts()`의
    데이터 신선도 감시가 영구 무력화된다. `HAL_Delay()`는 코드 어디에도 없어
    행(hang)까지는 가지 않았고, 그래서 증상이 조용했다.

- [x] **MCP2518FD가 웜 리셋 후 초기화 불가 — 전원 재인가 전까지 Bus B 영구 불능** — **해결됨**
  - **증상**: 콜드 부팅(전원 인가)은 정상인데, STM32만 리셋되는 워치독/소프트 리셋
    이후에는 `ExtCAN_Init()`이 `ERR__NO_DEVICE_DETECTED`(=1)로 실패. `mcp_ok=0`,
    `LED_ERR` 점등. 이후 어떤 소프트 리셋으로도 복구되지 않음.
  - **원인**: `ExtCAN_Init()`은 마지막에 전력 절감을 위해 MCP를 Sleep으로 재운다.
    MCP2518FD는 STM32의 NRST에 연결돼 있지 않으므로(회로도 IC5 — **하드웨어 리셋
    핀 자체가 없음**) 웜 리셋 후에도 `OSC.OSCDIS=1`인 채로 남는다.
    `Init_MCP251XFD()`는 장치 검출을 **내부 RAM에 `0xAA55AA55`를 쓰고 되읽는**
    방식으로 하는데, RAM 접근에는 장치 내부 클럭이 필요하다. 오실레이터가 멈춰
    있으니 검출이 실패한다. SPI RESET 명령은 Configuration 모드에서만 유효해
    무시되므로, 소프트웨어로는 빠져나올 수 없는 상태였다.
  - **수정**: `Init_MCP251XFD()` **호출 전에** OSC 레지스터의 `OSCDIS` 비트를 직접
    클리어하고 오실레이터 안정화(4 ms)를 대기. 이미 깨어 있으면 읽기만 하고
    지나가므로 콜드 부팅 경로에는 영향이 없다.
  - **함정**: 드라이버의 `MCP251XFD_WakeUp()`은 여기서 쓸 수 없다. 첫 줄에서
    `InternalConfig`의 파워 상태를 읽는데, 바로 위 `memset`으로 0
    (= `SLEEP_NOT_CONFIGURED`)이라 `ERR__CONFIGURATION`을 반환하고 장치를
    건드리지 않는다. 반드시 SFR을 직접 조작해야 한다.
  - **검증**: 웜 리셋 10회(대기시간 2~5초로 흩어 SPI 주기 100 ms에 대해 서로 다른
    위상에서 리셋) + 최종 5회, 전부 `mcp_ok=1`.
  - **관련 확인**: `CAN_CS`(PA4)에 4.7 kΩ 풀업(R12)이 있어, SPI 전송 도중 STM32가
    리셋돼도 CS가 High로 떠서 미완성 명령이 폐기된다. INT 라인 둘도 동일(R13/R40).
    이 덕분에 "전송 중 리셋" 시나리오는 하드웨어로 방어된다.
  - **방치했다면**: `PHASE6_SAFETY`(Alive-Flag 워치독)를 켠 뒤 워치독이 한 번이라도
    걸리면, 주행 중 LCD 통신이 죽은 채로 복구 불가 상태가 된다.

- [x] **링커 스크립트의 FLASH 크기가 실제 칩의 2배로 선언됨** — **해결됨**
  - **원인**: `STM32U575xx_FLASH.ld`의 `ROM` 영역이 `LENGTH = 2048K`였다. 실제 품번
    STM32U575V**G**T6Q는 **1 MB**이고(품번의 `G`), CubeProgrammer도 장치에서
    `NVM size : 1 MBytes`로 읽는다. 2048K는 `VI`(2 MB) 품번 기준 기본값이다.
  - **영향**: 현재 사용량이 작아(69 KB) 당장 문제는 없었으나, **1 MB를 넘겨도 링커가
    잡지 못하고** 플래시 단계에서야 실패한다.
  - **수정**: `LENGTH = 1024K`로 정정 + 근거 주석. 표시 사용률이 3.31% → 6.63%로
    실제 값이 됐다.

### 도구 관련 교훈

`STM32_Programmer_CLI`는 메모리를 읽기 위해 코어를 정지시키므로 **시계열 관측에
쓸 수 없다.** 같은 변수를 두 번 읽으면 값이 항상 같게 나와 "멈춰 있다"로 오독된다.
NVIC 같은 SCS 영역은 실행 중 판독 자체가 부정확했다(`ISPR = 0xFFFFFFFF` 같은 불가능한
값). 위 결함 두 건 모두 **OpenOCD + GDB**로 전환한 뒤에야 원인이 특정됐다.
런타임 상태를 관찰해야 하는 조사에서는 처음부터 GDB를 쓸 것.

### Phase 1 후속 — SD 부재 시 부팅 불가 해결 및 RTC/SD_DETECT 실측 (2026-08-29)

- [x] **SD 카드가 없으면 보드 전체가 죽는 구조** — **해결됨**
  - **실측**: 카드를 빼고 전원을 넣으면 `MX_SDMMC1_SD_Init()` → `HAL_SD_Init()` 실패 →
    `Error_Handler()` 무한루프 → ThreadX 진입 못 함 → `TEMP_IWDG_REFRESH` 리프레시가
    돌지 않아 **4초 주기 IWDG 리셋 루프**. `RCC_CSR` 의 `IWDGRSTF`(bit29)=1 로 확인했고
    `can_ok=0 / mcp_ok=0` 으로 CAN·MCP 초기화 이전에 죽는 것도 확인.
  - **왜 중요한가**: SD 는 소모품이고 커넥터 접촉 불량도 흔하다. 로깅용 주변장치 하나
    때문에 CAN 수신·무선 송신·LCD 중계가 전부 멈추는 단일 고장점이었다.
  - **수정** (CubeMX 재생성 안전하게 두 곳):
    1. `.ioc` 의 `ProjectManager.functionlistsort` 에서 SDMMC1 자동 호출 생성을 끔
       (`9-MX_SDMMC1_SD_Init-SDMMC1-false-HAL-true` → 마지막 필드 `false`).
       FileX 가 이미 같은 방식으로 처리돼 있어 검증된 방법이다.
    2. `main.c` 의 USER CODE 블록에서 직접 초기화. 전역 표식 `g_init_nonfatal` 을 세우고
       부른 뒤, `Error_Handler()` 앞머리에서 그 표식을 보고 **정지 대신 복귀**한다.
       실패 시 `SystemState.sd_ok = 0` + `LED_SD`(PD8) 점등으로 표시만 하고 진행.
  - **`.ioc` 만으로는 안 되는 이유**: `Error_Handler()` 호출이 생성 함수 **안에** 있어
    CubeMX 설정으로는 바꿀 수 없다. 다만 `Error_Handler()` 본문과 `USER CODE BEGIN 2`
    는 모두 USER CODE 블록이라 재생성 후에도 보존된다.
  - **범위 제한**: 표식이 켜진 구간에서만 비치명이다. NVIC 우선순위 불변식 위반 등
    진짜 치명 오류는 기존대로 정지한다.
  - **검증**: 카드 없음 → `can=1 mcp=1 err=0`, `LED_SD` 점등, 10초간 `uwTick` 정상 증가
    (리셋 없음). 카드 있음 → `LED_SD` 소등, 동일하게 정상.

- [x] **SD_DETECT(PD0) 극성 실측** — **기존 가정이 맞음, 수정 불필요**
  - 카드 있음 `PD0=0`(LOW), 카드 없음 `PD0=1`(HIGH). 핀은 입력 + 풀업(PUPDR=1).
  - `sd_logger.c` 의 `SD_DETECT_ABSENT_LEVEL = GPIO_PIN_SET` 가정과 일치한다.

- [x] **RTC / LSE 동작 실측** — **정상**
  - `PRER = 0x007F00FF`(Async 127 / Sync 255) → 32768 / (128 x 256) = 1.000 Hz.
    LSE 전환 시 `SynchPrediv` 를 249(LSI용)에서 255로 고친 것이 실기에서 확인됐다.
  - `ICSR = 0x27`(INITS·RSF) — 초기화 완료, 섀도 동기 정상. TR 이 5초에 5초 증가.
  - ⚠ **함정**: `RTC_TR` 을 읽으면 섀도 레지스터가 잠기고 `RTC_DR` 을 읽어야 풀린다.
    `TR` 만 반복해 읽으면 멈춘 값이 계속 나온다(`rtc_manager.c` 주석이 경고하는 그것).
    디버거로 확인할 때도 매번 `TR → DR` 순서로 읽어야 한다.

- [ ] **실제 시각을 RTC 에 넣는 경로가 없음** (2026-08-29 발견, Phase 4/5 에서 처리)
  - `MX_RTC_Init()` 은 2000-01-01 00:00:00 을 하드코딩한다. `PHASE5_SD_LOGGING` 을 켜면
    백업 레지스터 마커(`RTC_BKP_DR1 == 0x32F2`)로 재설정을 건너뛰어 VBAT 으로 시각이
    이어지지만, **최초 한 번 실제 시각을 넣어주는 주체가 없다.**
  - 결과적으로 로그 파일명이 항상 `000101_00xxxx.log` 가 된다.
  - 후보: 지상국이 UART 로 시각을 내려주거나(타입 0x02 확장), GPS 수신 후 파싱.
    둘 다 미구현이므로 Phase 4/5 진입 전에 결정해야 한다.

---

## Phase 2 실기 1일차 — Motor 더미 단독 통과, 다중 노드는 미해결 (2026-08-30)

### 결과 요약

| 항목 | 상태 |
|---|---|
| Motor 더미 단독 ↔ Bus A | **통과** (전 필드 정상, 버스 에러 0) |
| S2 (모터 error 플래그 sticky OR) | **통과** — `error=0x0004` 확인 |
| BMS 더미 | **미검증** — 이번 세션에 단 한 프레임도 수신 안 됨 |
| MPPT 더미 | **미검증** |
| 다중 노드(더미 3 + LCD) | **실패, 원인 미규명** |
| Bus B / LCD | 미착수 |
| S1, S3, S4, S5 | 미착수 |

Motor 단독 실측값: `BusV=124.5V, BusI=34.0A, rpm=506, speed=14.83m/s,
MotorT=46.1℃, HeatsinkT=37.9℃, DspT=39.7℃, PhaseB=-45.8A, PhaseC=15.3A,
odo=1621m, bus_ah=0.415`, 버스 `REC=0 TEC=0 LEC=0`, 프레임 유실 0.

### 발견·수정한 코드 결함

**1. CAN 고장 LED 가 정상 수신 중에 영구 점등 (커밋 `1542ab2`)**

수신 드레인이 `for(;;)` 로 돌며 `HAL_FDCAN_GetRxMessage()` 실패까지 읽어서,
매 드레인의 마지막 한 번이 반드시 **빈 FIFO** 를 읽었다. HAL 은 이를
`ErrorCode |= HAL_FDCAN_ERROR_FIFO_EMPTY` 로 기록하고, `HAL_FDCAN_IRQHandler`
끝에서 `ErrorCode != NONE` 이면 에러 콜백을 부른다. **`ErrorCode` 를 지우는
주체가 없어서** 첫 프레임 이후 모든 인터럽트가 에러 콜백을 재진입했고,
콜백이 무조건 `CAN_MarkError()` 를 부르므로 `SUBSYS_FAULT_HOLD_MS`(3초)
무오류 조건이 영원히 성립하지 않았다.

브레이크포인트로 확정한 콜스택:
`CAN_Receiver_ErrorCallback ← HAL_FDCAN_ErrorCallback ← HAL_FDCAN_IRQHandler`,
`ErrorCode = 0x100`.

수정 3가지 (하나만으로는 구멍이 남는다):
- 드레인 루프를 `HAL_FDCAN_GetRxFifoFillLevel()` 로 제한 — 빈 FIFO 를 안 읽는다
- 에러 콜백이 `FIFO_EMPTY`/`FIFO_FULL` 을 걸러내고, 처리한 비트를 `ErrorCode`
  에서 직접 지운다 — 안 그러면 **어떤 일회성 HAL 오류든** LED 를 영구 점등시킨다
- `RX_FIFO0_FULL` 을 고장 취급하지 않는다. FIFO0 는 이 칩에서 3칸 고정
  (`SRAMCAN_RF0_NBR`, 설정 불가)이라 버스트가 오면 차는 게 정상이다.
  실제 유실은 `MESSAGE_LOST` 뿐이며 그쪽은 그대로 폴트. FULL 은 1초 1회
  경고 로그만 남긴다.

**2. `openocd.cfg` 가 존재하지 않는 스크립트를 참조해 플래시 불가**

`interface/stlink-v2.cfg` 는 ST 배포본에 없다(제공되는 것은 `stlink.cfg` 와
`stlink-dap.cfg`). 또 `target/stm32u5x.cfg` 는 DAP transport 전제라 `stlink.cfg`
(hla) 로는 `hla newtap` 에러가 난다. `stlink-dap.cfg` 로 교체하고,
`build.ps1` 이 `-s <st_scripts>` 로 검색 경로를 넘기도록 고쳤다.
이게 막혀 있어서 이날 이전의 모든 "플래시했다"는 실제로 반영되지 않았을
가능성을 검토할 것.

**3. 더미 `Error_Handler` 가 아무 표시 없이 정지** (리포 밖, 미커밋)

`__disable_irq(); while(1){}` 이라 보드가 죽었는지 그냥 조용한 건지 구분이
불가능했다. LD2(PA5) 를 레지스터 직접 제어로 점멸시키도록 바꿨다
(클럭 설정 실패 시점에도 동작해야 하므로 HAL 미사용).

### 미해결 — 다중 노드 통신 실패

더미 3대 + LCD 를 모두 붙였을 때 **모든 프레임이 100% Form/Stuff 에러**로
깨졌고, 성공 수신이 0 이었다. 송신보드 전원을 꺼도 더미끼리 서로 깨졌다
(BMS 더미 `REC=255` 관측). 노드를 하나로 줄이자 즉시 정상화됐으므로
**나머지 두 더미 중 하나 또는 노드 수에 의존하는 물리 계층 문제**로 좁혀졌다.

배제된 것 (레지스터·실측으로 확인):
- 송신보드 비트레이트 — `NBTP` NBRP=10/16TQ, `FDCANSEL=1`(PLL1Q 80MHz) → 500kbps
- BMS 더미 비트레이트 — `SWS=1`(HSE), SYSCLK 8MHz, `BTR` 16TQ → 500kbps
- 더미 HSE 크리스털 미실장 가설 — `HSERDY=1` 로 정상 기동 확인 (X3 실장됨)
- FDCAN 커널 클럭이 HSE 16MHz 라는 가설 — `FDCANSEL=1` 로 PLL1Q 확인
- 종단저항 60Ω, 트랜시버 3.3V, CAN-H/L 배선, 공통 GND — 사용자 확인
- LCD 보드 — 제거해도 증상 동일

남은 후보: BMS/MPPT 더미 개별 결함, 노드 수 증가 시의 신호 무결성,
트랜시버 모듈 개체 불량, 동일 ID 중복 송신(플래시 착오).

### 내일 절차 (MPPT 추가)

1. Motor 더미만 연결된 상태에서 기준선 확인 — `tools/phase2_inspect.gdb`
2. **MPPT 더미를 추가** 연결하고 재측정
   - `mppt[0..3]` 의 `mppt_last_tick` 이 `uwTick` 을 따라가면 통과
   - `REC`/`TEC`/`LEC` 가 오르면 MPPT 더미가 범인 → 그 보드에 ST-Link 를 옮겨
     `Total test/dummy_inspect.gdb` 로 `SWS`/`BTR`/`ESR` 실측
3. 통과하면 BMS 더미 추가, 동일 절차
4. 3대 모두 통과하면 LCD 재연결 → Bus B


---

## Phase 2 실기 2일차 — 이틀간의 CAN 장애, 원인은 배선이었다 (2026-08-31)

### 결론

이틀 동안 "다중 노드에서 CAN 전멸"로 보였던 현상은 **코드 결함이 아니었다.**
원인은 두 가지가 겹친 것이다.

**1. 종단저항이 착탈식 노드 모듈에 실려 있었다.**
송신보드와 Motor 모듈에는 120Ω 이 있고 MPPT 모듈에는 없었다. 그래서 어느
보드를 꽂고 빼느냐에 따라 종단 개수가 같이 바뀌었고, 구성을 바꿀 때마다
증상이 달라졌다. 예: 보드+Motor=2개(60Ω, 정상) / 보드+MPPT=1개(120Ω, 전멸).
디버깅 중 노드를 하나씩 떼어내는 행위 자체가 매번 버스 조건을 바꿔서,
이분법으로 좁히려는 시도를 계속 무력화했다.

**2. MPPT 노드의 트랜시버 모듈(또는 그 배선)이 불량이었다.**
토폴로지를 보드—MPPT—Motor 로 바로잡아 종단을 양 끝에 둔 뒤에도 MPPT 만
CRC 에러를 내며 한 프레임도 도달하지 못했다. MPPT 는 버스 **중간**이라 신호
조건이 가장 좋은 자리였고, 보드↔Motor 통신이 MPPT 접속점을 지나 정상
동작했으므로 본선은 멀쩡했다. 변수를 하나만 바꾸는 시험 — Motor 가 쓰던
정상 모듈·배선에 MPPT Nucleo 만 옮겨 꽂기 — 으로 즉시 확정됐다.
바꾸자마자 4개 유닛이 전부 수신됐다.

### 확정된 결과

| 항목 | 상태 |
|---|---|
| MPPT 4유닛 수신 | **통과** — 유닛별 값 정상, 버스 REC=0/LEC=0 |
| S5 (필터 상한 0x34F 경계) | **통과** — MPPT#4 의 0x340~0x346 정상 수신 |
| Motor 단독/조합 | 통과 (전날에 이어 재확인) |
| BMS 더미 | **미검증** — 아직 한 번도 수신된 적 없음 |

### 배선에서 반드시 고칠 것

- **종단저항을 노드 모듈에서 분리할 것.** 버스 양 끝 두 곳에만 고정으로
  두고, 중간 노드 모듈의 120Ω 은 제거한다. 지금처럼 착탈식 모듈에 실려
  있으면 노드를 꽂고 뺄 때마다 종단이 바뀌어 재현성 있는 시험이 불가능하다.
  이번 이틀의 혼선이 대부분 여기서 나왔다.
- **MPPT 가 쓰던 트랜시버 모듈은 교체/폐기.** 불량이 확인됐다.

### 진단 도구에서 배운 것

노드를 떼었다 붙이는 이분법은 **버스 조건이 그 행위로 바뀌지 않을 때만**
유효하다. 종단이 노드에 딸려 있으면 "노드를 뺐더니 나아졌다"가 노드 때문인지
종단 때문인지 구분되지 않는다. 다음부터는 이분법 전에 종단 배치부터 고정할 것.

`tools/inspect.sh watch` 의 노드별 "마지막 수신 경과" 가 이번 판별의 핵심이었다.
값이 계속 커지면 그 노드는 전혀 안 들어오는 것이고, 작게 유지되면 정상이다.


### 2일차 최종 — Motor + MPPT 동시 동작 확인 (2026-08-31)

트랜시버 모듈 교체 후, MPPT Nucleo 가 `Error_Handler` 에 갇혀 있었다(LD2 점멸).
리셋 후 정상 기동. **어제 넣은 Error_Handler LED 점멸이 즉시 값을 했다** —
예전의 빈 `while(1)` 이었다면 "보드는 켜져 있는데 CAN 만 조용함"으로 보여
또 몇 시간을 배선 쪽에서 헤맸을 상황이었다.

부팅 실패 원인은 미확인(HSE 기동 실패 추정, 리셋으로 해소). 재발하면
SystemClock_Config 에 HSE 기동 재시도를 넣을 것.

최종 상태 (uwTick 209780):

| | |
|---|---|
| MPPT 4유닛 | 전부 현재값, tick 209619~209620 |
| Motor | 전 필드 정상, BusV 111.49V / 24.25m/s / odo 4101.6m |
| 버스 | `ECR=0x00000000` — TEC=0 REC=0 **CEL=0** (부팅 후 프로토콜 에러 0건) |
| S2 (모터 error sticky OR) | 통과 — `error=0x0004` |
| S5 (필터 상한 0x34F) | 통과 — MPPT#4 0x340~0x346 수신 |
| BMS | 미검증 (미연결) |

**RX FIFO 오버플로우 수정이 실부하에서 검증됨.** 두 노드 동시 송신에서
`RF0F(가득참)=1`, `RXF0S=3/3` 으로 3칸 FIFO 가 실제로 포화하는데,
`LED_CAN=0 can_ok=1` 이고 `RF0L(유실)=0` 이다. 수정 전 코드였다면 이 상태만으로
고장 LED 가 영구 점등됐다(커밋 `1542ab2` 참고).


### 트랜시버 모듈 불량 2건 — 모듈 교환 시험으로 확정 (2026-08-31 오후)

MPPT 노드가 살아 있는데(`LD2` 소등, `seq` 증가, `TEC=0`, `TXOK=1`) 송신보드는
그 프레임을 한 개도 못 받는 상태가 반복됐다. 오전에 판정해 교체한 **새 모듈도
불량**이어서 잠시 동작하다 다시 죽었고, 그 때문에 배선·종단·버스 토폴로지를
계속 의심하며 시간을 많이 썼다.

**판정 방법: 모듈 교환.** 정상 동작 중인 BMS 노드의 모듈과 MPPT 노드의 모듈만
맞바꿨다(종단·배선·보드는 그대로). 결과:

| | 교환 전 | 교환 후 |
|---|---|---|
| MPPT | 끊김 | **정상** (43~187ms) |
| BMS | 정상 | **끊김** |
| Motor | 정상 | 정상 |
| REC | 113~122 | 0~10 |

**고장이 모듈을 따라갔다** → 그 모듈이 불량. 폐기할 것.

### 이번에 통한 판별 원칙

증상만 보고 원인을 추정하는 것이 반복해서 빗나갔다. 실제로 통한 것은
**한 번에 변수 하나만 바꾸고, 고장이 그것을 따라가는지 보는 것**뿐이었다.

- 노드를 빼는 이분법은 종단이 노드 모듈에 실려 있으면 무효다(오전 교훈)
- "부품을 교체했다"는 그 부품이 정상이라는 뜻이 아니다 — 새것도 불량일 수 있다
- 노드 자신의 `TEC=0`/`TXOK=1` 은 "송신 성공"을 뜻하지만, 상대가 그 프레임을
  받았다는 뜻은 아니다. 양쪽을 동시에 봐야 한다

### 다중 프로브 주의

허브로 더미 Nucleo 들을 함께 연결하면 ST-Link 프로브가 4개 잡힌다
(별도 동글 1 + Nucleo 내장 3). 지정하지 않으면 OpenOCD 가 엉뚱한 보드를
읽는다. `tools/inspect.sh` / `tools/motor_watch.sh` 는 송신보드 동글 SN 을
고정하며, `WSC_BOARD_SN` 환경변수로 바꿀 수 있다. 목록은
`STM32_Programmer_CLI -l` 로 본다.


### Phase 2 완료 — 더미 3종 동시 수신, 시나리오 S1/S2/S5 통과 (2026-08-31)

트랜시버 모듈을 교체해 회로를 정상화한 뒤 **세 노드가 동시에, 에러 없이**
동작한다. `uwTick=76696` 기준:

| 노드 | 마지막 수신 | 값 |
|---|---|---|
| BMS BOT/TOP | 61ms 전 | SOC 54.8% / Stack 393.98V / cell_mv 4080~4092 / CellTemp 35.8℃ |
| MPPT 1~4 | 57ms 전 | 4유닛 개별값 정상, mode/errf 반영 |
| Motor | 49ms 전 | BusV 111.14V / 21.83m/s / rpm 745 / MotorT 63.1℃ |

버스: `ECR=0x00010000` — TEC=0, REC=0, **CEL=1** (부팅 후 프로토콜 에러 1건).
`LEC=0 EP=0 BO=0`, `RF0L=0`(유실 없음). `RF0F=1`/`RXF0S=3` 는 3노드 부하에서
3칸 FIFO 가 차는 정상 동작이며, 커밋 `1542ab2` 수정 덕에 고장으로 오인되지 않는다.

**시나리오 결과**

- **S1 통과** — BMS DETAIL 오프셋 +8 을 실물과 같이 **DLC 6** 으로 보낸 경우.
  더미 생성식 `cell_mv[c] = base_mv + (c%7)*4 - 12`, `BattVoltageSum =
  base_mv*96/100` 과 수신값이 정확히 일치했다:
  `cell_mv[0]=3141 → base_mv=3153 → 3153*96/100 = 3026`, 수신 `3026`.
  `coulomb_cc3=-9857` 도 tri_wave 범위(-15000~15000) 안.
  DLC 가 8 미만이어도 오프셋 4~5 의 값이 온전히 복원됨을 확인.
- **S2 통과** — Motor `error_flags=0x0004` sticky OR 누적
- **S5 통과** — MPPT#4(0x340~0x346)가 필터 상한 0x34F 경계에서 정상 수신

미착수: S3, S4(FIFO 오버플로우 — 더미에 전용 버스트 모드 없음)

### 도구 결함 2건 수정

1. **연결 실패를 0 값으로 위장** — 텔넷 응답이 비어도 디코더가 그대로 돌아
   전 필드 0 인 행을 찍었다. "노드가 전부 죽었다"로 오독되기 딱 좋다.
   실제로 서플라이가 꺼져 있을 때 이 오독이 발생했다. 이제 응답을 검사하고
   멈춘다.
2. **OpenOCD 준비 검사가 부실** — "Listening on port 4444" 만 확인했는데,
   타깃 연결에 실패해도 텔넷 포트는 열린다. `init mode failed` 도 함께 본다.


### S3 (수신 두절) 통과 — 다만 이건 Phase 2 항목이 아니었다 (2026-08-31)

**결과**

| 관측 | 값 |
|---|---|
| 검출 (MOTOR 기준) | 마지막 수신 후 **1363 ms** — 계획서 기준 "1~2초" 충족 |
| 검출 범위 | 8개 소스 전부 (`마스크 0xff`, 누적 8) |
| 재연결 후 | `마스크 0x00` 으로 해제, **누적은 8 유지** |

검출 순서는 `0x3c`(MPPT 1~4) → `0x3f`(BMS 추가) → `0xff`(MOTOR/ODO 추가)로,
노드를 뺀 순서를 그대로 반영한다. 임계값 `SCHED_TIMEOUT_MS=1000` 에 검사 주기가
`SCHED_TIMEOUT_TICKS=10` (=1s) 이므로 이론상 1000~2000ms 구간이고, 실측 1363ms 가
그 안에 들어온다.

**계획서의 배치 오류**

S3 는 타임아웃 검사 `Sched_CheckTimeouts()` 를 검증하는데, 이 함수는 TIM5
스케줄러 안에 있고 그 스케줄러는 `PHASE4_UART_PIPELINE` 뒤에 있다. 즉
**Phase 2 게이트에서는 노드가 끊겨도 아무 일도 일어나지 않는다.** 브링업
계획서가 S3 를 Phase 2 에 배치한 것은 부정확하다 — 실제로는 Phase 4 기능이다.

검증을 위해 `TEMP_S3_TIMEOUT_TEST` 임시 게이트를 두어 TIM5 만 켰다(전체 PHASE4 를
켜지 않고). 검증 후 0 으로 되돌렸다.

**막혔던 지점 2가지**

1. `Timer_Scheduler_Start()` 만 게이트를 푸는 것으로는 부족했다. TIM5 콜백에서
   `Timer_Scheduler_Tick()` 을 부르는 부분도 같은 게이트 뒤에 있어서, 타이머만
   돌고 스케줄러는 안 돌았다. 링커가 `Timer_Scheduler_Tick` 을 통째로 버린 것을
   보고 알아챘다(`nm` 에 심볼이 없었다).
2. 첫 복구 측정이 무효였다. 마스크가 `0x00` 이고 누적도 0 이었는데, 이는 래치
   해제가 아니라 **보드 재부팅**이었다(uwTick 이 153428 → 35840 으로 되돌아감).
   누적 카운터는 감소하지 않으므로, 이 값이 유지되는지로 둘을 구분해야 한다.

**관측 수단**

로그는 큐로 가는데 PHASE4 가 꺼져 있으면 큐가 없어 사라진다. 그래서
`g_timeout_mask`(소스별 비트) / `g_timeout_count`(누적)를 추가했다.


### S4 (FIFO 오버플로우) — 보드가 버스 포화를 유실 없이 견딤 (2026-08-31)

MPPT 더미에 `MPPT_BURST_MODE` 를 추가해 주기를 무시하고 최대 속도로 송신시켰다.

**버스트 실측**: 루프 2644회/5.1초 = 초당 521회, 루프당 8프레임(4유닛×2)이므로
**초당 4172 프레임 시도**. 500kbps 에서 8바이트 프레임이 약 260us 이므로 이론
최대가 약 3800 프레임/초 — 즉 **버스가 100% 포화**된 상태다(더미가 메일박스
대기로 자연히 throttle 된다).

**결과**: 24초 관측 동안 `LED_CAN=0`, `can_ok=1`, `RF0L=0`, 새 폴트 0건.
`RXF0S` 는 대부분 0 이었다 — FDCAN ISR(우선순위 1)이 들어오는 속도보다 빠르게
FIFO 를 비운다. **버스 포화에서도 프레임을 잃지 않는다.**

계획서의 기대 결과("LED_CAN 점등")는 충족되지 않았지만, 그것은 3칸 FIFO 가
포화 트래픽에서 넘칠 것이라는 가정에 기반한 것이고 실제로는 넘치지 않았다.
**"버스 포화에서 유실 0"이 더 정확한 합격 기준이다.** 계획서 기준을 그대로 두면
정상 동작을 실패로 판정하게 된다.

유실 검출 경로 자체는 별도로 동작이 확인됐다 — 디버거로 코어를 정지시키면
실제 유실이 발생하고, 그때 `LED_CAN` 이 켜졌다가 무오류 3초 후 소등된다
(`폴트경과 3182ms` 시점에 소등 확인).

### 이 측정에서 또 오염이 있었다

처음 S4 관측은 GDB 로 1~2초마다 코어를 세우며 읽었고, `LED_CAN=1` 을 보고
"버스트가 유실을 일으켰다"고 판단했다. **틀렸다.** `마지막폴트로부터` 값이
1054/1074/1061/1067/1065ms 로 **샘플링 주기와 정확히 일치**했다 — 폴트를 만든
것은 버스트가 아니라 디버거 정지였다. 사용자가 "물리 LED 가 안 켜졌다"고
지적해서 알아챘다.

같은 함정을 오늘만 세 번째 밟았다(motor_watch, inspect watch, 그리고 여기).
**CAN 수신 상태를 볼 때 코어를 세우면 안 된다** — RX FIFO0 가 3칸 고정이라
정지 자체가 유실을 만든다. 비침습(mdw) 경로로만 관측할 것.

### 프로브 판별

허브에 전부 연결하면 ST-Link 이 4개 잡힌다. 각 더미를 정지시켰을 때 송신보드에서
어느 노드가 멈추는지로 판별했다(`verify_image` 는 결과가 모호했다):

| SN | 보드 |
|---|---|
| 0667FF545383564867202841 | Motor |
| 066DFF485688494867114316 | MPPT |
| 066DFF555185754867225014 | BMS |
| 7 | 송신보드(별도 동글) |

`build_dummy.sh` 가 역할별로 SN 을 고정하며 `WSC_DUMMY_SN` 으로 덮어쓸 수 있다.

⚠ 판별 과정에서 `verify_image` 로 세 더미를 모두 halt 시켜 놓고 재개를 잊어
Bus A 가 1분 넘게 죽었다. 더미를 halt 했으면 반드시 `reset run` 으로 되돌릴 것.


### S4 를 Phase 4 로 이관 (2026-08-31 결정)

버스 포화(초당 4172프레임 시도, 이론 최대 약 3800)에서도 유실이 0 이었다.
TIM5 스케줄러를 함께 켜서 FDCAN1 과 같은 우선순위 1 로 경합시켜도 마찬가지였다
(`폴트경과 = uwTick`, 즉 부팅 이후 폴트 0건).

이유는 산술적으로 분명하다. 500kbps 에서 8바이트 프레임은 약 260us 이므로
포화 상태여도 263us 에 한 프레임이고, 160MHz 에서 이는 약 42,000 사이클이다.
프레임 하나를 꺼내 파싱하고 저장하는 데 드는 것은 수백 사이클이라, 3칸 FIFO 로도
ISR 이 압도적으로 앞선다. `RXF0S` 가 대부분 0 으로 읽힌 것이 그 증거다.

**계획서의 "LED_CAN 점등" 기대는 3칸 FIFO 가 포화에서 넘칠 것이라는 가정에
기반했고, 그 가정이 틀렸다.** 다만 이번 시험은 PHASE4 가 꺼져 있어 큐가 없고
UART DMA·SD 쓰기도 없다 — 스케줄러가 패킷을 만들기만 하고 전송은 즉시 실패하는
가벼운 상태였다. 설계 문서가 오버플로우를 경고한 것은 그 전체 부하 조건이다.

따라서 **S4 는 Phase 4 체크포인트로 옮긴다.** Phase 2 에서 확인된 것은
"정상 부하와 버스 포화에서 유실 0"이며 이것 자체가 유의미한 결과다.
유실 검출 경로는 별도로 동작이 확인됐다(디버거 정지로 실제 유실 유발 시
LED 점등, 무오류 3초 후 소등).

재사용 자산: MPPT 더미의 `MPPT_BURST_MODE` (평시 0). Phase 4 에서 그대로 쓴다.


### RFD900x 모뎀 설정 (2026-09-01 확인)

| 항목 | 값 | 근거 |
|---|---|---|
| 송신 출력 | **20 dBm** | 2026-08-21 결정. 실물 모뎀에서 확인 완료 |
| 듀티 사이클 | **50%** | 실물 확인 |
| 시리얼 속도 | **115200** 이어야 함 | 펌웨어 USART2 = 115200 8N1 |
| 흐름제어(RTS/CTS) | **켜야 함** | 펌웨어가 `UART_HWCONTROL_RTS_CTS`. 보드 PD3=CTS / PD4=RTS 이고 회로도에 `RFD900x_CTS`/`RFD900x_RTS` 넷이 있어 물리 연결됨. 끄면 모뎀 버퍼 오버런으로 **조용히 유실**되거나, 반대로 CTS 가 뜨면 송신이 아예 막힌다 |
| RSSI 리포트 | **꺼야 함** | 스트림이 바이너리 COBS(구분자 0x00)라, RSSI 텍스트가 섞이면 그 프레임이 깨진다 |
| NETID | 양쪽 일치 | — |

대역폭 여유: PRIMARY 17/s + DETAIL 약 6.6/s ≈ 24 패킷/s, LOG 16B + COBS 오버헤드
≈ 19B → 약 456 B/s ≈ 3.6 kbps. 듀티 50% 를 적용해도 최저 에어레이트에서 충분하다.


### Phase 4 UART 전송 검증 통과 (2026-09-01)

구성: 송신보드 + BMS 더미 1대 + RFD900x 2대(보드측/PC측, COM9 FTDI).
`PHASE4_UART_PIPELINE = 1`. MPPT/Motor 더미는 미연결.

**결과**

| 항목 | 값 |
|---|---|
| 프레임 무결성 | 2722개 중 CRC 통과 2719, 실패 1 → **99.96%** |
| 프레임 구조 | `[type 1B][LOG 16B]` = 17B → COBS 18B + `0x00` 구분자 = 19B |
| 발행률 | 18~23 패킷/초, 약 690 B/s |
| PRIMARY 주기 | 1Hz 키의 타임스탬프 간격이 **84회 연속 정확히 1000ms** → 실시간 구간 유실 0 |
| 큐 적체 | `UART_Queue enqueued = 0` 유지 (깊이 40) — 오버플로우 없음 |
| 값 종단 검증 | Stack 344.73V / MaxCell 3616 / MinCell 3566mV / SOC 364‰, cell_mv +4mV 간격 — 더미 생성식과 일치 |

큐 오버플로우 우려(선행 분석에서 "57패킷이 깊이 20 큐에 몰린다")는 해소가 확인됐다.
선행 수정(큐 메시지 16B 로 깊이 2배, DETAIL 10그룹 분산, PRIMARY/DETAIL 위상 분리,
crc8 테이블화)이 실부하에서 충분하다.

**운용상 알아야 할 것 2가지**

1. **수신측이 계속 읽지 않으면 데이터가 사라진다.** 캡처를 멈춘 사이 63초짜리
   구멍이 생겼다(1Hz 키 63개 유실). 모뎀/FTDI 버퍼가 넘친 것이다. 또 버퍼가
   깊어서, 읽기를 재개하면 **약 25초 지연된 과거 데이터부터** 나온다.
   지상국은 상시 수신 상태를 유지해야 한다.

2. **LCD 미연결 시 `ERR_CAN_TX_FAIL` 이 스트림의 28% 를 차지한다.** Bus B 에
   ACK 할 노드가 없어 브리지가 초당 3.2회 송신 실패 로그를 낸다. 실차에서는
   LCD 가 있으므로 평시엔 안 나지만, **Bus B 가 끊기면 무선 대역을 잠식한다.**
   FIFO-full 로그처럼 **속도 제한을 걸어야 한다**(현재 미적용).



### 지상국 연동 수정 + 오류 로그 레이트 리밋 (2026-09-01)

Phase 4 통과 후 지상국(`ArrowPoint-Telemetry`)을 실 스트림에 붙여 점검하고,
거기서 나온 항목과 위의 미적용 건(`ERR_CAN_TX_FAIL` 속도 제한)을 처리했다.

#### 지상국에서 찾은 것

| # | 문제 | 처리 |
|---|---|---|
| A-1 | XML 4곳의 레거시 `PropertyPlaceholderConfigurer` 가 Environment 를 안 봐서 **커맨드라인 인자가 조용히 무시**됨 (`uart.rx.port=COM9` 를 줘도 `port=NONE` 으로 기동) | bean 제거. Boot 의 `PropertySourcesPlaceholderConfigurer` 가 자동 등록되므로 동작은 동일하고 오버라이드만 살아남 |
| A-2 | 오류 패킷(`ERR_KEY` 0x80~)이 **전부 폐기**되고 있었음. 특히 `ERR_TIMEOUT` 은 source 가 `SRC_SYS` 라, 소스 기준으로 걸렀다면 "노드 사망"을 계속 놓쳤을 것 | `key >= 0x80` 이면 소스와 무관하게 가로채도록 변경. `LogErrorKey` 신설, 대시보드에 "차량 오류 로그" 패널 추가 |
| A-3 | 모터 Limit 표기가 "출력 제한 **동작 중**"(현재형) — sticky 누적값이라 틀린 표현 | "최근 5초 내 발생"으로 수정 |
| A-4 | `KEY_BMS_COULOMB_1/2/3`(0x16~0x18) 미처리 | raw 통과로 추가. **Ah 환산은 안 함** — 센스저항/CC 게인 설정이 어디에도 기록돼 있지 않아, 스케일을 지어내면 틀린 값이 맞는 값처럼 보인다 |
| B-1 | `application.properties` 기본값이 시뮬레이터 | 실 하드웨어 기본값으로 전환 (`uart.rx.enabled=true`, `port=COM9`, `simulate=false`) |
| B-2 | 시뮬레이터/실수신 동시 기동 가드 없음(으로 추정) | **오해였다.** `WscSimulatorService.start()` 에 이미 있음 — 수정 불필요 |

디코더 정합성은 펌웨어와 전수 대조했다. `LogPacket` 은 리틀엔디안 + CRC-8
poly 0x07 / init 0x00 / 15바이트 범위로 펌웨어와 일치하고, BMS/MPPT/Motor 의
모든 오프셋·스케일이 `types.h` 및 `timer_scheduler.c` 의 팩 순서와 맞는다.
TOP 의 PackCurrent 없는 슬롯 시프트도 정확히 반영돼 있다.

#### C-1 오류 보고 레이트 리밋 (`vcu_lcd_bridge.c`)

실측으로 재확인: 지상국 15초 수신 1378패킷 중 `SRC_CAN_ERR`/`ERR_CAN_TX_FAIL`
394건 = **28.6%** (앞서 추정한 28% 와 일치).

같은 `(키, 상세)` 조합은 `VCULCD_ERR_MIN_INTERVAL_MS`(1000ms) 안에서 한 번만
내보내고, **억제한 횟수를 `value[4..7]` 에 실어 보낸다.** 횟수를 버리지 않는
이유는 "몇 번 실패했는가"가 곧 심각도라서, 그걸 잃으면 한 번 튄 것과 계속
실패하는 것을 지상국에서 구분할 수 없기 때문이다. 지상국은 이를 합산해
`occurrences = count + suppressed` 로 실제 발생 횟수를 복원한다.

- 리미터를 **호출 경로마다 하나씩** 둬 잠금이 필요 없다. LCD→VCU 는
  `MCP_RX_Thread` 에서만, VCU→LCD 는 `VcuLcd_Thread_Entry` 에서만 호출되고,
  두 경로는 상세값 상위 바이트(0xB0/0xB1)부터 달라 같은 조합이 될 수 없다.
- 조합이 바뀌면 이전 조합의 억제분을 먼저 내보내고 넘어간다.
- **한계**: 고장이 아예 멎으면 마지막 억제분(최대 창 하나치 ≈ 10건)은 남는다.
  없애려면 주기 flush 가 필요하고 그러면 잠금이 필요해져, 되찾는 양에 비해 비싸다.

`ERR_CAN_TX_FAIL` 은 `ext_can_controller.c` 에서도 나오는데 페이로드 형식이
다르다. 그래서 지상국은 **상위 1바이트 태그로 좁혀서만** `value[4..7]` 을 억제
횟수로 읽는다 (0xB0/0xB1 = 브리지, 0xFF = TX FIFO 대기 초과, 그 외 = 드라이버
오류코드). 태그로 좁히지 않으면 `ERR_TIMEOUT` 의 경과 ms 가 억제 횟수로 둔갑한다.

#### 검증

합성 패킷 하네스(지상국 클래스 직접 호출)로 확인:

| 시나리오 | 기대 | 결과 |
|---|---|---|
| 수신 10건 / 억제 9씩 | 실발생 100 | 100 ✅ |
| `ERR_TIMEOUT` 경과 8500ms | 억제 0 | 0 ✅ |
| `ext_can_controller` 발 TX_FAIL 2종 | 억제 0, 문구 구분 | ✅ |
| 정상 데이터 키 회귀 (SOC 856‰) | 85.6% | 85.6% ✅ |

실 스트림 15초에서 1378패킷 디코드 / 실패 1건(포트 개방 시 잘린 첫 프레임).

펌웨어 빌드 통과 (경고 없음, RAM 7.07% / ROM 8.92%).
**보드 미연결 상태라 플래시·실동작 검증은 아직 안 했다.** 다음 보드 연결 시
`ERR_CAN_TX_FAIL` 비율이 28.6% → **약 3.8%** 로 떨어지는지 확인할 것.
(레이트 리밋이 1초에 1건이므로 유효 스트림 약 25패킷/s 기준 1/26 = 3.8% 가 하한이다.
 앞서 "1% 미만"이라 적었던 것은 오산 — 그 기준으로 보면 정상 동작을 실패로 오판한다.)


### D-1 무선 재연결 / 백로그 처리 (2026-09-02)

기존 결론은 "지상국이 상시 수신 상태를 유지해야 한다"는 운용 지침이었는데, 이건
불충분하다. **주행 중 지상국이 죽었다 살아나는 상황**이 실제로 있고, 그때 두 가지가
겹친다.

1. 모뎀/FTDI 버퍼에 쌓인 약 25초치 과거 데이터부터 흘러나온다.
2. 그 과거 데이터를 지상국이 방금 온 값으로 취급한다 — `lastUpdate` 를 도착 시각으로
   찍기 때문에 대시보드 신선도 판정이 **25초 전 속도를 현재값으로** 표시한다.
   주행 중에는 그냥 틀린 정보다.

#### 처리

| 항목 | 내용 |
|---|---|
| 백로그 배출 | 포트를 열면 `flushIOBuffers()` 로 드라이버 버퍼를 비우고, 회선이 `DRAIN_IDLE_MS`(120ms) 조용해질 때까지 읽어서 버린다. 그 정적이 곧 "현재를 따라잡았다"는 신호다. **리스너는 배출이 끝난 뒤에 붙인다** — 먼저 붙이면 백로그가 그대로 디코더로 들어간다 |
| 반쪽 프레임 | 배출의 부수 효과로 같이 해결된다. 정적 다음 첫 바이트는 반드시 새 프레임의 시작이라, 기존에 포트 개방마다 1건씩 나던 디코드 실패가 사라진다 |
| 자동 재연결 | 2초 주기 감시 스레드. 포트가 닫혔거나 `SILENCE_RECONNECT_MS`(15초) 무수신이면 닫고 다시 연다. USB 를 뽑았다 꽂아 핸들이 조용히 죽는 경우를 덮는다 |
| 지연 표시 | 차량 틱으로 링크 지연을 계산해 2초 이상이면 대시보드에 배너. 값을 지우지 않고 "지금이 아님"을 숫자로 못박는다 |
| 로그 도배 | 동글이 빠진 채 두면 2초마다 로그가 쌓인다(실측 50초에 73줄). 첫 회 + 이후 약 1분에 한 번만 남기도록 제한 → **90초에 3줄** |

#### 링크 지연 계산과 그 함정

`(지상국시각 - 차량틱)` 의 최솟값을 기준선으로 잡고, 현재 오프셋에서 빼서 밀린
시간을 얻는다. 기준선은 더 빠른 패킷이 오면 스스로 낮아지므로 초기값이 나빠도 자정된다.

문제는 **차량 리셋**이다. 틱이 0으로 돌아가면 오프셋이 껑충 뛰는데, 이건 자정되지
않아 가짜 지연이 영구히 남는다. 처음에는 "오프셋이 5분 넘게 뛰면 리셋" 으로 잡았으나
**틀렸다** — 점프 크기는 리셋 직전까지의 차량 가동 시간과 같아서, 2초만 켜졌다
리셋된 보드는 2초만 뛴다. 하네스가 이걸 잡아냈다.

그래서 크기가 아니라 **틱이 뒤로 갔다**는 사실로 판정한다. 리셋과 틱 랩(2^32ms,
약 49.7일)을 모두 덮는다. 임계값은 1초 — 펌웨어가 `PacketBuilder_Create` 직후 큐에
넣고 UART 스레드가 FIFO 순으로 빼므로 타임스탬프 역전은 밀리초 미만이라 오판 위험이
없고, 남는 가짜 지연을 경고 임계값(2초) 아래로 묶는다.

백로그 재생은 이 판정에 걸리지 않는다. 쌓인 순서대로 나오므로 틱이 뒤로 가지 않고,
**벽시계보다 느리게 진행할** 뿐이다.

#### 검증 (하네스 — 정체 모델)

| 시나리오 | lag | 결과 |
|---|---|---|
| 실시간 수신 | 0 ms | ✅ |
| 지상국 정체 1.5초 후 재개 | 1513 ms | ✅ |
| 백로그 800ms 소화 | 715 ms | ✅ |
| 백로그 1.3초 소화 | 216 ms | ✅ |
| 완전히 따라잡음 | 0 ms | ✅ |
| 차량 리셋(가동 2초 후) | 0 ms | ✅ 기준선 재설정 |
| 재연결 기록 | 1회 / 28450B 폐기 | ✅ |

실포트 검증: 동글 미연결 상태에서 2초 주기 재시도가 도는 것과, 로그가 90초에 3줄로
줄어든 것을 확인했다. **백로그 배출 자체는 동글이 있어야 확인된다** — 다음 보드 연결
시 `백로그 N바이트 폐기` 로그의 N 을 볼 것.

#### 참고: 로그 한글 인코딩

로그 스트림은 CP949 로 나간다(한국어 Windows 콘솔 기본값). 한국어 콘솔에서는 정상
표시되지만 UTF-8 로 읽으면 깨진다. CP949 에 없는 문자는 `?` 로 유실되므로 **로그
문자열에는 em-dash(—) 대신 하이픈을 쓴다.** 소스 주석은 UTF-8 이라 무관하다.


### 2차 점검에서 찾은 결함 (2026-09-02)

오늘 작업분을 다시 훑으면서 나온 것들. 앞의 1차 점검(문구 고정 / 누적 폐기량)에
이어 **동작에 영향을 주는 것 4건**이 더 나왔다.

| # | 결함 | 영향 | 처리 |
|---|---|---|---|
| 1 | 지연 배너가 **절대 표시되지 않음** | `.f1-laglag` 클래스에 `display:none` 을 두고 JS 는 `style.display=''`(인라인 해제)로 켜려 했다. 인라인을 지우면 클래스 규칙으로 되돌아가 계속 숨겨진다. D-1 의 핵심 산출물이 통째로 죽어 있었다 | 클래스에서 `display:none` 을 빼고 요소 인라인으로 옮김(`f1-sys-detail` 과 같은 방식). 같은 실수가 다른 요소에 없는지 전수 검사함 |
| 2 | `serialPort` 가 non-volatile인데 여러 스레드가 접근 | 자동 재연결이 생기면서 **쓰는 스레드가 늘었다.** `checkLink`/`isPortOpen`/`stop` 은 락 없이 읽는다. 이미 다시 연 포트를 계속 null 로 보면 **불필요한 재연결**(백로그 폐기 + 지연 기준선 초기화)이 일어난다 | `volatile` 로 변경. `consecutiveFailures` 도 같은 이유로 volatile |
| 3 | `stop()` 이 비동기화 | 감시 스레드가 `openPort()` 한가운데(배출은 최대 3초)일 때 종료하면, 배출이 끝난 뒤 포트가 **다시 열린 채 남는다.** COM 포트는 핸들 하나뿐이라 컨텍스트 재기동 시 다음 open 이 실패한다 — 지상국을 다시 띄워도 수신이 안 되는 상태 | `synchronized` + `closePort()` 재사용(리스너까지 제거). `checkLink` 의 재개방 직전에도 `shuttingDown` 재확인 |
| 4 | 배출이 상한에 걸려도 조용함 | `DRAIN_MAX_MS` 까지 읽었는데 회선이 안 조용해지면 "지금부터 실시간" 전제가 깨진 것이다. 이후 지연 기준선도 밀린 채로 잡혀 **지연이 실제보다 작게** 표시된다 | 따라잡지 못한 경우를 구분해 경고. 정상 스트림에서 이 경고가 뜨면 `DRAIN_IDLE_MS` 가정을 재계산하라는 안내 포함 |

#### 기계적으로 검증한 것

- **`key >= 0x80` 규칙의 안전성**: 데이터 키 최댓값은 `KEY_BMS_FET_STATUS = 0x33`,
  SYS_KEY 는 0~15. 0x80 이상은 없다 → 오류 키 판정이 데이터 키를 삼킬 일이 없다.
- **`LogErrorKey` 18개 값**: 펌웨어 `ERR_KEY` enum 을 스크립트로 뽑아 대조 → 완전 일치.
- **시각 기준**: `renderErrorLog`/`renderLinkLag` 가 쓰는 `now` 는 `dto.serverTime`
  (서버 시계)이고 `lastSeen`/`linkLastReconnectAt` 도 서버 시계라 시계 편차 문제 없음.
- **락 순서**: `this` → `frameBuffer` → snapshot 으로 일관. 순환 없음.

#### 수치 정정

`ERR_CAN_TX_FAIL` 의 수정 후 예상 비율을 "1% 미만"이라 적었던 것은 **오산**이다.
레이트 리밋이 1초에 1건이므로 유효 스트림 약 25패킷/s 기준 1/26 = **약 3.8%** 가
하한이다. 1% 를 기준으로 보면 정상 동작을 실패로 오판한다.

#### 검증 못 한 것

이 PC 에 COM 포트가 하나도 없어(`GetPortNames()` 빈 배열) **포트 개방/종료/재개방
경로는 실측하지 못했다.** 위 2·3번은 코드 근거에 의한 수정이며 하드웨어 확인이 필요하다.
다음 보드 연결 시 볼 것:

1. `백로그 N바이트 폐기` 의 N — 정상 스트림인데 수 KB면 `DRAIN_IDLE_MS` 가정이 빗나간 것
2. `배출이 상한에 걸렸다` 경고가 뜨는지
3. 지상국을 껐다 켰을 때 2회차가 포트를 정상적으로 잡는지 (3번 결함)
4. 지연 배너가 실제로 화면에 뜨는지 (1번 결함)


### C-2 해결 확정 — byte4~7 손상 (2026-09-02)

구성: 송신보드 + BMS 더미 1대(Bus A) + LCD 보드(Bus B). RFD900x 미연결.
release 빌드 플래시 후 OpenOCD `mdw` 로 비침습 관측(코어 정지 없음).

#### 결과

`Conf_MCP251XFD.h` 의 `MCP251XFD_TRANS_BUF_SIZE` 16 -> 18 수정이 **실물에서 확인됐다.**

BMS 더미는 SOC 를 `tri_wave(seq, 1200, 150, 980)` 로 만든다(150~980 permille).
130초(한 주기 이상) 추적:

| 값 | 측정 범위 | 설계 범위 |
|---|---|---|
| `VehicleData.bms_bot.soc_permille` | 156 ~ 973 | 150 ~ 980 |
| `0x501` payload[4..5] | 155 ~ 975 | 〃 |

LCD 화면도 같은 구간(20% 부근 바닥 포함)을 따라 움직이는 것을 눈으로 확인했다.
수정 전 증상은 "byte4~7 이 매번 랜덤, 서로 다른 두 샘플이 비트 단위로 동일"
(= 정지된 버퍼를 읽음)이었으므로, 파형을 따라간다는 것 자체가 종단 확인이다.

#### 함께 확인된 것

| 항목 | 값 | 판정 |
|---|---|---|
| `0x501` 송신 | 130초에 성공 1286 / 실패 0 (9.9/s) | 100ms 주기 정확 |
| 발행 간격 | last 101ms / min 101 / max 108 | 지터 작음 |
| MCP TEC/REC | 0 / 0 (`g_mcp_err_reads` 증가 중 = 실측된 0) | LCD 가 ACK 중 |
| Bus B 수신 | `ID=0x510 DLC=8` | LCD -> 보드 방향도 동작 |
| 타임아웃 마스크 | `0xFC` (MPPT1~4·MOTOR·ODO) | BMS BOT/TOP 만 연결한 구성과 일치 |
| C-1 리미터 | `err_lim_to_lcd`/`err_lim_to_vcu` 둘 다 valid=0, 억제=0 | LCD 연결 시 무개입 — 예상대로 |

payload[0..3] = 0 은 모터 더미 미연결이라 정상이다(속도·전류 0).
Bus A 중계(0x510 -> 0x420)도 실패 0 인데, CAN ACK 은 ID 가 아니라 버스의 다른
노드가 주므로 BMS 더미가 ACK 해서 성공한다.

#### 측정 방법에서 걸렸던 함정

`VehicleData.soc` 와 `payload[4..5]` 가 5~6 permille 차이 나서 불일치로 보였다.
읽는 순서를 뒤집자 **차이의 부호가 그대로 뒤집혔다**(+4/+2/+3 -> -6/-3/-5).
mdw 는 한 번마다 텔넷 왕복이 있어 두 값이 서로 다른 시각의 스냅샷이고,
SOC 가 초당 약 18 permille 로 움직이므로 왕복 지연만큼 차이가 난다. 값 불일치가
아니다. 램프 중인 신호를 순차로 읽을 때는 항상 이 검사를 먼저 할 것.

#### 도구 주의 (미수정)

`tools/inspect.sh` 의 `watch`/`busb` 는 주소를 `build/debug` ELF 에서 뽑는데
실제로 굽는 것은 `build/release` 다. release 는 `-g` 가 없어 DWARF 자체가 없고
두 빌드는 변수 배치가 다를 수 있다. **오래된 debug ELF 로 새 release 이미지를
읽으면 조용히 엉뚱한 주소를 읽는다.** 이번에는 구운 ELF 에서 직접 심볼을 뽑아
우회했다. 구조체 오프셋만 debug 에서 얻고 베이스는 release 에서 얻으면 안전하다.

#### 남은 항목

- **D-1**(백로그 배출/재연결/지연 배너): RFD900x 동글이 없어 미검증
- **C-3**(S4 전부하 재검증): MPPT 더미 필요


### 부팅 LED 미점등 — 원인 및 수정 (2026-09-02)

증상: 정상 동작 중인데 LED_BOARD_CHECK(PE12)가 켜지지 않는다.

#### 레지스터 관측

| 항목 | 값 |
|---|---|
| `SystemState` | can_ok=1 mcp_ok=1 uart_ok=1 sd_ok=0 error=0 |
| `PHASE5_SD_LOGGING` | 0 → sd_ok 는 healthy 식에 안 들어감 |
| GPIOE MODER | PE12=1(출력) — 설정 정상 |
| GPIOE ODR | **0x00000000** — PE12 소등 |

즉 `healthy = can_ok && mcp_ok && uart_ok = 1` 인데 LED 가 꺼져 있었다.

#### 원인 — 갱신 호출 누락

부팅 순서가 이렇다:

| 순서 | 위치 | 동작 | 그때 healthy |
|---|---|---|---|
| 1 | `main.c:213` CAN_Receiver_Init | can_ok=1 (refresh 없음) | — |
| 2 | `main.c:219` ExtCAN_Init | mcp_ok=1 + **refresh** | uart_ok=0 이라 **0** |
| 3 | `main.c:249` | **refresh** | 여전히 uart_ok=0 → **0** |
| 4 | `main.c:257` → `app_threadx.c:190` UART_Transmitter_Init | uart_ok=1 **(refresh 없음)** | — |

4번 이후로 아무도 다시 계산하지 않는다. 나머지 `SystemState_RefreshBoardLed()`
호출부는 전부 "오류 발생 / 오류 회복" 경로라, **정상 보드에서는 한 번도 타지 않는다.**
→ 고장이 한 번도 없는 보드일수록 부팅 LED 가 영영 안 켜진다.

`can_receiver.c:653` 주석이 `can_ok` 에 대해 똑같은 함정을 경고해 두었는데
(`한 번도 오류가 없는 정상 보드에서 ... LED_BOARD_CHECK가 절대 켜지지 않는다`),
`uart_ok` 쪽 refresh 호출에서 같은 실수가 났다.

#### 수정

`uart_transmitter.c` 의 `UART_Transmitter_Init()` 에서 `SystemState.uart_ok = 1;`
직후 `SystemState_RefreshBoardLed()` 를 호출한다. 이 함수는 커널 진입 전
`App_ThreadX_Init` 에서 실행되므로 GPIO 쓰기에 문제가 없다.

**검증(실측)**: 재빌드·플래시 후 `GPIOE_ODR = 0x00001000` (PE12=1)로 점등 확인.
Bus B 회귀 없음 — 0x501 송신 성공 +88/9초(실패 0), 발행간격 101/101/108ms 유지.

#### 곁가지로 드러난 것 — MPPT 더미가 버스에 살아 있다

앞의 C-2 기록에서 타임아웃 마스크 `0xFC`(MPPT1~4·MOTOR·ODO)를 두고 "BMS 만
연결한 구성과 일치한다"고 적었는데, **그 뒤 관측에서는 `0xC0`(MOTOR·ODO 만)이다.**
확인 결과 MPPT 4채널 전부 `last_tick` 경과 0ms 로 갱신되고 `in_v` 가 살아 움직인다
(89.40 / 91.27 / 82.17 / 72.83 V, 삼각파). BMS 더미는 0x040~0x07F 만 쓰고 MPPT ID 를
전혀 보내지 않으므로(코드 확인), **별도의 MPPT 소스가 CAN 버스에 물려 있다.**
USB 허브에는 ST-Link 가 2개(송신보드 동글 `7`, BMS `066DFF...5014`)뿐이라
MPPT 더미는 서플라이 전원만 받고 있는 것으로 보인다.

→ 한 시점의 스냅샷으로 "구성과 일치"라고 단정했던 것이 성급했다. 타임아웃 마스크는
   시간에 따라 바뀌므로 구성 판정에 쓸 때는 여러 번 읽어야 한다.

#### 근본 원인 — Phase 4 활성화가 부팅 LED 를 깨뜨렸다 (정정 추가)

"왜 갑자기 안 켜지나"의 답은 MPPT 더미도, 부팅 실패도 아니다.
`PHASE4_UART_PIPELINE` 이 **2026-09-01 커밋 `fd501bc` 에서 0 -> 1** 이 되었다.

| PHASE4 | healthy 식 | main.c:249 refresh 시점의 결과 |
|---|---|---|
| 0 (~08-31) | `can_ok && mcp_ok` | 둘 다 이미 1 -> **LED 점등** |
| 1 (09-01~) | `can_ok && mcp_ok && uart_ok` | uart_ok 는 아직 0 -> **LED 소등** |

`uart_ok` 만 유일하게 **마지막 refresh 이후에** 세워지는 플래그다
(`UART_Transmitter_Init` 은 `MX_ThreadX_Init` 안에서 실행). 그래서 Phase 4 를
켜는 순간, 다른 코드를 하나도 안 건드렸는데 부팅 LED 가 죽었다.

게이트를 하나 켜면 healthy 식의 항이 늘어나는데, 그 항이 언제 세워지는지는
게이트와 무관하게 정해져 있다 — 이 조합을 확인하지 않은 것이 원인이다.

Phase 4 이후에도 가끔 켜져 보였다면 그건 **고장->회복 사이클** 때문이다.
UART 큐 Full(`timer_scheduler.c:152`)이나 CAN RX FIFO 오버플로가 나면 플래그가
0 으로 내려갔다가 회복 경로에서 1 로 오르며 refresh 를 부른다. 즉 어제까지는
"뭔가 잠깐 잘못돼야 LED 가 켜지는" 상태였다. 디버거로 코어를 세우는 것만으로도
CAN 오버플로가 나므로 점검 중에는 쉽게 켜졌다.

#### 앞의 MPPT 기록 정정

바로 위 절에서 "MPPT 더미가 USB 허브에 없고 서플라이 전원만 받는 것으로 보인다"고
적은 것은 **틀렸다.** USB 재열거 결과 세 대가 모두 붙어 있다:

```
066DFF485688494867114316  MPPT 더미
066DFF555185754867225014  BMS 더미
7                          송신보드 동글
```

세션 초반 열거에는 MPPT 가 없었고(그래서 마스크가 0xFC 였다) 측정 도중에
연결되었다. 마스크는 각 시점에 정확했고, 바뀐 것은 구성 쪽이다.


### C-2 완결 — 0x501 8바이트 전체 검증 (2026-09-02)

앞의 C-2 확인은 **byte4~5(SOC)만** 증명한 반쪽이었다. byte0~3(속도·전류)과
byte6~7(전력)은 모터 더미가 없어 전부 0 이라, "0 이 제대로 간 것"인지 "안 써져서
0"인지 구분되지 않았다. 원래 증상이 "byte4~7 이 안 써진다"였으므로 **byte6~7 은
여전히 미검증 상태**였다.

MPPT 더미 보드(SN 066DFF485688494867114316)에 Motor 더미 펌웨어를 굽고
(개발보드 2대로 BMS + Motor 구성) 다시 측정했다.

| 원본(VehicleData) | | | | 0x501 페이로드 | | | |
|---|---|---|---|---|---|---|---|
| 속도 | 전류A | 전압V | SOC | 속도 | 전류A | SOC | 전력W |
| 14.59 | -16.5 | 124.54 | 727 | 14.08 | -16.1 | 720 | -1990 |
| 0.58 | -1.1 | 122.51 | 495 | 0.25 | -0.4 | 489 | 0 |
| 9.67 | 20.0 | 121.04 | 325 | 10.00 | 22.0 | 319 | 2732 |

전력 검산: 124.54 x (-16.1) = -2005W vs 실린 값 -1990W / 121.04 x 22.0 = 2663W vs
2732W — 모두 읽기 시차 범위 안이다.

**8바이트 전부 확인됐다**:

| 바이트 | 필드 | 확인 내용 |
|---|---|---|
| 0-1 | veh_speed | 0.25 ~ 14.59 m/s 램프 |
| 2-3 | bus_i | -16.5 ~ +22.0 A, **회생 음수 정상**(i16 부호) |
| 4-5 | soc_permille | 325 ~ 727 |
| 6-7 | motor_power_w | -1990 ~ +2732 W, **부호 반전 지점(전류 0 근처에서 0W) 정상** |

타임아웃 마스크 `0x3C`(MPPT1~4만) — BMS·MOTOR·ODO 전부 살아 있어 구성과 일치.
0x501 송신 11908건 / 실패 0.

→ `MCP251XFD_TRANS_BUF_SIZE` 16 -> 18 수정이 **8바이트 전 구간에서 확인**됐다.
   C-2 종료.

#### 측정 함정 (2번째) — 워드 경계

첫 측정에서 원본 속도·전류가 0 으로 나와 불일치처럼 보였다. 원인은 펌웨어가
아니라 읽기 방식이었다. `MotorData_t` 에서

```
bus_v     +624  (워드 정렬)
bus_i     +626  (+2)
motor_rpm +628  (워드 정렬)
veh_speed +630  (+2)
```

이고 OpenOCD `mdw` 는 정렬된 워드만 읽는다. +626/+630 을 직접 주면 엉뚱한 값이
나온다. 정렬된 워드를 읽어 상위 16비트를 꺼내야 한다:

```
word@624 = [bus_v | bus_i<<16]
word@628 = [motor_rpm | veh_speed<<16]
```

u16 필드를 mdw 로 읽을 때는 항상 오프셋 %4 를 먼저 확인할 것. 앞의 SOC 시차
함정과 함께, **관측 도구의 오류를 펌웨어 결함으로 오인하기 쉬운 두 번째 사례**다.


### C-3 (S4) — Phase 4 부하에서 버스 포화 통과 (2026-09-02)

구성: 송신보드 + BMS 더미 + MPPT 더미(`MPPT_BURST_MODE=1`) + LCD(Bus B).
`PHASE4_UART_PIPELINE=1`, `PHASE5_SD_LOGGING=0`. RFD900x 미연결.
관측은 전부 OpenOCD `mdw`(코어 정지 없음) — 디버거로 세우면 관측 행위가 유실을
만들어 예전 S4 오진의 원인이 됐다.

#### 자극 확인이 먼저였다

Bus A 수신 프레임 카운터가 없어서 **"자극이 실제로 걸렸는지" 확인할 수단이
없었다.** 통과 여부보다 이게 먼저라, Bus B 의 `g_mcp_rx_frames` 와 대칭으로
`g_can_rx_frames` 를 추가했다(FDCAN ISR 드레인 루프에서 프레임당 1회 증가,
단일 워드라 임계구역 불필요). 영구 유지한다 — 부하를 밖에서 셀 유일한 수단이다.

| 조건 | Bus A 수신 |
|---|---|
| 평시 (BMS + MPPT) | **208 프레임/s** |
| 버스트 | **4178 프레임/s** (평시의 20배) |

Phase 2 기록의 4172 프레임/s 와 일치한다. 포화가 실제로 걸렸음이 확인됐다.

#### 결과 — 60초 포화

| 항목 | 평시 | 포화 |
|---|---|---|
| TEC / REC / LEC | 0 / 0 / 0 | **0 / 0 / 0** |
| RX FIFO 최대 점유 | 0~1 / 3 | **0 / 3** |
| FIFO Full | 0 | **0** |
| 메시지 유실(RF0L) | 0 | **0** |
| `can_ok` / `uart_ok` | 1 / 1 | **1 / 1** |
| LED_CAN / LED_RFD900x | 소등 | **소등** |
| 0x501 발행 간격 | 101~107ms | **101~107ms** |

**유실 0, 오류 0, 중계 주기 무영향.** RX FIFO 점유가 포화 중에도 0 으로 읽힌 것이
핵심이다 — ISR 이 프레임 도착보다 압도적으로 빠르다는 뜻이고, Phase 2 에서
산술로 예측한 그대로다(500kbps 8바이트 프레임 약 240us = 160MHz 에서 약 38,000
사이클, 파싱은 수백 사이클).

즉 **"3칸 FIFO 가 포화에서 넘칠 것"이라는 원 계획서의 가정은 Phase 4 부하에서도
성립하지 않는다.** LED_CAN 점등을 기대한 것은 그 가정에 기반한 것이었다.
유실 검출 경로 자체는 별도로 동작이 확인돼 있다(디버거 정지로 실제 유실 유발 시
점등, 무오류 3초 후 소등).

#### 아직 안 덮인 조건

이번 시험은 Phase 4 의 **큐 + UART DMA** 까지만 걸었다. 남은 것:

- **RFD900x 흐름제어**: 모뎀이 없어 UART 가 멈추지 않고 계속 빠진다. 실제로는
  CTS 로 송신이 막히면 UART_Queue 압박이 이번보다 커진다. 원 계획서가 경고한
  "전체 부하"의 핵심 축이 이것이다.
- **SD 기록**: `PHASE5_SD_LOGGING=0` 이라 SD 경로 부하가 없다.

따라서 이번 결과는 **"CAN 수신 경로는 포화에서 안전"** 까지를 확정하고,
큐 포화 쪽은 RFD 연결 후 한 번 더 볼 필요가 있다. 버스트 모드는 규정대로
0 으로 되돌려 다시 구웠고, 평시 196 프레임/s 로 복귀를 확인했다.


### D-1 실물 검증 — 배출 판정 결함 발견·수정 (2026-09-02)

RFD900x 연결(COM9, FTDI). 구성: 송신보드 + BMS/MPPT 더미 + LCD + 모뎀.

#### 결함 — "정적이면 따라잡은 것" 판정이 틀렸다

배출 완료 판정을 `DRAIN_IDLE_MS`(120ms) 무데이터로 잡았고, 근거는 펌웨어
발행 스케줄의 최소 정적 174ms 였다. **RFD900x 를 계산에 넣지 않은 것이 오류였다.**

실측(60초 무수신 후 포트 개방, 10ms 간격 샘플링):

| 시각 | 도착 |
|---|---|
| 47ms | 첫 바이트 |
| 87~259ms | **172ms 동안 0바이트** |
| 259ms | 2185바이트 (백로그 계속) |
| 700ms 누적 | 7479바이트 |

모뎀이 공중 구간을 자체 패킷화하고 듀티 사이클이 걸려 있어 **백로그를 쏟아내는
중에도 100~200ms 공백이 생긴다.** 120ms 임계값은 그 공백에서 빠져나와, 남은
약 6800바이트를 실시간 데이터로 넘겼다 — 이 기능이 막으려던 바로 그 상황이다.
공백 길이는 링크 상태에 따라 달라지므로 임계값을 키우는 것으로는 해결되지 않는다.

앞선 기록에서 "RFD900x 가 자체 패킷화로 스트림을 뭉갤 가능성"을 위험으로 적어
두고도 임계값은 펌웨어 스케줄만 보고 정했다. 위험을 적는 것과 설계에 반영하는
것은 다른 일이다.

부수적으로, `flushIOBuffers()` 가 버린 양을 세지 않아 로그에 항상 "0바이트
폐기"로 찍히던 문제도 함께 고쳤다(먼저 `bytesAvailable()` 로 세어 합산).
다만 실측 결과 **포트를 여는 순간 OS 가 드라이버 버퍼를 이미 비우므로** 그 값은
보통 0 이고, 진짜 백로그는 그 뒤 모뎀에서 밀려 나온다.

#### 수정 — 데이터 자신의 시각으로 판정

정적 시간 대신 **차량 타임스탬프가 벽시계보다 얼마나 빨리 흐르는가**를 본다.
백로그 재생 중에는 19초치를 1초에 소화하므로 비가 1 을 크게 웃돌고, 따라잡으면
1 로 수렴한다. 노드 수·바이트레이트와 무관하게 스스로 보정된다.

```
DRAIN_CATCHUP_RATIO 1.5    (차량시간 증가분 / 벽시계 증가분)
DRAIN_CHECK_MS      300    평가 창
DRAIN_QUIET_MS      500    패킷이 아예 없으면 밀린 것도 없다
DRAIN_MAX_MS        5000   상한(= 판정 실패 시 최악의 실시간 손실량)
```

배출 단계에서 COBS+CRC 까지 확인해 타임스탬프만 꺼내고(`peekTimestamp`),
스냅샷에는 반영하지 않는다.

**검증(같은 조건 재현)**: 60초 무수신 후 기동 →
`백로그 배출 완료 - 3968바이트/208패킷을 버리고 현재 시점부터 수신 (676ms 소요)`.
수정 전 같은 조건에서 0바이트였다.

#### 자동 재연결 — 실제 탈착으로 검증됨

측정 중 사용자가 동글을 뽑았다 꽂았고, 그것이 그대로 시험이 됐다.

| 시각 | 동작 |
|---|---|
| 16:21:59 | 15초 무수신 감지 → 재연결 시도 (WARN 1줄) |
| ~16:22:56 | 2초 주기 재시도 28회, **ERROR 로그는 1줄만**(억제 동작) |
| 16:22:56 | **자동 복구** — "포트 복구됨 (28회 실패 후)" |
| 이후 | 추가 경고 없음 = 수신 지속(워치독 미발동) |

`SILENCE_RECONNECT_MS`(15초) 감지, 재시도, 로그 억제, 자동 복구가 모두 실물에서
동작했다. 프레임버퍼 오버플로 0.

#### 남은 것

- **지연 배너**: 배출이 제대로 동작하면 기동 직후 지연이 0 이라 배너가 뜰 일이
  거의 없다(정상). 로직 자체는 하네스로 전 경로 검증됐고 표시 버그도 고쳤으나,
  화면에 실제로 뜨는 것은 아직 못 봤다. 대시보드 로그인이 필요하다.
- **전부하 S4**: 모뎀 흐름제어까지 걸린 조건에서 버스트 재시험.


### 장치 고장 비트에 이력이 없던 문제 (2026-09-02)

사용자 보고: "MPPT 에러가 떴다 꺼졌다 반복한다", "오류가 떠도 기록이 안 된다".

#### 반복 점멸은 정상 — 더미가 일부러 만드는 값

`mppt_dummy.c:110`:

```c
d[3] = ((ph % 60u) == 59u) ? 0x40u : 0x00u;   /* error flags (일시적) */
```

주석에 목적이 적혀 있다 — "잠깐 떴다 사라지는 비트가 스케줄러의 sticky OR 로
실제 포착되는지 확인할 수 있게" 한 것이다. 실측 결과 **MPPT3(unit 2)만** 8~12초
간격으로 `0x40` 이 떴다 사라진다. `ph = seq + unit*37` 이고 slow 프레임 사이에
seq 가 5씩 진행하므로, 위상 오프셋상 unit 2 만 `ph%60==59` 에 걸린다.

즉 화면의 점멸은 고장이 아니라 **sticky 메커니즘이 제대로 동작한다는 증거**다.

#### 기록이 안 되던 것은 진짜 빈틈 — 수정함

A-2 에서 만든 오류 로그 패널은 `key >= 0x80`(ERR_KEY) 패킷만 담는다. 그런데
MPPT `errorFlags`, 모터 `errorFlags`/`limitFlags`, BMS `faultFlags`/`alarm` 은
**평범한 데이터 필드**로 와서 배지로만 표시되고, sticky 주기(5초)가 지나면
지워진다. 주행 중 새벽에 한 번 튄 고장은 아무도 안 보고 있으면 흔적이 없다.

→ 비트가 서 있는 채로 도착하면 같은 이벤트 목록에 남기도록 했다.
ERR_KEY 와 섞이지 않게 **0x100 이상의 의사 키**를 쓴다(실제 CAN 키가 아니다).

| 의사 키 | 대상 | 레벨 |
|---|---|---|
| 0x101 | MPPT 고장 플래그 | ERROR |
| 0x102 | 모터 고장 플래그 | ERROR |
| 0x103 | 모터 출력 제한 | WARNING |
| 0x104 | BMS Global Fault | ERROR |
| 0x105 | BMS Alarm | WARNING |

sticky 값이라 고장이 지속되면 DETAIL 주기마다 한 번씩 기록된다 —
**count 가 곧 "그 비트가 관측된 주기 수"** 이므로 일시적/지속적 구분에 쓸 수 있다.
비트 패턴이 다르면 별도 행으로 남는다.

#### 검증 (하네스 7항목 전부 통과)

| 항목 | 결과 |
|---|---|
| 플래그 0 이면 기록 안 함 | ✅ 정상 상태를 오염시키지 않음 |
| MPPT3 0x40 기록 (소스/키/상세/레벨) | ✅ |
| 다음 주기 0 으로 돌아가도 이력 유지 | ✅ |
| 같은 비트 재발 시 횟수만 증가 | ✅ 3회 |
| 다른 비트 패턴은 별도 행 | ✅ |
| 모터 Error=ERROR / Limit=WARNING | ✅ |
| ERR_KEY 이벤트와 공존 | ✅ |


### 백로그 배출 — 두 번 더 틀리고 고침 (2026-09-02)

앞 절에서 "타임스탬프 진행률 기준으로 바꿔 해결"이라고 적었으나 **성급했다.**
숫자가 0 -> 3968바이트로 바뀐 것만 보고, 실제로 따라잡았는지는 확인하지 않았다.

#### 진단 — 리스너가 본 첫 타임스탬프로 판정해야 했다

DEBUG 로 수신 패킷을 찍어 보니 벽시계 25ms 안에 보드 시각이
`6050 -> 275050 -> 315050` 으로 **305초를 건너뛰었다.** 배출이 3968바이트만 버리고
빠져나온 직후, 남은 백로그가 리스너로 통째로 쏟아진 것이다.

올바른 판정 기준은 **"배출 직후 리스너가 본 첫 패킷의 타임스탬프가 현재 보드
시각과 맞는가"** 다. OpenOCD 로 기동 직전 `uwTick` 을 읽어 비교하면 된다.

#### 두 번의 오류

| 시도 | 기준 | 왜 틀렸나 |
|---|---|---|
| 1차 | 정적 120ms | RFD900x 듀티 사이클로 재생 중에도 172ms 공백이 난다 |
| 2차 | 정적 500ms + 진행률 | 정적 판정에만 `sawReplay` 를 반영하고 **진행률 판정에는 빠뜨렸다.** 모뎀이 쉬는 창은 `dVeh=0` -> 진행률 0 -> 그 한 창으로 빠져나옴 |

2차에서 매번 정확히 `3968바이트/208패킷/655ms` 로 동일하게 멈춘 것이 단서였다.
같은 종류의 실수(정적을 따라잡음으로 오인)를 임계값만 바꿔 세 번 반복했다.

#### 최종 — 표본이 충분한 창에서만 판정한다

```
DRAIN_CATCHUP_RATIO         1.5   차량시간 증가분 / 벽시계 증가분
DRAIN_CHECK_MS              300   평가 창
DRAIN_MIN_FRAMES_PER_WINDOW 5     이 미만이면 판정 보류(모뎀이 쉬는 창)
DRAIN_QUIET_MS              500   단, 아직 재생을 본 적 없을 때만 종료 근거
DRAIN_MAX_MS              15000   상한
```

핵심은 **표본 부족한 창을 판정에 쓰지 않는 것**이다. 모뎀이 쉬는 구간은
"따라잡았다"가 아니라 "판정할 수 없다"로 다룬다.

#### 검증 (150초 백로그)

| | 수정 전 | 수정 후 |
|---|---|---|
| 배출량 | 3968B / 208패킷 / 655ms | **8743B / 459패킷 / 1262ms** |
| 리스너 첫 패킷 ts | 476212 (보드 664592) → **188초 유출** | **948684 (보드 941798)** |
| 판정 | 실패 | **차이 -6886ms = 현재 시점** (7초는 OpenOCD 읽기+기동 시간) |

### C-3 전부하 S4 — 최종 (2026-09-02)

구성: 버스트(4175 프레임/s) + RFD900x + **지상국 실수신**.
앞선 시도에서 "지상국 수신"이라고 라벨을 붙였으나 COM9 를 아무도 안 잡고 있었다.
COM9 를 직접 열어 보는 것으로 점유를 확인한 뒤 다시 돌렸다.

| 항목 | 결과 |
|---|---|
| Bus A 수신 | **4175 프레임/s** (평시 181/s) |
| TEC / REC / LEC | 0 / 0 / 0 |
| RX FIFO 최대 점유 | **0 / 3** |
| FIFO Full · 메시지 유실 | 0 |
| `can_ok` / `uart_ok` | 1 / 1 (**UART 실패 없음**) |
| LED_CAN / LED_RFD900x | 소등 유지 |
| 0x501 발행 간격 | 101~110ms |
| 지상국 디코드 | 60초에 1654패킷, **실패 0** |

스케줄러가 시간 구동이라 CAN 부하가 늘어도 UART 생산량은 그대로다. 따라서 이
시험이 실제로 본 것은 **FDCAN ISR(우선순위 1)이 UART 스레드(우선순위 5)를
굶기는가**이고, 답은 아니다 — 포화에서도 `uart_ok=1` 이 유지됐고 지상국 디코드
실패가 0 이었다. C-3 종료.


### 배출 로직 잔여 조기 종료 경로 제거 + inspect.sh 주소 해석 수정 (2026-09-02)

#### 배출 — 네 번째이자 마지막 수정

점검 중 표본 부족 창에서 빠져나오는 경로가 **아직 남아 있는 것**을 찾았다.

```java
} else if (!sawReplay) { caughtUp = true; break; }   // ← 제거
```

실측상 포트 개방 후 첫 바이트가 47ms 에 오고 곧 172ms 공백이 있으므로,
**첫 300ms 창에 1~4패킷만 잡히면 백로그가 남아 있어도 종료**된다.
"정적을 따라잡음으로 오인"하는 같은 실수의 네 번째 형태였다.

→ 표본이 모자라면 **창을 끊지 않고 늘린다.** 창 길이와 표본 수가 둘 다 찼을 때만
   진행률을 판정한다. 데이터가 아예 없는 경우는 `DRAIN_QUIET_MS` 가 처리한다.

**회귀 검증(2종)**

| 시나리오 | 배출 | 리스너 첫 ts vs 보드 |
|---|---|---|
| 즉시 재시작 | 8971B / 471패킷 / 1782ms | +13.5s (기동 소요) = 현재 |
| 120초 무수신 | 9085B / 477패킷 / 1305ms | +8.3s = 현재 |

#### tools/inspect.sh — 굽는 이미지에서 주소를 푼다

`watch`/`busb` 가 주소를 `build/debug` ELF 에서 뽑는데 `build.ps1` 은 release 를
굽는다. 두 빌드는 변수 배치가 달라, 낡은 debug ELF 로 새 release 이미지를 읽으면
**조용히 엉뚱한 주소를 읽는다.** 실제로 이번에 `uwTick` 이 8바이트 옮겨져 값이
상수처럼 보였다(전역 하나를 추가한 것만으로).

수정:
- 주소는 굽는 이미지(release)의 **심볼표에서 nm 으로** 뽑는다. release 는 `-g` 가
  없어 DWARF 는 없지만 심볼표는 남아 전역/파일정적이 그대로 읽힌다.
- 구조체 멤버만 debug ELF 에서 **오프셋**을 얻어 release 베이스에 더한다
  (구조체 정의는 같은 헤더라 두 빌드에서 동일하다).
- `busb` 는 전부 전역/파일정적이라 debug ELF 없이 nm 만으로 풀린다.
- debug ELF 가 release 보다 오래되면 참고 경고를 낸다.
- `WSC_ELF` 로 대상 ELF 를 바꿀 수 있다.

실행 확인: `busb` `watch` 둘 다 정상값(BMS/MPPT 수신 중, MOTOR 미연결로 경과=uwTick).


### Phase 5 (SD 로깅) 게이트 활성화 (2026-09-02)

task 14~16 은 이미 구현돼 있었다(rtc_manager.c / sd_logger.c / app_filex.c 배선,
CubeMX 의 `while(1)` 데드락 제거까지). 남은 것은 task 17 체크포인트였다.

#### 켜기 전에 잡은 결함 — ERR_TIMEOUT 소스 충돌

`ERR_TIMEOUT` 을 **세 곳이 모두 `SRC_SYS` 로** 내는데 페이로드 의미가 다르다:

| 위치 | value[0..3] | value[4..7] | 뜻 |
|---|---|---|---|
| timer_scheduler | 죽은 노드 ID | 경과 ms | 노드 무응답 |
| sd_logger | 연속 타임아웃 횟수 | 0 | SD 큐 무데이터 |
| uart_transmitter | 연속 타임아웃 횟수 | 0 | UART 큐 무데이터 |

지상국은 `SRC_SYS` 면 노드 무응답으로 해석하므로, SD 큐 타임아웃(횟수 5)이
`LogSource.name(5)` = **"MPPT3 무응답 0ms"** 로 표시된다. UART 쪽은 PHASE4 가
켜져 있어 **이미 잠재된 버그**였다.

→ 펌웨어: 각자 자기 소스(`SRC_SD_ERR` / `SRC_UART_ERR`)로 낸다.
→ 지상국: `LogErrorKey.name(key, source)` 오버로드로 큐 타임아웃은 "큐 무데이터"
   로 표시하고, 상세는 "큐에 N회 연속 데이터 없음" 으로 푼다.

#### 내가 틀렸던 것 — 덮어쓰기 우려

`fx_file_create` 가 `FX_ALREADY_CREATED` 를 통과시키고 곧바로 `fx_file_open` 하므로
기존 로그를 처음부터 덮어쓴다고 의심했다. **틀렸다.** FileX 소스를 확인하니
`fx_file_open(FX_OPEN_FOR_WRITE)` 은 주석 그대로 *"position the pointers to the end
of the file"* — 기본이 추가 기록이다. 가정하지 않고 소스를 본 것이 맞았다.

#### 결과

빌드 경고 0. RAM 7.07% -> **7.65%**, ROM 8.92% -> **10.25%**.

| 항목 | 값 |
|---|---|
| `sd_ok` / `sd_ready` | 1 / 1 (마운트 + 파일 생성·열기 성공) |
| SD_DETECT(PD0) | 0 = 카드 삽입 (풀업이라 0 은 능동적으로 끌린 것) |
| `buf_used` | 64~384 순환 (512B 도달 시 flush) |
| LED_SD / LED_ERR | 소등 |
| **LED_BOARD_CHECK** | **점등** — sd_ok 가 판정식에 들어온 상태에서도 유지 |

**전부하 S4 재시험 (버스트 + SD + 모뎀 + 지상국)**

| 항목 | 결과 |
|---|---|
| Bus A 수신 | 4176 프레임/s |
| TEC / REC / LEC | 0 / 0 / 0 |
| RX FIFO 최대 점유 | 0 / 3 |
| FIFO Full · 메시지 유실 | 0 |
| `can_ok` / `uart_ok` / `sd_ok` | 1 / 1 / 1 |
| 0x501 발행 간격 | 101~113ms (SD 없을 때 101~110) |
| 지상국 | 3분간 워치독 경고 0 = 수신 무중단 |

SD 쓰기 부하를 더해도 CAN 수신·무선 송신·중계 주기가 흔들리지 않았다.

#### 남은 것 — RTC 시각이 설정되지 않는다

`rtc.c` 는 CubeMX 기본값 **2000-01-01 00:00:00** 을 쓰고, PHASE5 가 켜지면
백업 레지스터 마커로 이후 재설정을 건너뛴다. 즉 캘린더는 리셋을 넘어 유지되지만
**실제 시각을 넣는 경로가 어디에도 없다.** 실측: `RTC_DR=0x2101 RTC_TR=0x0209`
-> 2000-01-01 00:02:09, 로그 파일명이 `000101_000209.log` 가 된다.

영향:
- 파일명이 경기 시각과 대응되지 않아 정렬·대조가 안 된다
- 지상국의 `rtcTime` 표시(SD 로그와 텔레메트리 대조용)가 무의미해진다
- 백업 도메인이 전원을 잃으면(VBAT 전지 유무 미확인) 매 콜드부팅마다 같은 시각에서
  시작해 파일명이 충돌한다. 덮어쓰기는 아니지만(append) 여러 주행이 한 파일에 섞인다

시각 주입 경로를 정해야 한다(지상국 명령 / 디버거 / 빌드 시각 등). 결정 필요.

#### task 17 미완 — SD 카드 실물 확인

파일 생성·기록은 보드 쪽 상태로 확인했으나, **파일명 형식과 내용은 카드를 빼서
PC 에서 봐야** 확정된다.


### RTC 시각 주입 — 빌드 시각을 굽는다 (2026-09-02)

팀 결정: **주행 직전에 빌드해서 구우면 그 시각이 RTC 출발점**이 된다.
보드에 시각 소스가 없고(GPS/네트워크 없음) 지상국->차량 명령 경로도 아직
꺼져 있어(PHASE45), 현재로선 이것이 유일한 주입 경로다.

#### 구현

`Core/Inc/build_time.h` 신설 — `__DATE__`/`__TIME__` 을 파싱하는 컴파일 타임 매크로.
`rtc.c` 는 PHASE5 일 때 이 값으로 `HAL_RTC_SetTime/SetDate` 를 호출한다
(BCD 리터럴 대신 `RTC_FORMAT_BIN`). 요일은 Zeller 로 계산해 채운다 — 펌웨어가
읽지는 않지만 RTC 레지스터 일관성을 위해서다.

#### 같이 잡아야 했던 함정 둘

**1. 고정 마커가 갱신을 막고 있었다.** 기존 로직은 `BKP_DR1 == 0x32F2` 면
SetTime 을 건너뛴다("한 번이라도 설정했으면 통과"). 그대로 두면 **새 펌웨어를
구워도 RTC 가 갱신되지 않아** 이 기능 자체가 무효가 된다.
→ 마커를 **빌드 스탬프**로 바꿨다. 초 단위까지 유일하고 32비트에 들어간다:
`(((YY*12+(MM-1))*31+(DD-1))*24+HH)*3600 + MI*60 + SS`, 최대 약 3.21e9.
- 같은 펌웨어로 재부팅 -> 스탬프 일치 -> 돌던 캘린더 유지
- 새로 구움 -> 스탬프 불일치 -> 그 빌드 시각으로 재설정

**2. `__DATE__`/`__TIME__` 은 rtc.c 가 컴파일된 시각이다.** make 는 소스가 안
바뀌면 건너뛰므로, 그대로 두면 며칠 전 시각이 구워진다. 주행 직전 빌드가 곧 시각
주입인데 그게 조용히 어긋난다.
→ `build.ps1` 이 빌드마다 `build/{release,debug}/rtc.o` 를 지워 강제 재컴파일한다.

#### 검증 (실측)

| 항목 | 결과 |
|---|---|
| 빌드·플래시 후 RTC | **2026-09-02 18:26:47** (PC 18:26:52, 5초 차 = 빌드->플래시->부팅 지연) |
| 요일 | 코드 3 = 수요일 ✅ |
| 예상 파일명 | `260902_182647.log` |
| **같은 펌웨어 리셋** | 18:27:18 -> 리셋 -> **18:27:39** (유지, 빌드 시각으로 안 되돌아감) ✅ |
| SD | `sd_ok/sd_ready=1`, `buf_used` 112~496 순환, LED 정상 |

#### 남는 제약 (문서화)

- 빌드 머신의 **로컬 시각**이다(UTC 아님). 경기 현지 시각을 쓰려면 빌드 PC 시계를
  현지로 맞춘다.
- 굽는 시점은 컴파일보다 수십 초~수 분 뒤라 그만큼 RTC 가 늦게 출발한다.
  로그 대조에는 충분하지만 초 단위 정확도가 필요하면 다른 경로가 필요하다.
- 백업 도메인(VBAT) 전지 유무는 미확인. 없으면 콜드부팅마다 빌드 시각에서 다시
  시작하므로, **주행 직전에 굽는 절차를 지키는 것이 곧 시각 보증**이다.


### SD 카드 손상 — ST FileX 드라이버가 MBR 을 덮어썼다 (2026-09-02)

Phase 5 를 켜고 첫 주행 시험 뒤 카드를 PC 에 꽂으니
`E:\ 을(를) 액세스할 수 없습니다. 파일 또는 디렉터리가 손상되었기 때문에 읽을 수 없습니다.`

#### 원인 — BOOT_READ / BOOT_WRITE 비대칭

`Middlewares/ST/filex/common/drivers/fx_stm32_sd_driver.c`:

```c
case FX_DRIVER_BOOT_READ:
    _fx_partition_offset_calculate(...)            /* 파티션 시작을 찾아 */
    sd_read_data(media_ptr, partition_start, ...)  /* 거기서 읽는다 */

case FX_DRIVER_BOOT_WRITE:
    sd_write_data(media_ptr, 0, ...)               /* ← 무조건 LBA 0 */
```

일반 섹터 읽기/쓰기는 이미 `logical_sector + fx_media_hidden_sectors` 를 쓴다.
**부트섹터 쓰기만 오프셋을 빼먹었다.**

유발 경로: `fx_fault_tolerant_enable()` -> `_fx_fault_tolerant_create_log_file()`
-> `FX_DRIVER_BOOT_WRITE` -> LBA 0 에 FAT 부트섹터 기록 -> **MBR 파괴**.
fault tolerant 는 PHASE5 와 함께 오늘 처음 켜졌고, 그래서 오늘 깨졌다.

#### 실측 증거 (원시 섹터 0)

```
EB 00 90 20 20 20 20 20 20 20 20 00 02 40 56 02
hidden_sectors(0x1C) = 8192
total_sec32 (0x20)   = 31108096
root_cluster(0x2C)   = 2
FT_index(116)        = 3
signature(0x1FE)     = 55 AA
```

- `EB 00 90` + 유효 BPB = 섹터 0 이 **MBR 이 아니라 FAT 부트섹터**다
- `hidden_sectors = 8192` = 이 부트섹터는 **LBA 8192 볼륨의 것**이므로 LBA 0 에
  있을 수 없다 -> 복사된 것이 증명된다
- `FT_index(116) = 3` = FileX 가 fault tolerant 로그 클러스터를 적는 자리
  (`FX_FAULT_TOLERANT_BOOT_INDEX = 116`)에 값이 들어 있다 -> **FileX 가 쓴 지문**
- `total_sec32 x 512` = 14.83GB, 8192섹터 오프셋 뒤 잔여와 일치

#### 수정

`FX_DRIVER_BOOT_WRITE` 가 `fx_media_hidden_sectors` 를 반영하도록 했다.
파티션 카드에서는 파티션 시작 LBA, 비파티션(슈퍼플로피)에서는 0 이라 원래 동작과 같다.
빌드 경고 0.

**⚠ 벤더 미들웨어 수정이라 CubeMX 재생성 시 되돌아간다.** 드라이버 주석에 명시했다.

#### 데이터

실제 볼륨(LBA 8192)은 무사하다. 파괴된 것은 섹터 0 하나뿐이므로 MBR 만 재구성하면
(오프셋 8192, 31108096섹터, 타입 0x0C) 로그가 복구된다. 이번 것은 시험 데이터라
포맷하기로 했다.

#### 재발 방지 절차

카드를 다시 넣기 전에 **반드시 수정본을 먼저 굽는다.** 순서를 지키지 않으면
포맷한 카드가 다시 깨진다.


### Phase 6 (안전장치) 게이트 활성화 (2026-09-02)

task 18.1~18.3 은 이미 구현돼 있었다. 남은 것은 task 19 체크포인트.

#### 켜기 전 점검 — 워치독 오작동 위험

Alive-Flag 방식은 **플래그가 하나라도 안 서면 보드를 리셋**시키므로, 켜기 전에
"없는 스레드를 요구하지 않는가"를 확인해야 했다.

| 확인 | 결과 |
|---|---|
| 요구 마스크 | 게이트별 조건부 -> 현재 `UART\|SD\|VCULCD` (0x0D). PHASE45 가 꺼져 있어 UART_RX 는 요구하지 않는다 ✅ |
| 직접 IWDG 리프레시 | `ext_can_controller.c` 등 나머지는 전부 `TEMP_IWDG_REFRESH` 아래. `#if PHASE6 && TEMP_IWDG_REFRESH` 빌드 가드가 동시 활성을 막는다 ✅ |
| Power_check(PD6) | HIGH 실측 -> 오탐 전원강하 없음 ✅ |
| 플래그 여유 | 검사 2s / IWDG 4s. 최악 대기(큐 타임아웃)는 2s 지만 스케줄러가 PRIMARY 를 1s 마다 큐에 넣어 스레드가 1s 내 기상 -> **2배 여유** |

게이트를 켜면서 `TEMP_IWDG_REFRESH` 를 0 으로 내렸다(가드 요구사항).

#### 결과 — 양방향 검증

**정상 동작**: 55초 관측, uwTick 단조 증가, **리셋 0회**.

`alive_flags` 가 `0x000D` <-> `0x0008` 로 교대하는 것이 정확한 동작이다:
- `0x0D` = UART|SD|VCULCD 전부 -> 리프레시 직전
- `0x08` = VCULCD 만 -> 리프레시 후 클리어되고, 20ms 루프인 VcuLcd 만 다시 세운 상태
- 비트1(UART_RX)은 한 번도 안 뜬다 -> PHASE45 꺼짐과 일치

**워치독이 실제로 무는가**: 코어를 6초 정지시켜 리프레시를 굶겼더니
uwTick 162,413 -> **8,828 ms**. IWDG 가 물어 리셋됐다. 리셋 후 SD 재마운트,
CAN 수신, LED 상태 모두 정상 복귀를 확인했다.

빌드 경고 0. RAM 7.65%, ROM 10.27% -> **10.31%**.

#### task 문서와 구현이 어긋난 곳 (문서 쪽이 낡음)

task 18.1 의 원래 문구는 아래처럼 적혀 있으나 구현/요구사항서가 이미 정정된 내용이다:
- "LED_SD(PE13), LED_CAN(PE14)" -> 실제는 **LED_SD=PD8, LED_CAN=PB15**
  (2026-08 회로도 실측 정정, `main.h` 주석 참고)
- "LED_BOARD_CHECK: Power_check(PD6) 3.3V 정상이면 점등" -> 실제는
  **현재 활성 서브시스템이 전부 정상이면 점등**(requirements.md 256, 2026-08 정정)

#### 미검증 — 전원 강하 긴급 Flush (task 18.2)

`g_power_down` 경로는 PD6 가 LOW 로 떨어져야 타는데, 정상 전원에서는 HIGH 라
**이번 시험에서 한 번도 실행되지 않았다.** 검증하려면 기록 중에 서플라이를 끊고
카드를 읽어, 마지막에 `ERR_POWER_DOWN`(CRITICAL, SRC_SYS) 레코드가 남았는지
확인해야 한다. 조기경보 여유가 3.3V 레일 벌크 캐패시턴스에 달려 있어
**실측 없이는 성공을 장담할 수 없는 항목이다.**

### task 18.2 결론 — 현 하드웨어로는 검증 불가능 (2026-09-03)

전원 강하 긴급 Flush 를 성립시키려 여러 방향을 팠고, **감지 자체가 원리적으로
불가능**하다는 결론에 도달했다. 아래는 그 근거와, 실제로 하려면 무엇이 필요한지다.

#### 먼저 — 내가 두 번 틀린 이유: 낡은 넷리스트

`WSC/WSC.d356` 은 **2026-05-18** 본이다. 실제 발주된 것은
`WSC/발주자료/WSC.d356` (**2026-08-10**) 이고 둘은 회로가 다르다.

| | 5월본 (WSC/) | 발주본 (발주자료/) |
|---|---|---|
| IC6 / IC7 / IC8 | 없음 | **있음** (LDO + 벅 2개) |
| `12V` / `12V_2` 네트 | 없음 | **있음** |
| L4 / L5, TP1~TP5 | 없음 | **있음** |
| IC1 핀87 (PD6) | 미연결 (노드 1개) | **`3.3V`** |
| J3 | AIN0~3 (4채널) | **AGND + AIN1 + AIN2** (2채널) |

5월본을 보고 "PD6 미연결", "온보드 레귤레이터 미실장" 이라고 두 번 잘못 판단했다.
**회로 확인은 반드시 `발주자료/` 쪽을 볼 것.** 보드 폴더 루트의 d356 은 낡았다.

#### 확정된 전원 계통 (발주본 실측)

```
J12-2 (12V) → D12 → IC7(LMR38020) → L4 → 5V [TP2] → IC6(LDO) → 3.3V [TP3] → MCU
J13-2 (12V_2, 절연) → D14 → IC8 → 5V_2 [TP1]
```

#### PD6 는 트리거될 수 없다

`IC1` 핀87 = **`3.3V` 네트**. MCU 를 먹이는 바로 그 레일이다.
GPIO 입력 임계값도 VDD 비례(약 0.5×VDD)라, 레일이 무너져도 PD6 는 VDD 를
그대로 따라간다 — **어떤 상황에서도 LOW 로 떨어지지 않는다.**

100ms 폴링도, 우선순위 0 EXTI 도 실패한 진짜 이유가 이것이다.
(EXTI 시도는 이후 되돌렸다. `timer_scheduler.c` / `gpio.c` 주석 참고)

따라서 `g_power_down` 경로는 **죽은 코드**다. 위험한 지뢰는 아니다 —
3.3V 에 단단히 물려 있어 오발화하지 않는다. 다만 발화하면 텔레메트리 생성과
SD 로깅을 **비가역적으로 정지**시키므로, 실감지 수단을 붙이기 전까지는
정리 대상으로 둔다.

#### 하위 레일을 감시해도 소용없다 — 계산

5V 레일 벌크: C31 10µF + C42 10µF + C59/C60/C61 22µF = **86 µF**(디레이팅 후 ~75 µF).
부하는 3.3V 계통 전체 + ADM3053 절연 CAN 2개 ≈ 350 mA.
IC6(LDO)가 드롭아웃되는 5.0V → 3.6V 구간:

```
t = C·ΔV / I = 75µF × 1.4V / 0.35A ≈ 0.3 ms
```

**0.3 ms.** SD 쓰기(프로그램 시간 수 ms)에 턱없이 부족하다.
3.3V 는 더 나쁘다 — LDO 가 붙들고 있다가 마지막에 무너진다.
내부 PVD(VDD 2.9V 트립)도 같은 이유로 수백 µs 밖에 못 번다.

즉 **레귤레이터 하위에서는 어떤 방법으로도 안 된다.**

#### 유일한 관측점은 12V — 수단은 이미 보드에 있다

실제 차량 고장 모드는 순단이 아니라 팩/MPPT 전압의 점진적 하강이다.
그 신호가 살아있는 유일한 지점이 **12V 입력**이다.

발주본에 이미 있는 것:

| 항목 | 내용 |
|---|---|
| `J3` 핀1/2/3 | **AGND / AIN1 / AIN2** |
| AIN1 → | 470Ω(R27) → **PC2** → 910Ω(R26) → AGND |
| AIN2 → | 470Ω(R25) → **PC3** → 910Ω(R24) → AGND |
| 분압비 | 910/1380 = **0.6594** → 입력 만스케일 **5.005 V** |
| C49 / C50 | **DNP** (0805 패드는 있음) |
| D17 | ADC 노드 클램프 |
| 현재 상태 | **CubeMX 에 ADC 미설정 — 완전히 미사용** |
| 테스트포인트 | TP1=5V_2, **TP2=5V**, TP3=3.3V, TP4=GND_2, TP5=EARTH |

**배선안 (PCB 수정 아님 — 기존 입력 커넥터 사용)**

```
12V → 외부 직렬 3.0 kΩ → J3 핀2 (AIN1),  기준 J3 핀1 (AGND)

  분압비 = 910 / (3000 + 470 + 910) = 0.2078
    12 V → 2.49 V  (ADC ~3090/4095)
    16 V → 3.32 V  (만스케일, 서지 여유)
  전류 = 12V / 4380Ω = 2.7 mA   (외부 저항 발열 22 mW)
```

선택 추가:
- `TP2(5V) → J3 핀3 (AIN2)` 직결 — 저항 불필요, 만스케일. LDO 건전성 감시용
- `C49`/`C50` 에 100nF 실장 — 소스 임피던스 ~723Ω 와 τ≈72µs. 조립 옵션이라 PCB 수정 아님

**주의**: PD6 는 분압이 없다. 12V 를 직접 물리면 핀이 파손된다. 반드시 AIN 계통을 쓸 것.

#### 경보 시 동작은 현재와 정반대여야 한다

지금 `g_power_down` 은 모든 기록을 **정지**시킨다. 그건 "남은 시간이 마이크로초"
라는 전제의 설계다. 12V 감시로 수 초를 확보하면 정반대여야 한다:

```
[12V 임계 이하]
  ├ 즉시 flush (지금까지 것 확정)
  ├ 100ms 주기 강제 flush   ← 손실 상한 1.2초 → 0.1초
  ├ CRITICAL 로그 (지상국 즉시 표시)
  └ 텔레메트리 계속 — 죽을 때까지 기록
```

임계값과 2차 트리거(BMS `KEY_BMS_VOLT_STACK`/`KEY_BMS_SAFETY`/`KEY_BMS_FET_STATUS`,
MPPT `KEY_MPPT_AUX_1` 의 12V_Aux)는 **CAN 쪽 팀 변경이 끝난 뒤** 정한다.

#### 검토했다가 접은 대안

**VBAT + BKPSRAM 미러** — 손실을 literal 0 으로 만드는 유일한 방법이다.
레코드를 SD 버퍼에 넣을 때 BKPSRAM(2KB=128레코드)에도 복사하고, `fx_file_write`
성공 시 지우고, 다음 부팅에 남은 것을 회수한다. 위험 구간이 32레코드(512B 버퍼)라
4배 여유. 감지가 아예 필요 없어 타이밍 경쟁 자체가 사라진다.

걸림돌은 `HAL_PWREx_EnableBkupRAMRetention()` 이 **LDO 공급일 때만** `BREN` 을
세우고 SMPS 면 `HAL_ERROR` 를 반환한다는 것(`stm32u5xx_hal_pwr_ex.c`).
`main.c:343` 은 `PWR_SMPS_SUPPLY` 다.

- **전기적 안전성**: 데이터시트 Figure 25 Note — SMPS 패키지에서 LDO 는
  **같은 VDD11 커패시터로 VCORE 를 조정**한다. 우리 패키지엔 VCAP 핀이 아예 없다
  (회로도 VCAP 0건, VDD11_1/VDD11_2 존재). 추가 부품 0, 배선 변경 0.
  AN5373: "can be selected **when the application runs**", "LDO or SMPS can be used
  in **all voltage scaling ranges**" — ST 가 권장하는 정규 동작이다.
- **비용**: 160MHz Range 1 typ — SMPS(3.0V) 7.15 mA vs LDO 13.50 mA → **약 21 mW**
  (85°C 에서 25 mW). MCU 코어 전류가 약 2배가 되지만 절대값은 작다.
- **접은 이유**: 12V 감시로 수 초를 확보하면 이게 불필요해진다. 12V 배선이
  불가능하다고 판명되면 다시 꺼낼 것.

`BREN` 을 LDO 에서 세운 뒤 SMPS 로 복귀시키는 편법은 **쓰지 않는다.**
평상시엔 통과하고 정작 전원이 끊기는 순간에만 실패할 수 있어, 신뢰성을
얻으려는 작업의 실패 모드로 부적절하다.

#### 미검증 — 부팅 시 LDO → SMPS 전환

`main()` 호출 순서는 `HAL_Init()` → `SystemPower_Config()` → `SystemClock_Config()`
이므로, 공급 절체는 **MSI 4MHz / 저전압 레인지**에서 일어난다(160MHz 아님).
CubeMX 주석이 "Switch to SMPS regulator **instead of LDO**" 인 점, SMPS 핀이 없는
패키지도 부팅해야 하는 점에서 `PWR_CR3.REGSEL` 리셋값은 0(LDO)으로 **추정**되나,
**측정하지 않았다.** 확정되면 "이 보드는 매 부팅마다 LDO 구간을 거치며 정상
동작한다"는 근거가 되어 LDO 전환의 위험도가 사실상 0 이 된다.

확인 방법 — `tools/pwr_probe.sh` (읽기 전용, 쓰기 없음):
```
PWR_BASE = 0x46020800
  CR3   0x46020808  REGSEL(bit1) : 0=LDO 1=SMPS   ← 선택
  SVMSR 0x4602083C  REGS(bit1)                     ← 실제 동작 중
  BDCR1 0x46020820  BREN(bit0)                     ← 백업 SRAM 유지
  VOSR  0x4602080C  VOS(bit17:16)
```
`reset halt` 직후 CR3 를 읽어 REGSEL 이 0 이면 확정.

#### 별건 — 지상국이 수신 데이터를 저장하지 않는다

`ArrowPoint-Telemetry` 자바 소스에 파일 출력 코드가 **한 줄도 없다**
(`FileOutputStream`/`Files.write`/`FileWriter` 전무). 복호화해서 화면에 뿌리고 버린다.

무선으로 나가는 항목은 `Sched_EmitBoth` **26곳**, SD 전용은 `Sched_EmitSdOnly`
**14곳**(셀 밸런싱·안전 플래그·모터 벡터 등 상세 진단)이다. 즉 주 텔레메트리는
이미 무선으로 나가고 있고, 무선 지연(200~500ms)이 SD 버퍼(최대 1.2초)보다 오히려
짧다. 받은 16바이트 레코드를 그대로 파일에 붙이면 **기존 `LogFileToCsv` 가
수정 없이 변환**한다(형식 동일).

전원 손실 보완일 뿐 아니라 **SD 카드 자체의 고장/파일시스템 손상**에 대한 유일한
대비책이 된다 — 현재 그 대비가 전혀 없다. 비용은 지상국 쪽 파일 기록기 하나뿐이다.

### Phase 4.5 완료 + MCP2518FD 모드 결함 (2026-09-03)

지상국 UI → RFD900x → UART RX → COBS → MCP CAN TX → LCD 표시까지 전 구간이
통과했다. 게이트 6개가 모두 켜졌다. 그 과정에서 결함 셋을 잡았다.

#### ① MCP2518FD 가 Configuration 모드에 영구 정착한다

`ExtCAN_Wake()` 는 첫 줄에서 `if (!mcp_asleep) return true;` 로 빠져나가며
**실제 동작 모드를 확인하지 않았다.** 그런데 같은 함수의 웨이크 경로는
`ExtCAN_EnterNormalMode()` **앞에서** `mcp_asleep` 을 내린다. MCP2518FD 는
버스가 11비트 연속 recessive 일 때만 Configuration 을 벗어나므로,
LCD(0x510, 5Hz)와 우리(0x501, 10Hz)가 떠드는 동안 이 전환은 실제로 실패한다.
실패하면 장치는 Configuration 에 남고 플래그는 이미 false 다. 이어서
`ExtCAN_Sleep()` 의 `EnterSleepMode` 까지 실패하면 플래그가 false 로 굳어
**복구 경로가 영영 사라진다.**

진단이 어려웠던 이유 — 카운터가 정상으로 보인다:

| 지표 | 갇힌 상태 |
|---|---|
| `g_vcu_lcd_tx_ok` | 계속 증가 (37,945 -> 38,023) |
| `TEC` / `REC` | 0 (갱신 1,289회 확인 — 최신값) |
| `opmode` | **4 (Configuration)** <- 이것만이 유일한 단서 |
| 실제 버스 | 아무것도 안 나감 |

(TX 실패 LOG 자체는 남는다. 다만 tx_ok 가 계속 오르고 TEC 가 0 이라 실시간
관측에서는 정상으로 오인하기 쉽다.)

**수정**: `mcp_asleep` 이 false 여도 `MCP251XFD_GetActualOperationMode()` 로
실제 모드를 읽고, Normal 이 아니면 `g_mcp_mode_recover` 를 올린 뒤 되돌린다.
100ms 마다 SPI 읽기 1회가 늘지만 상태가 자가 복구된다.
실측에서 `모드복구=1` 로 자동 복구를 확인했다.

#### ② 다중 프레임 텍스트가 버스트로 나가 계기판에서 유실된다

`ExtCAN_Transmit()` 이 청크를 연달아 밀어넣었다. 500kbps 에서 8바이트 프레임은
약 230us 라 3프레임이 1ms 안에 몰린다. 계기판(STM32U545)의 FDCAN RX FIFO0 는
3단인데 0x501(10Hz)이 겹치고 LVGL 렌더링이 배출을 지연시키면 넘친다.
실측: 15자(3프레임) 중 **1개만 도착**, 조립 타임아웃.

**수정**: 청크 사이 `MCP_TX_CHUNK_GAP_TICKS` (5ms). 사람이 보내는 메시지라
62바이트(9프레임)여도 45ms 로 체감되지 않는다. 마지막 청크 뒤에는 안 기다린다.
수정 후 `ABCDEFGHIJKLMNO` 15자가 온전히 조립되어 계기판 표시까지 확인됐다
(can_text frames +3, msgs +1).

#### ③ 0x510 ACC 명령에 범위 검증이 없었다

LCD 팀이 0x510 페이로드를 ACC 명령으로 확정했다(byte0=설정속도 0~150,
byte1=상태 0/1). `xlat_lcd_cmd()` 는 앞 2바이트를 그대로 통과시키고 있었는데,
이 값은 0x420 으로 VCU 에 중계되는 **차량 제어 경로**다.

범위를 벗어나면 프레임을 **통째로 버린다**(클램프하지 않는다). 쓰레기 속도를
150 으로 접어 넘기면 VCU 가 운전자의 정상 명령으로 취급한다. 전달을 끊으면
VCU 타임아웃으로 ACC 가 해제되는 방향이라 안전한 쪽으로 실패한다.

#### 계기판 쪽에서 발견해 전달한 것

- **RX FIFO 교착**: `RX_FIFO0_NEW_MESSAGE` 인터럽트에서만 배출하는데 FIFO 가
  가득 차면 그 인터럽트가 더는 뜨지 않아 **수신이 영구히 멎었다**
  (rx_count 2423 고정, tx_count 는 계속 증가). `FDCAN_IT_RX_FIFO0_FULL` /
  `MESSAGE_LOST` 를 추가하고 배출을 함수로 분리해 메인 루프에서도 부르게 고쳤다.
  효과는 실증됐다 — NVIC 이 전부 꺼진 상태에서도 폴링만으로 9프레임/초 유지.
- **버튼이 간헐적으로 죽는다 (미해결, LCD 팀 몫)**: `EXTI_FPR1 = 0x07` 로
  이벤트는 쌓이는데 핸들러가 안 돈다. SysTick·FDCAN 인터럽트는 정상.
  재현 조건 불규칙(횟수 무관). **POR 로만 복구되고 AIRCR 소프트웨어 리셋으로는
  복구되지 않는다.**
- **계기판은 구운 뒤 반드시 전원을 껐다 켜야 한다.** OpenOCD `reset run` 이
  실패하면 NVIC 초기화를 건너뛴 채로 동작한다. 이 함정으로 오래 헤맸다.
- 빌드 주의: `Debug/*.mk` 에 원저자 PC 절대경로가 박혀 있어 다른 PC 에서
  빌드되지 않는다.

#### SD 로깅 실측 (전 게이트 ON 상태)

`260903_143654.log` — **126,368 레코드 / CRC 실패 0 / 65분 연속**.
오늘 파일 10개 전부 16의 배수로 끝나 레코드 경계가 유지됐다.
RTC 빌드시각 주입 덕에 파일명·디렉터리 시각이 정확하다(2026-09-03).
오류 이벤트 49건: 부팅 직후 노드 무응답 2건(vehicle_ms 1,548 — 노드가 아직
안 올라온 정상 상황), 그리고 0x501 송신 실패 47건(detail 0xB1000501).

송신 실패 47건은 vehicle_ms 2,773,625~3,096,613 = **실시각 15:23:06~15:28:29**
구간에 흩어져 있다(파일명 14:36:54 기준, 마지막 레코드가 파일 mtime 15:42:04
와 일치해 변환식 검증됨). 이 5분 동안 우리 보드는 같은 파일에 계속 기록했으므로
**리셋 없이 정상 동작했고 상대만 응답이 없었다.**

원인은 확정하지 못했다. 후보 둘 다 같은 signature 를 남긴다:
  - 사용자의 전원 차단 (LCD 전원이 끊기면 ACK 가 없다)
  - MCP 가 Configuration 에 갇힌 구간 (FIFO 대기 초과로 HAL_ERROR -> TX_FAIL)
연속이었다면 레이트리밋 1/s 로 ~320건이어야 하는데 47건뿐이라 **간헐적**이었다.

(내 LCD 소거/재프로그래밍은 15:57~16:05 로 이 구간과 겹치지 않는다 —
 처음에 그렇게 적었다가 산출물 mtime 으로 확인해 정정했다.)
