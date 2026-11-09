/**
  ******************************************************************************
  * @file    rtc_manager.c
  * @brief   RTC 시각 관리 구현 (task 14.1)
  *
  *          Requirements: 11.1, 11.2, 11.5, 11.6, 11.7
  ******************************************************************************
  */
#include "rtc_manager.h"
#include "app_threadx.h"
#include "rtc.h"
#include "main.h"
#include "packet_builder.h"
#include "tx_api.h"
#include <stdio.h>
#include <string.h>

extern TX_QUEUE UART_Queue;
extern TX_QUEUE SD_Queue;
extern SYSTEM_STATE SystemState;

static volatile uint8_t rtc_busy = 0;

static HAL_StatusTypeDef rtc_read_raw(DATETIME *dt)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    HAL_StatusTypeDef st;

    st = HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
    if (st == HAL_OK)
        st = HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
    if (st != HAL_OK)
        return st;

    dt->year   = d.Year;
    dt->month  = d.Month;
    dt->date   = d.Date;
    dt->hour   = t.Hours;
    dt->minute = t.Minutes;
    dt->second = t.Seconds;
    return HAL_OK;
}

/**
 * @brief  RTC에서 현재 시각 읽기 → DATETIME.
 * @note   HAL 규칙: GetTime 후 반드시 GetDate를 호출해야 섀도 레지스터가
 *         언락된다 (순서 위반 시 다음 읽기가 멈춘 값을 반환).
 *         실패 시 ERR_RTC_READ ERROR LOG 생성 + LED_ERR 점등 (Req 11.7).
 * @return HAL_BUSY — GPS 보정이 RTC 를 쓰는 중. 오류가 아니므로 보고하지 않는다.
 *         호출부는 이번 읽기를 건너뛰면 된다.
 */
HAL_StatusTypeDef RTC_Manager_GetDateTime(DATETIME *dt)
{
    if (rtc_busy)
        return HAL_BUSY;

    HAL_StatusTypeDef st = rtc_read_raw(dt);

    if (st != HAL_OK)
    {
        LOG err_pkt;
        uint8_t val[8] = {0};
        uint32_t hal_err = (uint32_t)st;
        memcpy(&val[0], &hal_err, 4);

        PacketBuilder_Create(&err_pkt, LOG_ERROR, SRC_SYS,
                             (uint8_t)ERR_RTC_READ, val, 8);
        ULONG msg[LOG_QUEUE_MSG_WORDS] = {0};
        memcpy(msg, &err_pkt, sizeof(LOG));
        TelemQueue_Send(&UART_Queue, msg);
        TelemQueue_Send(&SD_Queue, msg);

        HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
        SystemState.error = 1;
        return HAL_ERROR;
    }
    return HAL_OK;
}

/**
 * @brief  DATETIME 6바이트를 LOG value 필드(8B)에 패킹 (뒤 2B는 0).
 */
void RTC_Manager_PackToValue(const DATETIME *dt, uint8_t *value_field)
{
    value_field[0] = dt->year;
    value_field[1] = dt->month;
    value_field[2] = dt->date;
    value_field[3] = dt->hour;
    value_field[4] = dt->minute;
    value_field[5] = dt->second;
    value_field[6] = 0;
    value_field[7] = 0;
}

/**
 * @brief  부팅 시각 기반 파일명 생성: "YYMMDD_HHMMSS.log"
 * @param  buf_len  최소 18 (17자 + NUL)
 */
void RTC_Manager_MakeFilename(const DATETIME *dt, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%02u%02u%02u_%02u%02u%02u.log",
             (unsigned)dt->year,  (unsigned)dt->month,  (unsigned)dt->date,
             (unsigned)dt->hour,  (unsigned)dt->minute, (unsigned)dt->second);
}


static uint8_t rtc_days_in_month(uint32_t y, uint32_t m)
{
    static const uint8_t d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2u && ((y % 4u == 0u && y % 100u != 0u) || (y % 400u == 0u)))
        return 29u;
    return d[m - 1u];
}

static uint8_t rtc_set_from_gps_locked(uint32_t utc_time, uint32_t utc_date, DATETIME *before);

