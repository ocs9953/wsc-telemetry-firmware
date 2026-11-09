# 요구사항 문서

## 소개

본 문서는 WSC(World Solar Challenge) 태양광 자동차 대회를 위한 텔레메트리 보드 펌웨어의 요구사항을 정의한다. STM32U575xx MCU 기반으로 HAL 라이브러리와 ThreadX(Azure RTOS)를 사용하며, 차량 내부 CAN 데이터를 수집하여 패키징한 후 RFD900x(UART)로 송신하고 SD 카드에 백업 로깅을 수행하는 완전한 이벤트 드리븐 RTOS 시스템을 구축하는 것이 목적이다.

## 용어 정의

- **Telemetry_Firmware**: STM32U575xx MCU에서 동작하는 텔레메트리 보드 펌웨어 시스템 전체
- **CAN_Receiver**: 내부 FDCAN1 주변장치를 통해 CAN 버스 데이터를 수신하는 모듈 [^2]
- **ExtCAN_Controller**: Emandhal/MCP251XFD 드라이버 라이브러리를 사용하여 MCP2518FD 외부 CAN 컨트롤러를 SPI1으로 제어하는 모듈 [^2]
- **VehicleData**: 최신 배터리, MPPT 등 차량 데이터를 저장하는 전역 구조체 (VehicleData_t) [^1]
- **Packet_Builder**: VehicleData의 최신 값을 16바이트 로그 패킷(LOG 구조체)으로 포장하는 모듈
- **UART_Queue**: ThreadX 메시지 큐(Message Size 4 ULONGs/16B, Depth 40)로, 타이머 인터럽트(Producer)에서 UART_Thread(Consumer)로 LOG 패킷을 전달한다
- **SD_Queue**: ThreadX 메시지 큐(Message Size 4 ULONGs/16B, Depth 100)로, 타이머 인터럽트(Producer)에서 SD_Logger_Thread(Consumer)로 LOG 패킷을 전달한다
- **UART_Thread**: Priority 5, Stack 1024B. UART_Queue에서 tx_queue_receive(2000 tick 타임아웃)로 패킷 대기 후, COBS 인코딩 및 USART2 DMA 송신을 수행하는 ThreadX 스레드
- **SD_Logger_Thread**: Priority 10, Stack 2048B. SD_Queue에서 tx_queue_receive(2000 tick 타임아웃)로 패킷 대기 후, SD_DoubleBuffer에 축적 및 SDMMC1 IDMA 기록을 수행하는 ThreadX 스레드
- **SD_DoubleBuffer**: SD 로깅 전용 더블 버퍼(Buffer A/B) 구조. SD_Logger_Thread에서 패킷을 활성 버퍼에 축적하고, 버퍼가 가득 차면 스왑하여 한 번에 SD 카드에 기록한다
- **COBS_Encoder**: COBS(Consistent Overhead Byte Stuffing) 인코딩을 수행하는 모듈. 16바이트 LOG 패킷을 18바이트 UART 프레임으로 변환하여 수신 측에서 패킷 경계를 식별할 수 있게 한다
- **SD_Logger**: SD_DoubleBuffer에서 가득 찬 버퍼를 받아 FileX 파일 시스템과 SDMMC1 IDMA를 통해 SD 카드에 섹터 단위로 기록하는 모듈
- **Timer_Scheduler**: TIM5 100ms 주기 인터럽트를 통해 패키징 및 타임아웃 검사를 스케줄링하는 Producer 모듈
- **LOG**: 16바이트 패킷 구조체 (timestamp 4B + level 1B + source 1B + key 1B + value 8B + checksum 1B), `__attribute__((packed))` 적용
- **RTC_Manager**: 내부 RTC 주변장치를 통해 실시간 시각(날짜/시간)을 관리하는 모듈. 부팅 시각 기록, 로그 파일명 생성, 주기적 시각 패킷 전송에 사용
- **DATETIME**: RTC에서 읽은 날짜/시간 정보를 저장하는 구조체 (year, month, date, hour, minute, second)
- **Timeout_Error**: 핵심 데이터가 1초 이상 갱신되지 않았을 때 설정되는 오류 플래그
- **Watchdog**: IWDG(Independent Watchdog)를 통한 시스템 행(hang) 방지 메커니즘

## 요구사항

### 요구사항 1: 시스템 초기화 및 RTOS 객체 생성

**사용자 스토리:** 개발자로서, 전원 인가 시 모든 주변장치와 RTOS 리소스가 올바른 순서로 초기화되기를 원한다. 이를 통해 시스템이 안정적으로 동작을 시작할 수 있다.

#### 인수 조건

1. WHEN 전원이 인가되면, THE Telemetry_Firmware SHALL GPIO, GPDMA1, RTC, FDCAN1, ICACHE, USART2, IWDG, SDMMC1, SPI1, TIM5 순서로 HAL 주변장치를 초기화한다. Timebase Source는 ThreadX와의 충돌 방지를 위해 SysTick 대신 TIM6을 사용한다.
2. WHEN ThreadX 커널이 시작되면(TX_TIMER_TICKS_PER_SECOND=1000, TX_APP_MEM_POOL_SIZE=16384), THE Telemetry_Firmware SHALL CubeMX가 생성한 app_threadx.c의 tx_application_define() 내부에서 다음 RTOS 객체를 직접 생성한다:
   - UART_Thread: Priority 5, Stack 1024 Bytes
   - SD_Logger_Thread: Priority 10, Stack 2048 Bytes
   - UART_Queue: Message Size 4 ULONGs(16바이트 = sizeof(LOG)), Queue Depth 40
   - SD_Queue: Message Size 4 ULONGs(16바이트 = sizeof(LOG)), Queue Depth 100
   - (2026-07: 메시지 크기를 8→4 ULONG으로 줄여 RAM 총량 변경 없이 깊이를 2배로 확보. TIM5 ISR의 틱당 최대 발행량 26패킷에 대한 여유 확보가 목적)
   - FileX 미디어 초기화
