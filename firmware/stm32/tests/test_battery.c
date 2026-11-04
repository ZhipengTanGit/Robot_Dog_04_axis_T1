#include "Battery.h"
#include "Battery_Config.h"
#include "BSP_Battery.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint8_t registers[256];
static uint8_t mock_awake;
static uint16_t mock_adc;
static uint32_t mock_now;
static unsigned reads, writes, releases, initializations;
static int adc_fails, all_ff, fail_read_reg, ignore_charge_write;
static int sleep_read_reg, sleep_returns_high;
static BSP_Battery_IoDiagnostics io;

void BSP_Battery_Init(void) { ++initializations; memset(&io, 0, sizeof(io)); }
void BSP_Battery_GetIoDiagnostics(BSP_Battery_IoDiagnostics *out) { *out = io; }
uint8_t BSP_Battery_IsAwake(void) { return mock_awake; }
uint32_t BSP_Millis(void) { return mock_now; }
void BSP_Battery_ReleaseBus(void) { ++releases; }

int BSP_Battery_ReadReg(uint8_t reg, uint8_t *value)
{
    int result = 0;
    ++reads;
    if (reg == sleep_read_reg) {
        mock_awake = sleep_returns_high ? 1u : 0u;
        result = BSP_BATTERY_IO_ASLEEP;
    } else if (reg == fail_read_reg) {
        result = BSP_BATTERY_IO_NACK;
    }
    io.last_result = result;
    if (result != 0) {
        io.last_error = result; io.last_error_ms = mock_now;
        io.last_stage = 5u; io.last_reg = reg; io.last_write = 0u;
        io.last_pins = result == BSP_BATTERY_IO_ASLEEP ? 3u : 7u;
        ++io.failures;
        return result;
    }
    *value = all_ff ? 0xffu : registers[reg];
    return 0;
}

int BSP_Battery_WriteReg(uint8_t reg, uint8_t value)
{
    ++writes;
    io.last_result = 0;
    if (!(ignore_charge_write && reg == 0x00u)) {
        registers[reg] = value;
    }
    return 0;
}

int BSP_Battery_ReadTemperatureAdc(uint16_t *raw)
{
#if BATTERY_TEMP_ADC_CONNECTED
    if (adc_fails) { return -1; }
    *raw = mock_adc;
    return 0;
#else
    (void)raw;
    return -1;
#endif
}

static Battery_Status get_status(void)
{
    Battery_Status status;
    Battery_GetStatus(&status);
    return status;
}

static void step(uint32_t ms)
{
    mock_now += ms;
    Battery_Task();
}

static void samples(unsigned count)
{
    while (count--) { step(BATTERY_POLL_MS); }
}

static uint16_t adc_for_temperature(int16_t temperature_dc)
{
    uint16_t raw;
    int16_t value;
    for (raw = 462u; raw <= 3740u; ++raw) {
        if (Battery_TemperatureFromAdc(raw, &value) == 0 && value == temperature_dc) {
            return raw;
        }
    }
    assert(0 && "Requested temperature has no exact ADC-table representation");
    return 0u;
}

static void reset_at(uint32_t now)
{
    memset(registers, 0, sizeof(registers));
    registers[0x00] = 0x30u; /* Existing chip permission: boost + charge. */
    registers[0x0a] = 0x80u;
    registers[0x22] = 0x02u;
    registers[0x74] = 0x30u;
    mock_awake = 1u;
    mock_adc = adc_for_temperature(250);
    mock_now = now;
    reads = writes = releases = initializations = 0u;
    adc_fails = all_ff = ignore_charge_write = 0;
    fail_read_reg = -1;
    sleep_read_reg = -1;
    sleep_returns_high = 0;
    Battery_Init();
    assert(initializations == 1u);
    assert(get_status().link == BATTERY_LINK_STARTING);
    Battery_Task(); /* First observation starts the wake settling period. */
}

static void reset_ready(void)
{
    reset_at(0u);
    samples(BATTERY_RECOVERY_SAMPLES);
    assert(get_status().link == BATTERY_LINK_READY);
    assert(get_status().snapshot_valid);
}

