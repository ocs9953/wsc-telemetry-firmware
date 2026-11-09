
#include "gps.h"
#include "app_threadx.h"
#include "rtc_manager.h"
#include "packet_builder.h"
#include "timer_scheduler.h"
#include "tx_api.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart1;
extern TX_QUEUE UART_Queue;
extern TX_QUEUE SD_Queue;

#if PHASE8_GPS

#define GPS_LINE_MAX      96u

#define GPS_STALE_MS      5000u

static uint8_t  rx_byte;
static char     line_buf[GPS_LINE_MAX];
static uint16_t line_len = 0;
static uint8_t  line_ovf = 0;

static char     ready_buf[GPS_LINE_MAX];
static volatile uint8_t ready_flag = 0;

static TX_SEMAPHORE gps_sem;

static GPS_DATA gps_data;

volatile uint32_t g_gps_sentences = 0;
volatile uint32_t g_gps_csum_err  = 0;
volatile uint32_t g_gps_ovf       = 0;
volatile uint32_t g_gps_fix       = 0;
volatile uint32_t g_gps_uart_err  = 0;

volatile uint32_t g_gps_rearm_fail = 0;
volatile uint32_t g_gps_rearm_ok   = 0;

static volatile uint8_t rx_armed = 0;

static void gps_rx_arm(void)
{
    if (HAL_UART_Receive_IT(&huart1, &rx_byte, 1) == HAL_OK) {
        rx_armed = 1u;
        return;
    }

    if (huart1.RxState == HAL_UART_STATE_BUSY_RX) {
        rx_armed = 1u;
        return;
    }

    rx_armed = 0u;
    g_gps_rearm_fail++;
}


static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static uint16_t nmea_check(char *line, uint16_t len)
{
    if (len < 4u || line[0] != '$') return 0u;

    uint16_t star = 0u;
    for (uint16_t i = 1u; i < len; i++) {
        if (line[i] == '*') { star = i; break; }
    }
    if (star == 0u || (uint16_t)(star + 2u) >= len) return 0u;

    int hi = hex_val(line[star + 1u]);
    int lo = hex_val(line[star + 2u]);
    if (hi < 0 || lo < 0) return 0u;

    uint8_t want = (uint8_t)((hi << 4) | lo);
    uint8_t got  = 0u;
    for (uint16_t i = 1u; i < star; i++) got ^= (uint8_t)line[i];

    if (got != want) return 0u;

    line[star] = '\0';
    return star;
}

static const char *nmea_field(const char *line, uint8_t n)
{
    const char *p = line;
    for (uint8_t i = 0u; i < n; i++) {
        p = strchr(p, ',');
        if (p == NULL) return NULL;
        p++;
    }
    return p;
}

static int fld_empty(const char *p)
{
    return (p == NULL) || (*p == '\0') || (*p == ',');
}

static int64_t parse_scaled(const char *p, uint8_t scale)
{
    if (fld_empty(p)) return 0;

    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    int64_t ip = 0;
    while (*p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); p++; }

    int64_t fp = 0;
    uint8_t got = 0u;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (got < scale) { fp = fp * 10 + (*p - '0'); got++; }
            p++;
        }
    }
    while (got < scale) { fp *= 10; got++; }

    int64_t pow10 = 1;
    for (uint8_t i = 0u; i < scale; i++) pow10 *= 10;

    int64_t v = ip * pow10 + fp;
    return neg ? -v : v;
}

static int32_t nmea_coord(const char *p, const char *hemi)
{
    if (fld_empty(p)) return 0;

    int64_t raw = parse_scaled(p, 5);
    int64_t deg = raw / 10000000;
    int64_t min_1e5 = raw % 10000000;

    int64_t v = deg * 10000000 + (min_1e5 * 100) / 60;

    if (!fld_empty(hemi) && (*hemi == 'S' || *hemi == 'W')) v = -v;

    if (v >  1800000000) v =  1800000000;
    if (v < -1800000000) v = -1800000000;
    return (int32_t)v;
}