3. WHEN 초기화가 완료되면, THE Telemetry_Firmware SHALL LED_BOARD_CHECK(PE12)를 요구사항 12.2의 실시간 건강 상태 규칙에 따라 갱신한다. **(2026-08 정정: 상시 점등에서 실시간 반영으로 변경 — 상세는 12.2)**
4. IF 특정 주변장치 초기화가 실패하면, THEN THE Telemetry_Firmware SHALL LED_ERR(PE11)를 점등하고 오류 LOG 패킷(level=ERROR, source=SYS, key=ERR_INIT_FAIL, value=HAL 에러 코드)을 생성하여 UART_Queue와 SD_Queue에 전송한다.
5. WHEN FDCAN1 초기화가 완료되면, THE CAN_Receiver SHALL **범위 필터 2개(StdFiltersNbr=2)**를 설정하여 BMS(HV)+MPPT(0x040~0x34F)와 Motor/VCU(**0x400~0x4FF**, 잠정 / 2026-08 LV BMS 제거로 0x6FF→0x4FF 축소)를 수신하도록 구성한다. (2026-07-24 BMS(HV) CAN ID가 우선순위 대역 0x040~0x07F로 재배치되며 MPPT와 함께 필터0에, Motor는 필터1에 커버. **2026-08 MPPT 4대 증설로 필터0 상한을 0x33F→0x34F로 확장** — MPPT 대역 0x310~0x346) [^1]
6. WHEN SPI1 초기화가 완료되면, THE ExtCAN_Controller SHALL MCP2518FD와 10MHz SPI Mode 0 통신을 설정하고 Software GPIO NSS 제어를 활성화한다.

### 요구사항 2: 전역 데이터 구조체 관리

**사용자 스토리:** 개발자로서, 차량의 최신 센서 데이터를 하나의 전역 구조체에서 관리하고 싶다. 이를 통해 데이터 접근이 일관되고 효율적으로 이루어진다.

#### 인수 조건

1. THE Telemetry_Firmware SHALL VehicleData_t 전역 구조체를 선언하여 배터리(BMS) 데이터, MPPT 데이터 및 기타 차량 센서 데이터를 저장한다. [^1]
2. THE VehicleData SHALL 각 데이터 필드에 대해 마지막 갱신 시각(timestamp)을 함께 저장한다.
3. THE VehicleData SHALL **정수 전용 저장 방식(all-int 전략)**을 사용한다. MPPT float 값은 CAN RX 시점에 스케일된 정수로 변환한다. value[8B] 필드는 `4×u16`, `4×i16`, `2×u32` 패킹을 지원한다.
4. WHEN CAN 인터럽트에서 데이터가 갱신되면, THE VehicleData SHALL 해당 필드의 마지막 갱신 시각을 HAL_GetTick() 값으로 업데이트한다.

### 요구사항 3: 내부 CAN 데이터 수신 (FDCAN1)

**사용자 스토리:** 개발자로서, CAN 버스에서 배터리(BMS)와 MPPT 데이터를 실시간으로 수신하고 싶다. 이를 통해 차량 상태를 지속적으로 모니터링할 수 있다.

#### 인수 조건

1. WHEN FDCAN1 Rx FIFO에 새 메시지가 도착하면, THE CAN_Receiver SHALL 인터럽트 콜백(HAL_FDCAN_RxFifo0Callback)에서 지연 연산 없이 즉시 CAN ID를 판별한다. [^1] [^2]
2. WHEN 유효한 CAN ID가 수신되면, THE CAN_Receiver SHALL VehicleData의 해당 변수 값만 최신 상태로 덮어쓰기한다. [^1]
3. THE CAN_Receiver SHALL Classic CAN 모드(500kbps, NominalPrescaler=10, TimeSeg1=12, TimeSeg2=3)로 동작한다.
4. WHEN CAN 메시지가 정상 수신되면, THE CAN_Receiver SHALL VehicleData를 갱신하고, 직전에 CAN 문제가 있었다면(can_ok=0) LED_CAN(PB15)을 소등하여 회복을 표시한다. **(2026-08 정정: "수신 표시 LED 미구동"에서 "실시간 회복 반영"으로 변경 — 상세는 12.3)**
5. IF FDCAN1에서 버스 오류가 발생하면, THEN THE CAN_Receiver SHALL 오류 LOG 패킷(level=ERROR, source=CAN, key=ERR_CAN_BUS_OFF, value=HAL FDCAN 에러 코드(4B)+CAN ID(4B))을 생성하여 UART_Queue와 SD_Queue에 전송하고 LED_CAN(PB15)를 점등한다.
6. IF FDCAN1 Rx FIFO 오버플로우가 발생하면, THEN THE CAN_Receiver SHALL 경고 LOG 패킷(level=WARNING, source=CAN, key=ERR_CAN_RX_OVERFLOW)을 생성하여 UART_Queue와 SD_Queue에 전송하고 LED_CAN(PB15)를 점등한다.
7. THE CAN_Receiver SHALL 하드웨어 필터를 사용하여 사전 정의된 CAN ID만 수신하고 나머지 메시지를 거부한다. [^1]

### 요구사항 4: 외부 CAN 컨트롤러 통신 (MCP2518FD)

**사용자 스토리:** 개발자로서, MCP2518FD 외부 CAN 컨트롤러를 통해 추가 CAN 채널의 데이터를 수신하고 싶다. 이를 통해 더 많은 차량 데이터를 수집할 수 있다.

#### 인수 조건

