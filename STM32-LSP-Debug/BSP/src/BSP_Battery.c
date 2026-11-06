#include "BSP_Battery.h"
#include "BSP_SysTick.h"
#include "Battery_Config.h"
#include <string.h>

#define BATTERY_I2C_PORT          GPIOB
#define BATTERY_I2C_SCL_PIN       GPIO_Pin_10
#define BATTERY_I2C_SDA_PIN       GPIO_Pin_11
#define BATTERY_AWAKE_PORT        GPIOA
#define BATTERY_AWAKE_PIN         GPIO_Pin_15
#define BATTERY_I2C_ADDRESS       0x75U
#define BATTERY_I2C_HALF_US       5UL
#define BATTERY_I2C_TIMEOUT_US    1000UL
#define BATTERY_I2C_RETRY_US      1000UL

static uint8_t battery_bsp_ready;
static uint8_t battery_sleep_seen;
static BSP_Battery_IoDiagnostics battery_io;
static struct {
    int error;
    uint32_t error_ms;
    uint8_t stage, error_stage, reg, writing, pins;
} battery_attempt;
#if BATTERY_TEMP_ADC_CONNECTED
static uint8_t battery_adc_ready;
#endif

static int Battery_Fail(int error)
{
    uint8_t awake = BSP_Battery_IsAwake();
    /* A sleep edge during cleanup also invalidates the whole call, even if
     * the pin has already risen again. Keep the earlier diagnostic cause. */
    if (error == BSP_BATTERY_IO_ASLEEP || (!awake && error != BSP_BATTERY_IO_ARGUMENT))
    {
        battery_sleep_seen = 1U;
    }
    if (battery_attempt.error == BSP_BATTERY_IO_OK)
    {
        /* Preserve the original levels before STOP/release changes them. */
        battery_attempt.pins = (uint8_t)(
            (GPIO_ReadInputDataBit(BATTERY_I2C_PORT, BATTERY_I2C_SCL_PIN) == Bit_SET ? 1U : 0U) |
            (GPIO_ReadInputDataBit(BATTERY_I2C_PORT, BATTERY_I2C_SDA_PIN) == Bit_SET ? 2U : 0U) |
            (awake ? 4U : 0U));
        battery_attempt.error = (!awake && error != BSP_BATTERY_IO_ARGUMENT) ?
            BSP_BATTERY_IO_ASLEEP : error;
        battery_attempt.error_stage = battery_attempt.stage;
        battery_attempt.error_ms = BSP_Millis();
    }
    return battery_attempt.error;
}

static void Battery_RecordFailure(void)
{
    ++battery_io.failures;
    battery_io.last_error = battery_attempt.error;
    battery_io.last_stage = battery_attempt.error_stage;
    battery_io.last_reg = battery_attempt.reg;
    battery_io.last_write = battery_attempt.writing;
    battery_io.last_pins = battery_attempt.pins;
    battery_io.last_error_ms = battery_attempt.error_ms;
    if (battery_io.first_error == BSP_BATTERY_IO_OK)
    {
        battery_io.first_error = battery_io.last_error;
        battery_io.first_stage = battery_io.last_stage;
        battery_io.first_reg = battery_io.last_reg;
        battery_io.first_write = battery_io.last_write;
        battery_io.first_pins = battery_io.last_pins;
        battery_io.first_error_ms = battery_io.last_error_ms;
    }
}

static void Battery_BeginAttempt(uint8_t reg, uint8_t writing)
{
    memset(&battery_attempt, 0, sizeof(battery_attempt));
    battery_attempt.reg = reg;
    battery_attempt.writing = writing;
}

void BSP_Battery_GetIoDiagnostics(BSP_Battery_IoDiagnostics *out)
{
    if (out != 0)
    {
        *out = battery_io;
    }
}

static void Battery_SclLow(void)
{
    GPIO_ResetBits(BATTERY_I2C_PORT, BATTERY_I2C_SCL_PIN);
}

static void Battery_SdaLow(void)
{
    GPIO_ResetBits(BATTERY_I2C_PORT, BATTERY_I2C_SDA_PIN);
}

static void Battery_SdaRelease(void)
{
    GPIO_SetBits(BATTERY_I2C_PORT, BATTERY_I2C_SDA_PIN);
}

