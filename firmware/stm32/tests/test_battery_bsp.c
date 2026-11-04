/* Exercise the real bit-banged BSP against an I2C GPIO slave model. No STM32
 * hardware, battery configuration writes, or driver internals are mocked. */
#include "BSP_Battery.h"
#include "BSP_SysTick.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

GPIO_TypeDef test_gpioa = { 0U }, test_gpiob = { 1U };
static unsigned scl, sda, awake, active, clocks, byte_number, shift;
static unsigned read_address, nack_now, starts, stops, attempts;
static unsigned nack_stage, nack_remaining, scl_stuck, sda_recovery_left;
static unsigned recovery_pulses, asleep_at_clock, wake_after_low_read;
static unsigned cleanup_scl_fault, delayed_us, gpio_reads;
static unsigned master_ack_mask, master_ack_count, last_address;
static const uint8_t slave_value = 0xa5U;

static unsigned physical_scl(void) { return scl && !scl_stuck; }
static unsigned physical_sda(void)
{
    if (!sda || sda_recovery_left) { return 0U; }
    if (!active) { return 1U; }
    if (read_address && clocks >= 10U && (clocks - 10U) % 9U < 8U) {
        unsigned index = (clocks - 10U) / 9U;
        unsigned bit = (clocks - 10U) % 9U;
        return ((slave_value ^ (index * 17U)) >> (7U - bit)) & 1U;
    }
    if (clocks != 0U && clocks % 9U == 0U && !(read_address && clocks >= 18U)) {
        return nack_now;
    }
    return 1U;
}

static void rising_scl(void)
{
    unsigned phase;
    if (!active) {
        if (sda_recovery_left) { --sda_recovery_left; ++recovery_pulses; }
        return;
    }
    ++clocks;
    if (asleep_at_clock && clocks == asleep_at_clock) { awake = 0U; }
    if (read_address && clocks > 9U) {
        if (clocks % 9U == 0U) {
            if (!sda) { master_ack_mask |= (1U << master_ack_count); }
            ++master_ack_count;
        }
        return;
    }
    if (clocks % 9U) {
        shift = ((shift << 1) | sda) & 0xffU;
        return;
    }
    phase = byte_number == 0U ? ((shift & 1U) ? BSP_BATTERY_STAGE_ADDRESS_READ : BSP_BATTERY_STAGE_ADDRESS_WRITE) :
        byte_number == 1U ? BSP_BATTERY_STAGE_REGISTER : BSP_BATTERY_STAGE_DATA_WRITE;
    nack_now = 0U;
    if (phase == nack_stage && nack_remaining) { nack_now = 1U; --nack_remaining; }
    if (byte_number == 0U) {
        last_address = shift;
        if (shift & 1U) { read_address = 1U; }
    }
    ++byte_number;
    shift = 0U;
}

void GPIO_ResetBits(GPIO_TypeDef *port, uint16_t pins)
{
    assert(port == GPIOB);
    if (pins & GPIO_Pin_10) {
        if (cleanup_scl_fault && active && clocks % 9U == 0U && nack_now) { scl_stuck = 1U; }
        scl = 0U;
    }
    if (pins & GPIO_Pin_11) {
        if (sda && physical_scl() && !sda_recovery_left) {
            ++starts;
            if (!active) { ++attempts; }
            active = 1U; clocks = 0U; byte_number = 0U; shift = 0U; read_address = 0U; nack_now = 0U;
        }
        sda = 0U;
    }
}

void GPIO_SetBits(GPIO_TypeDef *port, uint16_t pins)
{
    unsigned before_scl = physical_scl();
    assert(port == GPIOB);
    if (pins & GPIO_Pin_10) { scl = 1U; }
    if (!before_scl && physical_scl()) { rising_scl(); }
    if (pins & GPIO_Pin_11) {
        if (!sda && physical_scl() && !sda_recovery_left) { ++stops; active = 0U; }
        sda = 1U;
    }
}

