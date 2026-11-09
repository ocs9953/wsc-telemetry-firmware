/**
  ******************************************************************************
  * @file    cobs.h
  * @brief   COBS 인코딩/디코딩 인터페이스
  ******************************************************************************
  */
#ifndef __COBS_H
#define __COBS_H

#include <stdint.h>
#include <stddef.h>

/* COBS 인코딩: src(16B) → dst(최대 17B), 반환값 = 인코딩된 바이트 수 */
size_t COBS_Encode(const uint8_t *src, size_t src_len, uint8_t *dst);

/* COBS 디코딩: src → dst, 반환값 = 디코딩된 바이트 수, 0 = 오류 */
size_t COBS_Decode(const uint8_t *src, size_t src_len, uint8_t *dst);

#endif /* __COBS_H */
