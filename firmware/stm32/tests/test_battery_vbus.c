#include "Battery.h"
#include "Battery_Config.h"
#include "BSP_Battery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if !BATTERY_VBUS_CURRENT_SETUP_ENABLED || BATTERY_CONTROL_ENABLED
#error These tests require the VBUS-only configuration
#endif

static uint8_t regs[256];
static uint8_t awake;
static uint32_t now;
static unsigned reads, writes, releases;
static int fail_read_reg, fail_write_reg, ignore_write_reg;
static unsigned enable_writes;
static unsigned current_writes, fail_current_read_after_writes;
static int sleep_read_reg, sleep_write_reg, sleep_returns_high;
static BSP_Battery_IoDiagnostics io;

void BSP_Battery_Init(void) { memset(&io, 0, sizeof(io)); }
void BSP_Battery_GetIoDiagnostics(BSP_Battery_IoDiagnostics *out) { *out = io; }
uint8_t BSP_Battery_IsAwake(void) { return awake; }
uint32_t BSP_Millis(void) { return now; }
void BSP_Battery_ReleaseBus(void) { ++releases; }
int BSP_Battery_ReadTemperatureAdc(uint16_t *raw) { (void)raw; return -1; }

static int complete_io(int result, uint8_t reg, uint8_t write)
{
    io.last_result = result;
    if (result != 0) {
        ++io.failures;
        io.last_error = result; io.last_error_ms = now;
        io.last_stage = write ? 7u : 5u; io.last_reg = reg; io.last_write = write;
        io.last_pins = result == BSP_BATTERY_IO_ASLEEP ? 3u : 7u;
    }
    return result;
}

int BSP_Battery_ReadReg(uint8_t reg, uint8_t *value)
{
    ++reads;
    if (reg == 0x26u && current_writes >= fail_current_read_after_writes) {
        return complete_io(BSP_BATTERY_IO_NACK, reg, 0u);
    }
    if ((int)reg == sleep_read_reg) {
        awake = sleep_returns_high ? 1u : 0u;
        return complete_io(BSP_BATTERY_IO_ASLEEP, reg, 0u);
    }
    if ((int)reg == fail_read_reg) { return complete_io(BSP_BATTERY_IO_NACK, reg, 0u); }
    *value = regs[reg];
    return complete_io(0, reg, 0u);
}

int BSP_Battery_WriteReg(uint8_t reg, uint8_t value)
{
    ++writes;
    if (reg == 0x26u) { ++current_writes; }
    /* Current-only code must never enable charging, even during recovery. */
    if (reg == 0x00u && (value & 0x10u)) { ++enable_writes; }
    assert(reg == 0x26u || reg == 0x00u || reg == 0x02u);
    if ((int)reg == sleep_write_reg) {
        awake = sleep_returns_high ? 1u : 0u;
        return complete_io(BSP_BATTERY_IO_ASLEEP, reg, 1u);
    }
    if ((int)reg == fail_write_reg) { return complete_io(BSP_BATTERY_IO_NACK, reg, 1u); }
    if ((int)reg != ignore_write_reg) { regs[reg] = value; }
    return complete_io(0, reg, 1u);
}

static Battery_Status status(void)
{
    Battery_Status result;
    Battery_GetStatus(&result);
    return result;
}

static void step(void)
{
    now += BATTERY_POLL_MS;
    Battery_Task();
    assert(enable_writes == 0u);
}

static void reset(uint8_t permission, uint8_t current)
{
    memset(regs, 0, sizeof(regs));
    regs[0x00] = permission;
    regs[0x02] = 0xE5u; /* Keep-awake already set: charging tests must not rewrite it. */
    regs[0x0a] = 0xb5u;
    regs[0x22] = 0xafu;
    regs[0x23] = 0xf3u;
    regs[0x24] = 0xd7u;
    regs[0x25] = 0xe7u;
    regs[0x26] = current;
    regs[0x70] = 0x08u;
    regs[0x74] = 0x30u;
    regs[0x83] = 0x20u;
    awake = 1u;
    now = 0u;
    reads = writes = releases = enable_writes = 0u;
    current_writes = 0u;
    fail_current_read_after_writes = 0xffffffffu;
    fail_read_reg = fail_write_reg = ignore_write_reg = -1;
    sleep_read_reg = sleep_write_reg = -1;
    sleep_returns_high = 0;
    Battery_Init();
    Battery_Task();
    assert(reads == 0u && writes == 0u);
}