static void parse_gga(const char *line, GPS_DATA *d)
{
    if (fld_empty(nmea_field(line, 6))) return;

    uint8_t fix = (uint8_t)parse_scaled(nmea_field(line, 6), 0);
    d->fix  = fix;
    g_gps_fix     = fix;
    d->sats = (uint8_t)parse_scaled(nmea_field(line, 7), 0);

    if (fix == 0u) return;

    d->lat_1e7 = nmea_coord(nmea_field(line, 2), nmea_field(line, 3));
    d->lon_1e7 = nmea_coord(nmea_field(line, 4), nmea_field(line, 5));

    int64_t alt = parse_scaled(nmea_field(line, 9), 1);
    if (alt >  32767) alt =  32767;
    if (alt < -32768) alt = -32768;
    d->alt_dm = (int16_t)alt;

    d->utc_time = (uint32_t)parse_scaled(nmea_field(line, 1), 2);
    d->last_tick = HAL_GetTick();
}

static void parse_rmc(const char *line, GPS_DATA *d)
{
    const char *st = nmea_field(line, 2);
    if (fld_empty(st) || *st != 'A') return;

    int64_t kn = parse_scaled(nmea_field(line, 7), 3);
    int64_t cms = (kn * 51444) / 1000000;
    if (cms < 0) cms = 0;
    if (cms > 65535) cms = 65535;
    d->speed_cms = (uint16_t)cms;

    int64_t cog = parse_scaled(nmea_field(line, 8), 2);
    if (cog < 0) cog = 0;
    if (cog > 35999) cog = 35999;
    d->course_cdeg = (uint16_t)cog;

    const char *dt = nmea_field(line, 9);
    if (!fld_empty(dt)) {
        uint32_t ddmmyy = (uint32_t)parse_scaled(dt, 0);
        uint32_t dd = ddmmyy / 10000u;
        uint32_t mm = (ddmmyy / 100u) % 100u;
        uint32_t yy = ddmmyy % 100u;
        d->utc_date = (2000u + yy) * 10000u + mm * 100u + dd;
    }

    d->last_tick = HAL_GetTick();
}


void GPS_Init(void)
{
    memset(&gps_data, 0, sizeof(gps_data));
    line_len = 0u;
    line_ovf = 0u;
    ready_flag = 0u;

    tx_semaphore_create(&gps_sem, "GPS_Sem", 0);

    HAL_NVIC_SetPriority(USART1_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    {
        GPIO_InitTypeDef g = {0};
        g.Pin       = GPIO_PIN_7;
        g.Mode      = GPIO_MODE_AF_PP;
        g.Pull      = GPIO_PULLUP;
        g.Speed     = GPIO_SPEED_FREQ_LOW;
        g.Alternate = GPIO_AF7_USART1;
        HAL_GPIO_Init(GPIOB, &g);
    }

    gps_rx_arm();
}

void GPS_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) return;

    char c = (char)rx_byte;

    if (c == '\r') {
    } else if (c == '\n') {
        if (line_ovf) {
            line_ovf = 0u;
            g_gps_ovf++;
        } else if (line_len > 0u && ready_flag == 0u) {
            memcpy(ready_buf, line_buf, line_len);
            ready_buf[line_len] = '\0';
            ready_flag = 1u;
            tx_semaphore_ceiling_put(&gps_sem, 1);
        }
        line_len = 0u;
    } else if (c == '$') {
        line_len = 0u;
        line_ovf = 0u;
        line_buf[line_len++] = c;
    } else if (line_len > 0u) {
        if (line_len < (GPS_LINE_MAX - 1u)) {
            line_buf[line_len++] = c;
        } else {
            line_ovf = 1u;
        }
    }

    gps_rx_arm();
}

void GPS_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) return;

    line_len = 0u;
    line_ovf = 0u;
    g_gps_uart_err++;

    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_FEFLAG(&huart1);
    __HAL_UART_CLEAR_NEFLAG(&huart1);
    __HAL_UART_CLEAR_PEFLAG(&huart1);

    gps_rx_arm();
}