static void test_policy_thresholds(void)
{
    static const int16_t temperatures[] = { 100, 110, 120, 200, 210, 220, 430, 440, 450 };
    static const Battery_ChargePolicy from_off[] = {
        BATTERY_CHARGE_INHIBITED, BATTERY_CHARGE_INHIBITED, BATTERY_CHARGE_COOL,
        BATTERY_CHARGE_COOL, BATTERY_CHARGE_COOL, BATTERY_CHARGE_NORMAL,
        BATTERY_CHARGE_NORMAL, BATTERY_CHARGE_INHIBITED, BATTERY_CHARGE_INHIBITED
    };
    static const Battery_ChargePolicy from_normal[] = {
        BATTERY_CHARGE_INHIBITED, BATTERY_CHARGE_COOL, BATTERY_CHARGE_COOL,
        BATTERY_CHARGE_COOL, BATTERY_CHARGE_COOL, BATTERY_CHARGE_NORMAL,
        BATTERY_CHARGE_NORMAL, BATTERY_CHARGE_NORMAL, BATTERY_CHARGE_INHIBITED
    };
    Battery_PolicyContext policy;
    unsigned i, n;
    for (i = 0u; i < sizeof(temperatures) / sizeof(temperatures[0]); ++i) {
        Battery_PolicyReset(&policy);
        for (n = 0u; n < BATTERY_RECOVERY_SAMPLES; ++n) {
            (void)Battery_PolicyUpdate(&policy, 1u, temperatures[i], 0u);
            if (n + 1u < BATTERY_RECOVERY_SAMPLES) {
                assert(policy.mode == BATTERY_CHARGE_INHIBITED);
            }
        }
        assert(policy.mode == from_off[i]);
        Battery_PolicyReset(&policy);
        for (n = 0u; n < BATTERY_RECOVERY_SAMPLES; ++n) {
            (void)Battery_PolicyUpdate(&policy, 1u, 250, 0u);
        }
        assert(Battery_PolicyUpdate(&policy, 1u, temperatures[i], 0u) == from_normal[i]);
    }
    Battery_PolicyReset(&policy);
    for (n = 0u; n < BATTERY_RECOVERY_SAMPLES; ++n) {
        (void)Battery_PolicyUpdate(&policy, 1u, 250, 0u);
    }
    assert(Battery_PolicyUpdate(&policy, 1u, 210, 0u) == BATTERY_CHARGE_COOL);
    for (n = 1u; n < BATTERY_RECOVERY_SAMPLES; ++n) {
        assert(Battery_PolicyUpdate(&policy, 1u, 220, 0u) == BATTERY_CHARGE_COOL);
    }
    assert(Battery_PolicyUpdate(&policy, 1u, 219, 0u) == BATTERY_CHARGE_COOL);
    for (n = 1u; n < BATTERY_RECOVERY_SAMPLES; ++n) {
        assert(Battery_PolicyUpdate(&policy, 1u, 220, 0u) == BATTERY_CHARGE_COOL);
    }
    assert(Battery_PolicyUpdate(&policy, 1u, 220, 0u) == BATTERY_CHARGE_NORMAL);
    assert(Battery_PolicyUpdate(&policy, 0u, 250, 0u) == BATTERY_CHARGE_INHIBITED);
    for (n = 1u; n < BATTERY_RECOVERY_SAMPLES; ++n) {
        (void)Battery_PolicyUpdate(&policy, 1u, 250, 0u);
    }
    assert(Battery_PolicyUpdate(&policy, 1u, 250, 1u) == BATTERY_CHARGE_INHIBITED);
    assert(policy.recovery_count == 0u && policy.warm_count == 0u);
}

static void test_ntc_conversion(void)
{
    int temperature;
    int16_t measured = 1234;
    double maximum_error = 0.0;
    static const uint16_t invalid[] = { 0u, 1u, 461u, 3741u, 4094u, 4095u, 65535u };
    unsigned i;
    for (temperature = -200; temperature <= 800; ++temperature) {
        double c = temperature / 10.0;
        double ratio = exp(3950.0 * (1.0 / (c + 273.15) - 1.0 / 298.15));
        uint16_t raw = (uint16_t)(4095.0 * ratio / (1.0 + ratio) + 0.5);
        double error;
        assert(Battery_TemperatureFromAdc(raw, &measured) == 0);
        error = fabs(measured / 10.0 - c);
        if (error > maximum_error) { maximum_error = error; }
        assert(error <= 0.31); /* Nominal interpolation accuracy, no sensor tolerance. */
    }
    measured = 1234;
    for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(Battery_TemperatureFromAdc(invalid[i], &measured) != 0);
        assert(measured == 1234);
    }
    assert(Battery_TemperatureFromAdc(2048u, NULL) != 0);
    printf("NTC nominal table maximum error: %.2f C\n", maximum_error);
}