static void assert_configured(void)
{
    Battery_Status s = status();
    assert(s.link == BATTERY_LINK_READY && s.snapshot_valid);
    assert(!s.monitor_only && s.vbus_limit_only);
    assert(BATTERY_VBUS_REQUESTED_MA == 350u && BATTERY_VBUS_NOMINAL_MA == 350u);
    assert(s.applied_nominal_ma == 350u && s.control_verified);
    assert(s.charge_config_valid && (s.charge_config.raw26 & 0x1fu) == 3u);
    assert(!s.temperature_valid && !s.register_map_confirmed);
    assert(s.charge_policy == BATTERY_CHARGE_INHIBITED);
    assert(s.faults == BATTERY_FAULT_TEMP_UNCONNECTED);
    assert((regs[0x26] & 0x1fu) == 3u);
}

static void test_initial_write_and_preservation(void)
{
    uint8_t before[256];
    unsigned i, prior_writes;
    reset(0xf8u, 0xa8u); /* Change from 1250 mA, preserving the reserved bits. */
    memcpy(before, regs, sizeof(before));
    step();
    assert_configured();
    assert(writes == 1u && regs[0x26] == 0xa3u);
    for (i = 0u; i < sizeof(regs); ++i) {
        if (i != 0x26u) { assert(regs[i] == before[i]); }
    }
    assert(!status().charge_stop_verified);
    prior_writes = writes;
    for (i = 0u; i < 20u; ++i) { step(); }
    assert(writes == prior_writes);
    assert_configured();
}

static void test_already_set_and_disabled_permission(void)
{
    reset(0xe8u, 0xa3u);
    step();
    assert_configured();
    assert(writes == 0u && regs[0x00] == 0xe8u);
    assert(status().charge_stop_verified);
    reset(0xe8u, 0xbfu);
    step();
    assert_configured();
    assert(writes == 1u && regs[0x00] == 0xe8u);
    assert(status().charge_stop_verified);
    Battery_RequestCharging(1u);
    step();
    assert(regs[0x00] == 0xe8u && enable_writes == 0u);
}

static void test_sleep_and_reset_recovery(void)
{
    unsigned prior_reads, prior_writes;
    reset(0xf8u, 0xbfu);
    step();
    prior_reads = reads;
    prior_writes = writes;
    awake = 0u;
    step();
    assert(status().link == BATTERY_LINK_STANDBY);
    assert(!status().control_verified && !status().charge_config_valid);
    assert(status().applied_nominal_ma == 0u);
    assert(reads == prior_reads && writes == prior_writes && releases == 1u);
    step();
    assert(reads == prior_reads && writes == prior_writes);
    regs[0x26] = 0xbfu;
    awake = 1u;
    step();
    assert(status().link == BATTERY_LINK_STARTING);
    assert(reads == prior_reads && writes == prior_writes);
    step();
    assert_configured();
    assert(writes == prior_writes + 1u);
    regs[0x26] = 0xa2u; /* Silent IC reset/drift while L3 stays high. */
    step();
    assert_configured();
    assert(writes == prior_writes + 2u);
}

static void assert_failed_current(void)
{
    Battery_Status s = status();
    assert(!s.control_verified && !s.charge_config_valid);
    assert(s.applied_nominal_ma == 0u);
    assert(s.faults & (BATTERY_FAULT_CONTROL | BATTERY_FAULT_BUS));
    assert(enable_writes == 0u);
}

static void test_read_failures_and_no_auto_enable(void)
{
    reset(0xf8u, 0xbfu);
    fail_read_reg = 0x26;
    step();
    assert_failed_current();
    assert(!(regs[0x00] & 0x10u) && status().charge_stop_verified);
    fail_read_reg = -1;
    step();
    assert_configured();
    assert(!(regs[0x00] & 0x10u));

    fail_read_reg = 0x70; /* Snapshot failure, before current setup. */
    step();
    assert_failed_current();
    assert(status().link == BATTERY_LINK_BUS_ERROR);
    fail_read_reg = -1;
    step();
    assert_configured();
    assert(!(regs[0x00] & 0x10u));
}

