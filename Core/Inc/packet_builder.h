/**
  ******************************************************************************
  * @file    packet_builder.h
  * @brief   16바이트 LOG 패킷 생성 인터페이스
  ******************************************************************************
  */
#ifndef __PACKET_BUILDER_H
#define __PACKET_BUILDER_H

#include "types.h"
#include <stdbool.h>

/* 16바이트 LOG 패킷 생성 */
void PacketBuilder_Create(LOG *pkt, LOG_LEVEL level, LOG_SOURCE source,
                          uint8_t key, const void *value, uint8_t value_len);

/* CRC-8 체크섬 계산 (다항식 0x07, bytes[0..14]) */
uint8_t PacketBuilder_CalcChecksum(const LOG *pkt);

/* 체크섬 검증 */
bool PacketBuilder_VerifyChecksum(const LOG *pkt);

/* float 2개를 8바이트 value 필드에 패킹 (RTC/규정 전용) */
void PacketBuilder_PackFloat2(uint8_t *value_field, float f1, float f2);

/* 8바이트 value 필드에서 float 2개 언패킹 */
void PacketBuilder_UnpackFloat2(const uint8_t *value_field, float *f1, float *f2);

/* 정수 패킹 — 8바이트 value 필드에 u16×4 / i16×4 / u32×2 (리틀엔디안) */
void PacketBuilder_PackU16x4(uint8_t *value_field,
                             uint16_t a, uint16_t b, uint16_t c, uint16_t d);
void PacketBuilder_PackI16x4(uint8_t *value_field,
                             int16_t a, int16_t b, int16_t c, int16_t d);
void PacketBuilder_PackU32x2(uint8_t *value_field, uint32_t a, uint32_t b);

#endif /* __PACKET_BUILDER_H */