volatile uint32_t g_gps_rtc_sync   = 0;
volatile uint32_t g_gps_rtc_reject = 0;

#ifndef GPS_KEEP_ALL_SENTENCES
#define GPS_KEEP_ALL_SENTENCES  0
#endif

#if !GPS_KEEP_ALL_SENTENCES
#define GPS_CLEAN_MS            3000u
static uint32_t gps_last_unwanted = 0;
#endif

static uint8_t  rtc_done   = 0;
static uint32_t cand_time  = 0;
static uint32_t cand_date  = 0;
static uint32_t cand_tick  = 0;

#define RTC_EVT_UART  0x1u
#define RTC_EVT_SD    0x2u
static uint8_t  rtc_evt_pending = 0;
static ULONG    rtc_evt_msg[LOG_QUEUE_MSG_WORDS];

static void gps_rtc_event_flush(void)
{
    if (rtc_evt_pending & RTC_EVT_UART) {
        if (TelemQueue_Send(&UART_Queue, rtc_evt_msg) == TX_SUCCESS)
            rtc_evt_pending &= (uint8_t)~RTC_EVT_UART;
    }
    if (rtc_evt_pending & RTC_EVT_SD) {
        if (TelemQueue_Send(&SD_Queue, rtc_evt_msg) == TX_SUCCESS)
            rtc_evt_pending &= (uint8_t)~RTC_EVT_SD;
    }
}

static uint32_t gps_tod_sec(uint32_t t)
{
    return (t / 1000000u) * 3600u + ((t / 10000u) % 100u) * 60u + ((t / 100u) % 100u);
}

static void gps_try_rtc_sync(const GPS_DATA *d)
{
    gps_rtc_event_flush();

    if (rtc_done) return;
    if (d->fix == 0u || d->utc_date == 0u) return;

    uint32_t now = HAL_GetTick();

#if !GPS_KEEP_ALL_SENTENCES
    if ((now - gps_last_unwanted) < GPS_CLEAN_MS) { cand_date = 0u; return; }
#endif

    if (cand_date == 0u) {
        cand_time = d->utc_time;
        cand_date = d->utc_date;
        cand_tick = now;
        return;
    }
    if (d->utc_time == cand_time) return;

    if (d->utc_date != cand_date) { cand_date = 0u; return; }

    uint32_t s1 = gps_tod_sec(cand_time);
    uint32_t s2 = gps_tod_sec(d->utc_time);
    if (s2 <= s1) { cand_date = 0u; return; }

    uint32_t gps_dt = s2 - s1;
    uint32_t sys_dt = (now - cand_tick + 500u) / 1000u;
    uint32_t diff   = (gps_dt > sys_dt) ? (gps_dt - sys_dt) : (sys_dt - gps_dt);
    if (diff > 2u) { g_gps_rtc_reject++; cand_date = 0u; return; }

    DATETIME before;
    if (!RTC_Manager_SetFromGps(d->utc_time, d->utc_date, &before)) {
        g_gps_rtc_reject++;
        cand_date = 0u;
        return;
    }

    rtc_done       = 1u;
    g_gps_rtc_sync = 1u;

    {
        LOG pkt;
        uint8_t val[8];
        RTC_Manager_PackToValue(&before, val);
        PacketBuilder_Create(&pkt, LOG_INFO, SRC_SYS, (uint8_t)ERR_RTC_SYNCED, val, 8);
        memset(rtc_evt_msg, 0, sizeof(rtc_evt_msg));
        memcpy(rtc_evt_msg, &pkt, sizeof(LOG));
        rtc_evt_pending = RTC_EVT_UART | RTC_EVT_SD;
    }
    gps_rtc_event_flush();
}

#define GPS_CFG_RETRY_MS        5000u

volatile uint32_t g_gps_cfg_sent  = 0;
volatile uint32_t g_gps_unwanted  = 0;

#if !GPS_KEEP_ALL_SENTENCES
static const char *const gps_off_ids[] = { "GSV", "GSA", "GLL", "VTG" };
static uint32_t gps_cfg_tick = 0;
static uint8_t  gps_cfg_ever = 0;

