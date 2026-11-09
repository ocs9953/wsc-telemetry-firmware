#include "adc_sensor.h"
#include "freshness.h"
#include "main.h"

#define ADC_VREF_MV          3300u
#define ADC_FULL_SCALE       16383u
#define ADC_DIV_RTOP_OHM     470u
#define ADC_DIV_RBOT_OHM     910u

#define ADC_SAMPLE_PERIOD_MS 20u
#define ADC_POLL_TIMEOUT_MS  10u
#define ADC_NO_DATA          0xFFFFu

volatile uint32_t g_adc_vdda_mv = ADC_VREF_MV;

volatile uint16_t g_vbat_mv  = 0u;
volatile uint16_t g_vbat_raw = 0u;
volatile uint32_t g_vbat_seq = 0u;

#if PHASE9_ADC_SENSOR

#ifndef HAL_ADC_MODULE_ENABLED
#error "PHASE9_ADC_SENSOR 는 stm32u5xx_hal_conf.h 의 HAL_ADC_MODULE_ENABLED 가 필요하다 (CubeMX 재생성 시 꺼짐)"
#endif

extern VehicleData_t VehicleData;
extern volatile uint32_t g_init_fail_mask;

#define INIT_FAIL_ADC        0x4u

static ADC_HandleTypeDef hadc1;

volatile uint32_t g_adc_fail_cycles = 0;

volatile uint16_t g_adc_vrefint_raw = 0u;

#define ADC_SEQ_LEN      (ADC_POT_CHANNELS + 1u)
static const uint32_t adc_channel[ADC_SEQ_LEN] = { ADC_CHANNEL_3, ADC_CHANNEL_4, ADC_CHANNEL_VREFINT };

#define ADC_VBAT_PERIOD_MS   60000u
#define ADC_VBAT_DIVIDER     4u

volatile uint32_t g_vbat_fail = 0u;

static uint8_t ADC_VbatPath(uint8_t on)
{
    if (LL_ADC_IsEnabled(ADC1) != 0UL)
        return 0u;
    ADC_Common_TypeDef *common = __LL_ADC_COMMON_INSTANCE(ADC1);
    uint32_t path = LL_ADC_GetCommonPathInternalCh(common);
    path = on ? (path | LL_ADC_PATH_INTERNAL_VBAT) : (path & ~LL_ADC_PATH_INTERNAL_VBAT);
    LL_ADC_SetCommonPathInternalCh(common, path);
    return 1u;
}

_Static_assert(0xFFFFull * ADC_FULL_SCALE <= 0xFFFFFFFFull,
               "ADC sum can overflow at the count ceiling");

static uint8_t adc_clock_failed = 0u;

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1)
        return;

    RCC_PeriphCLKInitTypeDef clk = {0};
    clk.PeriphClockSelection = RCC_PERIPHCLK_ADCDAC;
    clk.AdcDacClockSelection = RCC_ADCDACCLKSOURCE_HSE;

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin  = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;

    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
    {
        adc_clock_failed = 1u;
    }
    else
    {
        __HAL_RCC_ADC12_CLK_ENABLE();
        __HAL_RCC_GPIOC_CLK_ENABLE();
        HAL_GPIO_Init(GPIOC, &gpio);
    }
    TX_RESTORE
}