1. THE ExtCAN_Controller SHALL Emandhal/MCP251XFD 드라이버 라이브러리를 사용하여 MCP2518FD를 Classic CAN 500kbps 모드로 초기화하고 제어한다.
2. THE ExtCAN_Controller SHALL SPI1을 통해 MCP2518FD와 10MHz(160MHz/16), Mode 0(CPOL=Low, CPHA=1Edge) 속도로 통신하며, Software GPIO 방식으로 NSS(Chip Select) 핀을 제어한다.
3. THE ExtCAN_Controller SHALL MCP251XFD 드라이버의 SPI 전송 콜백 함수(fnSPI_Transfer)를 HAL_SPI 기반으로 구현하여 드라이버에 등록한다.
4. WHEN MCP2518FD의 인터럽트 핀(CAN_INT: PB0 또는 CAN_RX_INT: PE9)이 활성화되면, THE ExtCAN_Controller SHALL MCP251XFD_ReceiveMessageFromFIFO()를 호출하여 수신 데이터를 읽고 VehicleData를 갱신한다. [^1] [^2]
5. IF MCP2518FD SPI 통신 타임아웃이 발생하면, THEN THE ExtCAN_Controller SHALL 오류 LOG 패킷(level=ERROR, source=SPI, key=ERR_SPI_TIMEOUT, value=HAL SPI 에러 코드(4B)+재시도 횟수(4B))을 생성하여 UART_Queue와 SD_Queue에 전송하고 LED_ERR(PE11)를 점등한다.
6. IF MCP2518FD SPI 데이터 전송이 실패하면, THEN THE ExtCAN_Controller SHALL 오류 LOG 패킷(level=ERROR, source=SPI, key=ERR_SPI_TRANSFER, value=HAL SPI 에러 코드(4B)+재시도 횟수(4B))을 생성하여 UART_Queue와 SD_Queue에 전송하고 LED_ERR(PE11)를 점등한다.

### 요구사항 5: 패킷 구조 및 직렬화

**사용자 스토리:** 개발자로서, 차량 데이터를 표준화된 16바이트 패킷으로 포장하고 싶다. 이를 통해 수신 측에서 데이터를 일관되게 파싱할 수 있다.

#### 인수 조건

1. THE Packet_Builder SHALL LOG 구조체를 `__attribute__((packed))` 속성을 적용하여 timestamp(4바이트) + level(1바이트) + source(1바이트) + key(1바이트) + value(8바이트) + checksum(1바이트) = 총 16바이트로 구성하며, 컴파일러의 패딩 삽입을 방지한다.
2. THE Packet_Builder SHALL timestamp 필드에 HAL_GetTick()으로 얻은 시스템 시간(밀리초)을 기록한다.
3. THE Packet_Builder SHALL checksum 필드에 패킷의 나머지 15바이트에 대한 CRC-8(다항식 0x07, CRC-8/SMBus) 연산 결과를 기록한다.
4. THE Packet_Builder SHALL value 필드(8바이트) 패킹을 지원한다: `PackU16x4`(4×u16), `PackI16x4`(4×i16), `PackU32x2`(2×u32). 기존 `PackFloat2/UnpackFloat2`는 Motor Odometer 등 float 원본 데이터 전용으로만 유지한다.
5. THE Packet_Builder SHALL source 필드를 **단일 통합 LOG_SOURCE enum**(2025-06)으로 정의한다 [^2]:
   - **데이터 소스** (INFO 데이터 패킷에 사용, 소스별 128-key 네임스페이스):
     - SRC_SYS: 시스템 전반 (RTC, 초기화, Watchdog 등)
     - SRC_BMS_BOT / SRC_BMS_TOP: BMS 하부/상부 보드 데이터
     - SRC_MPPT1 / SRC_MPPT2 / SRC_MPPT3 / SRC_MPPT4: MPPT 유닛 데이터 (2026-08 4대 증설, SRC_MPPT4는 SRC_MPPT3 뒤에 삽입되어 이후 소스 값이 +1 시프트됨 — 지상국 디코더 동시 갱신 필요)
     - SRC_MOTOR: 모터/VCU 데이터 (잠정 — WaveSculptor22 기반 virtual, VCU 확정 대기)
     - SRC_RESERVED1 / SRC_RESERVED2: 향후 데이터 소스 (예: ADC 센서)
   - **에러 소스** (WARNING/ERROR/CRITICAL 패킷에 사용):
     - SRC_CAN_ERR: FDCAN1/MCP2518FD CAN 버스 오류
     - SRC_SPI_ERR: MCP2518FD SPI 통신 오류
     - SRC_UART_ERR: USART2/RFD900x 통신 오류
     - SRC_SD_ERR: SD 카드/FileX 오류

   > **약칭 매핑**: 아래 인수 조건에서 `source=CAN` ≡ SRC_CAN_ERR, `source=SPI` ≡ SRC_SPI_ERR, `source=UART` ≡ SRC_UART_ERR, `source=SD` ≡ SRC_SD_ERR, `source=SYS` ≡ SRC_SYS (시스템 오류도 SRC_SYS 사용).
6. THE Packet_Builder SHALL level 필드를 다음 열거형으로 정의한다:
   - INFO(0): 정상 데이터 패킷 (BMS, MPPT, RTC 시각 등)
   - WARNING(1): 경고 (Timeout, 큐 Full 등 복구 가능한 상황)
   - ERROR(2): 오류 (통신 실패, SD 기록 실패 등)
   - CRITICAL(3): 치명적 오류 (전원 강하, 복구 불가능한 오류)
7. THE Packet_Builder SHALL key 필드를 level에 따라 다음과 같이 사용한다:
   - level=INFO (정상 데이터): key에 데이터 식별 인덱스를 저장한다. CAN 데이터의 경우 CAN ID를 매핑한 인덱스 번호(1바이트)를 사용하고, 시스템 데이터의 경우 RTC_TIME 등 사전 정의된 키를 사용한다. [^1]
   - level=WARNING/ERROR/CRITICAL (오류): key에 다음 에러 코드를 저장한다:
     - ERR_CAN_BUS_OFF: FDCAN1 버스 오프/오류 (source=CAN, level=ERROR)
     - ERR_CAN_RX_OVERFLOW: FDCAN1 Rx FIFO 오버플로우 (source=CAN, level=WARNING)
     - ERR_SPI_TIMEOUT: MCP2518FD SPI 통신 타임아웃 (source=SPI, level=ERROR)
     - ERR_SPI_TRANSFER: MCP2518FD SPI 전송 실패 (source=SPI, level=ERROR)
     - ERR_UART_DMA: USART2 DMA 전송 실패 (source=UART, level=ERROR)
     - ERR_UART_QUEUE_FULL: UART_Queue tx_queue_send 실패 (source=UART, level=WARNING)
     - ERR_SD_INIT: SD 카드/FileX 초기화 실패 (source=SD, level=ERROR)
     - ERR_SD_WRITE: SD 카드 기록 실패 (source=SD, level=ERROR)
     - ERR_SD_NOT_DETECTED: SD 카드 미감지 (source=SD, level=ERROR)
     - ERR_SD_QUEUE_FULL: SD_Queue tx_queue_send 실패 (source=SD, level=WARNING)
     - ERR_RTC_READ: RTC 읽기 실패 (source=SYS, level=ERROR)
     - ERR_INIT_FAIL: 주변장치 초기화 실패 (source=SYS, level=ERROR)
     - ERR_TIMEOUT: 핵심 데이터 1초 이상 미갱신 (source=SYS, level=WARNING)
     - ERR_POWER_DOWN: Power_check(PD6) 전원 강하 감지 (source=SYS, level=CRITICAL)
     - ERR_WATCHDOG: IWDG 리셋 직전 상태 (source=SYS, level=CRITICAL)
