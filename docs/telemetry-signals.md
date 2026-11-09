# 텔레메트리 신호 사전 (Signal Dictionary)

> CAN 신호 → VehicleData → LOG 패킷 매핑과 전송 스케줄의 **설계 원칙** 문서.
> **신호별 CAN ID·오프셋·티어·KEY의 정본(SSOT)은 [`can-signal-summary.md`](can-signal-summary.md)** —
> 지상국 디코더·코드 대조는 반드시 그 문서 최신본 기준으로 한다. 이 문서는 "왜 이렇게 설계했는가"만 다룬다.
>
> **상태: 🟢 BMS(HV)·MPPT 확정 / 🟠 Motor 잠정(virtual) — VCU 미확정. (2026-08 LV BMS 제거)**
> 설계 원칙: **확장에 견디도록 각 차원에 버퍼(빈 자리)를 미리 확보한다.**

---

## 0. 핵심 설계 원칙 (확장 대응)

1. **SD = 전체 기록, 무선 = 선별 구독.** 새 데이터 소스는 기본 "SD 전용"으로 들어오고,
   무선 송신 여부는 별도의 의식적 결정이다. → 소스가 늘어도 무선이 터지지 않는다.
2. **무선은 고정 예산.** 가용 ~2,400 B/s. 설계 캡 **1,800 B/s** (600 B/s 물리 마진).
   각 신호 비용 = `패킷크기(B) × 빈도(Hz)`. 합이 캡을 넘으면 우선순위 낮은 쪽을 강등.
   **소스 우선순위: 1=BMS(HV) > 2=MPPT > 3=Motor** (2025-06 확정).
   이 순위는 세 곳에 쓰인다 — (1) 대역 초과 시 강등 순서, (2) UART 큐 드롭 순서,
   (3) rate 차등(어떤 신호를 PRIMARY에 둘지) 판단 기준.
   현재 실사용량·여유는 `can-signal-summary.md` §8 참조.
3. **테이블 기반 스케줄러 (지향점).** 전송 대상을 `telemetry_map[]` 테이블로 선언. 소스 추가 = 행 추가.
   → 현재는 `timer_scheduler.c`가 그룹별 함수를 손으로 나열하는 방식이며, 이 테이블화는 §6·
   작업 목록의 "패킷 디스크립터 테이블 리팩터" 항목으로 유보되어 있다.
4. **소스별 네임스페이스.** `LOG_SOURCE`를 서브시스템 단위로 확장 → key 충돌 원천 차단.
   실제 enum은 `Core/Inc/types.h`의 `LOG_SOURCE` 참조.

---

## 0.5 엔드투엔드 데이터 파이프라인

```
[CAN Bus 500kbps] ── BMS·MPPT·Motor
      │  FDCAN1 RX (범위 필터 2개, 상세: can-signal-summary.md §1)
      ▼
[CAN RX ISR / 콜백]  ── can_receiver.c
      │  1) CAN ID → switch-case/범위 분기
      │  2) BMS 빅엔디안 → __REV / __REV16
      │  3) MPPT/Motor float → scaled int 변환 (×100 등)
      │  4) 전압 mV → cV (÷10) 재스케일
      ▼
[VehicleData_t]  ── 전역, 전부 정수. FDCAN·TIM5 ISR이 서로 선점 못 해 별도 락 불필요(design.md 참조)
      │
      ├───────────────┬───────────────────────────┐
      ▼               ▼                            │
[TIM5 스케줄러]   [SD 큐]  ◀── 전체 신호(SD_ONLY 포함)
 100ms 베이스 틱       │
 - PRIMARY 1s       ▼
 - DETAIL  5s(10그룹 분산)  [SD_Logger_Thread] → FileX → .log (raw 16B, COBS 없음)
      │
      ▼
[LOG 패킷 16B 생성]
      │  SD_ONLY 신호는 여기서 무선 큐 제외
      ▼
[UART 큐]
      │
      ▼
[UART_Thread]  ── COBS 인코딩 (~17B)
      ▼
[RFD900x]  ── 64kbps / 듀티 50% ──▶ 체이스카 (지상국 디코더)
```