static HAL_StatusTypeDef ADC_Sensor_Init(void)
{
    HAL_PWREx_EnableVddA();

    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_ASYNC_DIV1;
    hadc1.Init.Resolution            = ADC_RESOLUTION_14B;
    hadc1.Init.GainCompensation      = 0;
    hadc1.Init.ScanConvMode          = ADC_SCAN_ENABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait      = ENABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.NbrOfConversion       = ADC_SEQ_LEN     ;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    hadc1.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.LeftBitShift          = ADC_LEFTBITSHIFT_NONE;
    hadc1.Init.LowPowerAutoPowerOff  = ADC_LOW_POWER_NONE;
    hadc1.Init.TriggerFrequencyMode  = ADC_TRIGGER_FREQ_LOW;
    hadc1.Init.VrefProtection        = ADC_VREF_PPROT_NONE;
    hadc1.Init.OversamplingMode               = ENABLE;
    hadc1.Init.Oversampling.Ratio             = 15;
    hadc1.Init.Oversampling.RightBitShift     = ADC_RIGHTBITSHIFT_4;
    hadc1.Init.Oversampling.TriggeredMode     = ADC_TRIGGEREDMODE_SINGLE_TRIGGER;
    hadc1.Init.Oversampling.OversamplingStopReset = ADC_REGOVERSAMPLING_CONTINUED_MODE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK || adc_clock_failed)
        return HAL_ERROR;

    ADC_ChannelConfTypeDef ch = {0};
    ch.SingleDiff   = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    ch.Offset       = 0;
    ch.SamplingTime = ADC_SAMPLETIME_391CYCLES;
    static const uint32_t rank[ADC_SEQ_LEN] = { ADC_REGULAR_RANK_1, ADC_REGULAR_RANK_2, ADC_REGULAR_RANK_3 };
    for (uint8_t i = 0; i < ADC_SEQ_LEN; i++)
    {
        ch.Channel = adc_channel[i];
        ch.Rank    = rank[i];
        if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK)
            return HAL_ERROR;
    }

    ADC_InjectionConfTypeDef inj = {0};
    inj.InjectedChannel               = ADC_CHANNEL_VBAT;
    inj.InjectedRank                  = ADC_INJECTED_RANK_1;
    inj.InjectedSamplingTime          = ADC_SAMPLETIME_391CYCLES;
    inj.InjectedSingleDiff            = ADC_SINGLE_ENDED;
    inj.InjectedOffsetNumber          = ADC_OFFSET_NONE;
    inj.InjectedOffset                = 0;
    inj.InjectedLeftBitShift          = ADC_LEFTBITSHIFT_NONE;
    inj.InjectedNbrOfConversion       = 1;
    inj.InjectedDiscontinuousConvMode = DISABLE;
    inj.AutoInjectedConv              = DISABLE;
    inj.ExternalTrigInjecConv         = ADC_INJECTED_SOFTWARE_START;
    inj.ExternalTrigInjecConvEdge     = ADC_EXTERNALTRIGINJECCONV_EDGE_NONE;
    inj.InjecOversamplingMode         = DISABLE;
    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &inj) != HAL_OK)
        return HAL_ERROR;

    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
        return HAL_ERROR;

    ADC_VbatPath(0u);

    return HAL_OK;
}

static void ADC_Sensor_Accumulate(uint8_t idx, uint16_t raw)
{
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    AdcPotAcc_t *a = &VehicleData.adc_pot[idx];
    if (a->count == 0u)
    {
        a->min = raw;
        a->max = raw;
    }
    else
    {
        if (raw < a->min) a->min = raw;
        if (raw > a->max) a->max = raw;
    }
    if (a->count < 0xFFFFu)
    {
        a->sum += raw;
        a->count++;
    }
    a->last = raw;
    TX_RESTORE
}

static void ADC_Vbat_Measure(void)
{
    if (!ADC_VbatPath(1u))
    {
        g_vbat_fail++;
        return;
    }

    uint8_t  ok  = 0u;
    uint16_t raw = 0u;
    if (HAL_ADCEx_InjectedStart(&hadc1) == HAL_OK)
    {
        if (HAL_ADCEx_InjectedPollForConversion(&hadc1, ADC_POLL_TIMEOUT_MS) == HAL_OK)
        {
            raw = (uint16_t)HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_1);
            ok  = 1u;
        }
        (void)HAL_ADCEx_InjectedStop(&hadc1);
    }
    (void)ADC_VbatPath(0u);

    if (!ok)
    {
        g_vbat_fail++;
        return;
    }
    uint32_t mv = ((uint32_t)raw * g_adc_vdda_mv * ADC_VBAT_DIVIDER + ADC_FULL_SCALE / 2u)
                  / ADC_FULL_SCALE;
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    g_vbat_raw = raw;
    g_vbat_mv  = (uint16_t)(mv > 0xFFFFu ? 0xFFFFu : mv);
    g_vbat_seq++;
    TX_RESTORE
}