8. THE Packet_Builder SHALL 오류 패킷의 value 필드(8바이트)를 다음과 같이 구성한다:
   - CAN 오류: HAL FDCAN 에러 코드(4B) + CAN ID(4B)
   - SPI 오류: HAL SPI 에러 코드(4B) + 재시도 횟수(4B)
   - UART 오류: HAL UART 에러 코드(4B) + 미전송 패킷 수(4B)
   - SD 오류: FileX 에러 코드(4B) + 실패 시점 파일 오프셋(4B)
   - 큐 Full: 현재 큐 사용량(4B) + 드롭된 패킷 수(4B)
   - Timeout: 타임아웃 발생 데이터 key(4B) + 마지막 갱신 이후 경과 ms(4B)
   - 전원 강하: 감지 시점 tick(4B) + 미기록 버퍼 잔량(4B)
9. FOR ALL 유효한 LOG 패킷에 대해, 직렬화 후 역직렬화하면 원본과 동일한 LOG 구조체가 복원된다 (라운드트립 속성).

### 요구사항 6: 타이머 기반 패키징 스케줄링 (TIM5) — Producer

**사용자 스토리:** 개발자로서, 100ms 주기로 최신 차량 데이터를 패킷으로 포장하여 각 Consumer 스레드에 전달하고 싶다. 이를 통해 일정한 주기로 텔레메트리 데이터가 처리된다.

#### 인수 조건

1. THE Timer_Scheduler SHALL TIM5를 내부 클럭 160MHz 기준, Prescaler=15999, ARR=999로 설정하여 정확히 100ms(10Hz) 주기로 인터럽트를 발생시킨다.
2. WHEN TIM5 인터럽트(HAL_TIM_PeriodElapsedCallback)가 발생하면, THE Timer_Scheduler SHALL 해당 틱에 발행 예정인 그룹만 VehicleData의 최신 값을 읽어 LOG 패킷(level=INFO)으로 포장한다. **(2026-08 감속 개정)** 발행 주기와 위상은 다음과 같으며, 어떤 틱도 두 그룹을 동시에 발행하지 않는다: [^1]
   - **PRIMARY**: 10틱마다(=**1초**, `tick%10==0`) — 패킷 #1~#18
   - **DETAIL**: 5틱마다 그룹 1개씩(`tick%5==2`), 10개 그룹 → 그룹별 **5초** 주기
   - 타임아웃 검사: 10틱마다(=1초, `tick%10==5`) — 아래 6항
   - RTC 시각 패킷: 100틱마다(=10초, `tick%100==4`)  ※2026-08 규정 패킷 제거
   - (개정 전 200ms/1s는 신호의 실제 변화 시간상수 대비 과했고 RFD900x 이론 대역의 78%를 점유했다. 1s/5s로 낮춰 16%로 확보 — 누적 에너지는 기기가 자체 적분해 보내므로 샘플링 주기와 무관하게 정확하다. 위상 분리로 ISR 피크도 30→18로 감소.)
3. WHEN 패킷 포장이 완료되면, THE Timer_Scheduler SHALL tx_queue_send(..., TX_NO_WAIT)를 사용하여 해당 패킷을 UART_Queue와 SD_Queue에 각각 전송하고 즉시 반환한다.
4. IF UART_Queue tx_queue_send가 실패하면(큐 Full), THEN THE Timer_Scheduler SHALL 경고 LOG 패킷(level=WARNING, source=UART, key=ERR_UART_QUEUE_FULL, value=현재 큐 사용량(4B)+드롭된 패킷 수(4B))을 SD_Queue에만 전송하고 LED_RFD900x(PE15)를 점등한다.
5. IF SD_Queue tx_queue_send가 실패하면(큐 Full), THEN THE Timer_Scheduler SHALL 경고 LOG 패킷(level=WARNING, source=SD, key=ERR_SD_QUEUE_FULL, value=현재 큐 사용량(4B)+드롭된 패킷 수(4B))을 UART_Queue에만 전송하고 LED_SD(PD8)를 점등한다.
6. WHEN TIM5 tick이 1초 주기에 도달하면(10틱마다 1회, `tick%10==5`), THE Timer_Scheduler SHALL 각 핵심 데이터 필드의 마지막 갱신 시각을 검사한다.
   - 임계값이 1000ms이므로 100ms마다 검사할 실익이 없어 1초 주기로 둔다. 대신 검출 지연이 최대 ~2초(임계값의 2배)가 될 수 있음을 감수한 결정이다.
   - **이 검사는 DETAIL 발행 주기와 독립이어야 한다.** 2026-08 감속 개정 전에는 DETAIL 스텝 5에 얹혀 있었는데, DETAIL이 5초 주기가 되면서 그대로 두면 검출 지연이 최대 6초(임계값 1s + DETAIL 1주기 5s)로 늘어나므로 자체 1초 분주로 분리했다.
   - (2026-07-31 정정: 기존 문구는 "WHEN TIM5 인터럽트가 발생하면"이라 매 100ms 검사를 요구했으나, 구현은 처음부터 1초 주기였다 — 실제 설계 의도에 맞춰 문구를 정정.)