uint8_t GPIO_ReadInputDataBit(GPIO_TypeDef *port, uint16_t pin)
{
    ++gpio_reads;
    assert(gpio_reads < 40000U);
    if (port == GPIOA && pin == GPIO_Pin_15) {
        unsigned value = awake;
        if (!value && wake_after_low_read) { awake = 1U; wake_after_low_read = 0U; }
        return (uint8_t)value;
    }
    assert(port == GPIOB);
    return (uint8_t)(pin == GPIO_Pin_10 ? physical_scl() : physical_sda());
}

void GPIO_StructInit(GPIO_InitTypeDef *gpio) { memset(gpio, 0, sizeof(*gpio)); }
void GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *gpio) { (void)port; (void)gpio; }
void RCC_APB2PeriphClockCmd(uint32_t clock, int enable) { (void)clock; (void)enable; }
void GPIO_PinRemapConfig(uint32_t mapping, int enable) { (void)mapping; (void)enable; }
void Delay_us(uint32_t us) { delayed_us += us; assert(delayed_us < 20000U); }
uint32_t BSP_Millis(void) { return 1234U + delayed_us / 1000U; }

static void reset_model(void)
{
    scl = sda = awake = 1U;
    active = clocks = byte_number = shift = read_address = nack_now = starts = stops = attempts = 0U;
    nack_stage = nack_remaining = scl_stuck = sda_recovery_left = recovery_pulses = asleep_at_clock = 0U;
    wake_after_low_read = cleanup_scl_fault = delayed_us = gpio_reads = 0U;
    master_ack_mask = master_ack_count = last_address = 0U;
    BSP_Battery_Init();
}

static BSP_Battery_IoDiagnostics diagnostics(void)
{
    BSP_Battery_IoDiagnostics result;
    BSP_Battery_GetIoDiagnostics(&result);
    return result;
}

static void test_success_and_invalid_argument(void)
{
    uint8_t value = 0U;
    BSP_Battery_IoDiagnostics d;
    reset_model();
    assert(BSP_Battery_ReadReg(0x70U, &value) == 0 && value == slave_value);
    assert(attempts == 1U && starts == 2U && stops == 1U);
    assert(BSP_Battery_WriteReg(0x26U, 5U) == 0);
    d = diagnostics();
    assert(d.transactions == 2U && d.failures == 0U && d.last_result == 0);
    assert(BSP_Battery_ReadReg(0x70U, NULL) == BSP_BATTERY_IO_ARGUMENT);
    d = diagnostics();
    assert(d.retries == 0U && d.first_error == BSP_BATTERY_IO_ARGUMENT);
    assert(attempts == 2U);
    BSP_Battery_GetIoDiagnostics(NULL);
}

static void test_transient_nack_retry(void)
{
    uint8_t value = 0U;
    BSP_Battery_IoDiagnostics d;
    reset_model();
    nack_stage = BSP_BATTERY_STAGE_ADDRESS_WRITE; nack_remaining = 1U;
    assert(BSP_Battery_ReadReg(0x70U, &value) == 0 && value == slave_value);
    d = diagnostics();
    assert(d.transactions == 1U && d.failures == 1U && d.retries == 1U && d.recovered == 1U);
    assert(d.last_result == 0 && d.first_error == BSP_BATTERY_IO_NACK && d.last_error == BSP_BATTERY_IO_NACK);
    assert(d.first_stage == BSP_BATTERY_STAGE_ADDRESS_WRITE && d.first_reg == 0x70U && d.first_write == 0U);
    assert(d.first_pins == 7U && d.first_error_ms >= 1234U);
    assert(attempts == 2U && delayed_us >= 1000U);
    assert(BSP_Battery_ReadReg(0x71U, &value) == 0);
    d = diagnostics();
    assert(d.first_reg == 0x70U && d.last_reg == 0x70U && d.failures == 1U);
}