static void test_write_failures_and_ignored_writes(void)
{
    reset(0xf8u, 0xbfu);
    fail_write_reg = 0x26;
    step();
    assert_failed_current();
    assert(!(regs[0x00] & 0x10u));
    fail_write_reg = -1;
    step();
    assert_configured();
    assert(!(regs[0x00] & 0x10u));

    reset(0xf8u, 0xbfu);
    ignore_write_reg = 0x26;
    step();
    assert_failed_current();
    assert(!(regs[0x00] & 0x10u) && regs[0x26] == 0xbfu);
    ignore_write_reg = -1;
    step();
    assert_configured();
    assert(!(regs[0x00] & 0x10u));

    reset(0xf8u, 0xbfu);
    fail_write_reg = 0x26;
    ignore_write_reg = 0x00; /* Cannot claim successful OFF. */
    step();
    assert_failed_current();
    assert(!status().charge_stop_verified && (regs[0x00] & 0x10u));
}

static void test_stale_configuration(void)
{
    unsigned before_reads;
    reset(0xf8u, 0xbfu);
    step();
    assert_configured();
    before_reads = reads;
    now += BATTERY_STALE_MS + 1u;
    assert(status().link == BATTERY_LINK_STALE);
    assert(!status().control_verified && !status().charge_config_valid);
    assert(status().applied_nominal_ma == 0u);
    assert(reads == before_reads);
    Battery_Task();
    assert_configured();
}

static void test_mid_transaction_sleep_and_rewake(void)
{
    unsigned prior_reads, prior_writes;
    int returns_high;
    for (returns_high = 0; returns_high <= 1; ++returns_high) {
        reset(0xf8u, 0xa3u);
        step();
        prior_writes = writes;
        sleep_read_reg = 0x70;
        sleep_returns_high = returns_high;
        step();
        assert(status().link == (returns_high ? BATTERY_LINK_STARTING : BATTERY_LINK_STANDBY));
        assert(!status().snapshot_valid && !status().control_verified);
        assert(status().bus_error_count == 0u);
        assert(status().faults == BATTERY_FAULT_TEMP_UNCONNECTED);
        assert(status().last_io_failure.error == BSP_BATTERY_IO_ASLEEP);
        assert(status().last_io_failure.reg == 0x70u);
        assert(writes == prior_writes && (regs[0x00] & 0x10u));
        prior_reads = reads;
        sleep_read_reg = -1;
        awake = 1u;
        step();
        assert(status().link == BATTERY_LINK_STARTING && reads == prior_reads);
        step();
        assert_configured();
        assert(writes == prior_writes);
    }
}

static void test_configuration_sleep_is_not_ready(void)
{
    unsigned prior_reads;
    reset(0xf8u, 0xbfu);
    sleep_read_reg = 0x26; /* Configuration read, after the complete snapshot. */
    step();
    assert(status().link == BATTERY_LINK_STANDBY && !status().snapshot_valid);
    assert(status().faults == BATTERY_FAULT_TEMP_UNCONNECTED);
    assert(status().last_io_failure.reg == 0x26u);
    assert(writes == 0u && status().bus_error_count == 0u);

    reset(0xf8u, 0xbfu);
    sleep_write_reg = 0x26; /* IP5310_ApplyVbusCurrent has an internal OFF path. */
    sleep_returns_high = 1;
    step();
    assert(status().link == BATTERY_LINK_STARTING && !status().snapshot_valid);
    assert(status().faults == BATTERY_FAULT_TEMP_UNCONNECTED);
    assert(status().last_io_failure.error == BSP_BATTERY_IO_ASLEEP);
    assert(status().last_io_failure.reg == 0x26u && status().last_io_failure.write);
    assert(writes == 1u && (regs[0x00] & 0x10u)); /* No OFF transaction replay. */
    prior_reads = reads;
    sleep_write_reg = -1;
    step();
    assert(status().link == BATTERY_LINK_STARTING && reads == prior_reads);
    step();
    assert_configured();
}

