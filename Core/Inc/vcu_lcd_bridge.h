#ifndef __VCU_LCD_BRIDGE_H
#define __VCU_LCD_BRIDGE_H

#include "main.h"
#include "tx_api.h"
#include <stdint.h>
#include <stdbool.h>

void VcuLcd_Init(void);

void VcuLcd_Thread_Entry(ULONG thread_input);

bool VcuLcd_OnLcdFrame(uint32_t can_id, const uint8_t *data, uint8_t dlc);

#endif