static void test_persistent_nack_and_stages(void)
{
    unsigned i;
    static const unsigned stages[] = { BSP_BATTERY_STAGE_ADDRESS_WRITE, BSP_BATTERY_STAGE_REGISTER, BSP_BATTERY_STAGE_ADDRESS_READ };
    for (i = 0U; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        uint8_t value = 0x5aU;
        BSP_Battery_IoDiagnostics d;
        reset_model(); nack_stage = stages[i]; nack_remaining = 20U;
        assert(BSP_Battery_ReadReg(0x74U, &value) == BSP_BATTERY_IO_NACK);
        d = diagnostics();
        assert(value == 0x5aU && attempts == 2U && d.failures == 2U && d.retries == 1U && d.recovered == 0U);
        assert(d.first_stage == stages[i] && d.last_stage == stages[i] && d.last_result == BSP_BATTERY_IO_NACK);
    }
}

static void test_awake_loss_no_retry(void)
{
    uint8_t value = 0x5aU;
    BSP_Battery_IoDiagnostics d;
    reset_model(); asleep_at_clock = 5U;
    assert(BSP_Battery_ReadReg(0x70U, &value) == BSP_BATTERY_IO_ASLEEP);
    d = diagnostics();
    assert(value == 0x5aU && attempts == 1U && d.failures == 1U && d.retries == 0U);
    assert(d.first_error == BSP_BATTERY_IO_ASLEEP && !(d.first_pins & 4U));
    reset_model(); asleep_at_clock = 5U; wake_after_low_read = 1U;
    assert(BSP_Battery_ReadReg(0x70U, &value) == BSP_BATTERY_IO_ASLEEP);
    d = diagnostics();
    assert(awake && d.retries == 0U && attempts == 1U);
    reset_model(); awake = 0U;
    assert(BSP_Battery_ReadReg(0x70U, &value) == BSP_BATTERY_IO_ASLEEP);
    assert(attempts == 0U && diagnostics().retries == 0U);
}

static void test_scl_timeout_is_bounded(void)
{
    uint8_t value = 0x5aU;
    BSP_Battery_IoDiagnostics d;
    reset_model(); scl_stuck = 1U;
    assert(BSP_Battery_ReadReg(0x70U, &value) == BSP_BATTERY_IO_SCL_TIMEOUT);
    d = diagnostics();
    assert(value == 0x5aU && d.retries == 1U && d.failures == 2U);
    assert(d.first_stage == BSP_BATTERY_STAGE_START && !(d.first_pins & 1U));
    assert(delayed_us >= 5000U && delayed_us <= 5100U);
}

static void test_cleanup_sleep_edge_blocks_retry(void)
{
    uint8_t value = 0x5aU;
    BSP_Battery_IoDiagnostics d;
    reset_model();
    nack_stage = BSP_BATTERY_STAGE_ADDRESS_WRITE; nack_remaining = 1U;
    asleep_at_clock = 10U; wake_after_low_read = 1U;
    assert(BSP_Battery_ReadReg(0x70U, &value) == BSP_BATTERY_IO_ASLEEP);
    d = diagnostics();
    assert(awake && value == 0x5aU && attempts == 1U && d.retries == 0U);
    assert(d.last_result == BSP_BATTERY_IO_ASLEEP && d.first_error == BSP_BATTERY_IO_NACK);
    assert(d.first_stage == BSP_BATTERY_STAGE_ADDRESS_WRITE && d.first_pins == 7U);
}

static void test_sda_nine_clock_recovery(void)
{
    uint8_t value = 0U;
    reset_model(); sda_recovery_left = 9U;
    assert(BSP_Battery_ReadReg(0x70U, &value) == 0 && value == slave_value);
    assert(recovery_pulses == 9U && attempts == 1U && diagnostics().retries == 0U);
}