static void test_wake_sleep_and_monitor(void)
{
    unsigned before_reads, before_writes;
    Battery_Status status;
    reset_at(0u);
    assert(reads == 0u && writes == 0u);
    step(BATTERY_WAKE_SETTLE_MS);
    assert(reads == 0u && writes == 0u);
    step(BATTERY_POLL_MS - BATTERY_WAKE_SETTLE_MS - 1u);
    assert(reads == 0u && writes == 0u);
    step(1u);
    status = get_status();
    assert(status.link == BATTERY_LINK_READY && status.snapshot_valid);
    assert(status.chip.soc_min_pct == 25u && status.chip.soc_max_pct == 50u);
#if BATTERY_CONTROL_ENABLED
    assert(!status.monitor_only && status.charge_stop_verified);
#else
    assert(status.monitor_only && !status.control_verified);
    assert(!status.temperature_valid && (status.faults & BATTERY_FAULT_TEMP_UNCONNECTED));
    assert(writes == 0u && registers[0x00] == 0x30u);
#endif
    before_reads = reads;
    before_writes = writes;
    mock_awake = 0u;
    samples(1u);
    status = get_status();
    assert(status.link == BATTERY_LINK_STANDBY && !status.snapshot_valid);
    assert(!status.temperature_valid && !status.control_verified);
    assert(status.motion_request == BATTERY_MOTION_STOP_REQUEST);
    assert(reads == before_reads && writes == before_writes && releases == 1u);
    samples(8u);
    assert(reads == before_reads && writes == before_writes);
    mock_awake = 1u;
    samples(1u);
    assert(get_status().link == BATTERY_LINK_STARTING && !get_status().snapshot_valid);
    assert(reads == before_reads && writes == before_writes);
    registers[0x74] = 0x70u;
    samples(1u);
    status = get_status();
    assert(status.snapshot_valid && status.chip.soc_min_pct == 50u);
    assert(status.charge_policy == BATTERY_CHARGE_INHIBITED);
#if !BATTERY_CONTROL_ENABLED
    Battery_RequestCharging(0u);
    Battery_Task();
    Battery_RequestCharging(1u);
    Battery_Task();
    assert(writes == 0u);
#endif
}