**프로세스 정합성 점검**

| 단계 | 점검 항목 | 상태 |
|---|---|---|
| CAN RX | 범위 필터 2개로 전 소스 커버 | ✅ (can-signal-summary.md §1) |
| 변환 | float→int·엔디안·전압 재스케일을 RX 시점 1회 처리 | ✅ |
| VehicleData | 정수 단일 표현 → 하위 경로 분기 없음 | ✅ |
| 분기 | SD=전체 / 무선=선별, 티어(PRIMARY/DETAIL/SD_ONLY)로 제어 | ✅ |
| 스케줄 | 단일 베이스 틱(100ms)에서 PRIMARY/DETAIL 파생 + DETAIL 10서브틱 분산 | ✅ (2026-07, 큐 버스트 해소) |
| 무선 용량 | 현재 사용량·여유는 can-signal-summary.md §8 참조 | ✅ |
| 프레이밍 | 큐=raw 16B, 송신 직전 COBS → SD/무선 디코더 공유 | ✅ |

---

## 1. CAN 버스 토폴로지

| 버스 | 컨트롤러 | 역할 / 연결 장치 | 속도 | RX | TX |
|---|---|---|---|---|---|
| **Bus A** | FDCAN1 (내부) | **BMS(BOT+TOP) · MPPT ×4 · Motor** | 500 kbps | ✅ | ✖ |
| **Bus B** | MCP2518FD (SPI1) | LCD 보드 (지상국 메시지 TX) | 500 kbps | _[미정]_ | ✅ |

> **CAN 버스 부하** (500 kbps): BMS 70여 ID + MPPT 21 ID + Motor ~15 ID(🟠 잠정), 100ms 주기 가정
> ≈ 110~125 kbps = **~25% 점유**. 여유 충분.
> ✅ **해결됨 (2026-07)**: BMS 측 비트타이밍 오계산 경고는 타 팀과 500kbps 합의로 해소. 실측 권장.

> CAN ID 맵·FDCAN1 필터 상세는 **can-signal-summary.md §1** 참조 (여기서는 원칙만 유지 — ID가
> 개정 때마다 이 문서까지 갱신해야 하는 이중 관리를 피하기 위함, 2026-07-31 문서 정리).

---

## 2. 전송 주기 그룹 (원칙)

> TIM5 베이스 틱 = **100ms**. 정확한 신호별 티어 배정은 `can-signal-summary.md` §2~§5 기준.

| 그룹 | 주기 | TIM5 분주 | 선정 기준 |
|---|---|---|---|
| **PRIMARY** | **1s (1Hz)** | `tick%10==0` | 실시간 감시가 필요한 핵심 값(전압/전류/Fault/속도 등) |
| **DETAIL** | **5s (0.2Hz)**, 10개 그룹에 분산 발행 | `tick%5==2`, 500ms마다 1그룹 | 진단·추세 파악용(셀전압, 온도, 쿨롱카운터 등) — 무선 전송, ISR 버스트 방지를 위해 그룹 단위로 분산(2026-07) |
| **SD_ONLY** | 무선 없음, SD만 | 상동 | 사후 분석 전용(보호 이벤트 스냅샷, 상세 비트필드 등) — 평상시엔 안 바뀌는 값 |

> **확정 이력**: 무선 주 신호는 FAST/MED 구분 없이 균일 주기로 통일. DETAIL은 애초 10s 설계였으나
> 실시간성 확보를 위해 1s로 상향했다가, **2026-08 감속 개정으로 PRIMARY 200ms→1s / DETAIL 1s→5s**로
> 낮췄다(물리량 시간상수 대비 과했고 RFD900x 대역 여유 확보). 상세: can-signal-summary.md §8.

---

## 3. 패킷 묶음 설계 원칙 (int 통일 + 4×u16)

- **int 통일**: 모든 신호를 정수로 저장. float 원본(MPPT/Motor)은 **CAN RX 수신 시점**에 scaled int로
  변환하여 VehicleData에 저장. 이후 무선·SD 경로는 전부 정수만 처리.
