/**
  ******************************************************************************
  * @file    cobs.c
  * @brief   COBS 인코딩/디코딩 구현 (task 8.1)
  *
  *          COBS(Consistent Overhead Byte Stuffing): 데이터에서 0x00을 제거해
  *          0x00을 프레임 delimiter로 쓸 수 있게 한다. 16B 입력 → 최대 17B
  *          출력(+ 호출자가 0x00 delimiter 추가 → 총 18B).
  *          Requirements: 7.2, 7.3
  ******************************************************************************
  */
#include "cobs.h"

/**
 * @brief  COBS 인코딩. dst 크기는 최소 src_len + 1 이어야 한다.
 * @retval 인코딩된 바이트 수 (delimiter 미포함)
 */
size_t COBS_Encode(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t  read  = 0;
    size_t  write = 1;      /* dst[0]은 첫 코드 바이트 자리 */
    size_t  code_pos = 0;   /* 현재 코드 바이트 위치 */
    uint8_t code = 1;       /* 다음 0x00까지의 거리 (자기 자신 포함) */

    while (read < src_len)
    {
        if (src[read] == 0x00)
        {
            dst[code_pos] = code;
            code_pos = write++;
            code = 1;
        }
        else
        {
            dst[write++] = src[read];
            code++;
            if (code == 0xFF)   /* 254바이트 비-제로 블록 한계 */
            {
                dst[code_pos] = code;
                code_pos = write++;
                code = 1;
            }
        }
        read++;
    }
    dst[code_pos] = code;
    return write;
}

/**
 * @brief  COBS 디코딩. src에 0x00이 포함되면 형식 오류.
 * @retval 디코딩된 바이트 수, 0 = 오류
 */
size_t COBS_Decode(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t read  = 0;
    size_t write = 0;

    while (read < src_len)
    {
        uint8_t code = src[read++];
        if (code == 0x00) return 0;                          /* 코드 바이트는 0 불가 */
        if (read + (size_t)(code - 1) > src_len) return 0;   /* 길이 초과 */

        for (uint8_t i = 1; i < code; i++)
        {
            uint8_t b = src[read++];
            if (b == 0x00) return 0;                         /* 데이터에 0 불가 */
            dst[write++] = b;
        }
        if (code != 0xFF && read < src_len)
            dst[write++] = 0x00;                             /* 제거됐던 0 복원 */
    }
    return write;
}