static void test_invalid_data_and_recovery(void)
{
    Battery_Status status, before;
    reset_ready();
    before = get_status();
    registers[0x74] = 0xf0u;
    fail_read_reg = 0x77;
    samples(1u);
    status = get_status();
    assert(status.link == BATTERY_LINK_BUS_ERROR && !status.snapshot_valid);
    assert(status.faults & BATTERY_FAULT_BUS);
    assert(status.bus_error_count == before.bus_error_count + 1u);
    assert(status.last_good_ms == before.last_good_ms);
    assert(status.last_io_failure.error == BSP_BATTERY_IO_NACK);
    assert(status.last_io_failure.reg == 0x77u);
    assert(memcmp(&status.chip, &before.chip, sizeof(status.chip)) == 0);
    fail_read_reg = -1;
    samples(1u);
    status = get_status();
    assert(status.snapshot_valid && status.chip.soc_min_pct == 75u);
    assert(status.charge_policy == BATTERY_CHARGE_INHIBITED);
    registers[0x74] = 0x20u;
    samples(1u);
    status = get_status();
    assert(status.link == BATTERY_LINK_DATA_ERROR && !status.snapshot_valid);
    assert(status.faults & BATTERY_FAULT_DATA);
    assert(status.motion_request == BATTERY_MOTION_STOP_REQUEST);
    registers[0x74] = 0x30u;
    registers[0x76] = 0x50u; /* Contradictory hot and cold status. */
    samples(1u);
    assert(get_status().link == BATTERY_LINK_DATA_ERROR);
    registers[0x76] = 0u;
    samples(BATTERY_RECOVERY_SAMPLES);
    before = get_status();
    all_ff = 1;
    samples(1u);
    status = get_status();
    assert(status.link == BATTERY_LINK_DATA_ERROR && !status.snapshot_valid);
    assert(status.faults & BATTERY_FAULT_DATA);
    assert(status.bus_error_count == before.bus_error_count);
    assert(memcmp(&status.chip, &before.chip, sizeof(status.chip)) == 0);
    all_ff = 0;
    samples(1u);
    assert(get_status().snapshot_valid && get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
#if !BATTERY_CONTROL_ENABLED
    assert(writes == 0u);
#endif
}

static void test_sleep_during_transaction(void)
{
    unsigned prior_reads, prior_writes;
    Battery_Status s;
    int returns_high;
    for (returns_high = 0; returns_high <= 1; ++returns_high) {
        reset_ready();
        prior_writes = writes;
        sleep_read_reg = 0x70;
        sleep_returns_high = returns_high;
        samples(1u);
        s = get_status();
        assert(s.link == (returns_high ? BATTERY_LINK_STARTING : BATTERY_LINK_STANDBY));
        assert(!s.snapshot_valid && !s.control_verified);
        assert(s.bus_error_count == 0u);
        assert(!(s.faults & (BATTERY_FAULT_BUS | BATTERY_FAULT_CONTROL)));
        assert(s.last_io_failure.error == BSP_BATTERY_IO_ASLEEP);
        assert(s.last_io_failure.reg == 0x70u);
        assert(writes == prior_writes); /* No OFF access after the sleep edge. */
        prior_reads = reads;
        sleep_read_reg = -1;
        mock_awake = 1u;
        samples(1u);
        assert(get_status().link == BATTERY_LINK_STARTING && reads == prior_reads);
        samples(1u);
        assert(get_status().snapshot_valid);
        assert(get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
        assert(get_status().last_io_failure.error == BSP_BATTERY_IO_ASLEEP);
    }
}

static void test_low_battery_debounce(void)
{
    reset_ready();
    registers[0x74] = 0u;
    samples(BATTERY_CRITICAL_SAMPLES - 1u);
    assert(!get_status().critical_battery);
    samples(1u);
    assert(get_status().critical_battery);
    assert(get_status().motion_request == BATTERY_MOTION_STOP_REQUEST);
    registers[0x74] = 0x10u;
    samples(10u);
    assert(get_status().critical_battery); /* 3..25% cannot clear critical. */
    registers[0x74] = 0x30u;
    samples(BATTERY_RECOVERY_SAMPLES - 1u);
    assert(get_status().critical_battery);
    samples(1u);
    assert(!get_status().critical_battery);
    assert(get_status().motion_request == BATTERY_MOTION_NORMAL);
    registers[0x74] = 0u;
    samples(2u);
    fail_read_reg = 0x74;
    samples(1u);
    fail_read_reg = -1;
    samples(2u);
    assert(!get_status().critical_battery); /* Transport gap breaks consecutiveness. */
    samples(1u);
    assert(get_status().critical_battery);
    registers[0x74] = 0x30u;
    samples(4u);
    mock_awake = 0u;
    samples(1u);
    mock_awake = 1u;
    samples(1u);
    samples(4u);
    assert(get_status().critical_battery); /* New wake needs a new recovery sequence. */
    samples(1u);
    assert(!get_status().critical_battery);
}

static void test_stale_wrap_and_delays(void)
{
    Battery_Status status;
    reset_at(UINT32_MAX - 100u);
    samples(BATTERY_RECOVERY_SAMPLES);
    status = get_status();
    assert(status.snapshot_valid && status.link == BATTERY_LINK_READY);
    assert(status.last_good_ms == mock_now);
    mock_now += BATTERY_STALE_MS;
    assert(get_status().snapshot_valid); /* Boundary is stale only after 1500 ms. */
    ++mock_now;
    status = get_status();
    assert(!status.snapshot_valid && status.link == BATTERY_LINK_STALE);
    assert(status.faults & BATTERY_FAULT_STALE);
    assert(!status.temperature_valid && !status.control_verified && !status.charge_stop_verified);
    assert(status.applied_nominal_ma == 0u && status.motion_request == BATTERY_MOTION_STOP_REQUEST);
    Battery_Task();
    status = get_status();
    assert(status.snapshot_valid && status.charge_policy == BATTERY_CHARGE_INHIBITED);
    samples(BATTERY_RECOVERY_SAMPLES - 2u);
    assert(get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
    samples(1u);
#if BATTERY_CONTROL_ENABLED
    assert(get_status().charge_policy == BATTERY_CHARGE_NORMAL);
#endif
    registers[0x74] = 0u;
    samples(2u);
    step(BATTERY_STALE_MS + 1u);
    assert(!get_status().critical_battery);
    samples(1u);
    assert(!get_status().critical_battery);
    samples(1u);
    assert(get_status().critical_battery);
    Battery_Init();
    status = get_status();
    assert(!status.snapshot_valid && !status.critical_battery && !status.control_verified);
    assert(status.link == BATTERY_LINK_STARTING && status.bus_error_count == 0u);
    Battery_GetStatus(NULL);
}

static void test_hardware_ntc_and_motion(void)
{
    reset_ready();
    registers[0x76] = 0x20u;
    samples(1u);
    assert(get_status().snapshot_valid && (get_status().faults & BATTERY_FAULT_NTC));
    assert(get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
    assert(get_status().motion_request == BATTERY_MOTION_LIMIT_REQUEST);
    registers[0x76] = 0x40u;
    samples(1u);
    assert(get_status().motion_request == BATTERY_MOTION_STOP_REQUEST);
    registers[0x76] = 0x10u;
    samples(1u);
    assert(get_status().motion_request == BATTERY_MOTION_STOP_REQUEST);
    registers[0x76] = 0u;
    registers[0x83] = 0x20u;
    samples(1u);
    assert(get_status().motion_request == BATTERY_MOTION_LIMIT_REQUEST);
    registers[0x83] = 0u;
    registers[0x70] = 8u;
    samples(1u);
    assert(get_status().motion_request == BATTERY_MOTION_LIMIT_REQUEST);
}

#if BATTERY_CONTROL_ENABLED
static void test_sleep_during_initial_stop(void)
{
    unsigned prior_reads;
    reset_at(0u);
    sleep_read_reg = 0x00;
    sleep_returns_high = 1;
    samples(1u);
    assert(get_status().link == BATTERY_LINK_STARTING);
    assert(!get_status().snapshot_valid && !get_status().charge_stop_verified);
    assert(!(get_status().faults & (BATTERY_FAULT_BUS | BATTERY_FAULT_CONTROL)));
    assert(get_status().last_io_failure.error == BSP_BATTERY_IO_ASLEEP);
    assert(writes == 0u);
    prior_reads = reads;
    sleep_read_reg = -1;
    samples(1u);
    assert(get_status().link == BATTERY_LINK_STARTING && reads == prior_reads);
    samples(1u);
    assert(get_status().snapshot_valid && get_status().charge_stop_verified);
    assert(get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
}

static void test_control_temperature_and_profiles(void)
{
    Battery_Status status;
    unsigned i;
    reset_at(0u);
    for (i = 1u; i < BATTERY_RECOVERY_SAMPLES; ++i) {
        samples(1u);
        status = get_status();
        assert(status.charge_policy == BATTERY_CHARGE_INHIBITED);
        assert(status.charge_stop_verified && !(registers[0x00] & 0x10u));
    }
    samples(1u);
    status = get_status();
    assert(status.charge_policy == BATTERY_CHARGE_NORMAL && status.control_verified);
    assert(status.applied_nominal_ma == 650u && (registers[0x00] & 0x10u));
    assert((registers[0x24] & 0x1fu) == 6u && (registers[0x26] & 0x1fu) == 5u);
    mock_adc = adc_for_temperature(210);
    samples(1u);
    assert(get_status().charge_policy == BATTERY_CHARGE_COOL);
    assert(get_status().applied_nominal_ma == 250u);
    assert((registers[0x24] & 0x1fu) == 2u && (registers[0x26] & 0x1fu) == 2u);
    mock_adc = adc_for_temperature(220);
    samples(BATTERY_RECOVERY_SAMPLES - 1u);
    assert(get_status().applied_nominal_ma == 250u);
    samples(1u);
    assert(get_status().applied_nominal_ma == 650u);
    registers[0x26] = 0x1fu; /* Simulated chip reset/discarded current setting. */
    samples(1u);
    status = get_status();
    assert(status.faults & BATTERY_FAULT_CONTROL);
    assert(status.charge_policy == BATTERY_CHARGE_INHIBITED);
    assert(status.applied_nominal_ma == 0u && status.charge_stop_verified);
    assert(!(registers[0x00] & 0x10u));
    samples(BATTERY_RECOVERY_SAMPLES - 1u);
    assert(get_status().applied_nominal_ma == 0u);
    samples(1u);
    assert(get_status().applied_nominal_ma == 650u);
    assert((registers[0x26] & 0x1fu) == 5u);

    mock_adc = 4095u; /* Disconnected/open sensor must never turn into 25 C. */
    samples(1u);
    status = get_status();
    assert(!status.temperature_valid && (status.faults & BATTERY_FAULT_TEMP_SENSOR));
    assert(status.charge_policy == BATTERY_CHARGE_INHIBITED && status.charge_stop_verified);
    mock_adc = 0u;
    samples(1u);
    assert(!get_status().temperature_valid && get_status().charge_stop_verified);
    mock_adc = adc_for_temperature(250);
    adc_fails = 1;
    samples(1u);
    assert(!get_status().temperature_valid && get_status().charge_stop_verified);
    adc_fails = 0;
    samples(BATTERY_RECOVERY_SAMPLES);
    assert(get_status().applied_nominal_ma == 650u);
    ignore_charge_write = 1;
    adc_fails = 1;
    samples(1u);
    status = get_status();
    assert(!status.control_verified && !status.charge_stop_verified);
    assert(status.faults & BATTERY_FAULT_CONTROL);
    assert(registers[0x00] & 0x10u); /* Failed OFF is reported, never called verified. */
    ignore_charge_write = 0;
    samples(1u);
    assert(get_status().charge_stop_verified && !(registers[0x00] & 0x10u));
    adc_fails = 0;
    samples(BATTERY_RECOVERY_SAMPLES);
    Battery_RequestCharging(0u);
    samples(1u); /* Requests are applied on the next scheduled 250 ms sample. */
    assert(!get_status().charge_requested && get_status().charge_stop_verified);
    assert(!(registers[0x00] & 0x10u));
}

static void test_requests_cannot_forge_sample_time(void)
{
    unsigned i, before_reads;
    reset_at(0u);
    samples(1u);
    before_reads = reads;
    for (i = 0u; i < 50u; ++i) {
        Battery_RequestCharging(1u);
        Battery_Task();
    }
    assert(reads == before_reads);
    assert(get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
    assert(!(registers[0x00] & 0x10u));
    samples(BATTERY_RECOVERY_SAMPLES - 2u);
    assert(get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
    samples(1u);
    assert(get_status().charge_policy == BATTERY_CHARGE_NORMAL);
    mock_now += BATTERY_STALE_MS + 1u;
    Battery_RequestCharging(1u); /* A command must not erase the real scheduler gap. */
    Battery_Task();
    assert(get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
    assert(get_status().charge_stop_verified && !(registers[0x00] & 0x10u));
    samples(BATTERY_RECOVERY_SAMPLES - 2u);
    assert(get_status().charge_policy == BATTERY_CHARGE_INHIBITED);
    samples(1u);
    assert(get_status().charge_policy == BATTERY_CHARGE_NORMAL);
}
#endif

int main(void)
{
    assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_WRONG_MODE);
    test_policy_thresholds();
    test_ntc_conversion();
    test_wake_sleep_and_monitor();
    test_invalid_data_and_recovery();
    test_sleep_during_transaction();
    test_low_battery_debounce();
    test_stale_wrap_and_delays();
    test_hardware_ntc_and_motion();
#if BATTERY_CONTROL_ENABLED
    test_sleep_during_initial_stop();
    test_control_temperature_and_profiles();
    test_requests_cannot_forge_sample_time();
    puts("Battery integration tests passed (charger control enabled, mocked hardware)");
#else
    puts("Battery integration tests passed (default read-only monitor)");
#endif
    return 0;
}
