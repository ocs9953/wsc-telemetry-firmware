# WSC 텔레메트리 송신 보드 펌웨어

World Solar Challenge 2027 출전 태양광 자동차(KUST)의 텔레메트리 송신 보드 펌웨어입니다.
차량 CAN 데이터를 수집해 SD 카드에 기록하고, 동시에 RFD900x 무선 모뎀으로 지원 차량의 지상국에 실시간 전송합니다.

```
BMS · MPPT · 모터 컨트롤러 ──CAN 500k (Bus A)──► FDCAN1 ┐
                                                          ├─► 고정 길이 레코드 ─┬─► RFD900x (UART, COBS) ─► 지상국 대시보드
계기판(LCD) ◄──CAN (Bus B)── MCP2518FD (SPI) ◄────────────┘                    └─► SD 카드 (FileX)
```

## 하드웨어

| 항목 | 내용 |
|---|---|
| MCU | STM32U575VGT (Cortex-M33, 160 MHz) |
| RTOS / 파일시스템 | ThreadX, FileX (SDMMC1) |
| 차량 CAN (Bus A) | 내장 FDCAN1, 500 kbps, 수신 위주 |
| 계기판 CAN (Bus B) | MCP2518FD (SPI) — 차량 버스와 물리적으로 분리 |
| 무선 | RFD900x (UART) |
| 클럭 | 외부 크리스털(HSE) — CAN 비트 타이밍 허용 오차를 맞추기 위해 내부 발진기에서 변경 |

## 주요 기능

- **이기종 CAN 통합**: BMS·MPPT·모터 컨트롤러의 메시지를 해석해 시각·출처·항목·값·CRC-8을 담은 16바이트 레코드로 정규화
- **우선순위 기반 전송**: 신호별 실제 변화 속도에 맞춰 무선 송신 주기를 정해 무선 점유율을 78%에서 16%로 낮춤
- **무선과 독립된 기록**: 무선이 끊겨도 모든 항목을 SD 카드에 기록. 카드 분리·멈춤에도 수신과 무선 송신은 계속되고, 카드는 리셋 없이 복구
- **신선도(Freshness)**: 프레임별 마지막 수신 시각으로 묶음별 stale 상태를 판정해 지상국에 함께 전송
- **양방향 통신**: 지상국 문자 메시지를 계기판으로 중계, 계기판 명령은 범위를 검증한 뒤 차량 버스로 전달
- **자체 감시**: 모든 스레드가 살아 있을 때만 워치독 갱신, 외부 CAN 컨트롤러 모드 주기 점검 및 자동 복구
- **단계 게이트**: `Core/Inc/phase_config.h`의 스위치로 기능을 단계별로 켜며 보드 검증

## 폴더

| 경로 | 내용 |
|---|---|
| `Core/` | 애플리케이션 코드 (CAN 수신, 패킷 생성, COBS, UART 송신, SD 로거, 신선도, 스케줄러 등) |
| `Lib/MCP251XFD/` | MCP2518FD 드라이버 (Emandhal/MCP251XFD, 원저작자 코드) |
| `Drivers/`, `Middlewares/` | STM32 HAL, CMSIS, ThreadX, FileX (ST 제공) |
| `docs/` | CAN 신호 정리, 계기판 CAN 인터페이스 사양, 무선 패킷 설계 |
| `tests/host/` | 호스트 단위 시험(COBS·패킷), 장애 주입·연속성 측정 스크립트, 벤치 시험 기록 |
| `tests/dummy_nodes/` | 벤치용 BMS·MPPT·모터 더미 노드 (STM32F4) |
| `tools/` | OpenOCD 기반 실시간 관측 스크립트 |

## 검증

- **호스트 시험**: 실제 `cobs.c`, `packet_builder.c`를 PC용 gcc로 빌드해 무작위 입력으로 검증 (`tests/host/test_main.c`)
- **장애 주입**: SD 카드 분리, 클럭 차단, 인터럽트 차단 중에도 CAN 수신이 끊기지 않는지 순번 누락과 최대 수신 간격으로 판정 (`tests/host/CAN_CONTINUITY_2026-09-21.md`, `SD_FAULT_VALIDATION_*.md`)
- **비정지 관측**: 디버거가 코어를 멈추면 수신 FIFO가 넘치므로, 코어를 멈추지 않고 메모리를 읽는 방식으로 관측 (`tests/host/ocd_*.js`)
- **CI**: GitHub Actions에서 단계 게이트 조합별 ARM 빌드 (`.github/workflows/ci.yml`)

## 빌드

STM32 for VSCode 확장 또는 make로 빌드합니다. 툴체인 경로는 `ARM_GCC_PATH`로 지정합니다.

```sh
make -f STM32Make.make ARM_GCC_PATH=/path/to/arm-none-eabi/bin
```
