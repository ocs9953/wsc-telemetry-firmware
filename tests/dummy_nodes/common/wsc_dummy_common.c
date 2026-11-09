#include "main.h"
#include "wsc_dummy_common.h"

uint8_t wsc_can_send(CAN_HandleTypeDef *hcan, uint16_t std_id, const uint8_t *data)
{
    return wsc_can_send_dlc(hcan, std_id, data, 8u);
}

uint8_t wsc_can_send_dlc(CAN_HandleTypeDef *hcan, uint16_t std_id,
                         const uint8_t *data, uint8_t dlc)
{
    CAN_TxHeaderTypeDef hdr;
    uint32_t mailbox;

    hdr.StdId              = std_id;
    hdr.ExtId              = 0;
    hdr.IDE                = CAN_ID_STD;
    hdr.RTR                = CAN_RTR_DATA;
    hdr.DLC                = dlc;
    hdr.TransmitGlobalTime = DISABLE;

    uint32_t start = HAL_GetTick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(hcan) == 0u)
    {
        if ((HAL_GetTick() - start) > 10u)
            return 0u;
    }

    if (HAL_CAN_AddTxMessage(hcan, &hdr, (uint8_t *)data, &mailbox) != HAL_OK)
        return 0u;

    return 1u;
}

void wsc_heartbeat_tick(CAN_HandleTypeDef *hcan, uint16_t std_id,
                        uint32_t period_ms)
{
    static uint32_t hb_seq   = 0u;
    static uint32_t hb_next  = 0u;
    static uint8_t  hb_armed = 0u;
    uint8_t  d[8];
    uint32_t now = HAL_GetTick();

    if (std_id == 0u) return;

    if (!hb_armed) { hb_armed = 1u; hb_next = now + period_ms; return; }

    if ((now - hb_next) >= 0x80000000u) return;
    hb_next += period_ms;

    d[0] = (uint8_t)(hb_seq      ); d[1] = (uint8_t)(hb_seq >>  8);
    d[2] = (uint8_t)(hb_seq >> 16); d[3] = (uint8_t)(hb_seq >> 24);
    d[4] = (uint8_t)(now         ); d[5] = (uint8_t)(now   >>  8);
    d[6] = (uint8_t)(now   >> 16); d[7] = (uint8_t)(now   >> 24);

    hb_seq++;
    (void)wsc_can_send(hcan, std_id, d);
}