7. IF 핵심 데이터가 1초(1000ms) 이상 갱신되지 않았다면, THEN THE Timer_Scheduler SHALL 경고 LOG 패킷(level=WARNING, source=SYS, key=ERR_TIMEOUT, value=타임아웃 발생 데이터 key(4B)+마지막 갱신 이후 경과 ms(4B))을 생성하여 UART_Queue와 SD_Queue에 전송한다.
8. THE Timer_Scheduler SHALL 인터럽트 핸들러 내에서 지연(Delay) 연산을 수행하지 않고 tx_queue_send() 후 즉시 반환한다.

### 요구사항 7: UART 양방향 통신 — UART_Thread (TX) + UART_RX (RX)

**사용자 스토리:** 개발자로서, 패킷 데이터를 RFD900x 무선 모뎀을 통해 지상국으로 전송하고, 지상국에서 보낸 메시지를 수신하여 차량 내부(LCD 보드)로 전달하고 싶다. 이를 통해 양방향 텔레메트리 통신이 가능하다.

#### 인수 조건

##### TX (텔레메트리 보드 → 지상국)

1. THE UART_Thread SHALL ThreadX 스레드(Priority 5, Stack 1024B)로 생성되어, UART_Queue에서 tx_queue_receive(..., 2000)으로 유한 타임아웃(2초) 대기한다. 타임아웃 시 IWDG를 리프레시하고 다시 대기한다. 데이터가 없을 때는 Suspend 상태로 CPU 자원을 소모하지 않는다.
2. WHEN UART_Queue에서 패킷이 수신되면, THE UART_Thread SHALL 타입 헤더(0x01, CAN 데이터)를 앞에 추가한 뒤 COBS 인코딩하여 프레임으로 변환하고 USART2(115200bps, 8N1, HW Flow Control RTS/CTS)로 DMA 기반 송신한다.
3. THE COBS_Encoder SHALL 타입 헤더(1B) + LOG 패킷(16B) = 17바이트 원본을 COBS 인코딩하여 0x00 바이트를 제거하고, 0x00 delimiter를 추가하여 수신 측에서 패킷 경계를 식별할 수 있도록 한다.
4. THE UART_Thread SHALL DMA 전송(GPDMA1_Channel2)을 사용하여 CPU 블로킹 없이 데이터를 송신한다.
5. THE UART_Thread SHALL DMA 전송 완료를 ThreadX 세마포어 또는 이벤트 플래그로 대기하여, 전송 완료 전까지 다음 패킷을 처리하지 않는다.
6. WHEN UART DMA 전송이 완료되면, THE UART_Thread SHALL 직전에 송신 실패 상태였다면(uart_ok=0) LED_RFD900x(PE15)를 소등하여 회복을 표시한다. **(2026-08 정정: 매 전송마다 짧게 점등하던 토글 방식에서, "실패=점등/정상=소등"의 실시간 반영 방식으로 변경 — 육안으로 지속 상태를 바로 판별할 수 있도록 함)**
7. IF UART DMA 전송 중 오류가 발생하면, THEN THE UART_Thread SHALL 오류 LOG 패킷(level=ERROR, source=UART, key=ERR_UART_DMA, value=HAL UART 에러 코드(4B)+미전송 패킷 수(4B))을 SD_Queue에 전송하고 LED_RFD900x(PE15)를 점등한다.

##### RX (지상국 → 텔레메트리 보드)

8. THE Telemetry_Firmware SHALL UART_RX_Thread(Priority 7, Stack 1024B)를 생성하여 USART2 UART RX 수신을 전담한다. UART DMA(GPDMA1 RX 채널) + IDLE 인터럽트 조합으로 가변 길이 프레임을 수신하고, IDLE 인터럽트에서 수신 완료 이벤트를 알리면 UART_RX_Thread가 깨어나 COBS 디코딩 및 후속 처리를 수행한다.
9. WHEN 완전한 COBS 프레임이 수신되면, THE UART_RX_Thread SHALL COBS 디코딩 후 첫 바이트(타입 헤더)를 확인한다:
   - 0x01: CAN 데이터 (현재 미사용, 향후 확장용)
   - 0x02: 텍스트 메시지 → CAN TX를 통해 LCD 보드로 전달
10. WHEN 타입 0x02 메시지가 수신되면, THE UART_RX_Thread SHALL 메시지 페이로드를 8바이트씩 분할하여 ExtCAN_Transmit()을 통해 MCP2518FD CAN 버스로 LCD 보드에 전송한다.
11. IF UART RX 수신 중 프레임 오류(COBS 디코딩 실패, 버퍼 오버플로우 등)가 발생하면, THEN THE UART_RX_Thread SHALL 해당 프레임을 폐기하고 다음 0x00 delimiter부터 재동기화한다.

### 요구사항 8: SD 카드 비동기 로깅 — SD_Logger_Thread (Consumer, 더블 버퍼)

**사용자 스토리:** 개발자로서, 모든 텔레메트리 데이터를 SD 카드에 백업 저장하고 싶다. SD 카드의 섹터 단위 기록 특성을 활용하여 쓰기 횟수를 최소화하고 수명과 성능을 극대화한다.

#### 인수 조건