void ADC_Sensor_Thread_Entry(ULONG arg)
{
    (void)arg;

    if (ADC_Sensor_Init() != HAL_OK)
    {
        TX_INTERRUPT_SAVE_AREA
        TX_DISABLE
        g_init_fail_mask |= INIT_FAIL_ADC;
        TX_RESTORE
        HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
        for (;;) { tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND); }
    }

    const ULONG period = (ADC_SAMPLE_PERIOD_MS * TX_TIMER_TICKS_PER_SECOND) / 1000u;
    uint8_t  vbat_first = 1u;
    uint32_t vbat_last  = 0u;

    for (;;)
    {
        if (HAL_ADC_Start(&hadc1) == HAL_OK)
        {
            uint16_t raw[ADC_SEQ_LEN];
            uint8_t  got = 0u;
            while (got < ADC_SEQ_LEN &&
                   HAL_ADC_PollForConversion(&hadc1, ADC_POLL_TIMEOUT_MS) == HAL_OK)
            {
                raw[got] = (uint16_t)HAL_ADC_GetValue(&hadc1);
                got++;
            }
            (void)HAL_ADC_Stop(&hadc1);

            if (got == ADC_SEQ_LEN)
            {
                uint32_t now = HAL_GetTick();

                uint16_t vref_raw = raw[ADC_POT_CHANNELS];
                uint16_t vref_cal = *VREFINT_CAL_ADDR;
                if (vref_raw != 0u && vref_cal != 0u && vref_cal != 0xFFFFu)
                {
                    uint32_t vdda = __LL_ADC_CALC_VREFANALOG_VOLTAGE(ADC1, vref_raw,
                                                                    ADC_RESOLUTION_14B);
                    if (vdda > 2700u && vdda < 3600u)
                        g_adc_vdda_mv = vdda;
                }
                g_adc_vrefint_raw = vref_raw;
                for (uint8_t i = 0; i < ADC_POT_CHANNELS; i++)
                {
                    ADC_Sensor_Accumulate(i, raw[i]);
                    Fresh_Note(FRESH_FR_ADC_FIRST + i, now);
                }

                if (vbat_first || (now - vbat_last) >= ADC_VBAT_PERIOD_MS)
                {
                    vbat_first = 0u;
                    vbat_last  = now;
                    ADC_Vbat_Measure();
                }
            }
            else
            {
                g_adc_fail_cycles++;
            }
        }
        else
        {
            g_adc_fail_cycles++;
        }
        if ((LL_ADC_GetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(ADC1))
             & LL_ADC_PATH_INTERNAL_VBAT) != 0UL)
            (void)ADC_VbatPath(0u);
        tx_thread_sleep(period);
    }
}

#endif


static uint32_t ADC_RawToInputMv(uint32_t raw)
{
    uint64_t num = (uint64_t)raw * g_adc_vdda_mv * (ADC_DIV_RTOP_OHM + ADC_DIV_RBOT_OHM);
    uint64_t den = (uint64_t)ADC_FULL_SCALE * ADC_DIV_RBOT_OHM;
    return (uint32_t)((num + den / 2u) / den);
}

void ADC_Sensor_PackSummary(const AdcPotAcc_t *acc, uint8_t *out)
{
    uint16_t v[4];

    if (acc->count == 0u)
    {
        v[0] = v[1] = v[2] = v[3] = (uint16_t)ADC_NO_DATA;
    }
    else
    {
        uint32_t avg_raw = (acc->sum + acc->count / 2u) / acc->count;
        v[0] = (uint16_t)ADC_RawToInputMv(avg_raw);
        v[1] = (uint16_t)ADC_RawToInputMv(acc->min);
        v[2] = (uint16_t)ADC_RawToInputMv(acc->max);
        v[3] = acc->max;
    }

    for (uint8_t i = 0; i < 4u; i++)
    {
        out[i * 2u]      = (uint8_t)(v[i] & 0xFFu);
        out[i * 2u + 1u] = (uint8_t)(v[i] >> 8);
    }
}