static void gps_send_pubx40_off(const char *id)
{
    char body[40];
    char line[48];
    int n = snprintf(body, sizeof(body), "PUBX,40,%s,0,0,0,0,0,0", id);
    if (n <= 0 || n >= (int)sizeof(body)) return;
    uint8_t cs = 0u;
    for (int i = 0; i < n; i++) cs ^= (uint8_t)body[i];
    int m = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, cs);
    if (m <= 0 || m >= (int)sizeof(line)) return;
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)m, 100u);
}

static void gps_note_unwanted(void)
{
    g_gps_unwanted++;
    uint32_t now = HAL_GetTick();
    gps_last_unwanted = now;
    if (gps_cfg_ever && (now - gps_cfg_tick) < GPS_CFG_RETRY_MS) return;
    gps_cfg_ever = 1u;
    gps_cfg_tick = now;
    for (unsigned i = 0; i < sizeof(gps_off_ids) / sizeof(gps_off_ids[0]); i++)
        gps_send_pubx40_off(gps_off_ids[i]);
    g_gps_cfg_sent++;
}
#endif

void GPS_Thread_Entry(unsigned long thread_input)
{
    (void)thread_input;

    static char work[GPS_LINE_MAX];
    GPS_DATA d;

    d = gps_data;

    for (;;) {
        uint8_t emit = 0u;

        if (tx_semaphore_get(&gps_sem, 1000) == TX_SUCCESS && ready_flag) {
            TX_INTERRUPT_SAVE_AREA
            TX_DISABLE
            memcpy(work, ready_buf, sizeof(work));
            ready_flag = 0u;
            TX_RESTORE

            uint16_t len = (uint16_t)strnlen(work, GPS_LINE_MAX - 1u);
            uint16_t body = nmea_check(work, len);
            if (body == 0u) {
                g_gps_csum_err++;
                d.csum_err++;
            } else {
                g_gps_sentences++;
                d.sentences++;

                if (body >= 6u) {
                    const char *t = &work[3];
                    if (strncmp(t, "GGA", 3) == 0)      { parse_gga(work, &d); emit = 1u; }
                    else if (strncmp(t, "RMC", 3) == 0) parse_rmc(work, &d);
#if !GPS_KEEP_ALL_SENTENCES
                    else if (strncmp(t, "GSV", 3) == 0 || strncmp(t, "GSA", 3) == 0 ||
                             strncmp(t, "GLL", 3) == 0 || strncmp(t, "VTG", 3) == 0)
                        gps_note_unwanted();
#endif
                }
            }
        }

        if (d.fix != 0u && (HAL_GetTick() - d.last_tick) > GPS_STALE_MS) {
            d.fix     = 0u;
            g_gps_fix = 0u;
            emit      = 1u;
        }

        if (!rx_armed) {
            TX_INTERRUPT_SAVE_AREA
            TX_DISABLE
            gps_rx_arm();
            TX_RESTORE
            if (rx_armed) g_gps_rearm_ok++;
        }

        gps_try_rtc_sync(&d);

        {
            TX_INTERRUPT_SAVE_AREA
            TX_DISABLE
            gps_data = d;
            TX_RESTORE
        }

        if (emit)
            Telemetry_EmitGps();
    }
}

void GPS_GetSnapshot(GPS_DATA *out)
{
    if (out == NULL) return;

    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    *out = gps_data;
    TX_RESTORE
}

#else

volatile uint32_t g_gps_uart_err = 0;
void GPS_Init(void) { }
void GPS_RxCpltCallback(UART_HandleTypeDef *huart) { (void)huart; }
void GPS_ErrorCallback(UART_HandleTypeDef *huart)  { (void)huart; }
void GPS_GetSnapshot(GPS_DATA *out) { if (out) memset(out, 0, sizeof(*out)); }

void GPS_Thread_Entry(unsigned long thread_input)
{
    (void)thread_input;
    for (;;) { tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND); }
}

#endif