1. THE SD_Logger_Thread SHALL ThreadX 스레드(Priority 10, Stack 2048B)로 생성되어, SD_Queue에서 tx_queue_receive(..., 2000)으로 유한 타임아웃(2초) 대기한다. 타임아웃 시 IWDG를 리프레시하고 다시 대기한다. 데이터가 없을 때는 Suspend 상태로 CPU 자원을 소모하지 않는다.
2. WHEN SD_Queue에서 패킷이 수신되면, THE SD_Logger_Thread SHALL SD_DoubleBuffer의 현재 활성 버퍼에 패킷을 순차적으로 Append한다.
3. THE SD_DoubleBuffer SHALL Buffer A와 Buffer B 각각 16바이트 패킷 × 32개 = 512바이트 크기로, SD 카드 섹터(512바이트) 단위 기록에 정확히 맞춰 Read-Modify-Write 오버헤드를 방지한다.
4. WHEN 활성 버퍼가 가득 차면, THE SD_Logger_Thread SHALL 활성 버퍼를 비활성 버퍼로 스왑하고, 가득 찬 버퍼를 SDMMC1 IDMA를 통해 한 번의 Write 동작으로 SD 카드에 기록한다.
5. THE SD_Logger_Thread SHALL SDMMC1의 내장 IDMA(4-bit 버스, HW Flow Control, ClockDiv=2)를 활용하여 CPU 블로킹 없이 데이터를 기록한다.
6. WHEN SD 카드 기록이 성공하면, THE SD_Logger_Thread SHALL 직전에 SD 문제 상태였다면(sd_ok=0) LED_SD(PD8)를 소등하여 회복을 표시한다. **(2026-08 정정: 매 기록마다 짧게 점등하던 방식에서, "문제=점등/정상=소등"의 실시간 반영 방식으로 변경)**
7. THE SD_Logger_Thread SHALL SD 카드 수명을 보호하고 전원 차단 시 유실 구간을 제한하기 위해, 주기적으로(10초 간격) 파일 동기화(Flush/Sync)를 수행한다. (2026-08: 근거였던 WSC Cruiser 10초 기록 조항이 없어졌으나, 주기 자체는 자체 기준으로 유지한다.)
8. IF SD 카드가 감지되지 않으면(SD_DETECT: PD0), THEN THE SD_Logger_Thread SHALL 오류 LOG 패킷(level=ERROR, source=SD, key=ERR_SD_NOT_DETECTED)을 UART_Queue에 전송하고 SD 로깅을 비활성화하며 LED_SD(PD8)를 점등한다.
9. IF SD 카드 초기화(FileX 마운트)가 실패하면, THEN THE SD_Logger_Thread SHALL 오류 LOG 패킷(level=ERROR, source=SD, key=ERR_SD_INIT, value=FileX 에러 코드(4B))을 UART_Queue에 전송하고 SD 로깅을 비활성화하며 LED_SD(PD8)를 점등한다.
10. IF SD 카드 기록 중 오류가 발생하면, THEN THE SD_Logger_Thread SHALL 오류 LOG 패킷(level=ERROR, source=SD, key=ERR_SD_WRITE, value=FileX 에러 코드(4B)+실패 시점 파일 오프셋(4B))을 UART_Queue에 전송하고 SD 로깅 상태를 비활성화하며 LED_SD(PD8)를 점등한다.
11. WHEN 시스템이 부팅되면, THE SD_Logger_Thread SHALL RTC 시간 기반으로 새 로그 파일을 생성한다.

### 요구사항 9: RTOS 이벤트 드리븐 아키텍처

**사용자 스토리:** 개발자로서, 수신/패키징/송신/로깅이 ThreadX 스레드와 메시지 큐 기반의 Producer-Consumer 모델로 동작하기를 원한다. 이를 통해 CPU 자원을 최적화하고 추후 기능 확장 시에도 통신 타이밍이 밀리지 않는 견고한 구조를 확보한다.

#### 인수 조건

1. THE Telemetry_Firmware SHALL CAN 수신(인터럽트), 패키징(타이머 인터럽트 Producer), UART 송신(UART_Thread Consumer), SD 로깅(SD_Logger_Thread Consumer)을 완전히 분리된 이벤트 드리븐 구조로 운영한다.
2. THE Telemetry_Firmware SHALL 기존 while(1) 폴링 메인 루프를 사용하지 않고, ThreadX 커널 스케줄러가 스레드 실행을 관리하도록 한다.
3. THE UART_Thread와 SD_Logger_Thread SHALL 데이터가 없을 때 tx_queue_receive(..., 2000)으로 유한 타임아웃(2초) 대기한다. 타임아웃 발생 시 IWDG를 리프레시하고, 연속 타임아웃 횟수를 누적하여 임계값(예: 5회) 초과 시 경고 LOG 패킷(level=WARNING, source=SYS, key=ERR_TIMEOUT)을 생성한다. 이를 통해 TIM5 고장 등으로 데이터가 유입되지 않는 상황에서도 워치독 리셋을 방지한다.
4. THE Timer_Scheduler(Producer) SHALL tx_queue_send(..., TX_NO_WAIT)를 사용하여 인터럽트 컨텍스트에서 블로킹 없이 패킷을 큐에 전달한다.
5. THE Telemetry_Firmware SHALL UART_Thread(Priority 5)가 SD_Logger_Thread(Priority 10)보다 높은 우선순위로 동작하도록 한다.
6. THE Telemetry_Firmware SHALL IWDG(Independent Watchdog)를 넉넉한 타임아웃(예: 4~8초)으로 설정한다. 각 핵심 스레드(UART_Thread, UART_RX_Thread, SD_Logger_Thread)는 직접 HAL_IWDG_Refresh()를 호출하지 않고, 자신의 Alive Flag만 SET한다. Timer_Scheduler(TIM5 ISR)에서 모든 핵심 스레드의 Alive Flag가 SET되어 있을 때만 HAL_IWDG_Refresh()를 호출하고 플래그를 초기화한다. 하나라도 SET되지 않으면 리프레시하지 않아 IWDG 타임아웃으로 시스템이 자동 리셋된다.
7. THE Telemetry_Firmware SHALL 인터럽트 핸들러(CAN Rx, TIM5) 내에서 블로킹 함수 호출을 수행하지 않는다.

### 요구사항 10: 모듈화 코드 구조

**사용자 스토리:** 개발자로서, 기능별로 코드가 깔끔하게 분리되기를 원한다. 이를 통해 유지보수와 확장이 용이하다.

#### 인수 조건

1. THE Telemetry_Firmware SHALL CAN 수신부, 패키징 함수, COBS 인코더, SD 로깅 제어부, UART 송신부, ThreadX 스레드/큐 초기화부를 별도의 소스 파일(.c/.h)로 모듈화한다.
2. THE Telemetry_Firmware SHALL 모든 커스텀 코드를 `/* USER CODE BEGIN ... */`와 `/* USER CODE END ... */` 주석 블록 내부에만 작성한다.
3. THE Telemetry_Firmware SHALL example_code 폴더의 기존 코드 구조(링버퍼, 로거, 타입 정의)를 참고하여 일관된 코딩 스타일을 유지한다.
4. THE Telemetry_Firmware SHALL 타입 정의(VehicleData_t, LOG, 열거형 등)를 전용 헤더 파일(types.h)에 집중 관리한다.

