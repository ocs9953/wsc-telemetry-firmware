/* Host-test stub — packet_builder.c only needs HAL_GetTick() from this header. */
#ifndef STUB_STM32U5XX_HAL_H
#define STUB_STM32U5XX_HAL_H
#include <stdint.h>
static inline uint32_t HAL_GetTick(void) { return 123456u; }
#endif