static void test_original_failure_survives_failed_off(void)
{
    reset(0xf8u, 0xa3u);
    step();
    fail_read_reg = 0x70;
    fail_write_reg = 0x00;
    step();
    assert_failed_current();
    assert(status().link == BATTERY_LINK_BUS_ERROR);
    assert(io.last_reg == 0x00u && io.last_write); /* OFF itself subsequently failed. */
    assert(status().last_io_failure.error == BSP_BATTERY_IO_NACK);
    assert(status().last_io_failure.reg == 0x70u && !status().last_io_failure.write);
    assert(status().last_io_failure.at_ms == now);
    fail_read_reg = fail_write_reg = -1;
    step();
    assert_configured();
    assert(status().last_io_failure.reg == 0x70u); /* Success does not erase evidence. */
}

static void test_runtime_current_profiles_and_ram_target(void)
{
    uint16_t target;
    unsigned i, j, before_writes;
    uint8_t before[256];
    reset(0xf8u, 0xa3u);
    assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_NOT_READY);
    step();
    for (i = 0u; i < 32u; ++i) {
        target = IP5310_VbusNominalFromCode((uint8_t)i);
        memcpy(before, regs, sizeof(before));
        before_writes = writes;
        if (target > BATTERY_BOARD_MAX_NOMINAL_MA) {
            assert(Battery_RequestNominalCurrent(target) == BATTERY_CURRENT_BOARD_LIMIT);
            step();
            assert(writes == before_writes && memcmp(before, regs, sizeof(before)) == 0);
            continue;
        }
        assert(Battery_RequestNominalCurrent(target) == BATTERY_CURRENT_ACCEPTED);
        assert(status().current_request_state == BATTERY_CURRENT_PENDING && writes == before_writes);
        assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_BUSY);
        step();
        assert(status().current_request_state == BATTERY_CURRENT_APPLIED);
        assert(status().requested_nominal_ma == target && status().applied_nominal_ma == target);
        assert(status().control_verified && status().charge_config_valid);
        assert(regs[0x26u] == (uint8_t)(0xa0u | i));
        for (j = 0u; j < sizeof(regs); ++j) { if (j != 0x26u) { assert(regs[j] == before[j]); } }
        before_writes = writes;
        for (j = 0u; j < 5u; ++j) { step(); }
        assert(writes == before_writes && status().applied_nominal_ma == target);
    }
    assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_ACCEPTED); step();
    awake = 0u; step(); awake = 1u; regs[0x26u] = 0xa3u; step(); step();
    assert(!status().control_verified && (regs[0x26u] & 0x1fu) == 3u);
    assert(status().requested_nominal_ma == 650u); /* Drift does not reapply a manual command. */
    assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_ACCEPTED); step();
    assert(status().applied_nominal_ma == 650u);
    Battery_Init(); Battery_Task(); step(); assert_configured(); /* Reboot returns to the 350mA fallback. */
}

static void test_runtime_request_rejections(void)
{
    unsigned before_writes;
    reset(0xf8u, 0xa3u); step(); before_writes = writes;
    assert(Battery_RequestNominalCurrent(0u) == BATTERY_CURRENT_UNSUPPORTED);
    assert(Battery_RequestNominalCurrent(1000u) == BATTERY_CURRENT_UNSUPPORTED);
    assert(Battery_RequestNominalCurrent(2000u) == BATTERY_CURRENT_UNSUPPORTED);
    assert(Battery_RequestNominalCurrent(65535u) == BATTERY_CURRENT_UNSUPPORTED);
    assert(Battery_RequestNominalCurrent(3050u) == BATTERY_CURRENT_BOARD_LIMIT);
    assert(Battery_RequestNominalCurrent(4350u) == BATTERY_CURRENT_BOARD_LIMIT);
    assert(writes == before_writes && status().requested_nominal_ma == 350u);
    awake = 0u;
    assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_NOT_READY);
    awake = 1u; now += BATTERY_STALE_MS + 1u;
    assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_NOT_READY);
}