### 요구사항 11: RTC 시각 관리 및 시각 데이터 전송

**사용자 스토리:** 개발자로서, RTC를 통해 실제 날짜/시간 정보를 관리하고 이를 SD 카드 로그 파일명과 텔레메트리 패킷에 활용하고 싶다. 이를 통해 지상국에서 데이터의 절대 시각을 파악하고, SD 카드 로그 파일을 부팅 시각별로 구분할 수 있다.

#### 인수 조건

1. WHEN 시스템이 부팅되면, THE RTC_Manager SHALL 먼저 RTC Backup Register를 확인하여 이미 시간이 세팅되어 동작 중(VBAT 유지)인 경우 초기화를 건너뛰고, 그렇지 않은 경우에만 HAL_RTC_GetTime/HAL_RTC_GetDate를 호출하여 현재 날짜/시간을 DATETIME 구조체에 읽어온다.
2. THE RTC_Manager SHALL 부팅 시 읽은 DATETIME 값을 SD_Logger_Thread에 전달하여 로그 파일명 생성에 사용한다.
3. THE Timer_Scheduler SHALL 10초(10000ms) 주기로 RTC에서 현재 시각을 읽어 DATETIME 구조체를 갱신한다. (2026-08: 주기의 근거였던 WSC 규정 조항은 없어졌으나 로그 시각 동기 용도로 유지한다.)
4. WHEN RTC 시각이 갱신되면, THE Timer_Scheduler SHALL 현재 시각 정보를 LOG 패킷(level=INFO, source=SYS, key=RTC_TIME)으로 포장하여 UART_Queue와 SD_Queue에 각각 tx_queue_send()로 전송한다.
5. THE RTC_Manager SHALL DATETIME 구조체를 year(1B) + month(1B) + date(1B) + hour(1B) + minute(1B) + second(1B) = 6바이트로 구성하여 LOG 패킷의 value 필드(8바이트)에 저장한다.
6. THE RTC_Manager SHALL 24시간 포맷(RTC_HOURFORMAT_24)으로 시각을 관리한다.
7. IF RTC 읽기에 실패하면, THEN THE RTC_Manager SHALL 오류 LOG 패킷(level=ERROR, source=SYS, key=ERR_RTC_READ)을 생성하여 UART_Queue와 SD_Queue에 전송하고 LED_ERR(PE11)를 점등한다.

### 요구사항 12: 오류 처리 및 상태 표시

**사용자 스토리:** 개발자로서, 시스템의 현재 상태와 오류를 LED와 로그를 통해 파악하고 싶다. 이를 통해 현장에서 빠르게 문제를 진단할 수 있다.

#### 인수 조건

1. THE Telemetry_Firmware SHALL 시스템 상태(SD 카드, CAN, UART, 오류)를 SYSTEM_STATE 구조체(sd_ok/can_ok/uart_ok/mcp_ok/error)로 관리한다.
2. **(2026-08 정정: 상시 점등 → 실시간 반영)** THE Telemetry_Firmware SHALL LED_BOARD_CHECK(PE12)를 `SystemState_RefreshBoardLed()`를 통해 실시간으로 갱신한다: **현재 활성화된** 서브시스템(can_ok, mcp_ok는 항상; uart_ok는 PHASE4_UART_PIPELINE 활성 시; sd_ok는 PHASE5_SD_LOGGING 활성 시)이 전부 정상이면 점등, 하나라도 비정상이면 소등한다. can_ok/mcp_ok/uart_ok/sd_ok가 바뀌는 모든 지점에서 재호출되며, 부팅 직후(CAN/MCP 초기화 직후)에도 1회 호출되어 초기 상태를 즉시 반영한다.
   - 참고: 이는 `Power_check` 핀 기반 "전원 정상" 표시와는 별개다. 전원 정상 표시는 GPIO/펌웨어를 거치지 않는 별도의 **패시브 하드웨어 LED(3.3V 레일에 직결)**로 구현하기로 결정 — 소프트웨어 요구사항 아님. 과거 버전은 이 LED를 `Power_check`(PB1) 핀 감지 기반으로 정의했었으나, 회로도(`WSC.kicad_sch`) 대조 결과 PB1이 미배선 상태로 확인되어 하드웨어 쪽에서 패시브 방식으로 단순화함.
3. **(2026-08 정정: 오류 시 점등만 하던 단방향 표시 → 실시간 양방향 표시)** THE Telemetry_Firmware SHALL 아래 3개 LED를 "문제 진행 중=점등 / 정상=소등"으로 실시간 구동한다(문제가 해소되면 다음 정상 이벤트에서 자동 소등, 별도 리셋 불필요):
   - LED_SD(PD8): sd_ok 반영. SD 문제(ERR_SD_INIT, ERR_SD_WRITE, ERR_SD_NOT_DETECTED, ERR_SD_QUEUE_FULL) 발생 시 점등, 다음 SD 기록 성공 시 소등. **(2026-08 정정: PE13→PD8, 회로도 실측 대조로 발견된 핀 불일치 수정)**
   - LED_CAN(PB15): can_ok 반영. CAN 문제(ERR_CAN_BUS_OFF, ERR_CAN_RX_OVERFLOW, 프로토콜 오류) 발생 시 점등, 다음 CAN 메시지 정상 수신 시 소등. **(2026-08 정정: PE14→PB15, 동일 사유)**
   - LED_RFD900x(PE15): uart_ok 반영. UART 송신 문제(ERR_UART_DMA, ERR_UART_QUEUE_FULL) 발생 시 점등, 다음 DMA 송신 성공 시 소등.
   - LED_ERR(PE11)는 이 실시간 방식과 달리 **상시 래치**로 유지한다(아래 4항). SPI/MCP2518FD 오류(ERR_SPI_TIMEOUT, ERR_SPI_TRANSFER)도 mcp_ok를 통해 LED_BOARD_CHECK 집계에는 실시간 반영되지만, 여러 치명적 오류가 LED_ERR 하나를 공유하므로 개별 회복으로 LED_ERR을 소등하지 않는다.