- **4×u16 묶기**: `value[8B]`에 의미적으로 유사한 신호 4개를 묶어 패킷 효율 2배.
- **같은 source 안에서만 묶기**: LOG 패킷의 `source` 필드는 1개 → BMS_BOT와 BMS_TOP는 별도 패킷.

### 인코딩 방식

| 묶음 방식 | 한 패킷 수용 | 사용 상황 |
|---|---|---|
| `4 × u16` | 4개 신호 | 전압·전류·온도 (재스케일 후 u16 범위 내) |
| `2 × u32` | 2개 신호 | 비트필드 fault flags (분할 불가) |
| `4 × i16` | 4개 신호 | 부호 있는 온도·전류 혼합 |

### 스케일 규칙 (VehicleData 저장 시 변환, 예시)

| 원본 신호 | 원본 타입 | 변환 규칙 | 저장 타입 | 정밀도 |
|---|---|---|---|---|
| Stack/Pack Voltage | u32 / mV | ÷10 → cV | u16 | 0.01V |
| MaxCell/MinCell Voltage | u16 / mV | 그대로 | u16 | 1mV |
| Global Fault Flags | u32 / 비트 | 그대로 | u32 | 비트 그대로 |
| MPPT/Motor 전압 (V) | float | ×100 | u16 | 0.01V |
| MPPT/Motor 전류 (A) | float | ×100 또는 ×10 | u16/i16 | 신호별 상이 |
| MPPT/BMS/Motor 온도 (℃) | float or i16·0.1℃ | ×10 → i16 | i16 | 0.1℃ |

> 전체 신호의 정확한 스케일 표는 **can-signal-summary.md §6** 참조.

### COBS 인코딩 위치

큐에는 **raw 16B LOG 패킷**만 적재한다. UART_Thread가 송신 직전에만 COBS 인코딩(~17B)한다.
SD 기록은 COBS 없이 raw 16B 그대로 저장 → SD 파일도 무선과 동일한 디코더로 분석 가능.

---

## 4. LOG_SOURCE 네임스페이스 (원칙)

각 소스가 독립된 128-key 공간을 가진다 → 소스 간 key 충돌 불가, 무한 증설 가능.
실제 enum(`SRC_SYS`, `SRC_BMS_BOT/TOP`, `SRC_MPPT1~4`, `SRC_MOTOR`, `SRC_*_ERR` 등)은
`Core/Inc/types.h`의 `LOG_SOURCE`가 정본이다 — 여기서는 스냅샷을 유지하지 않는다(2026-07-31,
과거 이 문서의 enum 스냅샷이 실제 코드와 어긋나 있었던 것을 계기로 제거).

---

## 5. 누적값 (PRIMARY, 1s)

| 신호 | VehicleData 필드 | LOG key | 주기 |
|---|---|---|---|
| 이동 거리 | `distance_traveled` | `KEY_MOTOR_ODO` (SRC_MOTOR) | 1s (PRIMARY) |
| 배터리 누적에너지 | `battery_accumulated_energy` | `KEY_MOTOR_ODO` (SRC_MOTOR) | 1s (PRIMARY) |

> **2026-08 변경**: WSC 규정에서 10초 주기 기록 조항이 없어져 규정 전용 패킷(`KEY_REG_DATA` ×2, 10s)과 `*_reg.log` 파일을 제거했다. 위 두 값은 원래부터 CAN 0x40E(Motor Odometer)에서 와서 PRIMARY의 `KEY_MOTOR_ODO`로 **1초마다** 나가고 있었으므로 전송 경로가 사라진 것이 아니라 중복이 사라진 것이다.
>
> 함께 삭제된 `solar_accumulated_energy`는 **대입하는 코드가 없어 항상 0.0f**였다(태양광 적산 미구현). 실제로 필요해지면 MPPT 출력 적산을 새로 구현하고 새 키(SRC_SYS 16 이상)를 할당할 것 — `KEY_SYS_RESERVED15`(구 `KEY_REG_DATA`)는 과거 로그 해석 때문에 재사용 금지다.

---

## 6. 패킷 디스크립터 테이블 리팩터 (지향점, 미구현)