static int Battery_SclRelease(void)
{
    uint32_t remaining = BATTERY_I2C_TIMEOUT_US;
    if (BSP_Battery_IsAwake() == 0U)
    {
        return Battery_Fail(BSP_BATTERY_IO_ASLEEP);
    }
    GPIO_SetBits(BATTERY_I2C_PORT, BATTERY_I2C_SCL_PIN);
    while (GPIO_ReadInputDataBit(BATTERY_I2C_PORT, BATTERY_I2C_SCL_PIN) == Bit_RESET)
    {
        if (BSP_Battery_IsAwake() == 0U)
        {
            return Battery_Fail(BSP_BATTERY_IO_ASLEEP);
        }
        if (remaining == 0UL)
        {
            return Battery_Fail(BSP_BATTERY_IO_SCL_TIMEOUT);
        }
        --remaining;
        Delay_us(1UL);
    }
    return BSP_Battery_IsAwake() ? BSP_BATTERY_IO_OK : Battery_Fail(BSP_BATTERY_IO_ASLEEP);
}

void BSP_Battery_ReleaseBus(void)
{
    if (battery_bsp_ready != 0U)
    {
        /* GPIO_Mode_Out_OD: a one releases the pin to the external pull-up. */
        GPIO_SetBits(BATTERY_I2C_PORT, BATTERY_I2C_SCL_PIN | BATTERY_I2C_SDA_PIN);
    }
}

uint8_t BSP_Battery_IsAwake(void)
{
    if (battery_bsp_ready == 0U)
    {
        return 0U;
    }
    return (GPIO_ReadInputDataBit(BATTERY_AWAKE_PORT, BATTERY_AWAKE_PIN) == Bit_SET) ? 1U : 0U;
}

static int Battery_Stop(void)
{
    battery_attempt.stage = BSP_BATTERY_STAGE_STOP;
    if (BSP_Battery_IsAwake() == 0U)
    {
        return Battery_Fail(BSP_BATTERY_IO_ASLEEP);
    }
    Battery_SclLow();
    Battery_SdaLow();
    Delay_us(BATTERY_I2C_HALF_US);
    if (Battery_SclRelease() != 0)
    {
        return battery_attempt.error;
    }
    Delay_us(BATTERY_I2C_HALF_US);
    Battery_SdaRelease();
    Delay_us(BATTERY_I2C_HALF_US);
    if (BSP_Battery_IsAwake() == 0U)
    {
        return Battery_Fail(BSP_BATTERY_IO_ASLEEP);
    }
    return (GPIO_ReadInputDataBit(BATTERY_I2C_PORT, BATTERY_I2C_SDA_PIN) == Bit_SET) ?
        BSP_BATTERY_IO_OK : Battery_Fail(BSP_BATTERY_IO_STOP);
}

static int Battery_RecoverBus(void)
{
    uint8_t pulse;
    battery_attempt.stage = BSP_BATTERY_STAGE_RECOVERY;
    Battery_SdaRelease();
    for (pulse = 0U; pulse < 9U; ++pulse)
    {
        Battery_SclLow();
        Delay_us(BATTERY_I2C_HALF_US);
        if (Battery_SclRelease() != 0)
        {
            return battery_attempt.error;
        }
        Delay_us(BATTERY_I2C_HALF_US);
    }
    return Battery_Stop();
}

static int Battery_Start(uint8_t repeated)
{
    uint8_t stage = repeated ? BSP_BATTERY_STAGE_RESTART : BSP_BATTERY_STAGE_START;
    battery_attempt.stage = stage;
    Battery_SdaRelease();
    Delay_us(BATTERY_I2C_HALF_US);
    if (Battery_SclRelease() != 0)
    {
        return battery_attempt.error;
    }
    Delay_us(BATTERY_I2C_HALF_US);
    if (GPIO_ReadInputDataBit(BATTERY_I2C_PORT, BATTERY_I2C_SDA_PIN) == Bit_RESET)
    {
        if (repeated != 0U)
        {
            return Battery_Fail(BSP_BATTERY_IO_SDA_STUCK);
        }
        if (Battery_RecoverBus() != 0)
        {
            return battery_attempt.error;
        }
        battery_attempt.stage = stage;
    }
    Battery_SdaLow();
    Delay_us(BATTERY_I2C_HALF_US);
    Battery_SclLow();
    return 0;
}