4. THE Telemetry_Firmware SHALL LED_ERR(PE11)를 기타 오류(ERR_SPI_TIMEOUT, ERR_SPI_TRANSFER, ERR_RTC_READ, ERR_INIT_FAIL, ERR_TIMEOUT, ERR_POWER_DOWN, ERR_WATCHDOG) 발생 시 점등하는 상시 래치로 사용하며, 모든 오류 발생 시 해당 오류 정보를 LOG 패킷(적절한 level, source, key, value)으로 포장하여 가능한 큐(UART_Queue 및/또는 SD_Queue)에 전송한다.
5. IF 복구 불가능한 오류(level=CRITICAL)가 발생하면, THEN THE Telemetry_Firmware SHALL LED_ERR(PE11)를 점등하고 인터럽트를 비활성화한 후 무한 루프에 진입한다.
6. WHEN Power_check(PD6) 핀을 통해 전원 강하(전원 꺼짐)가 감지되면, THE Telemetry_Firmware SHALL CRITICAL LOG 패킷(source=SYS, key=ERR_POWER_DOWN, value=감지 시점 tick(4B)+미기록 버퍼 잔량(4B))을 생성하고, 즉시 SD_Logger의 긴급 Flush를 수행하여 SRAM에 남은 마지막 데이터를 SD 카드에 안전하게 저장한 후 무한 루프에 진입한다.

### 요구사항 13: 지상국 → 차량 메시지 전달 (CAN TX via MCP2518FD)

**사용자 스토리:** 개발자로서, 지상국 대시보드에서 보낸 메시지(예: "통신 끊김", 경고 텍스트 등)를 RFD900x(UART)로 수신한 뒤 MCP2518FD를 통해 CAN 버스로 전송하여, 탑승자의 LCD 보드에 표시하고 싶다. 이를 통해 지상국과 탑승자 간 실시간 커뮤니케이션이 가능하다.

#### 인수 조건

1. WHEN UART RX에서 타입 0x02(텍스트 메시지) 프레임이 수신되면, THE Telemetry_Firmware SHALL 메시지 페이로드를 ExtCAN_Transmit()을 통해 CAN 버스로 LCD 보드에 전달한다.
2. THE ExtCAN_Controller SHALL MCP251XFD_TransmitMessageToFIFO()를 사용하여 CAN 버스에 메시지를 전송한다.
3. THE ExtCAN_Controller SHALL CAN TX 전용 FIFO(MCP251XFD_FIFO2 등)를 설정하여 RX FIFO와 분리한다.
4. THE ExtCAN_Controller SHALL CAN TX 메시지의 CAN ID를 사전 정의된 LCD 보드용 ID로 설정한다. [^3]
5. IF CAN TX 전송이 실패하면, THEN THE ExtCAN_Controller SHALL 오류 LOG 패킷(level=ERROR, source=CAN(SRC_CAN_ERR), key=ERR_CAN_TX_FAIL, value=MCP251XFD 에러 코드(4B)+재시도 횟수(4B))을 생성하여 UART_Queue와 SD_Queue에 전송한다.
6. THE ExtCAN_Controller SHALL CAN TX 메시지 페이로드를 최대 8바이트(Classic CAN)로 제한한다. 8바이트 중 첫 1바이트는 제어 바이트(bit7: 1=마지막 프레임/0=계속, bit6~0: 시퀀스 번호 0~127)로 사용하고, 나머지 7바이트에 실제 데이터를 담는다. 지상국 메시지가 7바이트를 초과하면 복수의 CAN 프레임으로 분할 전송하며, LCD 보드는 제어 바이트를 기반으로 프레임을 재조립한다.
7. THE Telemetry_Firmware SHALL 지상국 → 차량 메시지 프로토콜을 다음과 같이 정의한다: COBS 프레임의 첫 바이트가 타입 헤더(0x01=CAN 데이터, 0x02=텍스트 메시지)이며, 타입 0x02 수신 시 나머지 페이로드를 UTF-8 텍스트로 해석하여 CAN TX로 전달한다.

---

[^1]: **해결됨 — CAN ID 목록 확정 (2026-07-24 PR#31 기준)**: BMS(HV) PRIMARY BOT(0x040~0x043)/TOP(0x044~0x047), BMS(HV) DETAIL BOT(0x048~0x050)/TOP(0x051~0x059), BMS(HV) SD_ONLY BOT(0x05A~0x06C)/TOP(0x06D~0x07F), BMS(HV) LEGACY(0x111~0x133/0x211~0x233, FET 0x123/0x223만 파싱), MPPT#1(0x310~0x316), MPPT#2(0x320~0x326), MPPT#3(0x330~0x336), BMS(LV, Prohelion)는 base 0x600(변경 없음). Motor/VCU는 잠정(0x400~0x4FF, WaveSculptor22 기반 virtual — VCU 출력 표 확정 시 교체). 전체 신호 표·VehicleData 매핑은 `can-signal-summary.md`(SSOT) 참조 — 설계 원칙은 `telemetry-signals.md`.

[^2]: **해결됨 — FDCAN1/MCP2518FD 역할**: FDCAN1 = 차량 센서 전체 RX(BMS·MPPT·Motor), 범위 필터 2개(0x040~0x34F, 0x400~0x6FF). MCP2518FD = **향후 저우선순위 주변장치용 버스로 예약**(현재 확정 용도: 지상국→LCD 메시지 TX). LCD 외 일부 센서 RX 가능성 있으나 **미확정** → 확정 시 추가. 소스 구분: 데이터 패킷은 소스별 LOG_SOURCE(SRC_BMS_BOT 등), 에러 패킷은 SRC_CAN_ERR/SRC_SPI_ERR 등.

[^3]: **[미정] LCD 보드용 CAN ID**: LCD 보드가 수신할 CAN ID가 아직 확정되지 않았다. 확정되면 ExtCAN_Transmit()의 CAN ID 설정을 업데이트해야 한다. 지상국 → 차량 메시지 프로토콜(COBS 프레임 + 타입 헤더)은 확정됨: 0x01=CAN 데이터, 0x02=텍스트 메시지.