static void test_runtime_request_failure_is_not_retried(void)
{
    unsigned i, before_current;
    reset(0xf8u, 0xa3u); step();
    assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_ACCEPTED);
    ignore_write_reg = 0x26; step();
    assert(status().current_request_state == BATTERY_CURRENT_FAILED);
    assert(status().current_request_result == IP5310_ERROR_VERIFY);
    assert_failed_current();
    assert(!(regs[0x00] & 0x10u));
    before_current = current_writes;
    ignore_write_reg = -1;
    for (i = 0u; i < 12u; ++i) { step(); }
    assert(current_writes == before_current && status().current_request_state == BATTERY_CURRENT_FAILED);
    assert(!status().control_verified && status().applied_nominal_ma == 0u);
    assert(Battery_RequestNominalCurrent(650u) == BATTERY_CURRENT_ACCEPTED); step();
    assert(status().current_request_state == BATTERY_CURRENT_APPLIED && status().applied_nominal_ma == 650u);
    assert(!(regs[0x00] & 0x10u) && enable_writes == 0u);

    reset(0xf8u, 0xa3u); step();
    fail_current_read_after_writes = 1u;
    assert(Battery_RequestNominalCurrent(850u) == BATTERY_CURRENT_ACCEPTED); step();
    assert(current_writes == 1u && (regs[0x26] & 0x1fu) == 7u); /* Write occurred; readback failed. */
    assert(status().current_request_state == BATTERY_CURRENT_FAILED && !status().control_verified);
    assert(status().current_request_result == IP5310_ERROR_IO);
    fail_current_read_after_writes = 0xffffffffu;
    for (i = 0u; i < 4u; ++i) { step(); }
    assert(current_writes == 1u && status().current_request_state == BATTERY_CURRENT_FAILED);
    assert(status().applied_nominal_ma == 850u); /* Later read may confirm physical bits, not revive the failed command. */

    reset(0xf8u, 0xa3u); step();
    assert(Battery_RequestNominalCurrent(1250u) == BATTERY_CURRENT_ACCEPTED);
    awake = 0u; step();
    assert(status().current_request_state == BATTERY_CURRENT_FAILED && !current_writes);
    awake = 1u; step(); step();
    assert(!current_writes); /* An old command must not apply after standby. */
}

static void test_keep_awake_is_independent_of_current(void)
{
    reset(0xf8u, 0xa3u);
    regs[0x02] = 0x65u;
    step();
    assert_configured();
    assert(writes == 1u && regs[0x02] == 0xe5u && status().keep_5v_verified);
    assert(regs[0x00] == 0xf8u && regs[0x26] == 0xa3u && current_writes == 0u);

    reset(0xf8u, 0xa3u);
    regs[0x02] = 0x65u; ignore_write_reg = 0x02;
    step();
    assert_configured();
    assert(!status().keep_5v_verified && status().power_setup_result == IP5310_ERROR_VERIFY);
    step(); step(); step(); step();
    assert(writes == 1u && current_writes == 0u && regs[0x00] == 0xf8u && regs[0x26] == 0xa3u);
    assert_configured(); /* Power-option failure must not trigger charger OFF. */

    reset(0xf8u, 0xa3u);
    fail_read_reg = 0x01; step();
    assert_configured();
    assert(!status().power_config_valid && status().power_setup_result == IP5310_ERROR_IO);
    assert(writes == 0u && regs[0x00] == 0xf8u);
}

int main(void)
{
    test_initial_write_and_preservation();
    test_already_set_and_disabled_permission();
    test_sleep_and_reset_recovery();
    test_read_failures_and_no_auto_enable();
    test_write_failures_and_ignored_writes();
    test_stale_configuration();
    test_mid_transaction_sleep_and_rewake();
    test_configuration_sleep_is_not_ready();
    test_original_failure_survives_failed_off();
    test_runtime_current_profiles_and_ram_target();
    test_runtime_request_rejections();
    test_runtime_request_failure_is_not_retried();
    test_keep_awake_is_independent_of_current();
    puts("Battery VBUS current-only tests passed");
    return 0;
}