static void test_write_not_replayed_and_cleanup_preserves_error(void)
{
    unsigned i;
    static const unsigned stages[] = { BSP_BATTERY_STAGE_ADDRESS_WRITE, BSP_BATTERY_STAGE_REGISTER, BSP_BATTERY_STAGE_DATA_WRITE };
    for (i = 0U; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        BSP_Battery_IoDiagnostics d;
        reset_model(); nack_stage = stages[i]; nack_remaining = 1U; cleanup_scl_fault = 1U;
        assert(BSP_Battery_WriteReg(0x26U, 5U) == BSP_BATTERY_IO_NACK);
        d = diagnostics();
        assert(attempts == 1U && d.failures == 1U && d.retries == 0U && d.recovered == 0U);
        assert(d.first_error == BSP_BATTERY_IO_NACK && d.last_error == BSP_BATTERY_IO_NACK);
        assert(d.first_stage == stages[i] && d.first_pins == 7U && d.first_write == 1U);
        assert(scl_stuck && delayed_us >= 1000U);
    }
}

static void test_auxiliary_sensor_transactions(void)
{
    uint8_t data[7], i;
    static const uint8_t cmd[3] = { 0xacU, 0x33U, 0U };
    BSP_Battery_IoDiagnostics before, after;
    reset_model();
    before = diagnostics();
    assert(BSP_Battery_Aht20Transfer(cmd, 3U, NULL, 0U) == 0);
    assert(last_address == 0x70U && starts == 1U && stops == 1U);
    assert(BSP_Battery_Aht20Transfer(NULL, 0U, data, 7U) == 0);
    assert(last_address == 0x71U && master_ack_count == 7U && master_ack_mask == 0x3fU);
    for (i = 0U; i < 7U; ++i) { assert(data[i] == (uint8_t)(slave_value ^ (i * 17U))); }
    after = diagnostics();
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    assert(BSP_Battery_Aht20Transfer(NULL, 0U, data, 8U) == BSP_BATTERY_IO_ARGUMENT);
    assert(BSP_Battery_Aht20Transfer(cmd, 3U, data, 7U) == BSP_BATTERY_IO_ARGUMENT);

    reset_model(); memset(data, 0x5a, sizeof(data));
    asleep_at_clock = 30U;
    assert(BSP_Battery_Aht20Transfer(NULL, 0U, data, 7U) == BSP_BATTERY_IO_ASLEEP);
    for (i = 0U; i < 7U; ++i) { assert(data[i] == 0x5aU); }
    assert(diagnostics().failures == 0U && diagnostics().transactions == 0U);
    reset_model(); awake = 0U;
    assert(BSP_Battery_Aht20Transfer(cmd, 3U, NULL, 0U) == BSP_BATTERY_IO_ASLEEP);
    assert(starts == 0U && delayed_us == 0U);
    reset_model(); nack_stage = BSP_BATTERY_STAGE_ADDRESS_READ; nack_remaining = 1U;
    assert(BSP_Battery_Aht20Transfer(NULL, 0U, data, 7U) == BSP_BATTERY_IO_NACK);
    assert(attempts == 1U && diagnostics().failures == 0U);
    /* An auxiliary fault does not poison the following battery transaction. */
    assert(BSP_Battery_ReadReg(0x70U, &data[0]) == 0 && data[0] == slave_value);
    assert(diagnostics().transactions == 1U && diagnostics().failures == 0U);
}

int main(void)
{
    test_success_and_invalid_argument();
    test_transient_nack_retry();
    test_persistent_nack_and_stages();
    test_awake_loss_no_retry();
    test_scl_timeout_is_bounded();
    test_cleanup_sleep_edge_blocks_retry();
    test_sda_nine_clock_recovery();
    test_write_not_replayed_and_cleanup_preserves_error();
    test_auxiliary_sensor_transactions();
    puts("BSP battery I2C tests passed: retry, persistent faults, sleep, timeout, recovery, write and diagnostics.");
    return 0;
}
