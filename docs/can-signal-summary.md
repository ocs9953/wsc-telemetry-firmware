# CAN 신호 요약 (CAN Signal Summary)

> 지금까지 확정된 CAN ID · 신호 · 패킷 묶음 · VehicleData 매핑의 **빠른 참조용** 파일.
> 상세 설계 원칙은 `telemetry-signals.md` 참조.
>
> **상태**: BMS(HV) Prohelion D1000 Gen1 (2026-09 교체) / MPPT 확정 / Motor **잠정(virtual)** — VCU 미확정, WaveSculptor22 매뉴얼 기반 placeholder 
>
> **2026-09-18 신뢰성 보강 — 지상국 반영 완료(ArrowPoint `LogErrorKey`/`WscTelemetryDecoder`)**:
> - 새 오류 키 **`ERR_STACK_OVERFLOW` = 0x94** (CRITICAL, 부팅 후 1회): value = [스레드 번호 u32 (types.h `BOOT_THREAD_*`)][RCC_CSR u32]. 스택 오류 시 보드는 이제 **즉시 리셋**한다.
> - **`ERR_WATCHDOG` = 0x91** 을 실제로 사용 (ERROR, 부팅 후 1회): 직전 리셋이 IWDG. value = [RCC_CSR u32][0]. 두 보고 모두 무선 1회 + SD 세션 헤더에 기록.
> - `ERR_INIT_FAIL` 은 이제 **새 비트가 생길 때마다** 전체 마스크를 다시 보낸다(bit2 = ADC).
> - `ERR_TIMEOUT`(SRC_SYS) 경고는 큐에 들어갔을 때만 "보고 완료"로 잠근다(실패 시 1초 뒤 재시도).
> - FDCAN 수신: ID별 최소 DLC 미달 프레임은 값·신선도를 갱신하지 않고 버린다(`g_can_short_frames`).
> - SD: 드라이버 대기 한도 10s → **750ms**(워치독 4s보다 짧게), 전송 완료 콜백에서만 SD 생존 비트 갱신. 쓰기/flush 오류 상태 분리.
>
> **2026-09 HV BMS를 Prohelion D1000 Gen1(BMU 1 + CMU 4, 32S)로 교체 — 지상국 디코더 필수 재동기**:
> - BMU CAN 베이스를 **0x040**으로 설정 → BMS ID: 0x040(heartbeat), 0x041~0x04C(CMU 1~4), 0x134~0x13D(팩). **리틀엔디안**. 필터 변경 없음. 상세 §2.
> - **LOG source 변경**: BMS 데이터는 이제 **`SRC_BMS` = 10**(구 `SRC_RESERVED2`, 데이터에 쓰인 적 없음). 구 `SRC_BMS_BOT`(1)/`SRC_BMS_TOP`(2)은 `SRC_RETIRED_*`로 **퇴역·재사용 금지**. 다른 source 값은 불변.
> - **BMS 키 전면 교체**(§2-4): PRIMARY 8→**3**(PACK_VI/SOC_TEMP/STATUS), DETAIL 18→**16**(팩 4 + CMU 12), SD_ONLY 40→**5**(팩 BAL_PRECHG 1 + CMU 시리얼 4). 식별 정보(DeviceID·Serial·FW build·HW ver·Model ID)와 v4 전용 12V 전압은 수신하지 않는다(2026-09-16).
> - PRIMARY 총 16→**11패킷** → 번호 재배치(BMS #1~#3, MPPT #4~#7, Motor #8~#11).
> - `ERR_TIMEOUT` 경고 value[1]에 **CMU 번호(1~4, 팩=0)** 추가. CMU 임계는 3000ms.
> - 아래 2026-07-24 PR#31 BMS 이력은 **폐기된 구 BMS** 기록이다.
>
> **(폐기됨) 2026-07-24 BMS(HV) CAN ID 전면 재배치 (PR#31, `CAN DATA/BMS_CAN_ID_명세_최신_PR31.xlsx` 기준) — 지상국 디코더 필수 재동기**:
> BMS(HV) 전체가 우선순위(PRIMARY/DETAIL/SD_ONLY) 기반 연속 대역 **0x040~0x07F**로 이전됨. 기존 0x500대/0x1xx/0x2xx 체계는 완전히 폐기.
> - **PRIMARY**: BOT 0x040(Fault)/0x041(Volt)/0x042(Curr)/**0x043(SOC, 신규)**, TOP 0x044/0x045/0x046/**0x047(SOC)**
> - **DETAIL**: BOT 0x048~0x050(9개), TOP 0x051~0x059(9개)
> - **SD_ONLY**: BOT 0x05A~0x06C(19개), TOP 0x06D~0x07F(19개)
> - **LEGACY(유지, 대부분 미파싱)**: FET 프레임만 파싱 — BOT 0x123/TOP 0x223 (FET_Status/CHG/DSG/PDSG). 그 외 0x111/211(전압 중복)/0x112/212(전류 중복)/0x120/220(상태 중복)/0x133/233(SOC 중복)은 수신되어도 무시.
> - MPPT는 general_id 공장설정이 0x600→0x300으로 변경되어 base가 **0x310/0x320/0x330**으로 이전(로터리 스위치 1/2/3, 오프셋 구조 동일). **2026-08 4대로 증설되며 #4(로터리 4, base 0x340) 추가** — #1~#3 base는 변경 없음.
> - FDCAN1 HW 필터: 0(**0x040~0x34F**, BMS(HV) 신규대역+레거시+MPPT ×4 / 2026-08 MPPT4 수용 위해 0x33F→0x34F 확장) / 1(**0x400~0x6FF**, Motor + LV BMS / 2026-08 LV 제거로 0x4FF 로 줄였다가 2026-09-23 LV 복원으로 0x6FF 로 되돌림).
> - (히스토리) SOC는 처음엔 BOT 0x503/TOP 0x603로 도입하려다 BMS(LV) CMU0(0x601~0x603)와 충돌해 0x504로, 이후 PRIMARY 대역 편입 요청에 따라 최종 0x043/0x047로 재조정됨.
>
> **2026-07-13 대규모 갱신(위 PR#31로 대체된 과거 이력)**: (1) MPPT/Motor 무선 티어(PRIMARY/DETAIL/SD_ONLY) 재배치 — `CAN DATA/CAN_수신_데이터_정리본.xlsx` 기준. (2) BMS(LV, Prohelion D1000 Gen1) 신규 구현 — §4.5. (3) HV BMS TOP RUN ID 0x600→0x503 이전(LV base 0x600 충돌 회피, 이후 위 PR#31로 0x047로 재이전).
>
> **2026-07 갱신**: BMS 상세온도·쿨롱카운터, MPPT aux(+3/+4/+6), Motor DSP온도·SD_ONLY 진단 — 전부 파싱+송출 구현 완료. (BMS 상세 오프셋은 위 PR#31 기준 0x048~0x07F 대역으로 이전됨.)
>
> **크로스팀 이슈 해결(2026-07)**: BMS 보드(STM32U545) 자체 경고(비트타이밍 1.667Mbit/s 오계산, Classic CAN 상한 초과 가능성)는 **타 팀과 통신 속도·설정 합의 완료로 해소됨** — 500kbps로 통일. 보드 실측으로 최종 확인 권장.
>
> **WaveSculptor22 공식 매뉴얼 확보(2026-07)**: `CAN DATA/TritiumWaveSculptor22_Manual.pdf` Appendix C(CAN Communications Protocol)를 직접 추출해 대조한 결과, 기존 구현에서 **바이트 순서 오류 5건**(V Vector, I Vector, BackEMF, 전원 레일, Slip Speed) 발견·수정. 상세: §4, 아래 "구현 시 발견된 수정 사항" 참조. (VCU 재전송 경로는 여전히 미확정 — Motor 상태는 잠정 유지)

---

## 1. CAN 버스 구성

| 버스 | 컨트롤러 | 속도 | 역할 |
|---|---|---|---|
| Bus A | FDCAN1 (내부) | 500 kbps | BMS · MPPT · Motor 수신 전용 |
| Bus B | MCP2518FD (SPI) | 미정 | 향후 저우선순위 주변장치(LCD TX 확정, 센서 RX 미확정) |

### FDCAN1 범위 필터 (하드웨어 2개, StdFiltersNbr=2)

| HW 필터 | 범위 | 커버 대상 |
|---|---|---|
| 0 | **0x040 ~ 0x34F** | BMS(HV) Prohelion 블록(0x040~0x13F, 2026-09) + MPPT ×4(0x310~0x346) |
| 1 | **0x400 ~ 0x6FF** | Motor/VCU(0x4xx 잠정) + LV BMS(0x600~0x6FF) *(2026-09-23 LV 복원으로 0x4FF→0x6FF. 0x500~0x5FF 도 함께 열린다 — 지금 Bus A 에 쓰는 노드는 없음)* |

> 필터0의 0x040~0x34F는 중간에 빈 구간(0x080~0x110, 0x234~0x30F 등)을 포함하지만 하드웨어가 그대로 통과시키고 SW switch-case default에서 무시하므로 무해하다. `StdFiltersNbr`는 2 유지(CubeMX 생성코드 `fdcan.c` 수정 불필요), ID 판별은 SW switch-case.
> **2026-08 MPPT 4대 증설로 필터0 상한을 0x33F→0x34F로 확장했다.** 이 상한을 넓히지 않으면 MPPT4(0x340~0x346)가 하드웨어 필터에서 전량 거부되어 SW까지 도달하지 못한다.

> **모터 데이터 경로**: 실제로는 VCU가 모터 컨트롤러 앞단에 위치하며, 우리는 **VCU가 재전송한 프레임**을 수신한다. VCU 설계 미완성이므로 아래 모터 매핑은 WaveSculptor22 네이티브 스펙을 그대로 가져온 **virtual placeholder**다. VCU CAN 출력 스펙 확정 시 ID·레이아웃·엔디안 전면 교체.

---

## 1.5 CAN ID 빠른 조회 (구현 기준)

> `Core/Src/can_receiver.c` 기준, 현재 코드가 실제로 수신·파싱 중인 CAN ID를 대분류별 한 눈에 보는 표.
> 각 ID의 오프셋별 필드·스케일·무선 패킷 매핑 등 상세는 아래 §2~4.5 참조.
> **'구분' 컬럼**은 텔레메트리 보드가 무선으로 얼마나 자주 내보내는지에 따른 분류다
> (PRIMARY=200ms 무선, DETAIL=1s 무선, SD_ONLY=무선 미전송·SD카드만). BMS 칩 자체의
> RUN/TEST 프레임 구분(소스 측 명칭, `BMS_CAN_ID_명세.xlsx`의 시트명 유래)과는 다른 축이며,
> BMS RUN 프레임은 거의 전부 PRIMARY로, TEST 프레임은 DETAIL 또는 SD_ONLY로 나뉘어 들어간다.

### BMS(HV) — 23개 (Prohelion D1000 Gen1, 베이스 0x040, 리틀엔디안, 2026-09 교체)

| 대상 | 구분 | CAN ID | 수신 데이터 |
|---|---|---|---|
| BMU | (미수신) | 0x040 | Heartbeat: DeviceID(u32), Serial(u32) — 식별용이라 수신 안 함 |
| CMU1~4 | DETAIL(온도)/SD_ONLY(시리얼) | 0x041/0x044/0x047/0x04A | Serial(u32), PCB_T(i16, 0.1℃), Cell_T(i16, 0.1℃) |
| CMU1~4 | DETAIL | 0x042/0x045/0x048/0x04B | Cell0~3 (i16×4, mV) |
| CMU1~4 | DETAIL | 0x043/0x046/0x049/0x04C | Cell4~7 (i16×4, mV) |
| BMU | PRIMARY/DETAIL | 0x134 | SOC_Ah(f32), SOC_%(f32) |
| BMU | DETAIL | 0x135 | BalanceSOC_Ah(f32), BalanceSOC_%(f32) |
| BMU | DETAIL | 0x136 | ChgCellVErr, CellTMargin, DchgCellVErr (i16), Capacity(u16, Ah) |
| BMU | PRIMARY/SD_ONLY | 0x137 | Contactor(u8), PrechargeState(u8), TimerElapsed(u8), TimerCnt(u8) *(12V u16 은 v4 전용이라 미수신)* |
| BMU | PRIMARY/DETAIL | 0x138 | MinCell, MaxCell (u16, mV), 위치 CMU·Cell (u8×4) |
| BMU | PRIMARY/DETAIL | 0x139 | MinCellT, MaxCellT (0.1℃), 위치 CMU (u8×2) |
| BMU | PRIMARY | 0x13A | PackV(u32, mV), PackI(i32, mA) |
| BMU | PRIMARY/SD_ONLY | 0x13B | BalRise, BalFall(u16, mV), StatusFlags(u8), CMU수(u8) *(FW build 미수신)* |
| BMU | DETAIL | 0x13C | Fan0, Fan1(rpm), I12V 팬+컨택터, I12V CMU(mA) |
| BMU | PRIMARY | 0x13D | ExtStatus(u32) *(HW ver·Model ID 미수신)* |

> 상세·비트 정의·무선 매핑: §2. BMU 블록 0x040~0x13F 전체와 부트로더 0x7F0~0x7F4는 우리 보드의 **송신 금지** ID다.

### MPPT — 28개 (리틀엔디안 float, 확정 / 2026-08 3대→4대 증설로 21→28)

| MPPT | 구분 | CAN ID | 수신 데이터 |
|---|---|---|---|
| #1 | PRIMARY | 0x310 | Input_Voltage(float, V), Input_Current(float, A) |
| #1 | PRIMARY | 0x311 | Output_Voltage(float, V), Output_Current(float, A) |
| #1 | DETAIL | 0x312 | Mosfet_Temp(float, ℃), Controller_Temp(float, ℃) |
| #1 | SD_ONLY | 0x313 | 12V_Aux(float, V), 3V_Aux(float, V) |
| #1 | DETAIL | 0x314 | MaxOutput_Voltage(float, V), MaxInput_Current(float, A) |
| #1 | DETAIL | 0x315 | Error_Flags(u8), Mode(u8) *(0x312와 한 패킷)* |
| #1 | DETAIL | 0x316 | PowerConn_Voltage(float, V), PowerConn_Temp(float, ℃) |
| #2 | (동일) | 0x320~0x326 | #1과 동일 오프셋 구조 (base=0x320) |
| #3 | (동일) | 0x330~0x336 | #1과 동일 오프셋 구조 (base=0x330) |
| #4 | (동일) | 0x340~0x346 | #1과 동일 오프셋 구조 (base=0x340, **2026-08 증설**) |

### Motor — 13개 (리틀엔디안 float, 잠정 — VCU 미확정, WaveSculptor22 virtual)

| 구분 | CAN ID | 수신 데이터 |
|---|---|---|
| DETAIL | 0x401 | Limit_Flags(u16), Error_Flags(u16), Active_Motor(u16), TxErrCnt(u8), RxErrCnt(u8) |
| PRIMARY | 0x402 | Bus_Voltage(float, V), Bus_Current(float, A) |
| PRIMARY | 0x403 | Motor_Velocity(float, rpm), Vehicle_Velocity(float, m/s) |
| PRIMARY | 0x40B | Motor_Temp(float, ℃), HeatSink_Temp(float, ℃) |
| PRIMARY | 0x407 | BEMFd(float, V), BEMFq(float, V) |
| PRIMARY | 0x40E | Odometer(float, m), DC_Bus_AmpHours(float, Ah) |
| DETAIL | 0x40C | DSP_Board_Temp(float, ℃) |
| DETAIL | 0x404 | Phase B_Current(float, A), Phase C_Current(float, A) |
| SD_ONLY | 0x405 | Vd(float, V), Vq(float, V) |
| SD_ONLY | 0x406 | Id(float, A), Iq(float, A) |
| SD_ONLY | 0x408 | 15V 레일(float, V) *(byte 0~3 Reserved)* |
| SD_ONLY | 0x409 | 3.3V 레일(float, V), 1.9V 레일(float, V) |
| SD_ONLY | 0x417 | Slip_Speed(float, Hz) *(byte 0~3 Reserved)* |

### 전체 요약

| 대분류 | CAN ID 개수 | 상태 |
|---|---|---|
| BMS(HV) | 23 | Prohelion D1000 Gen1 (베이스 0x040: 0x040~0x04C, 0x134~0x13D, 2026-09) |
| MPPT | 28 | 확정 (base 0x310/0x320/0x330/0x340 — 2026-08 4대 증설로 21→28) |
| Motor | 13 | 잠정(VCU 미확정) |
| **합계** | **64** | |

---

## 2. BMS(HV) CAN ID 상세 — Prohelion D1000 Gen1 (2026-09 교체)

> 출처: [Prohelion BMS D1000 Gen1 문서](https://docs.prohelion.com/Battery_Management_Systems/Prohelion_BMS_D1000_Gen1/index.html) — "CAN ID's And Format", "Transmitted CAN Packets", "Incoming CAN Packets".
> 구성: **BMU 1대 + CMU 4대, 32S (CMU 1대 = 8S)**. 500 kbps.
> **엔디안: 리틀엔디안** — DBC 전 신호가 `@1`(Intel). **SOC·Balance SOC 는 IEEE-754 float32** — DBC `SIG_VALTYPE_ … : 1` 로 확인. 부호는 DBC `@1+`/`@1-` 및 데이터시트 union 타입(MSP430: `int`=16bit, `long`=32bit)으로 확인했다(§2-2의 "DBC 대조" 참고).
> **BMU CAN 베이스 ID = 0x040** (기본값 0x600에서 Profinity로 변경). 모든 ID는 `CAN_ID_BMS_BASE` + 오프셋으로 유도한다.

### 2-1. ID 할당 (베이스 0x040)

| 오프셋 | 우리 ID | (기본 0x600) | 내용 | 주기 |
|---|---|---|---|---|
| +0x00 | 0x040 | 0x600 | BMU heartbeat: DeviceID u32, Serial u32 | 1 Hz |
| +0x01+3n | 0x041/044/047/04A | 0x601/604/607/60A | CMU n 상태: Serial u32, PCB_T i16(0.1℃), Cell_T i16(0.1℃) | ~1 Hz |
| +0x02+3n | 0x042/045/048/04B | 0x602/… | CMU n Cell0~3 i16×4 (mV) | ~1 Hz |
| +0x03+3n | 0x043/046/049/04C | 0x603/… | CMU n Cell4~7 i16×4 (mV) | ~1 Hz |
| +0x0D~0xEF | 0x04D~0x12F | 0x60D~0x6EF | CMU 예약 (5번째 이후) — **다른 노드 사용 금지** | — |
| +0xF0 | 0x130 | 0x6F0 | 공장 설정·캘리브레이션 명령 — **송신 금지** | — |
| +0xF1~0xF3 | 0x131~0x133 | 0x6F1~0x6F3 | 예약 | — |
| +0xF4 | 0x134 | 0x6F4 | Pack SOC: SOC_Ah f32(0=만충), SOC_% f32 | 1 Hz |
| +0xF5 | 0x135 | 0x6F5 | Balance SOC: Ah f32, % f32 | 1 Hz |
| +0xF6 | 0x136 | 0x6F6 | 충전 제어: ChgCellVErr i16(mV), CellTMargin i16(0.1℃), DchgCellVErr i16(mV), Capacity u16(Ah) | 10 Hz |
| +0xF7 | 0x137 | 0x6F7 | 프리차지: Contactor u8, State u8, 12V u16(mV, v4만), unused u16, TimerElapsed u8, TimerCnt u8(10ms) | 1 Hz + 변화 시 |
| +0xF8 | 0x138 | 0x6F8 | 최소/최대 셀전압: Min u16, Max u16(mV), MinCMU, MinCell, MaxCMU, MaxCell (u8) | 10 Hz |
| +0xF9 | 0x139 | 0x6F9 | 최소/최대 셀온도: Min u16, Max u16(0.1℃), MinCMU u8, -, MaxCMU u8, - | 1 Hz |
| +0xFA | 0x13A | 0x6FA | 팩 전압 u32(mV) / 전류 i32(mA) | 10 Hz |
| +0xFB | 0x13B | 0x6FB | 팩 상태: BalRise u16, BalFall u16(mV), Flags u8, CMU수 u8, FW build u16 | 1 Hz |
| +0xFC | 0x13C | 0x6FC | 팬: Fan0 u16, Fan1 u16(rpm), I12V 팬+컨택터 u16, I12V CMU u16(mA) | 1 Hz |
| +0xFD | 0x13D | 0x6FD | 확장 상태: Flags u32, HW ver u8, Model ID u8, unused u16 | 1 Hz |
| +0xFE~0xFF | 0x13E~0x13F | 0x6FE~0x6FF | 예약 | — |
| 고정 | 0x7F0~0x7F4 | 0x7F0~0x7F4 | 부트로더 (베이스와 무관) — **송신 금지** | — |

> **CMU 번호 n은 BMU가 부팅 시 할당**한다. 배선상 몇 번째 모듈인지와 같다는 보장이 문서에 없으므로, 물리 위치는 **CMU Serial**(`KEY_BMS_CMU_SERIAL`)로 대조한다.
> **셀 전압은 signed**: -32768 = 셀 없음, -32767 = 여분 셀, 그 밖의 음수 = 이중 채널 불일치(신뢰 불가).
> **0x6F9 최소/최대 온도**는 문서상 u16이지만 비트가 같으므로 i16으로 받는다(영하 표현 대비).
> **버스 우선순위**: BMS 블록(0x040~0x13D)은 MPPT(0x310~)·Motor(0x4xx)보다 여전히 높다. 단 BMS 내부에서는 Prohelion 고정 배치 때문에 CMU 셀 프레임이 팩 핵심 프레임보다 ID가 낮다(바꿀 수 없음, 현재 부하 ~1.3%에선 무해).

### 2-2. 비트 정의

**0x6F7 byte0 컨택터 드라이버**: 0x01 C1 오류, 0x02 C2 오류, 0x04 C1 출력 ON, 0x08 C2 출력 ON, 0x10 12V 컨택터 전원 OK(v5+), 0x20 C3 오류, 0x40 C3 출력 ON.
**0x6F7 byte1 프리차지 상태**: 0 Error, 1 Idle, 2 Measure, 3 Pre-charge, 4 Run, 5 Enable Pack.
**0x6FB byte4 상태**: 0x01 셀 과전압, 0x02 셀 저전압, 0x04 셀 과온, 0x08 측정 불신(채널 불일치), 0x10 CMU 통신 타임아웃, 0x20 차량 통신 타임아웃(EVDC 0x505 끊김), 0x40 Setup 모드, 0x80 CMU CAN 전원 상태.
**0x6FD 확장 상태**: 하위 8비트는 0x6FB와 동일 + 0x100 절연 시험 실패, 0x200 SOC 무효, 0x400 CAN 12V 저전압(곧 꺼짐), 0x800 컨택터 고착/미결합, 0x1000 여분 셀 검출.

**DBC 대조 (2026-09-16, [공식 DBC](https://docs.prohelion.com/Battery_Management_Systems/Prohelion_BMS_D1000_Gen1/Communications_Protocol/DBC.html))** — 데이터시트 표와 비교한 결과:

| 항목 | 데이터시트 | DBC | 구현 |
|---|---|---|---|
| CMU 셀 전압·온도 (+1~+3+3n) | `data_16` (signed) | (DBC에 없음 — ID가 CMU 수에 따라 변함) | i16 |
| 0x6F4/0x6F5 SOC | `data_fp` | float32 (`SIG_VALTYPE_ 1`) | float → u16 스케일 |
| 0x6F6 충전 제어 3항목 | `data_16` | `@1-` | i16 |
| 0x6F6 Total Pack Capacity | `data_u16` | `@1-` 불일치 | u16 (실값 범위에선 무관) |
| 0x6F8 Min/Max 셀전압 | `data_u16` | `@1+` | u16 |
| 0x6F9 Min/Max 셀온도 | `data_u16` | `@1+` (0.1℃) | **i16** — 0~3276.7℃에선 동일, 영하가 2의 보수로 오면 i16만 맞음 |
| 0x6FA 전압 / 전류 | `data_u32` / `data_32` | `@1+` / `@1-` | u32 / i32 |
| 0x600 Serial | `data_u32` | `@1-` 불일치 | u32 (식별자라 무관) |
| **0x6F7 타이머 경과 플래그 위치** | **byte6** (`data_u8[6]`), byte4~5 Unused | **byte4** (`32\|8`), byte5~6 Unused | 불일치 → `byte4 \| byte6` 로 읽음 (Unused 는 0이라 어느 쪽이 맞아도 동일). 실물 프레임으로 확정 필요 |

**예약 ID**: 데이터시트 원문 — "Do not transmit data on reserved IDs, as this may trigger configuration or boot load events and mode changes." → 송신 금지 목록(BMU 블록 전체 + 0x7F0~0x7F4)으로 반영됨.

### 2-3. BMU 수신 (참고 — 우리 보드는 송신하지 않음)

BMU는 **EV Driver Controls 스위치 상태(기본 0x505)**를 **>5 Hz**로 받아야 컨택터를 결합한다(Off 0x0000 → Start 0x0070 → Run 0x0030). 끊기면 0x6FB의 0x20(차량 통신 타임아웃)이 선다. **VCU↔BMS 사이의 사항이며 텔레메트리 보드와 무관** — 우리는 이 플래그를 받아 기록만 한다.

### 2-4. 무선/SD 패킷 매핑 (`SRC_BMS` = 10)

| key | 티어 | 슬롯 | 출처 |
|---|---|---|---|
| 0x00 PACK_VI | PRIMARY | PackV u16(cV), PackI i16(10mA), MinCell u16, MaxCell u16(mV) | 0x13A, 0x138 |
| 0x01 SOC_TEMP | PRIMARY | SOC u16(‰), MinCellT i16, MaxCellT i16(0.1℃), pad | 0x134, 0x139 |
| 0x02 STATUS | PRIMARY | **누적 상태 u32** (0x6FD·0x6FB 비트 OR + bit31 프리차지 Error 거침, sticky), Contactor u8 (현재값 | 오류비트 0x23 누적), PrechargeState u8 (현재), StatusFlags u8 (0x6FB **현재값**), CMU수 u8 | 0x13D, 0x137, 0x13B |
| 0x10 CELL_LOC | DETAIL | MinV CMU, MinV Cell, MaxV CMU, MaxV Cell, MinT CMU, MaxT CMU, pad×2 (u8) | 0x138, 0x139 |
| 0x11 SOC_DETAIL | DETAIL | SOC_Ah u16(0.01Ah), BalSOC_Ah u16(0.01Ah), BalSOC u16(‰), pad | 0x134, 0x135 |
| 0x12 CHARGER | DETAIL | ChgCellVErr i16, CellTMargin i16, DchgCellVErr i16, Capacity u16 | 0x136 |
| 0x13 FAN_12V | DETAIL | Fan0, Fan1(rpm), I12V 팬, I12V CMU(mA) (u16×4) | 0x13C |
| 0x14+n CMU_CELL_A | DETAIL | CMU n Cell0~3 (i16×4, mV) | +0x02+3n |
| 0x18+n CMU_CELL_B | DETAIL | CMU n Cell4~7 (i16×4, mV) | +0x03+3n |
| 0x1C+n CMU_TEMP | DETAIL | CMU n PCB_T, Cell_T (i16, 0.1℃), pad×2 | +0x01+3n |
| 0x20 BAL_PRECHG | SD_ONLY | BalRise u16, BalFall u16(mV), TimerElapsed u8, TimerCnt u8, pad×2 | 0x13B, 0x137 |
| 0x21~0x23 | — | 비움 (식별 정보·v4 12V 제거, 2026-09-16) | — |
| 0x24+n CMU_SERIAL | SD_ONLY | CMU n Serial u32, pad | +0x01+3n |

> 타임아웃 경고(`ERR_TIMEOUT`)는 팩(BMU 프레임, 1000ms)과 CMU 1~4(각 3000ms — CMU가 ~1Hz라 1000ms면 지터로 깜빡임)를 따로 감시한다. value[0]=source(10), **value[1]=CMU 번호(1~4, 팩은 0)**.
>
> **STATUS 패킷의 sticky 의미 (2026-09-16)** — BMU 송신(1Hz, 0x6F7 은 상태 변화 시에도)과 1s 스케줄러는 비동기라, 현재값만 보내면 1초 안에 떴다 사라진 고장이 한 번도 전송되지 않을 수 있다. 그래서 u32 와 컨택터 오류 비트는 **"직전 전송 이후 한 번이라도 떴던 비트"**(CAN RX 에서 OR 누적, 큐 투입 성공 시 해제), byte6(0x6FB 플래그)은 **현재값**이다. 지상국은 u32 를 "최근 1초 내 발생", byte6 을 "지금 상태"로 표기할 것. u32 의 bit31(`BMS_SEEN_PRECHARGE_ERROR`)은 텔레메트리 보드가 덧붙인 비트다(Prohelion 은 bit0~12만 사용).

### 2-5. 폐기된 구 BMS (BQ 기반 BOT/TOP, ~2026-08)

구 체계(빅엔디안, PRIMARY 0x040~0x047 / DETAIL 0x048~0x059 / SD_ONLY 0x05A~0x07F / LEGACY 0x111~0x233, `SRC_BMS_BOT`=1·`SRC_BMS_TOP`=2)는 2026-09 전량 삭제했다. 스택/팩 전압 구분, 쿨롱카운터, Safety/PF, CUV/COV 스냅샷, CB_Status 밸런싱 비트맵, FET 상태는 Prohelion에 대응 항목이 없어 더 이상 기록되지 않는다.
**source 값 1·2는 `SRC_RETIRED_BMS_BOT/TOP`으로 남겨 재사용을 금지**한다 — 기존 SD 로그 해석용. 과거 로그의 키 레이아웃은 git 이력(2026-08 이전 이 문서)을 참고.

---

## 3. MPPT CAN ID 상세

> 제조사: Elmar Solar Best MPPT
> **base 주소 = general_id(공장설정) + 로터리 스위치×0x10** (데이터시트 §8.3). 2026-07 general_id를 0x600→0x300으로 발주 반영 — 로터리 스위치 1/2/3/4 사용.
> **로터리 스위치를 반드시 1/2/3/4로 설정** (0이면 base=general_id 그대로라 충돌 가능)
> 전체 신호 타입: **FLOAT** → CAN RX 시점에 scaled int로 변환하여 VehicleData 저장.
> **2026-08: MPPT 3대 → 4대 증설.** #4만 신규 추가되고 #1~#3의 base 주소는 변경 없음.

| MPPT | 로터리 | base 주소 |
|---|---|---|
| MPPT #1 | 1 | **0x310** (구 0x610) |
| MPPT #2 | 2 | **0x320** (구 0x620) |
| MPPT #3 | 3 | **0x330** (구 0x630) |
| MPPT #4 | 4 | **0x340** (2026-08 증설) |

### TX 프레임 (MPPT → STM32, 7개)

| base+ | CAN ID 예 (#1) | 오프셋 | 신호명 | 원본 타입 | 단위 | 변환 규칙 | 무선 |
|---|---|---|---|---|---|---|---|
| +0 | 0x310 | 0~3 | Input_Voltage | FLOAT | V | ×100 → u16 | PRIMARY #7[0] |
| +0 | 0x310 | 4~7 | Input_Current | FLOAT | A | ×100 → u16 | PRIMARY #7[1] |
| +1 | 0x311 | 0~3 | Output_Voltage | FLOAT | V | ×100 → u16 | PRIMARY #7[2] |
| +1 | 0x311 | 4~7 | Output_Current | FLOAT | A | ×100 → u16 | PRIMARY #7[3] |
| +2 | 0x312 | 0~3 | Mosfet_Temp | FLOAT | ℃ | ×10 → i16 | **DETAIL 1s** — KEY_MPPT_TEMP_FLAGS[0] (2026-07-13 PRIMARY→DETAIL) |
| +2 | 0x312 | 4~7 | Controller_Temp | FLOAT | ℃ | ×10 → i16 | **DETAIL 1s** — KEY_MPPT_TEMP_FLAGS[1] |
| +3 | 0x313 | 0~3 | 12V_Aux | FLOAT | V | ×100 → u16 | **SD_ONLY** — KEY_MPPT_AUX_1[0] (2026-07-13 DETAIL→SD_ONLY) |
| +3 | 0x313 | 4~7 | 3V_Aux | FLOAT | V | ×1000 → u16 | **SD_ONLY** — KEY_MPPT_AUX_1[1] |
| +4 | 0x314 | 0~3 | MaxOutput_Voltage | FLOAT | V | ×100 → u16 | DETAIL 1s — KEY_MPPT_MAXLIM[0] (2026-07-13 AUX_1→MAXLIM 분리) |
| +4 | 0x314 | 4~7 | MaxInput_Current | FLOAT | A | ×100 → u16 | DETAIL 1s — KEY_MPPT_MAXLIM[1] |
| +5 | 0x315 | 3 | Error_Flags | u8 | 비트 | **sticky OR 누적** | **DETAIL** — KEY_MPPT_TEMP_FLAGS[2] (2026-07-13 PRIMARY→DETAIL, 2026-08 sticky화) |
| +5 | 0x315 | 4 | Mode | u8 | — | 그대로 | **DETAIL 1s** — KEY_MPPT_TEMP_FLAGS[3] |
| +5 | 0x315 | 5 | Limit_Flags | u8 | 비트 | 그대로 | (미사용 — 슬롯 없음) |
| +6 | 0x316 | 0~3 | PowerConn_Voltage | FLOAT | V | ×100 → u16 | DETAIL 1s — KEY_MPPT_AUX_2[0] |
| +6 | 0x316 | 4~7 | PowerConn_Temp | FLOAT | ℃ | ×10 → i16 | DETAIL 1s — KEY_MPPT_AUX_2[1] |

> 2026-07-13 티어 재배치: MPPT DETAIL(무선 1s)은 **TEMP_FLAGS(base+2+base+5), MAXLIM(base+4), AUX_2(base+6)** 3패킷/유닛 × **4유닛** = **12개**. SD_ONLY(무선 미전송)는 **AUX_1(base+3 12V/3V)** 1패킷/유닛 × 4 = **4개**. PRIMARY는 IV_IN_OUT만(4개).
> (2026-08 4대 증설 반영 — 유닛당 패킷 구성은 동일하고 유닛 수만 3→4)

### RX 프레임 (STM32 → MPPT, 3개, 별도 설계)

| base+ | CAN ID 예 (#1) | 내용 |
|---|---|---|
| +8 | 0x318 | Mode 설정 |
| +A | 0x31A | MaxOutput_Voltage 설정 |
| +B | 0x31B | MaxInput_Current 설정 |

---

## 4. Motor (잠정 / virtual — WaveSculptor22 기반)

> **출처**: `CAN DATA/TritiumWaveSculptor22_Manual.pdf`(Tritium WaveSculptor 22 User's Manual TRI88.004 ver 4) Appendix C (CAN Comms Protocol) — 2026-07 원문 직접 확보·대조 완료 (이전엔 매뉴얼 없이 추정).
> **base 주소**: 0x400 가정 (실제 VCU 재전송 ID로 교체 예정 — VCU 경유 구조는 여전히 미확정, Motor 상태 잠정 유지).
> **원본 타입**: 전부 **IEEE 754 32비트 float** → CAN RX 시점에 scaled int 변환 (MPPT와 동일 전략).
> **엔디안**: 리틀엔디안(Intel) — 매뉴얼 §18.1.4 "least significant byte first" 확인됨(BMS와 반대, `__REV` 불필요).
> **바이트 순서(중요)**: 매뉴얼 표는 "Variable / Bits" 형식으로 **먼저 나열된 변수가 bits 63..32(byte 4~7), 나중 변수가 bits 31..0(byte 0~3)**. 2026-07: 이 순서를 반대로 착각해 V/I Vector·BackEMF·전원레일·Slip Speed **5곳에서 바이트 순서 오류**가 있었음 — 매뉴얼 직접 대조로 발견·수정(`can_receiver.c` 참조). 아래 표는 전부 정정된 값.

### 4-1. 무선 PRIMARY 200ms 대상

| base+ | CAN ID | 오프셋 | 신호명 | 원본 타입 | 단위 | 변환 규칙 | 무선 패킷 |
|---|---|---|---|---|---|---|---|
| +0x02 | **0x402** | 0~3 | Bus_Voltage | FLOAT | V | ×100 → u16 | #10 BUS_VEL[0] |
| +0x02 | 0x402 | 4~7 | Bus_Current | FLOAT | A | ×10 → i16 (regen 음수) | #10 BUS_VEL[1] |
| +0x03 | **0x403** | 0~3 | Motor_Velocity | FLOAT | rpm | 반올림 → i16 (역방향 음수) | #10 BUS_VEL[2] |
| +0x03 | 0x403 | 4~7 | Vehicle_Velocity | FLOAT | m/s | ×100 → u16 | #10 BUS_VEL[3] |
| +0x0B | **0x40B** | 4~7 | HeatSink_Temp | FLOAT | ℃ | ×10 → i16 | #11 TEMP[0] |
| +0x0B | 0x40B | 0~3 | Motor_Temp | FLOAT | ℃ | ×10 → i16 | #11 TEMP[1] (슬롯[2][3]=pad) |
| +0x07 | **0x407** | 4~7 | BEMFd (항상 0V) | FLOAT | V | ×100 → i16 | #12 BEMF[0] (2026-07-13 SD_ONLY→PRIMARY) |
| +0x07 | 0x407 | 0~3 | BEMFq (피크 전압) | FLOAT | V | ×100 → i16 | #12 BEMF[1] |
| +0x0E | **0x40E** | 0~3 | Odometer | FLOAT | m | 반올림 없음 | #13 ODO[0] (2026-07-13 PRIMARY 신규, 규정 10s와 병행) |
| +0x0E | 0x40E | 4~7 | DC_Bus_AmpHours | FLOAT | Ah | 반올림 없음 | #13 ODO[1] |

> 2026-07-13: 0x401 Limit/Error Flags는 PRIMARY(구 #14)에서 빠지고 아래 DETAIL KEY_MOTOR_STATUS로 통합됨. Motor PRIMARY는 이제 BUS_VEL/TEMP/BEMF/ODO 4패킷.

### 4-2. 무선 DETAIL 1s

| base+ | CAN ID | 오프셋 | 신호명 | 원본 타입 | 단위 | 저장 |
|---|---|---|---|---|---|---|
| +0x0C | **0x40C** | 0~3 | DSP_Board_Temp | FLOAT | ℃ | ×10 → i16 — KEY_MOTOR_DSP_TEMP (1패킷) |
| +0x01 | **0x401** | 0~1 | Limit_Flags | u16 | 비트 | **sticky OR 누적** — KEY_MOTOR_STATUS[0] |
| +0x01 | 0x401 | 2~3 | Error_Flags | u16 | 비트 | **sticky OR 누적** — KEY_MOTOR_STATUS[1] |
| +0x01 | 0x401 | 4~5 | Active_Motor | u16 | — | 그대로 — KEY_MOTOR_STATUS[2] |
| +0x01 | 0x401 | 6 | Transmit_Error_Count | u8 | — | 그대로 — KEY_MOTOR_STATUS byte6 |
| +0x01 | 0x401 | 7 | Receive_Error_Count | u8 | — | 그대로 — KEY_MOTOR_STATUS byte7 |
| +0x04 | **0x404** | 0~3 | Phase B 전류 | FLOAT | Arms | ×10 → i16 — KEY_MOTOR_PHASE_I[0] (2026-07-13 SD_ONLY→DETAIL) |
| +0x04 | 0x404 | 4~7 | Phase C 전류 | FLOAT | Arms | ×10 → i16 — KEY_MOTOR_PHASE_I[1] |

> **2026-07-13**: 0x401 Status Information 전체(Limit/Error/Active_Motor/TX·RX ErrCnt)를 단일 KEY_MOTOR_STATUS(DETAIL) 패킷으로 통합. Phase 전류(0x404)는 SD_ONLY→DETAIL 승격.
> 0x40E는 여기(구 DETAIL/규정)가 아니라 **PRIMARY(#13, §4-1)**로 이동함. 단, WSC 규정 제출용 10s 누적 패킷(SRC_SYS/KEY_REG_DATA)은 그대로 병행 유지.

### 4-3. SD_ONLY (무선 제외 — 진단/사후 분석) — 구현 완료·매뉴얼 대조 정정(2026-07, 잠정 스케일: 전압류 ×100, 전류류 ×10)

| base+ | CAN ID | 오프셋 | 신호명 | 원본 단위 | 변환 규칙 | 무선 패킷(SD_Queue 전용) |
|---|---|---|---|---|---|---|
| +0x05 | 0x405 | 4~7 | Vd (전압벡터 실수부) | V | ×100 → i16 | KEY_MOTOR_V_VECTOR[0] |
| +0x05 | 0x405 | 0~3 | Vq (전압벡터 허수부) | V | ×100 → i16 | KEY_MOTOR_V_VECTOR[1] |
| +0x06 | 0x406 | 4~7 | Id (전류벡터 실수부) | A | ×10 → i16 | KEY_MOTOR_I_VECTOR[0] |
| +0x06 | 0x406 | 0~3 | Iq (전류벡터 허수부, 토크 성분) | A | ×10 → i16 | KEY_MOTOR_I_VECTOR[1] |
| +0x08 | 0x408 | 4~7 | 15V 레일 (**byte 0~3은 Reserved, 3.3V 아님**) | V | ×100 → u16 | KEY_MOTOR_RAILS[0] |
| +0x09 | 0x409 | 4~7 | 3.3V 레일 | V | ×100 → u16 | KEY_MOTOR_RAILS[1] |
| +0x09 | 0x409 | 0~3 | 1.9V 레일 (DSP) | V | ×100 → u16 | KEY_MOTOR_RAILS[2] |
| +0x17 | 0x417 | 4~7 | Slip Speed (**byte 0~3은 Reserved**) | Hz | ×10 → i16 | KEY_MOTOR_SLIP |

> **2026-07-13 SD_ONLY 축소**: Phase 전류(0x404)는 DETAIL로, BackEMF(0x407)는 PRIMARY로 승격되어 이 표에서 빠짐. 현재 SD_ONLY는 V_VECTOR(0x405)/I_VECTOR(0x406)/RAILS(0x408·0x409)/SLIP(0x417) 4패킷.
> **매뉴얼 대조로 밝혀진 구조 오류**: 0x408은 15V만 담고 있고(byte 0~3은 Reserved), 3.3V는 별도 프레임 0x409에 있음 — 이전엔 0x408 하나에서 15V+3.3V를 잘못 읽고 있었음. 0x417도 byte 0~3이 아니라 4~7에서 읽어야 함.
> **참고**: 매뉴얼 원문 네이티브 송신 주기는 대부분 200ms(Phase Current/Vector/BackEMF/Slip)·1s(전원레일)이지만, 우리는 대역 절약을 위해 SD_ONLY(무선 미전송)로 분류함 — WaveSculptor 자체 배선 주기와 무관한 우리 팀의 대역폭 정책 결정.

> **무선 미전송, SD_Queue만** — `Sched_EmitSdOnly()`가 `to_uart=0`으로 전송. 대역폭 계산에서 제외.

> **참고**: DriveCommand(+0x01 RX)·PowerCommand(+0x02 RX)는 드라이버→모터 **명령** 프레임이므로 텔레메트리 보드는 수신·기록만, 송신 안 함. 모터는 명령 250ms 미수신 시 중립 진입(우리 alive-flag 워치독으로 단절 감지).
> **규모**: PRIMARY 4패킷(BUS_VEL/TEMP/BEMF/ODO) + DETAIL 3패킷 + SD_ONLY 4패킷. FDCAN1 HW 필터 1(0x400~0x4FF 범위)로 전부 수용.

---

## 4.5 BMS(LV) — Prohelion D1000 Gen1, 베이스 0x600 (2026-09-23 복원)

> 저전압 보조 배터리. **4S2P** — 직렬 4셀, 병렬 2조. HV(§2)와 **같은 기종**이며 기본 베이스 **0x600 그대로** 쓴다.
> HV 는 Profinity 로 베이스를 0x040 으로 옮겨 놨으므로 두 블록은 겹치지 않는다. 오프셋 표·엔디안·스케일은 §2 와 동일하다.
>
> **이력**: 2026-07-13 에 처음 구현했고, 2026-08 팀 결정으로 차량 구성에서 빠지며 수신·파싱·송출·필터가 전부 삭제됐다(이 절도 함께 지워졌다). 2026-09-23 다시 쓰기로 해 복원했다. 삭제는 git 이력 이전에 이뤄져 되돌릴 커밋이 없었고, HV 파서를 재사용해 새로 구현했다.

### 4.5-1. 구성과 CMU

| 항목 | 값 | 근거 |
|---|---|---|
| CMU 수 | **1** (`BMS_LV_CMU_COUNT`) | 직렬 4셀 — CMU 1대(최대 8S)로 충분 |
| 유효 셀 | 4 (`BMS_LV_CELLS`) | CMU 의 셀 4~7 칸은 Prohelion 이 **-32768(셀 없음)** 으로 보고한다 |
| 병렬 2조 | 프레임 구조 영향 없음 | BMU 에게는 병렬 한 묶음이 한 셀로 보인다 |
| 데이터 구조 | `VehicleData.bms_lv` (`BmsData_t`) | HV 와 같은 타입. CMU 배열의 남는 3칸(84B)은 쓰지 않는다 — 구조체를 쪼개면 파서·스케줄러·지상국이 전부 따라 바뀌어 얻는 것보다 잃는 게 크다 |

### 4.5-2. 수신하는 ID

§2-1 표의 "(기본 0x600)" 열이 곧 LV ID 다. 파서는 HV 와 같은 `CAN_StoreBmsCmu` / `CAN_StoreBmsPack` 을 쓰고, CMU 수만 `BMS_LV_CMU_COUNT` 로 제한한다.

| ID | 내용 | 무선 | 신선도 감시 |
|---|---|---|---|
| 0x601 / 0x602 / 0x603 | CMU0 상태 / Cell0~3 / Cell4~7 | SD 만 | — (CMU 이상은 BMU 가 0x6FB 로 알린다) |
| 0x6F4 | SOC (Ah·% float32) | **PRIMARY** | 확인 |
| 0x6F8 | 최소/최대 셀 전압 | **PRIMARY** | 확인 |
| 0x6F9 | 최소/최대 셀 온도 | **PRIMARY** | 확인 |
| 0x6FA | 팩 전압 / 전류 | **PRIMARY** | 확인 |
| 0x6FB | 팩 상태 (플래그·CMU 수) | SD 만 | 확인 |
| 그 밖의 0x6F5~0x6FD | 밸런스 SOC·충전 제어·프리차지·팬·확장 상태 | SD 만 | — |

### 4.5-3. 무선·SD 패킷 (`SRC_BMS_LV` = 8)

키는 **HV 와 같은 `BMS_KEY` 열거**를 쓴다. 키는 소스별 공간이라 `SRC_BMS`(10)와 `SRC_BMS_LV`(8)가 같은 키 번호를 써도 섞이지 않는다.

| 키 | 티어 | 슬롯 |
|---|---|---|
| `KEY_BMS_PACK_VI` (0x00) | PRIMARY 1s | PackV(cV,u16), PackI(10mA,i16), MinCell(mV), MaxCell(mV) |
| `KEY_BMS_SOC_TEMP` (0x01) | PRIMARY 1s | SOC(‰,u16), MinCellT(0.1℃,i16), MaxCellT(0.1℃,i16), pad |

HV 의 `KEY_BMS_STATUS` 같은 상태 패킷은 LV 로 보내지 않는다 — 보조 배터리는 SOC 와 팩 V/I 로 상태 판단이 되고, 무선 대역은 주행 데이터가 먼저다. 상세는 SD 에 남는다.

### 4.5-4. 소스 값 8 을 되살린 이유

`LOG_SOURCE` 값 8 은 2026-08 제거 때 **지우지 않고 `SRC_RESERVED3` 으로 비워 뒀던** 원래의 `SRC_BMS_LV` 자리다. 같은 의미로 되돌렸으므로:
- 뒤따르는 `SRC_CAN_ERR`(11)~`SRC_SD_ERR`(14)는 한 칸도 움직이지 않았다.
- 2026-07~08 의 구 LV 로그도 같은 값·같은 뜻이라 그대로 해석된다.
- (`SRC_ADC` 를 끝(15)에 붙였던 이유 — "8 에는 구 LV 로그가 남아 있다" — 가 이제는 "8 은 LV 가 쓰고 있다"가 됐다.)

### 4.5-5. 더미 노드

`tests/dummy_nodes/BMS_Dummy` 의 `send_bms_lv()` 가 위 ID 들을 Prohelion 형식으로 만든다(셀 3.0~3.5V, 팩 12~14V, 셀 4~7 은 -32768).
같은 더미의 HV 쪽(BOT/TOP)은 아직 **구 BQ 형식**이다 — 수신측 HV 가 2026-09 에 Prohelion 으로 바뀐 것이 더미에는 반영되지 않았다. LV 만 현행 형식이다.

## 4.6 온보드 ADC — J3 리니어 포텐셔미터 (2026-09-17, CAN 아님)

> 센서: HGSI **LPPS-22** 리니어 포텐셔미터 ×2 (5kΩ, 비율 출력 0~100% × 구동 전압). **5V 구동**.
> 배선: J3-1 AGND(센서 파랑), J3-2 AIN1(센서1 검정), J3-3 AIN2(센서2 검정). 센서 갈색(+V)은 J3에 핀이 없어 **보드 5V에서 별도 배선**.
> 회로: AINx → 470Ω → PC2/PC3(ADC1_IN3/IN4) → 910Ω → AGND. 분압 0.659 → **입력 5.0V = ADC 3.3V**. 12V 구동 금지(핀에 7.9V).
> **부하 오차**: 분압 합 1.38kΩ이 센서 5kΩ를 끌어내려 스트로크 중간에서 최대 약 48% 낮게 읽힌다(양 끝은 정확).
> **상단 포화 여유 3mV**: 5V 입력 → 핀 3.297V (VREF+ 3.3V). 저항·5V 레일 오차로 스트로크 끝이 쉽게 포화된다.
> → 하드웨어 수정 권장: R24~R27을 47kΩ(직렬)/82kΩ(접지)로(5V → 3.18V), C49/C50에 100nF 실장 후 `adc_sensor.c` 분압 상수 수정.

| 항목 | 값 |
|---|---|
| 게이트 | `PHASE9_ADC_SENSOR` (phase_config.h) |
| 샘플링 | ADC_Thread(우선순위 12)가 **50Hz**, 14비트, 하드웨어 오버샘플 ×16 |
| 전송 | DETAIL(5s) step 3 — `SRC_ADC`(=**15**) / `KEY_ADC_POT + n` (0x10=AIN1, 0x11=AIN2) |
| 슬롯 | AvgIn, MinIn, MaxIn (J3 입력단 **mV**, u16) + MaxRaw (14비트 코드, u16) — 직전 5초 요약. **mm 아님** |
| 샘플 없음 | 4슬롯 모두 **0xFFFF** (ADC 초기화·클럭 설정 실패 시 `ERR_INIT_FAIL` bit2 도 섬 — 다른 비트가 먼저 보고됐어도 새 비트로 재보고) |
| 포화 | MaxRaw = **16383** 이면 그 5초 안에 ADC 가 포화 → MaxIn(과 AvgIn)은 실제보다 낮을 수 있음 |
| mm 환산 | **지상국에서** 센서 실측 다점 보정표(mV → mm)로 한다. 펌웨어는 스트로크·구동전압을 모른다(2026-09-17 점검 반영 — 부하 오차·포화·미확정 스트로크를 안은 값을 0.01mm 로 보내면 정밀한 값처럼 보이기 때문) |

> `SRC_ADC`를 빈 슬롯 8이 아니라 **끝(15)** 에 붙인 이유: 8에는 구 LV BMS 로그가 남아 있다. 기존 source 값은 하나도 움직이지 않았다.

---

## 4.7 신선도 상태표 — `SRC_SYS` / `KEY_SYS_FRESHNESS` = 16 (2026-09-18)

> **데이터 패킷은 그대로다.** 1초마다 이 패킷 1개가 추가로 나가(무선+SD), "각 값이 지금 최신인가"만 알린다.
> 장치 단위 수신 시각(`bms_last_tick` 등)만으로는 "장치는 통신 중인데 특정 프레임만 멈춘" 경우를 못 잡았다.

- value = **[stale u32][never u32]**. 비트 i = 아래 묶음 i. **stale 은 never 를 포함**한다. 둘 다 0 이면 정상.
- 펌웨어는 **프레임별** 마지막 수신 시각을 기록하고(`freshness.c`), 묶음의 필수 프레임을 **전부** 본다 — 하나라도 한 번도 안 왔으면 never, 하나라도 기준 시간을 넘겼으면 stale.
- 기준 시간 = **CAN 원본 주기 × 3, 최소 1초**. 판정이 1초 주기라 실제 감지는 최대 약 1초 더 늦다.
- 길이 미달(DLC) 프레임은 기록하지 않는다. ADC 는 샘플 획득 성공 시각을 쓴다. 합성 데이터 모드는 모든 프레임을 "방금 수신"으로 기록한다.

| 비트 | 묶음 | 필수 프레임 (베이스 0x040 기준) | 기준 |
|---|---|---|---|
| 0 | BMS 팩 전압·전류 | 0x13A | 1s |
| 1 | BMS 셀 전압 최소/최대 | 0x138 | 1s |
| 2 | BMS 충전 제어 | 0x136 | 1s |
| 3 | BMS SOC | 0x134 | 3s |
| 4 | BMS 셀 온도 최소/최대 | 0x139 | 3s |
| 5 | BMS 상태 | 0x13B, 0x13D | 3s |
| 6 | BMS 프리차지·컨택터 | 0x137 | 3s |
| 7 | BMS Balance SOC·팬 | 0x135, 0x13C | 3s |
| 8~11 | CMU 1~4 | 각 +1/+2/+3 세 프레임 | 3s |
| 12~15 | MPPT 1~4 전력 | base+0, +1 | 3s 임시(주기 실측 필요) |
| 16~19 | MPPT 1~4 온도·상태 | base+2, +5 | 3s 임시 |
| 20 | 모터 버스·속도 | 0x402, 0x403 | 1s 임시(VCU 확정 후) |
| 21 | 모터 온도 | 0x40B, 0x40C | 3s 임시 |
| 22 | 모터 상태 | 0x401 | 1s 임시 |
| 23 | 주행거리 | 0x40E | 3s 임시 |
| 24~25 | ADC AIN1/AIN2 | 샘플 획득 성공 | 1s |
| 26 | LV 팩 V/I | 0x6FA, 0x6F8 | 1s (§4.5, 2026-09-23) |
| 27 | LV SOC·온도·상태 | 0x6F4, 0x6F9, 0x6FB | 3s |
| 28~31 | 예약 | | |

**지상국 표시 규칙** (`fresh.js`, 나쁜 순):
미수신(값 대신 "—") > 오래됨(마지막 값 흐리게) > **상태 확인 불가**(상태표가 3초 넘게 안 옴 · 이 기능 없는 펌웨어) > 확인 대기(화면 연 직후 5초) > 정상. 미장착 장치는 `fresh.js` 의 `DISABLED` 에 묶음 번호를 넣어 "사용 안 함"으로 뺀다.
정상이 아닌 값은 **계산(팩 전력·MPPT 합계·에너지 수지·누적 발전량)과 그래프에서 제외**하고, 그래프는 그 구간을 끊는다.

## 4.8 코인 셀(VBAT) 전압 — `SRC_SYS` / `KEY_SYS_VBAT` = 17 (2026-09-29)

> CAN 과 무관한 보드 자체 값이다. 부팅 직후 1회 + **60초마다** 1패킷(무선+SD). 첫 값은 SD 준비 전이라 카드에 빠질 수 있다.

- value = **[VBAT mV u16][원시 코드 u16][VDDA mV u16][0]**, 전부 LE. 원시 코드는 14비트(칩 내부 1/4 분압 VBAT/4).
- 보드의 VBAT 는 코인 홀더(H1)·C24 에만 연결돼 있어 **코인 셀 자체**를 잰다. 전압만 읽을 수 있고 용량(%)은 못 읽는다.
- 측정: ADC1 injected 그룹 1회(`ADC_CHANNEL_VBAT`, IN18). 분압 경로는 그 변환 동안만 켠다(켜 두면 코인에서 전류를 끈다). 환산은 같은 스레드가 VREFINT 로 잰 VDDA 를 쓴다.
- **판정(정상 / 교체 시기 / 없음)은 지상국**이 한다 — `VehicleSnapshot.VBAT_LOW_MV`(2700) / `VBAT_ABSENT_MV`(2000). 2026-09-29 실측으로 확정: 코인 있음 3,292 mV(멀티미터 3,252), 코인 없음 1,027 → 960 mV(떠 있는 핀, 천천히 내려감).
- `PHASE9_ADC_SENSOR` 가 꺼져 있으면 재지 않으므로 보내지 않는다.

---

## 5. 무선 패킷 묶음 목록 (PRIMARY 1s)

> 패킷 구조: `[timestamp 4B][level 1B][source 1B][key 1B][value 8B][checksum 1B]` = 16B
> 슬롯 인덱스: value[8B] 안의 0~3번 위치.

| # | source | key | 슬롯[0] | 슬롯[1] | 슬롯[2] | 슬롯[3] | 묶음 |
|---|---|---|---|---|---|---|---|
| 1 | SRC_BMS | PACK_VI | PackV (cV) | PackI (10mA) | MinCell (mV) | MaxCell (mV) | u16,i16,u16,u16 |
| 2 | SRC_BMS | SOC_TEMP | SOC (‰) | MinCellT (0.1℃) | MaxCellT (0.1℃) | pad | u16,i16,i16,- |
| 3 | SRC_BMS | STATUS | 누적 상태 (u32, sticky) | Contactor(현재|오류누적)·Precharge(현재) (u8×2) | StatusFlags(현재)·CMU수 (u8×2) | — | u32,u8×4 |
| 4 | **SRC_BMS_LV** | PACK_VI | PackV (cV) | PackI (10mA) | MinCell (mV) | MaxCell (mV) | u16,i16,u16,u16 (§4.5, 2026-09-23) |
| 5 | **SRC_BMS_LV** | SOC_TEMP | SOC (‰) | MinCellT (0.1℃) | MaxCellT (0.1℃) | pad | u16,i16,i16,- (§4.5) |
| 6 | SRC_MPPT1 | IV_IN_OUT | InV (×100) | InI (×100) | OutV (×100) | OutI (×100) | 4×u16 |
| 7 | SRC_MPPT2 | IV_IN_OUT | (동일) | | | | 4×u16 |
| 8 | SRC_MPPT3 | IV_IN_OUT | (동일) | | | | 4×u16 |
| 9 | SRC_MPPT4 | IV_IN_OUT | (동일) | | | | 4×u16 |
| 10 | SRC_MOTOR | BUS_VEL | BusV (×100) | BusI (×10) | MotorRPM | VehSpeed (×100) | u16,i16,i16,u16 (잠정) |
| 11 | SRC_MOTOR | TEMP | HeatsinkT (×10) | MotorT (×10) | pad | pad | i16,i16,-,- (잠정) |
| 12 | SRC_MOTOR | BEMF | BEMFd (×100) | BEMFq (×100) | pad | pad | i16,i16,-,- (잠정) |
| 13 | SRC_MOTOR | ODO | Odometer (m) | DC_Bus_AmpHours (Ah) | — | — | 2×float (잠정) |

> **2026-09-23 LV BMS 복원 — 지상국 디코더 필수 반영**: PRIMARY 11→**13패킷**. `SRC_BMS_LV`(8) 두 패킷이 #4/#5 에 들어가고 MPPT·Motor 가 두 칸씩 밀렸다.
> 패킷은 source·key 로 스스로를 밝히므로 **번호가 밀려도 디코딩은 깨지지 않는다** — 다만 source 8 을 모르는 디코더는 LV 패킷을 조용히 버린다.

> 잠정 = 잠정값 (Motor: WaveSculptor22 기반, VCU 확정 시 교체).
>
> **2026-08 고장 플래그 sticky화 — 지상국 표시 문구 반영 필요 (값의 의미 변경)**:
> - 대상: **Motor `KEY_MOTOR_STATUS`의 Limit_Flags/Error_Flags**(0x401), **MPPT `KEY_MPPT_TEMP_FLAGS`의 Error_Flags**(0x315 byte3).
> - 이 비트들의 의미가 "**전송 시점에 떠 있는 비트**"에서 "**직전 주기(5초) 동안 한 번이라도 떴던 비트의 합집합**"으로 바뀌었다. 펌웨어가 CAN RX마다 OR로 누적하고, 패킷에 실은 직후 클리어한다.
> - **이유**: DETAIL 주기가 5초로 늦춰지면서, 잠깐 떴다 사라지는 고장 비트(전류 제한 진입, 일시적 오류 등)가 샘플링 전에 덮어써져 **한 번도 관측되지 않는** 문제가 있었다. BMS Fault와 달리 Motor/MPPT 플래그는 기기 측 래치가 보장되지 않는다.
> - **지상국 조치**: "현재 제한 중"이 아니라 "최근 5초 내 발생"으로 표기할 것. 비트가 한 주기만 뜨고 다음 주기에 0으로 돌아오는 것은 **정상 동작**이며 통신 오류가 아니다.
> - Active_Motor / TX·RX ErrCnt / MPPT Mode는 상태·카운터값이라 **종전대로 최신값 덮어쓰기**다.
>
> **2026-08 MPPT 4대 증설 — 지상국 디코더 필수 반영 (전송 포맷 변경)**:
> - PRIMARY에 **#10 SRC_MPPT4**(base 0x340) 신규 추가 → 이후 패킷 번호 전부 +1 시프트(Motor #10~#13→#11~#14, BMS_LV #14~#15→#15~#16, BMS SOC #16~#17→#17~#18). PRIMARY 총 17→**18패킷**.
> - **LOG 패킷 `source` 바이트 값이 시프트됨** — `SRC_MPPT4`를 `SRC_MPPT3` 뒤(=6)에 삽입했기 때문:
>   `SRC_MOTOR 6→7`, `SRC_BMS_LV 7→8`, `SRC_RESERVED1/2 8·9→9·10`, `SRC_CAN_ERR 10→11`, `SRC_SPI_ERR 11→12`, `SRC_UART_ERR 12→13`, `SRC_SD_ERR 13→14`.
>   (`SRC_SYS`~`SRC_MPPT3` 0~5는 불변) → **디코더의 source 매핑 테이블을 반드시 함께 갱신할 것.** 안 하면 Motor·BMS_LV·에러 패킷을 조용히 오독한다.
> - CAN ID는 MPPT4용 **0x340 신규 추가만** — 기존 BMS/Motor/BMS_LV/MPPT#1~#3 ID는 **변경 없음**.
>
> ### 2026-08 개정 — LV BMS 제거 (팀 결정)
> - 저전압 보조배터리 BMS(Prohelion D1000 Gen1, base 0x600)를 **차량 구성에서 제외**하기로 결정 → 수신·파싱·송출·필터를 전부 삭제했다.
> - **FDCAN1 필터 1을 0x400~0x6FF → 0x400~0x4FF로 축소** (Motor 최대 ID 0x417). LV 대역이 하드웨어에서 차단된다.
> - PRIMARY **18→16패킷**(LV SOC/PACK_VI 2개 제거, 이후 BMS SOC가 #15/#16으로 당겨짐), DETAIL **43→33**, SD_ONLY **50→48**.
> - `LOG_SOURCE`의 **값 8은 삭제하지 않고 `SRC_RESERVED3`로 남겼다.** 지우면 뒤따르는 `SRC_CAN_ERR`(11)~`SRC_SD_ERR`(14)가 전부 당겨져 지상국과 무성 불일치가 난다. 위 2026-08 MPPT4 이력의 시프트 표는 그대로 유효하다.
> - FDCAN1 HW 필터0 상한 `0x33F → 0x34F`로 확장(0x340~0x346 수용). 필터 자체는 수신 보드 내부 설정이라 타 보드 영향 없음.
>
> **2026-07-24 BMS(HV) SOC PRIMARY 신규 추가 (구 #16/#17 → 현 #17/#18)** — 지상국 디코더 필수 반영: BMS(HV) CAN ID가 0x040~0x07F 대역으로 재배치되며 SOC(0x043 BOT/0x047 TOP)가 새 PRIMARY 항목으로 추가됨. §2 참고.
>
> **2026-07-13 티어 재배치 (CAN_수신_데이터_정리본.xlsx 기준)** — 지상국 디코더 필수 반영:
> - MPPT TEMP_FLAGS(구 #8/#10/#12, MosfetT/CtrlT/Flags/Mode)는 **PRIMARY→DETAIL**로 이동 (아래 §"MPPT DETAIL" 참고). PRIMARY MPPT는 이제 IV_IN_OUT 1개/유닛만.
> - Motor #14 TEMP_FLAGS는 해체: 온도는 **KEY_MOTOR_TEMP(#11, PRIMARY)**, Limit/Error Flags는 **KEY_MOTOR_STATUS(DETAIL)**로 분리.
> - Motor 0x407 BEMF: **SD_ONLY→PRIMARY**(#12). Motor 0x40E ODO: 규정 10s에 더해 **PRIMARY(#13)** 신규.
> - BMS_LV SOC/PACK_VI(#14/#15): 신규 PRIMARY.

---

## 6. 변환 규칙 요약 (CAN RX → VehicleData)

| 신호 | 원본 | 변환 | 저장 타입 | 정밀도 |
|---|---|---|---|---|
| BMS Pack Voltage | u32 / mV | ÷10 (포화) | **u16 (cV)** | 0.01V |
| BMS Min/MaxCell Voltage | u16 / mV | 그대로 | u16 (mV) | 1mV |
| BMS CMU 셀 전압 | i16 / mV | 그대로 (음수=이상 표식) | i16 (mV) | 1mV |
| BMS SOC % / Ah | float | ×10 / ×100 | u16 (‰ / 0.01Ah) | 0.1% / 0.01Ah |
| BMS Pack Current | i32 / mA | ÷10 (포화 ±327A) | i16 | 10mA |
| BMS 온도(Min/MaxCell, CMU PCB/Cell) | (u)16 / 0.1℃ | 그대로 | i16 | 0.1℃ |
| BMS 상태 플래그(0x6FD/0x6FB/0x6F7) | u32/u8 | 그대로 | u32/u8 | — |
| MPPT 전압 (V) | FLOAT | ×100 | u16 | 0.01V |
| MPPT 전류 (A) | FLOAT | ×100 | u16 | 0.01A |
| MPPT 온도 (℃) | FLOAT | ×10 | i16 | 0.1℃ |
| (잠정) Motor Bus 전압 (V) | FLOAT | ×100 | u16 | 0.01V |
| (잠정) Motor Bus 전류 (A) | FLOAT | ×10 | i16 (regen 음수) | 0.1A |
| (잠정) Motor 속도 (rpm) | FLOAT | 반올림 | i16 (역방향 음수) | 1rpm |
| (잠정) Vehicle 속도 (m/s) | FLOAT | ×100 | u16 | 0.01 m/s |
| (잠정) Motor 온도 (℃) | FLOAT | ×10 | i16 | 0.1℃ |
| (잠정) Motor Flags | u16 | 그대로 | u16 | 비트 |
| MPPT aux 전압(12V/3V/MaxOut/PowerConn) | FLOAT | ×100(3V만 ×1000) | u16 | 0.01V(3V는 0.001V) |
| MPPT aux 전류(MaxInput) | FLOAT | ×100 | u16 | 0.01A |
| MPPT PowerConn 온도 | FLOAT | ×10 | i16 | 0.1℃ |
| (잠정) Motor DSP온도 | FLOAT | ×10 | i16 | 0.1℃ |
| (잠정) Motor SD_ONLY 전압류(V벡터/BackEMF/레일) | FLOAT | ×100 | i16/u16 | 0.01V |
| (잠정) Motor SD_ONLY 전류류(Phase/I벡터)/Slip | FLOAT | ×10 | i16 | 0.1A / 0.1Hz |
| BMS 엔디안 | 리틀엔디안 (Prohelion, 2026-09) | 변환 불필요 | — | — |
| MPPT / (잠정) Motor 엔디안 | 리틀엔디안(Intel) | 변환 불필요 | — | — |

---

## 7. VehicleData_t 구조 (CAN ID 대응 주석 포함)

> **정확한 선언의 정본은 `Core/Inc/types.h`**다. 이 절은 "어느 필드가 어느 CAN ID에서 오는가"를
> 한 눈에 보기 위한 것이며, types.h에는 없는 CAN ID 주석이 붙어 있는 게 존재 이유다.
> **구조체를 수정하면 여기도 같이 고칠 것** — 2026-07-31 점검에서 `BmsLvData_t`가 통째로 누락되고
> `VehicleData_t`의 규정 필드가 "미구현 주석"으로 남아 있는 드리프트가 발견되어 정정했다.
> (필드 단위 대조 결과 BmsData_t/MpptData_t/MotorData_t는 types.h와 일치 확인됨.)

```c
/* HV BMS — Prohelion D1000 Gen1 (2026-09). 베이스 0x040, 괄호는 기본 0x600 기준 ID */
#define BMS_CMU_COUNT      4
#define BMS_CELLS_PER_CMU  8

typedef struct {
    uint32_t serial;                      /* +0x01+3n (0x601…) CMU 시리얼                */
    int16_t  pcb_temp, cell_temp;         /* +0x01+3n (0.1℃)                           */
    int16_t  cell_mv[BMS_CELLS_PER_CMU];  /* +0x02+3n Cell0~3 / +0x03+3n Cell4~7 (mV, 음수=이상) */
    uint32_t last_tick;
} BmsCmu_t;

typedef struct {
    /* PRIMARY */
    uint16_t pack_v_cv;          /* 0x13A (0x6FA) mV÷10 포화     */
    int16_t  pack_current;       /* 0x13A (0x6FA) mA÷10 포화     */
    uint16_t min_cell_mv, max_cell_mv;          /* 0x138 (0x6F8) */
    uint16_t soc_permille;                      /* 0x134 (0x6F4) %×10 */
    int16_t  min_cell_temp, max_cell_temp;      /* 0x139 (0x6F9) 0.1℃ */
    uint32_t ext_status;                        /* 0x13D (0x6FD) */
    uint8_t  contactor_status, precharge_state; /* 0x137 (0x6F7) */
    uint8_t  status_flags, cmu_count;           /* 0x13B (0x6FB) */
    /* DETAIL */
    uint8_t  min_cell_mv_cmu, min_cell_mv_cell, max_cell_mv_cmu, max_cell_mv_cell; /* 0x138 */
    uint8_t  min_cell_temp_cmu, max_cell_temp_cmu;                                  /* 0x139 */
    uint16_t soc_ah_cah;                        /* 0x134 Ah×100 */
    uint16_t bal_soc_ah_cah, bal_soc_permille;  /* 0x135 (0x6F5) */
    int16_t  chg_cell_v_err_mv, cell_temp_margin, dchg_cell_v_err_mv; /* 0x136 (0x6F6) */
    uint16_t pack_capacity_ah;                  /* 0x136 */
    uint16_t fan0_rpm, fan1_rpm, i12v_fan_ma, i12v_cmu_ma;            /* 0x13C (0x6FC) */
    /* SD_ONLY */
    uint16_t bal_rise_mv, bal_fall_mv;            /* 0x13B */
    uint8_t  precharge_timer_elapsed, precharge_timer_count; /* 0x137 */

    uint32_t bms_last_tick;      /* BMU 프레임 */
    BmsCmu_t cmu[BMS_CMU_COUNT];
} BmsData_t;

typedef struct {
    uint16_t in_v;              /* Input Voltage  (×100)   */
    uint16_t in_i;              /* Input Current  (×100)   */
    uint16_t out_v;             /* Output Voltage (×100)   */
    uint16_t out_i;             /* Output Current (×100)   */
    int16_t  mosfet_temp;       /* Mosfet Temp    (×10)    */
    int16_t  ctrl_temp;         /* Controller Temp(×10)    */
    uint8_t  error_flags;
    uint8_t  mode;

    /* 2026-07 추가 — +3/+4/+6 aux 신호 */
    uint16_t aux_12v, aux_3v, max_out_v, max_in_i;
    uint16_t powerconn_v;   int16_t powerconn_temp;

    uint32_t mppt_last_tick;
} MpptData_t;

/* (잠정) virtual — WaveSculptor22 기반, VCU 확정 시 교체 */
typedef struct {
    uint16_t bus_v;             /* Bus Voltage   (×100, 0.01V)        */
    int16_t  bus_i;             /* Bus Current   (×10, 0.1A, regen 음수)*/
    int16_t  motor_rpm;         /* Motor Velocity(rpm, 역방향 음수)    */
    uint16_t veh_speed;         /* Vehicle Velocity (×100, 0.01 m/s)  */
    int16_t  heatsink_temp;     /* Heat-sink Temp(×10, 0.1℃)          */
    int16_t  motor_temp;        /* Motor Temp    (×10, 0.1℃)          */
    uint16_t error_flags;       /* Status ErrorFlags (비트)            */
    uint16_t limit_flags;       /* Status LimitFlags (비트)            */

    /* 2026-07 추가 — 0x401 Status Information 확장 필드 */
    uint8_t  can_rx_err_count, can_tx_err_count;
    uint16_t active_motor;

    /* 2026-07 추가 — 0x40C DETAIL(무선) + 0x404~0x417 SD_ONLY(SD만, 잠정) */
    int16_t  dsp_temp;
    int16_t  phase_b_i, phase_c_i;
    int16_t  v_vector_d, v_vector_q, i_vector_d, i_vector_q;
    int16_t  bemf_d, bemf_q;
    uint16_t rail_15v, rail_3v3, rail_1v9;
    int16_t  slip_speed;

    uint32_t motor_last_tick;   /* 타임아웃 워치독                     */
} MotorData_t;

typedef struct {
    BmsData_t   bms;            /* HV BMS (Prohelion D1000 Gen1, 2026-09) */
    MpptData_t  mppt[MPPT_UNIT_COUNT];   /* MPPT_UNIT_COUNT=4 (2026-08 3→4 증설) */
    MotorData_t motor;          /* (잠정) virtual — VCU 확정 시 교체 */

    /* WSC 규정 누적 데이터 (RegData_t reg로 묶는 것은 예약 상태 — 현재는 평면 필드) */
    float    distance_traveled;          /* 이동 거리 (m, Motor 0x40E Odometer)        */
    float    solar_accumulated_energy;   /* 태양광 패널 누적 에너지 (Wh)               */
    float    battery_accumulated_energy; /* 배터리 누적 에너지 (Motor 0x40E DC Bus Ah) */
    uint32_t regulation_last_tick;
} VehicleData_t;
```

---

## 8. 무선 전송 용량 요약

> 2026-07-24: BMS(HV) SOC PRIMARY 편입(+2패킷) 반영해 재계산 (UART 115200 8N1 = 11.5 KB/s는 여유 충분하므로 병목은 RFD900x air rate).

| 항목 | 계산 | 수치 |
|---|---|---|
| RFD900x Air rate | — | 64 kbps |
| Duty cycle | — | 50% |
| 가용 패킷 | — | **~170 패킷/s** |
| PRIMARY (**1s/1Hz**) | 11패킷(BMS 3 + MPPT 4 + Motor 4) × 1Hz | 11 패킷/s |
| DETAIL (**5s/0.2Hz**) | 33패킷(BMS 16 + MPPT 12 + Motor 3 + ADC 2) ÷ 5s | 6.6 패킷/s |
| **무선 합계** | 11+6.6+1(신선도 상태표) | **~19 패킷/s** |
| **점유율** | 19/170 | **~10%** (2026-09 BMS 교체 전 16%, 2026-08 감속 개정 전 78%) — GPS 2패킷/s 별도 |

**PRIMARY 11패킷**: BMS 3(PACK_VI/SOC_TEMP/STATUS) + MPPT IV ×4 + Motor(BUS_VEL/TEMP/BEMF/ODO) 4.
**DETAIL 33패킷**: BMS 16(팩 CELL_LOC/SOC_DETAIL/CHARGER/FAN_12V 4 + CMU당 셀A/셀B/온도 3 ×4) + MPPT 12(TEMP_FLAGS/MAXLIM/AUX_2 ×4) + Motor 3(DSP_TEMP/STATUS/PHASE_I) + ADC 2(포텐셔미터 ×2, 2026-09-17).

> **2026-08 감속 개정 (200ms/1s → 1s/5s)**: 신호의 실제 변화 시간상수 대비 기존 주기가 과했다.
> - 모터 RPM·차속은 차체 관성(200~300kg, 순항 1~2kW) 때문에 **초 단위**로만 변하고, 버스 전류·셀전압 강하(V=I×R)는 스로틀/램프 속도인 **100ms~1s**, 가장 빠른 축인 MPPT 입력(구름 그림자가 어레이 통과)도 **약 170ms**다. 온도류는 수십 초~분. **5Hz가 필요한 신호는 없었다.**
> - **누적 에너지(Odometer, DC_Bus_AmpHours, 쿨롱카운터, 규정 누적값)는 기기가 자체 적분해 보내므로 샘플링 주기와 무관하게 정확하다** — 순간값을 자주 뜰 이유가 없는 결정적 근거.
> - Fault/Alarm은 BMS에서 래치되어 언제 폴링해도 유실되지 않는다.
> - 실익은 대역폭 절감보다 **링크 마진**이다. 170 pkt/s는 64kbps·듀티 50% **이론값**이고 아웃백 장거리·이동 중에는 재전송·페이딩으로 실효 처리량이 크게 떨어진다. 78% → 16%로 낮춰 통신 거리와 유실 내성을 확보했다.
> - 잃는 것: 200ms~1s 대역의 파형 "모양"(급가속·회생 전류 엔벨로프). 단 피크값은 BMS MinCell/MaxCell·모터 Limit/Error 플래그로 기기가 자체 보고하고, 진짜 모터 튜닝은 kHz급 로깅이 필요해 200ms로도 어차피 불가능했다. CUV/COV 스냅샷·Safety/PF·CB_Status는 **BMS가 이벤트 시점에 캡처해둔 값**이라 5초에 회수해도 내용이 동일하다.
>
> **SD_ONLY(무선 제외, SD_Queue 전용)**: BMS 5(BAL_PRECHG 1 + CMU_SERIAL 4) + MPPT AUX_1 4 + Motor(V_VECTOR/I_VECTOR/RAILS/SLIP) 4 = 13패킷/5s = 2.6 패킷/s. 무선 대역폭 계산에서 제외. (2026-09 BMS 교체 전 48패킷)
> **SD 기록량**: 11 + 6.6 + 2.6 = 20.2 패킷/s × 16B ≈ **0.32 KB/s** (8시간 주행 ≈ 9MB, GPS 제외. 2026-09 이전 0.6KB/s·17MB, 감속 개정 전 2.9KB/s·84MB).

### 8.1 순간 버스트 vs 큐 깊이 (2026-07 신설)

위 "패킷/s"는 **평균**이고, 실제 유실을 좌우하는 건 **한 TIM5 ISR이 큐에 한 번에 밀어넣는 양**이다.
소비 스레드(UART pri 5 / SD pri 10)는 ISR 우선순위 1보다 낮아 **ISR이 리턴하기 전까지 큐를 전혀
비우지 못하므로**, 한 틱의 발행량이 큐 깊이를 넘으면 초과분은 그대로 드롭된다.

| | 개정 전 (~2026-07) | 틱분산 후 (MPPT 3대) | MPPT 4대 (200ms/1s) | 1s/5s, 위상분리 (2026-08) | **현재 (2026-09 BMS 교체)** |
|---|---|---|---|---|---|
| 최악 틱의 UART 유입 | 57 (PRIMARY 17 + DETAIL 40) | 26 | 30 | 18 (PRIMARY 단독) | **12** (DETAIL step 6 MPPT) |
| 최악 틱의 SD 유입 | 106 (PRIMARY 17 + DETAIL 89) | 26 | 30 | 18 (동일) | **12** (동일, PRIMARY는 11) |
| UART_Queue 깊이 | 20 | **40** | 40 | 40 | 40 |
| SD_Queue 깊이 | 50 | **100** | 100 | 100 | 100 |
| 결과 | UART 65% / SD 53% **매초 드롭** | 여유 1.5×~3.8× | 여유 1.33×~3.3× | 여유 2.2× ~ 5.6× | 여유 **3.3× ~ 8.3×** |

> **2026-08 감속 개정으로 피크가 30 → 18로 떨어졌다.** 주기를 늦추면서 **PRIMARY와 DETAIL의 위상을
> 어긋내 배치**했기 때문이다 — PRIMARY는 `tick%10==0`, DETAIL 그룹은 `tick%5==2`(=틱 2,7 mod 10),
> 타임아웃은 `tick%10==5`, 규정 패킷은 `tick%100==4`라서 **어떤 틱도 두 작업을 동시에 수행하지 않는다.**
> 따라서 피크는 "가장 큰 단일 작업" = PRIMARY 18패킷이고, DETAIL 최대 그룹(step 6 MPPT 12)은 그보다 작다.
>
> 이제 MPPT를 더 늘려도 DETAIL(step 6)이 아니라 **PRIMARY(유닛당 +1)** 가 먼저 자란다 — 큐 깊이 40까지
> 여유 22패킷이므로 당장은 한계가 멀다. 신호 추가 시 `timer_scheduler.c`의 주기 상수 주석에 있는
> 위상 배치표에서 **새 작업이 기존 잔여류(mod 10)와 겹치지 않는지** 확인할 것.

개정 내용:
1. **DETAIL 10 그룹 분산** (2026-07) — `Sched_EmitDetailStep()`이 DETAIL 전량을 10개 그룹으로
   나눠 한 틱에 하나씩만 발행한다. 그룹별 갱신 주기는 유지되므로 **평균 수치는 불변**이다.
2. **큐 메시지 32B → 16B** (2026-07) — `LOG`가 16B인데 메시지가 32B라 절반이 패딩이었다. RAM 총량을
   그대로 두고 깊이만 2배(20/50 → 40/100)로 늘렸다.
3. **작업별 위상 분리** (2026-08 감속 개정) — 이전에는 PRIMARY가 짝수 틱마다 나가서 DETAIL 그룹과
   반드시 겹쳤고, 10초 패킷까지 phase 0에 얹혀 피크가 튀었다. 지금은 각 작업이 **서로 다른 mod 10
   잔여류**를 점유해 어떤 틱도 두 작업을 수행하지 않는다(0=PRIMARY, 2·7=DETAIL, 5=타임아웃,
   4=규정, 3=IWDG). 그 결과 피크가 30 → 18로 떨어졌다.
4. **타임아웃 검사를 DETAIL에서 분리** (2026-08) — DETAIL 스텝 5에 얹혀 있었는데 DETAIL이 5초
   주기가 되면서 CAN 두절 검출 지연이 최대 6초(임계값 1s + DETAIL 1주기 5s)로 늘어날 뻔했다.
   자체 1초 분주(`tick%10==5`)로 분리해 검출 지연을 종전(최대 ~2초)대로 유지한다.

> **부작용**: DETAIL 패킷이 한 주기(5초) 안에서 최대 4.5초 스태거된다. `LOG`마다 자체 `timestamp`가
> 있어 지상국 시계열 재구성에는 영향이 없다. 다만 "한 주기치 DETAIL = 동일 시점 스냅샷"을 전제하는
> 디코더를 만들면 안 된다.
>
> **신호 추가 시 확인할 것**: 평균 점유율(16%)뿐 아니라 **새 작업이 기존 위상(mod 10 잔여류)과
> 겹치지 않는지**도 같이 봐야 한다. 위상 배치표와 스텝별 분배는 `timer_scheduler.c`의 주기 상수 주석
> 및 `Sched_EmitDetailStep()` 주석 표 참조.

---

## 9. 잔여 확인 사항

| 항목 | 상태 |
|---|---|
| BMS 보드 FDCAN 비트타이밍 확인 (1.667Mbit/s 산출 경고) | 해결 — 타 팀과 500kbps 합의 완료(2026-07), 실측 권장 |
| Motor: VCU CAN 출력 스펙 확정 | 잠정 (WaveSculptor22 virtual 사용 중) |
| Motor: VCU 재전송 ID·엔디안 확인 (DBC) | 대기 |
| Motor: 실제 VCU base 주소 (가정 0x400) | 대기 |
| BMS 500kbps 보드 실측 | 대기 (합의는 완료, 실측만 남음) |
| MPPT 로터리 1/2/3/**4** 현장 확인 (base 0x610→0x310 이전 + 2026-08 #4 증설 후 재확인 필요) | 대기 |
| MCP2518FD 센서 RX 범위 (LCD TX 확정, 센서 RX 미확정) | 대기 |
| SD 파일 포맷 결정 | 미결 |
| KEY enum 슬롯 순서 문서화 | 2026-07-13 티어 재배치로 키 대량 변경 — 지상국 디코더 **재동기 필수** (§5 표 + 각 티어 표 참조) |
| BMS 쿨롱카운터 물리 단위 환산 (현재 raw) | 대기 (BQ76972 데이터시트 스케일 확인) |
| BMS 셀 전압(0x048~04B BOT/0x051~054 TOP 등)·CUV/COV 스냅샷·CB 밸런싱 비트맵·Safety/PF 상세 | 파싱+송출 구현 완료 (2026-07, xlsx 정밀 오프셋 확인, 2026-07-24 PR#31로 ID 이동) |
| Safety/PF 상태 바이트 비트 정의 해석(대시보드/지상국 표시용) | 대기 (xlsx 비트맵 시트는 확보, 코드 반영은 별도) |
| BMS 상세온도·쿨롱카운터 (xlsx 기준) | 파싱+송출 구현 완료 (2026-07) |
| MPPT aux(+3/+4/+6) | 파싱+송출 구현 완료 (2026-07) |
| Motor DSP온도(DETAIL)·SD_ONLY 진단 | 파싱+송출 구현 완료 (2026-07, 잠정 스케일) |
| 2026-07-13 티어 재배치(CAN_수신_데이터_정리본.xlsx 기준) | MPPT/Motor PRIMARY/DETAIL/SD_ONLY 재분류 구현 완료 (§5·§3·§4) |
| **BMS(HV) CAN ID 전면 재배치 (0x500대/0x1xx/0x2xx → 0x040~0x07F, PR#31)** | 코드 반영 완료, **실물 HV BMS 보드가 새 펌웨어(PR#31)로 재플래시됐는지 배포 전 확인 필요** (미완 시 옛 ID로 송신 중이라 프레임이 전혀 안 들어올 수 있음) |
