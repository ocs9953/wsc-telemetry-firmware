# 호스트 PC 순수 로직 테스트 (task 7.2~7.4, 8.2~8.3)

`Core/Src/cobs.c`·`Core/Src/packet_builder.c`를 실제 소스 그대로 네이티브 gcc로
컴파일해 실행하는 property test. ARM 크로스 컴파일러가 아니라 이 PC에서 직접
빌드·실행되므로 초 단위로 수만 케이스를 검증할 수 있다.

`stub_include/stm32u5xx_hal.h`는 `packet_builder.c`가 요구하는 `HAL_GetTick()`
선언만 제공하는 최소 스텁이며, 실제 HAL을 대체하지 않는다(cobs.c는 스텁 불필요).

## 실행 방법

네이티브 gcc(MinGW-w64 등, ARM용 arm-none-eabi-gcc 아님) 필요:

```sh
gcc -std=c11 -Wall -Wextra -Wconversion \
  -I tests/host/stub_include -I Core/Inc \
  tests/host/test_main.c Core/Src/packet_builder.c Core/Src/cobs.c \
  -o tests/host/test_host.exe
./tests/host/test_host.exe
```

> ✅ **2026-08-24 해소**: MinGW-w64(`winget install BrechtSanders.WinLibs.POSIX.UCRT`, gcc 16.1.0)를
> 설치해 **전체 테스트를 실제로 실행했고 전부 PASS**했다. `test_crc8_table_equivalence()` 포함
> 미실행 항목이 더 이상 없다(그전까지 PowerShell로 우회 검증하던 CRC 등가성도 이제 여기서 확인된다).
>
> 컴파일 자체도 `-Wall -Wextra -Wconversion`에서 경고 0이다.
> 산출물 `test_host.exe`는 `.gitignore`에 등록되어 커밋되지 않는다.

## 검증 항목 (마지막 실행: 2026-08-24, 전부 PASS)

- **Property 1** (라운드트립): `sizeof(LOG)==16` 호스트 컴파일러에서도 확인
- **신규 2026-07-31** (`test_crc8_table_equivalence`, ⏳ 미실행): `packet_builder.c`의 crc8이 테이블
  룩업으로 바뀌었으므로, 원본 비트 단위 구현을 테스트 파일에 참조로 두고 **256 바이트값 전수 +
  랜덤 15바이트 20,000회 + 알려진 답(`CRC-8('123456789')==0xF4`)** 으로 대조한다. 온-와이어
  체크섬이 바뀌면 지상국 디코더가 전부 깨지므로 이 등가성은 반드시 유지되어야 한다.
- **신규 2026-07-31** (`test_struct_layout` 보강, ⏳ 미실행): `LOG_QUEUE_MSG_WORDS * 4 == sizeof(LOG)`
  — 불일치 시 ThreadX 큐 용량이 조용히 절반이 된다(2026-07 8→4 ULONG 수정의 회귀 방지).
- **Property 2** (체크섬 무결성): 패킷 2000개 × 120비트 플립 = 240,000회, 전부 검출
- **Property 3** (float 패킹): `PackFloat2`/`UnpackFloat2` 5,000회 라운드트립
- **Property 4** (COBS 라운드트립): 20,000회 (길이 0~16 랜덤 포함)
- **Property 5** (COBS 0x00 부재): 위 20,000회 인코딩 결과 전수 검사 + 손상 입력 거부 확인
- **신규**: `PackU16x4`/`PackI16x4`(경계값 INT16_MIN/MAX 포함)/`PackU32x2` 각 5,000회
