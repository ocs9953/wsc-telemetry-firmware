# WSC CAN 더미 노드 3종 (STM32F446 개발보드)

송신 보드(`WSC_Transmitter_Code_V1`) CAN 수신 경로를 실차 없이 검증하기 위한
**BMS / MPPT / Motor(VCU) 역할 대역 노드**다. 각 폴더가 독립된 CubeMX 프로젝트다.

각 더미의 빌드는 이 폴더 안에서 돌린다(`cd tests/dummy_nodes && ./build_dummy.sh BMS`).
스크립트의 경로는 전부 이 폴더 기준이라 위치를 옮기면 깨진다.

```
tests/dummy_nodes/
├── common/            3개 프로젝트가 공유하는 소스 (각 프로젝트에 링크/복사)
│   ├── wsc_dummy_common.h    CAN ID 맵 · 바이트오더 헬퍼 · 파형 생성기
│   └── wsc_dummy_common.c    CAN 송신 래퍼
├── BMS_Dummy/         BMS(HV) 2팩 — BOT/TOP
├── MPPT_Dummy/        Elmar Best MPPT 4대
└── Motor_Dummy/       Tritium WaveSculptor22 (VCU 경유)
```

---

## 1. 왜 3보드로 나눴나

한 보드에서 전부 보내면 **버스 부하와 프레임 간 시간 간격이 실차와 달라진다.**
송신 보드에서 확인하려는 결함(FDCAN RX FIFO0 3칸 오버플로우, 필터 통과 범위,
신선도 타임아웃)은 전부 "여러 노드가 서로 다른 타이밍에 동시에 쏠 때" 나타나므로,
역할별로 물리 노드를 분리했다.

세 보드를 다 켤 필요는 없다. 검증 대상에 맞춰 필요한 것만 켜면 된다.

---

## 2. 배선

세 보드 모두 동일하다.

| F446 핀 | 기능 | 연결 |
|---|---|---|
| **PB8** | CAN1_RX | 트랜시버 RXD |
| **PB9** | CAN1_TX | 트랜시버 TXD |
| GND | — | 트랜시버 GND + **버스 GND 공통** |
| 3V3 / 5V | — | 트랜시버 전원 (소자 사양에 맞춰) |

Nucleo-F446RE 기준 PB8/PB9는 아두이노 헤더 **D15/D14** 다.

> **[주의] F446 에는 CAN 트랜시버가 내장돼 있지 않다.**
> PB8/PB9는 로직 레벨 TX/RX 일 뿐이라 여기에 CAN_H/CAN_L 을 직접 물리면
> 아무 통신도 되지 않는다. SN65HVD230 / TJA1050 같은 트랜시버 모듈이 보드마다
> 1개씩 필요하다.

### 종단 저항

CAN 버스는 **양 끝 2군데만** 120 Ω 이어야 한다. 더미 3보드 + 송신보드 =
4노드를 한 버스에 물리므로, 트랜시버 모듈에 달린 120 Ω 점퍼는
**양 끝 노드 2개만 남기고 나머지는 반드시 제거**한다. 4개 다 켜면 30 Ω 이 되어
드라이버가 우성(dominant) 레벨을 못 만들고 통신이 전부 실패한다.

송신 보드 쪽 종단은 SW2/SW3 스위치로 제어한다.

---

## 3. 클럭과 비트타이밍

세 프로젝트 모두 동일하며, `.ioc` 에 이미 반영돼 있다.

| 항목 | 값 |
|---|---|
| MCU | STM32F446RETx |
| 클럭 소스 | **HSE 8 MHz 직결** (PLL 미사용) |
| SYSCLK / APB1 | 8 MHz |
| CAN1 Prescaler | 1 |
| BS1 / BS2 | 13 TQ / 2 TQ |
| **비트레이트** | **500 kbps** |
| 샘플 포인트 | 87.5 % (CiA 권장) |

검산: `8 MHz / (1 x (1 + 13 + 2)) = 500,000 bps`

### [중요] HSI 를 쓰면 안 된다

F446 의 내장 RC(HSI)는 **±1 %** 다. CAN 이 요구하는 발진자 허용오차는

```
df <= SJW / (2 x 10 x NBT) = 1 / (2 x 10 x 16) = 0.3125 %
```

이므로 HSI는 규격을 3배 초과한다. 짧은 테스트에서는 우연히 통할 수 있으나
온도가 변하면 간헐적 폼 에러/스터프 에러로 나타나고, 원인 추적이 매우 오래 걸린다.

이건 가정이 아니라 **이번 프로젝트 송신 보드에서 실제로 겪은 결함**이다.
FDCAN 이 MSI RC(+1.46 %)로 구동되고 있었고, 2026-08 에 HSE 크리스털로 교체했다.
같은 실수를 더미에서 반복하지 않도록 처음부터 HSE 로 잡았다.

Nucleo-F446RE 는 ST-Link 의 MCO 에서 8 MHz 가 공급되므로 별도 크리스털 없이도
크리스털급 정확도가 나온다. **단, 보드에 따라 SB 점퍼 설정이 필요하다** —
동작하지 않으면 UM1724 의 "MCO from ST-Link" 절을 확인할 것.

---

## 4. 프로젝트에 코드 넣기

`.ioc` 를 CubeMX 로 열어 **Generate Code** 하면 HAL 골격이 나온다.
그 다음 아래 3곳만 손대면 된다.