static int Battery_WriteByte(uint8_t value)
{
    uint8_t bit;
    uint8_t nack;
    for (bit = 0U; bit < 8U; ++bit)
    {
        if ((value & 0x80U) != 0U)
        {
            Battery_SdaRelease();
        }
        else
        {
            Battery_SdaLow();
        }
        Delay_us(BATTERY_I2C_HALF_US);
        if (Battery_SclRelease() != 0)
        {
            return battery_attempt.error;
        }
        Delay_us(BATTERY_I2C_HALF_US);
        Battery_SclLow();
        value = (uint8_t)(value << 1);
    }
    Battery_SdaRelease();
    Delay_us(BATTERY_I2C_HALF_US);
    if (Battery_SclRelease() != 0)
    {
        return battery_attempt.error;
    }
    Delay_us(BATTERY_I2C_HALF_US);
    nack = (uint8_t)GPIO_ReadInputDataBit(BATTERY_I2C_PORT, BATTERY_I2C_SDA_PIN);
    if (nack != 0U)
    {
        /* Diagnose the ACK phase while SCL is still physically HIGH. */
        (void)Battery_Fail(BSP_BATTERY_IO_NACK);
    }
    Battery_SclLow();
    return (nack == 0U) ? BSP_BATTERY_IO_OK : battery_attempt.error;
}

static int Battery_ReadByte(uint8_t *value, uint8_t acknowledge)
{
    uint8_t bit;
    uint8_t data = 0U;
    Battery_SdaRelease();
    for (bit = 0U; bit < 8U; ++bit)
    {
        Delay_us(BATTERY_I2C_HALF_US);
        if (Battery_SclRelease() != 0)
        {
            return battery_attempt.error;
        }
        Delay_us(BATTERY_I2C_HALF_US);
        data = (uint8_t)(data << 1);
        if (GPIO_ReadInputDataBit(BATTERY_I2C_PORT, BATTERY_I2C_SDA_PIN) == Bit_SET)
        {
            data |= 1U;
        }
        Battery_SclLow();
    }
    /* ACK intermediate bytes; NACK the final byte before STOP. */
    if (acknowledge) { Battery_SdaLow(); }
    else { Battery_SdaRelease(); }
    Delay_us(BATTERY_I2C_HALF_US);
    if (Battery_SclRelease() != 0)
    {
        return battery_attempt.error;
    }
    Delay_us(BATTERY_I2C_HALF_US);
    Battery_SclLow();
    *value = data;
    return 0;
}

static int Battery_ReadAttempt(uint8_t reg, uint8_t *value)
{
    uint8_t data;
    Battery_BeginAttempt(reg, 0U);
    if (value == 0)
    {
        (void)Battery_Fail(BSP_BATTERY_IO_ARGUMENT);
        goto failed;
    }
    if (BSP_Battery_IsAwake() == 0U)
    {
        (void)Battery_Fail(BSP_BATTERY_IO_ASLEEP);
        goto failed;
    }
    if (Battery_Start(0U) != 0) { goto failed; }
    battery_attempt.stage = BSP_BATTERY_STAGE_ADDRESS_WRITE;
    if (Battery_WriteByte((uint8_t)(BATTERY_I2C_ADDRESS << 1)) != 0) { goto failed; }
    battery_attempt.stage = BSP_BATTERY_STAGE_REGISTER;
    if (Battery_WriteByte(reg) != 0) { goto failed; }
    if (Battery_Start(1U) != 0) { goto failed; }
    battery_attempt.stage = BSP_BATTERY_STAGE_ADDRESS_READ;
    if (Battery_WriteByte((uint8_t)((BATTERY_I2C_ADDRESS << 1) | 1U)) != 0) { goto failed; }
    battery_attempt.stage = BSP_BATTERY_STAGE_DATA_READ;
    if (Battery_ReadByte(&data, 0U) != 0) { goto failed; }
    if (Battery_Stop() != 0) { goto failed; }
    *value = data;
    return BSP_BATTERY_IO_OK;

failed:
    Battery_RecordFailure();
    if (battery_attempt.error != BSP_BATTERY_IO_ASLEEP &&
        battery_attempt.error != BSP_BATTERY_IO_ARGUMENT)
    {
        (void)Battery_Stop();
    }
    BSP_Battery_ReleaseBus();
    return battery_attempt.error;
}

