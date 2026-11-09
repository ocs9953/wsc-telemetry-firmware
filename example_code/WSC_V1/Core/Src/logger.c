/* Core/Src/logger.c */
#include "logger.h"
#include "usart.h"
#include "ringbuffer.h"
#include <string.h>

// [버퍼 참조]
extern ring_buffer_t LORA_BUFFER; // LoRa용
extern ring_buffer_t SD_BUFFER;   // SD카드용

// [상태 변수 참조] SD카드가 정상일 때만 저장하기 위해 필요
extern SYSTEM_STATE sys_state;

int SYS_LOG(LOG_LEVEL level, LOG_SOURCE source, int key, void *payload) {

    // 1. 로그 패킷 생성 (지역 변수 사용 - Safe)
    LOG local_packet;

    // 헤더 작성
    local_packet.timestamp = HAL_GetTick();
    local_packet.level = level;
    local_packet.source = source;
    local_packet.key = key;

    // 데이터 복사
    if (payload != NULL) {
        memcpy(local_packet.value, payload, 8);
    } else {
        memset(local_packet.value, 0, 8);
    }

    // 체크섬 계산
    local_packet.checksum = 0;
    uint8_t *p = (uint8_t*)&local_packet;
    for(int i = 0; i < sizeof(LOG) - 1; i++) {
        local_packet.checksum ^= p[i];
    }

    // ---------------------------------------------------------
    // 2. 버퍼 등록 (여기가 핵심!)
    // ---------------------------------------------------------

    // (1) LoRa 버퍼에 등록 (무조건 전송)
    ring_buffer_queue_arr(&LORA_BUFFER, (char *)&local_packet, sizeof(LOG));

    // (2) SD 버퍼에 등록 (SD카드가 정상 초기화된 경우에만)
    if (sys_state.SD) {
        ring_buffer_queue_arr(&SD_BUFFER, (char *)&local_packet, sizeof(LOG));
    }

    return 0;
}