uint8_t RTC_Manager_SetFromGps(uint32_t utc_time, uint32_t utc_date, DATETIME *before)
{
    rtc_busy = 1u;
    uint8_t ok = rtc_set_from_gps_locked(utc_time, utc_date, before);
    rtc_busy = 0u;
    return ok;
}

static uint8_t rtc_set_from_gps_locked(uint32_t utc_time, uint32_t utc_date, DATETIME *before)
{
    uint32_t yy = utc_date / 10000u;
    uint32_t mo = (utc_date / 100u) % 100u;
    uint32_t dd = utc_date % 100u;
    uint32_t hh = utc_time / 1000000u;
    uint32_t mi = (utc_time / 10000u) % 100u;
    uint32_t ss = (utc_time / 100u) % 100u;

    if (yy < 2024u || yy > 2099u)          return 0u;
    if (mo < 1u || mo > 12u)               return 0u;
    if (dd < 1u || dd > rtc_days_in_month(yy, mo)) return 0u;
    if (hh > 23u || mi > 59u || ss > 59u)  return 0u;

    int32_t tod = (int32_t)(hh * 60u + mi) + (int32_t)RTC_LOCAL_UTC_OFFSET_MIN;
    while (tod < 0)
    {
        tod += 1440;
        if (dd > 1u) { dd--; }
        else { if (mo > 1u) { mo--; } else { mo = 12u; yy--; } dd = rtc_days_in_month(yy, mo); }
    }
    while (tod >= 1440)
    {
        tod -= 1440;
        if (dd < rtc_days_in_month(yy, mo)) { dd++; }
        else { dd = 1u; if (mo < 12u) { mo++; } else { mo = 1u; yy++; } }
    }
    hh = (uint32_t)(tod / 60);
    mi = (uint32_t)(tod % 60);

    DATETIME cur;
    if (rtc_read_raw(&cur) != HAL_OK)
        return 0u;
    if (before != NULL)
        *before = cur;

    if (cur.year == (uint8_t)(yy - 2000u) && cur.month == (uint8_t)mo && cur.date == (uint8_t)dd)
    {
        int32_t cur_s = (int32_t)cur.hour * 3600 + (int32_t)cur.minute * 60 + (int32_t)cur.second;
        int32_t new_s = (int32_t)hh * 3600 + (int32_t)mi * 60 + (int32_t)ss;
        int32_t delta = new_s - cur_s;
        if (delta <= 0 && delta > -2)
            return 0u;
    }

    RTC_TimeTypeDef st = {0};
    st.Hours          = (uint8_t)hh;
    st.Minutes        = (uint8_t)mi;
    st.Seconds        = (uint8_t)ss;
    st.TimeFormat     = RTC_HOURFORMAT12_AM;
    st.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    st.StoreOperation = RTC_STOREOPERATION_RESET;
    if (HAL_RTC_SetTime(&hrtc, &st, RTC_FORMAT_BIN) != HAL_OK)
        return 0u;

    RTC_DateTypeDef sd = {0};
    {
        int y = (int)yy, m = (int)mo, d2 = (int)dd;
        if (m < 3) { m += 12; y -= 1; }
        int k = y % 100, j = y / 100;
        int h = (d2 + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
        sd.WeekDay = (uint8_t)(((h + 5) % 7) + 1);
    }
    sd.Month = (uint8_t)mo;
    sd.Date  = (uint8_t)dd;
    sd.Year  = (uint8_t)(yy - 2000u);
    if (HAL_RTC_SetDate(&hrtc, &sd, RTC_FORMAT_BIN) != HAL_OK)
        return 0u;

    DATETIME chk;
    if (rtc_read_raw(&chk) != HAL_OK)
        return 0u;
    if (chk.year != sd.Year || chk.month != sd.Month || chk.date != sd.Date)
        return 0u;
    {
        int32_t want = (int32_t)hh * 3600 + (int32_t)mi * 60 + (int32_t)ss;
        int32_t got  = (int32_t)chk.hour * 3600 + (int32_t)chk.minute * 60 + (int32_t)chk.second;
        if (got - want < 0 || got - want > 2)
            return 0u;
    }

    return 1u;
}