int BSP_Battery_ReadReg(uint8_t reg, uint8_t *value)
{
    int result;
    ++battery_io.transactions;
    battery_sleep_seen = 0U;
    result = Battery_ReadAttempt(reg, value);
    if (battery_sleep_seen) { result = BSP_BATTERY_IO_ASLEEP; }
    if (result != BSP_BATTERY_IO_OK && result != BSP_BATTERY_IO_ASLEEP &&
        result != BSP_BATTERY_IO_ARGUMENT && BSP_Battery_IsAwake() != 0U)
    {
        ++battery_io.retries;
        /* One bounded, read-only retry. START retains the 9-clock bus clear
         * if SDA is held low; no writes or charge-enables are replayed here. */
        Delay_us(BATTERY_I2C_RETRY_US);
        result = Battery_ReadAttempt(reg, value);
        if (battery_sleep_seen) { result = BSP_BATTERY_IO_ASLEEP; }
        if (result == BSP_BATTERY_IO_OK) { ++battery_io.recovered; }
    }
    battery_io.last_result = result;
    return result;
}

int BSP_Battery_WriteReg(uint8_t reg, uint8_t value)
{
    ++battery_io.transactions;
    battery_sleep_seen = 0U;
    Battery_BeginAttempt(reg, 1U);
    if (BSP_Battery_IsAwake() == 0U)
    {
        (void)Battery_Fail(BSP_BATTERY_IO_ASLEEP);
        goto failed;
    }
    if (Battery_Start(0U) != 0) { goto failed; }
    battery_attempt.stage = BSP_BATTERY_STAGE_ADDRESS_WRITE;
    if (Battery_WriteByte((uint8_t)(BATTERY_I2C_ADDRESS << 1)) != 0) { goto failed; }
    battery_attempt.stage = BSP_BATTERY_STAGE_REGISTER;
    if (Battery_WriteByte(reg) != 0) { goto failed; }
    battery_attempt.stage = BSP_BATTERY_STAGE_DATA_WRITE;
    if (Battery_WriteByte(value) != 0) { goto failed; }
    if (Battery_Stop() != 0) { goto failed; }
    battery_io.last_result = BSP_BATTERY_IO_OK;
    return BSP_BATTERY_IO_OK;

failed:
    Battery_RecordFailure();
    if (battery_attempt.error != BSP_BATTERY_IO_ASLEEP)
    {
        (void)Battery_Stop();
    }
    BSP_Battery_ReleaseBus();
    battery_io.last_result = battery_sleep_seen ? BSP_BATTERY_IO_ASLEEP : battery_attempt.error;
    return battery_io.last_result;
}

#if BATTERY_TEMP_ADC_CONNECTED
static void Battery_TemperatureAdcInit(void)
{
    GPIO_InitTypeDef gpio;
    ADC_InitTypeDef adc;
    uint32_t remaining;

    battery_adc_ready = 0U;
    RCC_APB2PeriphClockCmd(BATTERY_TEMP_ADC_GPIO_CLOCK | RCC_APB2Periph_ADC1, ENABLE);
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = BATTERY_TEMP_ADC_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(BATTERY_TEMP_ADC_GPIO_PORT, &gpio);
    ADC_DeInit(ADC1);
    ADC_StructInit(&adc);
    adc.ADC_Mode = ADC_Mode_Independent;
    adc.ADC_ScanConvMode = DISABLE;
    adc.ADC_ContinuousConvMode = DISABLE;
    adc.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    adc.ADC_DataAlign = ADC_DataAlign_Right;
    adc.ADC_NbrOfChannel = 1U;
    ADC_Init(ADC1, &adc);
    ADC_RegularChannelConfig(ADC1, BATTERY_TEMP_ADC_CHANNEL, 1U, ADC_SampleTime_239Cycles5);
    ADC_Cmd(ADC1, ENABLE);
    Delay_us(10UL);
    ADC_ResetCalibration(ADC1);
    remaining = 10000UL;
    while (ADC_GetResetCalibrationStatus(ADC1) != RESET)
    {
        if (remaining-- == 0UL)
        {
            ADC_Cmd(ADC1, DISABLE);
            return;
        }
        Delay_us(1UL);
    }
    ADC_StartCalibration(ADC1);
    remaining = 10000UL;
    while (ADC_GetCalibrationStatus(ADC1) != RESET)
    {
        if (remaining-- == 0UL)
        {
            ADC_Cmd(ADC1, DISABLE);
            return;
        }
        Delay_us(1UL);
    }
    battery_adc_ready = 1U;
}
#endif