원래 설계 의도는 스케줄러가 아래와 같은 선언적 테이블 하나로 구동되는 것이었다. 현재는
`timer_scheduler.c`가 그룹별 함수(`Sched_EmitPrimary`, `Sched_EmitBmsDetailWireless` 등)를
손으로 나열하는 방식으로 구현되어 있다.

```c
typedef struct {
  LOG_SOURCE source;
  uint8_t    key;
  uint16_t   vd_offset;   /* offsetof(VehicleData_t, ...) */
  uint8_t    type;        /* TYPE_F32 / TYPE_U16 / TYPE_I16 */
  uint8_t    rate_group;  /* RATE_PRIMARY / DETAIL / SD_ONLY */
} TelemetryEntry;

/* 소스 추가 = 행 추가. 스케줄러 로직 무수정. */
static const TelemetryEntry telemetry_map[] = { ... };
```

> **2026-07-31**: 이 아이디어는 시스템 리뷰에서 "패킷 디스크립터 테이블 리팩터"로 독립적으로
> 재발견되어 작업 목록의 유보 작업 목록에 구체적 실행 계획(테이블 필드 구성, escape hatch 설계 등)과
> 함께 기록되어 있다. 실행 시 그 항목을 따른다 — 여기 코드 스케치는 최초 설계 의도의 기록으로만 유지.

---

## 7. VCU 확정 시 연쇄 갱신 체크리스트

Motor 데이터는 VCU(모터 컨트롤러와 우리 보드 사이의 중계 장치) 스펙이 아직 미확정이라
WaveSculptor22 네이티브 CAN 스펙을 그대로 가져온 **virtual placeholder**로 동작 중이다.
VCU 출력 표가 확정되면 다음을 동시에 갱신한다:

- [ ] `can-signal-summary.md` §4 (Motor CAN ID 상세) — 실제 VCU ID·오프셋·엔디안으로 전면 교체
- [ ] `Core/Inc/types.h` — `MotorData_t` 필드/스케일
- [ ] `Core/Src/can_receiver.c` — `CAN_StoreMotor()` 파싱 로직
- [ ] `Core/Src/timer_scheduler.c` — Motor 패킷 발행 로직
- [ ] 지상국 디코더 — source/key→신호 매핑 동기화 (저장소 밖)

> 이미 완료된 항목(`VehicleData_t` 중첩 구조, `LOG_SOURCE` 서브시스템 확장 등)은 코드에 반영되어
> 있으므로 이 목록에서 제외했다 — 실제 구조는 `Core/Inc/types.h` 참조.

---

## 8. 잔여 결정/대기 항목

> 외부 스펙 확인이 필요한 블로커(VCU 미확정, MPPT 로터리 실측, BMS 비트타이밍 실측
> 확정 등)는 **can-signal-summary.md §9 "잔여 확인 사항"**에서 관리한다. 여기는 그 문서에 속하지
> 않는, 순수 아키텍처 결정 사항만 남긴다.

1. **MCP2518FD 센서 RX 범위** — LCD 메시지 TX는 확정. LCD 외 센서 RX 대상·ID는 미확정 →
   확정 시 `ext_can_controller.c`의 `ExtCAN_ProcessMessage()`에 파싱 추가.
   (관련: 현재 RX 필터가 `AcceptanceMask=0x000`로 전 ID를 통과시키고 있음 — 작업 목록의 알려진 이슈 참조)
2. **LCD 보드 CAN TX ID** — 미정, 계기판 팀과 합의 필요.
3. **SD 파일 포맷** — 결정됨: 부팅 시각 기반 단일 파일(`YYMMDD_HHMMSS.log`) + 규정 전용 파일
   (`YYMMDD_HHMMSS_reg.log`) 2종 (`sd_logger.c` 구현 완료). 세션/소스별 추가 분할은 현재 계획 없음.
4. **Fault flags 패킷의 pad 슬롯** — PRIMARY 패킷 #3/#6(`KEY_BMS_FAULT`)은 `Global_Fault(u32)` +
   `pad(u32)` 구성이라 4바이트가 비어 있다. 다른 신호로 채울지, 0x00으로 유지할지 미결
   (채우면 지상국 디코더 재동기 필요).