### (1) 소스 추가

`common/` 의 2개 파일과 해당 역할의 `App/` 2개 파일을 프로젝트에 추가하고,
인클루드 경로에 `common/` 과 `App/` 을 넣는다.

### (2) `Core/Src/main.c` — 초기화

```c
/* USER CODE BEGIN Includes */
#include "bms_dummy.h"      /* 또는 mppt_dummy.h / motor_dummy.h */
/* USER CODE END Includes */
```

```c
/* USER CODE BEGIN 2 */
if (HAL_CAN_Start(&hcan1) != HAL_OK) { Error_Handler(); }
BMS_Dummy_Init(&hcan1);     /* 또는 MPPT_ / Motor_ */
/* USER CODE END 2 */
```

### (3) `Core/Src/main.c` — 메인 루프

```c
while (1)
{
  /* USER CODE BEGIN 3 */
  BMS_Dummy_Task();         /* 또는 MPPT_ / Motor_ */
  /* USER CODE END 3 */
}
```

`*_Task()` 는 전부 논블로킹이라 루프에서 그냥 계속 부르면 된다.
`HAL_GetTick()` 기반으로 자기 주기를 스스로 관리한다.

---

## 5. 송신 프레임 요약

| 더미 | ID 범위 | 프레임 수 | 주기 |
|---|---|---|---|
| BMS | 0x040–0x047, 0x048+0..8, 0x051+0..8, 0x05A+0..18, 0x06D+0..18, 0x123, 0x223 | 최대 66 | PRIMARY 100 ms / DETAIL 500 ms / SD_ONLY 1 s |
| MPPT | 0x310/0x320/0x330/0x340 + 0..6 | 28 | 전압·전류 200 ms / 나머지 1 s |
| Motor | 0x401–0x409, 0x40B, 0x40C, 0x40E, 0x417 | 13 | 0x402·0x403 100 ms / 6종 200 ms / 5종 1 s |

송신 보드의 FDCAN 하드웨어 필터는 **0x040–0x34F** 와 **0x400–0x4FF** 두 구간만
통과시킨다. 위 ID는 전부 이 범위 안에 있다.

### 바이트오더 — 계열마다 다르다

| 더미 | 형식 |
|---|---|
| BMS | **빅엔디안 정수** (수신측이 `__REV16`/`__REV` 로 뒤집음) |
| MPPT | **리틀엔디안 float** — 단, `base+5`(Status)만 u8 비트필드 |
| Motor | **리틀엔디안 float** — 단, `0x401`만 리틀엔디안 u16/u8 |

한쪽만 틀려도 오류 없이 조용히 엉뚱한 값으로 해석되므로,
`wsc_dummy_common.h` 의 `be16()/be32()` 와 `lef()/le16()` 을 반드시 구분해서 쓴다.

---

## 6. 단독 구동 시 나타나는 정상 증상

**더미 한 대만 켜고 버스에 다른 노드가 없으면 `wsc_can_send()` 가 0 을 반환한다.**

CAN 은 수신 노드의 ACK 비트가 없으면 송신이 완료되지 않아 TX 메일박스가 비지
않는다. 고장이 아니라 **버스에 상대가 없다는 뜻**이다. 최소 2노드를 켜거나
CAN 분석기를 붙이면 정상 동작한다.

이때 프레임마다 10 ms 타임아웃이 걸리므로, BMS 더미처럼 한 틱에 최대 66 프레임을
보내는 경우 `Task()` 한 번이 최대 660 ms 를 잡아먹고 주기가 크게 밀린다.
**리스너가 없을 때만 나타나는 현상**이며(정상 버스에서는 66 × 260 µs ≈ 17 ms),
단독 구동 자체가 의미 없는 상태이므로 그대로 두었다.

TEC 가 올라가 결국 bus-off 로 가지만 `.ioc` 에서 **AutoBusOff(ABOM) = ENABLE**
로 두었으므로 하드웨어가 스스로 복구한다. 그래서 더미를 송신 보드보다 먼저
켜 두어도, 나중에 버스가 살아나면 자동으로 정상 송신으로 돌아온다.
CubeMX 기본값인 `DISABLE` 로 두면 여기서 **영구 정지**하므로 되돌리지 말 것.

### 링커 경고 `_close / _lseek / _read / _write is not implemented`

newlib 의 표준 syscall 스텁 경고다. 더미는 printf·파일 I/O 를 쓰지 않으므로
무해하다. STM32CubeIDE 로 열어서 다시 Generate 하면 IDE 가 `syscalls.c` 를
추가하면서 사라진다.

---

## 7. 대조 기준 (SSOT)

값의 ID·바이트오프셋·스케일·엔디안의 **유일한 정답은 송신 보드의 파서**다.

```
Core/Src/can_receiver.c
docs/can-signal-summary.md  §7
```

원본 규격 문서:

- `CAN DATA/BMS_CAN_ID_명세_최신_PR31.xlsx`
- `CAN DATA/MPPT_Elmar Best datasheet.pdf` §8.5
- `CAN DATA/TritiumWaveSculptor22_Manual.pdf` Appendix C §18.4

수신측 파서가 바뀌면 더미도 같이 고쳐야 한다. 한쪽만 고치면 테스트가
"통과"하면서 실차에서 틀리는 최악의 상태가 된다.