void BSP_Battery_Init(void)
{
    GPIO_InitTypeDef gpio;

    memset(&battery_io, 0, sizeof(battery_io));
    memset(&battery_attempt, 0, sizeof(battery_attempt));
    battery_sleep_seen = 0U;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);
    /* PA15 is JTAG TDI after reset; retain SWD on PA13/PA14. */
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = BATTERY_AWAKE_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(BATTERY_AWAKE_PORT, &gpio);

    /* Preload released levels before selecting open-drain output mode. */
    GPIO_SetBits(BATTERY_I2C_PORT, BATTERY_I2C_SCL_PIN | BATTERY_I2C_SDA_PIN);
    gpio.GPIO_Pin = BATTERY_I2C_SCL_PIN | BATTERY_I2C_SDA_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(BATTERY_I2C_PORT, &gpio);
    battery_bsp_ready = 1U;
    BSP_Battery_ReleaseBus();
#if BATTERY_TEMP_ADC_CONNECTED
    Battery_TemperatureAdcInit();
#endif
}

/* The same main-loop master also serves AHT20. Keep this entry point address
 * restricted: peripheral commands cannot access charger configuration. All
 * edges retain the L3 sleep guard. No transaction occurs while IP5310 sleeps,
 * even though the AHT20 itself still has 3.3 V. Auxiliary faults are returned to
 * AHT20, not added to the charger's diagnostic counters. Never call from ISR. */
int BSP_Battery_Aht20Transfer(const uint8_t *tx, uint8_t tx_length,
                            uint8_t *rx, uint8_t rx_length)
{
    uint8_t i;
    uint8_t data[7];
    int result;
    if ((tx_length == 0U && rx_length == 0U) ||
        (tx_length != 0U && rx_length != 0U) || tx_length > 3U || rx_length > 7U ||
        (tx_length != 0U && tx == 0) || (rx_length != 0U && rx == 0)) {
        return BSP_BATTERY_IO_ARGUMENT;
    }
    battery_sleep_seen = 0U;
    Battery_BeginAttempt(0U, tx_length != 0U);
    if (!BSP_Battery_IsAwake()) { return BSP_BATTERY_IO_ASLEEP; }
    if (Battery_Start(0U) != 0) { goto failed; }
    battery_attempt.stage = rx_length ? BSP_BATTERY_STAGE_ADDRESS_READ : BSP_BATTERY_STAGE_ADDRESS_WRITE;
    if (Battery_WriteByte(rx_length ? 0x71U : 0x70U) != 0) { goto failed; }
    battery_attempt.stage = rx_length ? BSP_BATTERY_STAGE_DATA_READ : BSP_BATTERY_STAGE_DATA_WRITE;
    for (i = 0U; i < tx_length; ++i) {
        if (Battery_WriteByte(tx[i]) != 0) { goto failed; }
    }
    for (i = 0U; i < rx_length; ++i) {
        if (Battery_ReadByte(&data[i], (uint8_t)(i + 1U < rx_length)) != 0) { goto failed; }
    }
    if (Battery_Stop() != 0) { goto failed; }
    if (battery_sleep_seen) { return BSP_BATTERY_IO_ASLEEP; }
    if (rx_length) { memcpy(rx, data, rx_length); }
    return BSP_BATTERY_IO_OK;
failed:
    result = battery_attempt.error;
    if (result != BSP_BATTERY_IO_ASLEEP) { (void)Battery_Stop(); }
    BSP_Battery_ReleaseBus();
    return battery_sleep_seen ? BSP_BATTERY_IO_ASLEEP : result;
}

int BSP_Battery_ReadTemperatureAdc(uint16_t *raw)
{
#if BATTERY_TEMP_ADC_CONNECTED
    uint8_t sample;
    uint16_t count;
    uint32_t sum = 0UL;
    uint32_t remaining;
    if ((raw == 0) || (battery_adc_ready == 0U))
    {
        return -1;
    }
    for (sample = 0U; sample < 8U; ++sample)
    {
        ADC_ClearFlag(ADC1, ADC_FLAG_EOC);
        ADC_SoftwareStartConvCmd(ADC1, ENABLE);
        remaining = 1000UL;
        while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET)
        {
            if (remaining-- == 0UL)
            {
                ADC_SoftwareStartConvCmd(ADC1, DISABLE);
                return -1;
            }
            Delay_us(1UL);
        }
        count = ADC_GetConversionValue(ADC1);
        /* Reject each rail sample before averaging intermittent probe faults. */
        if ((count <= 16U) || (count >= 4079U))
        {
            return -1;
        }
        sum += count;
    }
    *raw = (uint16_t)((sum + 4UL) / 8UL);
    return 0;
#else
    (void)raw;
    return -1;
#endif
}
