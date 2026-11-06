#include "Battery.h"
#include "Battery_Config.h"
#include "BSP_Battery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if !BATTERY_KEEP_5V_ON_ENABLED || BATTERY_CONTROL_ENABLED || BATTERY_VBUS_CURRENT_SETUP_ENABLED
#error Isolate keep-awake configuration from charger writes in these tests
#endif

static uint8_t regs[256], awake;
static uint32_t now;
static unsigned reads, writes, releases, reads_at[256];
static unsigned fail_read_call;
static int fail_read, fail_write, ignore_write, fail_after_write;
static int sleep_read, sleep_write, sleep_high;
static BSP_Battery_IoDiagnostics io;

void BSP_Battery_Init(void) { memset(&io, 0, sizeof(io)); }
void BSP_Battery_GetIoDiagnostics(BSP_Battery_IoDiagnostics *out) { *out = io; }
uint8_t BSP_Battery_IsAwake(void) { return awake; }
uint32_t BSP_Millis(void) { return now; }
void BSP_Battery_ReleaseBus(void) { ++releases; }
int BSP_Battery_ReadTemperatureAdc(uint16_t *raw) { (void)raw; return -1; }
static int failure(int code, uint8_t reg, uint8_t write)
{
    io.last_result = io.last_error = code;
    io.last_error_ms = now; io.last_stage = write ? 7U : 5U;
    io.last_reg = reg; io.last_write = write; io.last_pins = 7U;
    ++io.failures;
    return code;
}
int BSP_Battery_ReadReg(uint8_t reg, uint8_t *value)
{
    ++reads; ++reads_at[reg];
    if ((int)reg == sleep_read) {
        awake = (uint8_t)sleep_high;
        return failure(BSP_BATTERY_IO_ASLEEP, reg, 0U);
    }
    if (((int)reg == fail_read && (!fail_read_call || reads_at[reg] == fail_read_call)) ||
        (fail_after_write && writes && reg == 0x02U)) {
        return failure(BSP_BATTERY_IO_NACK, reg, 0U);
    }
    *value = regs[reg];
    return BSP_BATTERY_IO_OK;
}
int BSP_Battery_WriteReg(uint8_t reg, uint8_t value)
{
    ++writes;
    assert(reg == 0x02U); /* No charge/boost permission, KEY or protection writes. */
    if ((int)reg == sleep_write) {
        awake = (uint8_t)sleep_high;
        return failure(BSP_BATTERY_IO_ASLEEP, reg, 1U);
    }
    if (fail_write) { return failure(BSP_BATTERY_IO_NACK, reg, 1U); }
    if (!ignore_write) { regs[reg] = value; }
    return BSP_BATTERY_IO_OK;
}
static Battery_Status state(void)
{
    Battery_Status s;
    Battery_GetStatus(&s);
    return s;
}
static void step(void) { now += BATTERY_POLL_MS; Battery_Task(); }
static void steps(unsigned count) { while (count--) { step(); } }
static void reset(uint8_t raw02, uint32_t start)
{
    memset(regs, 0, sizeof(regs));
    memset(reads_at, 0, sizeof(reads_at));
    regs[0x00] = 0x30U; regs[0x01] = 0x11U; regs[0x02] = raw02; regs[0x0e] = 0x44U;
    regs[0x70] = 0x08U; regs[0x74] = 0x30U; regs[0x26] = 0xADU;
    awake = 1U; now = start;
    reads = writes = releases = 0U;
    fail_read_call = 0U;
    fail_read = sleep_read = sleep_write = -1;
    fail_write = ignore_write = fail_after_write = sleep_high = 0;
    Battery_Init(); Battery_Task();
    assert(reads == 0U && writes == 0U);
}
static void test_setup_and_read_only_cadence(void)
{
    uint8_t before[256];
    unsigned i, previous_reads;
    reset(0x6DU, 0U);
    memcpy(before, regs, sizeof(regs));
    step();
    assert(writes == 1U && regs[0x02] == 0xEDU);
    assert(state().power_config_valid && state().keep_5v_verified && state().power_setup_result == IP5310_OK);
    assert(state().power_config_before_valid && state().power_config_before.raw02 == 0x6DU);
    for (i = 0U; i < sizeof(regs); ++i) { if (i != 2U) { assert(regs[i] == before[i]); } }
    previous_reads = reads_at[1];
    steps(3U); assert(reads_at[1] == previous_reads && writes == 1U);
    step(); assert(reads_at[1] == previous_reads + 1U && writes == 1U);
    steps(20U); assert(writes == 1U && state().keep_5v_verified);
    reset(0xEDU, 0U); step();
    assert(writes == 0U && state().keep_5v_verified); /* Already configured. */
}
static void test_unknown_and_drift_never_rewrite(void)
{
    unsigned code;
    for (code = 0U; code < 8U; ++code) {
        if (code == 3U || code == 7U) { continue; }
        reset((uint8_t)((code << 5) | 5U), 0U); step(); steps(16U);
        assert(writes == 0U && !state().keep_5v_verified && state().power_setup_result == IP5310_ERROR_DATA);
        assert(state().snapshot_valid && state().link == BATTERY_LINK_READY);
        assert(state().faults == BATTERY_FAULT_TEMP_UNCONNECTED);
        regs[0x02] = 0x65U; steps(8U);
        assert(writes == 0U && state().power_setup_result == IP5310_ERROR_VERIFY);
    }
    reset(0x6DU, 0U); step();
    regs[0x02] = 0x6DU; /* Silent reset/drift: report it without endless writes. */
    steps(20U);
    assert(writes == 1U && !state().keep_5v_verified && state().power_config_valid);
    assert(state().power_setup_result == IP5310_ERROR_VERIFY && regs[0x02] == 0x6DU);
    awake = 0U; step(); awake = 1U; step(); step();
    assert(writes == 2U && state().keep_5v_verified);
    assert(state().power_config_before.raw02 == 0x6DU);
}
static void test_failures_are_bounded_and_do_not_change_charge(void)
{
    unsigned before;
    reset(0x6DU, 0U); fail_write = 1; step(); steps(16U);
    assert(writes == 1U && !state().keep_5v_verified && regs[0x00] == 0x30U);
    fail_write = 0; steps(8U);
    assert(writes == 1U && !state().keep_5v_verified); /* No replay after NACK clears. */
    reset(0x6DU, 0U); ignore_write = 1; step(); steps(16U);
    assert(writes == 1U && !state().keep_5v_verified && state().power_setup_result == IP5310_ERROR_VERIFY);
    assert(state().snapshot_valid && regs[0x00] == 0x30U);
    reset(0x6DU, 0U); fail_after_write = 1; step();
    assert(writes == 1U && !state().power_config_valid && state().power_setup_result == IP5310_ERROR_IO);
    assert(regs[0x02] == 0xEDU && regs[0x00] == 0x30U);
    fail_after_write = 0; steps(4U);
    assert(writes == 1U && state().keep_5v_verified); /* Later reads can confirm a committed write. */
    reset(0x6DU, 0U); fail_after_write = 1; step();
    fail_after_write = 0; regs[0x02] = 0x6DU; steps(16U);
    assert(writes == 1U && !state().keep_5v_verified); /* Unknown write outcome never permits replay. */
    reset(0x6DU, 0U); fail_read = 0x01; step();
    assert(!state().power_config_valid && !state().power_config_before_valid && writes == 0U);
    assert(state().snapshot_valid && state().power_setup_result == IP5310_ERROR_IO);
    before = reads_at[1];
    steps(3U); assert(reads_at[1] == before && writes == 0U);
    step(); assert(reads_at[1] == before + 1U && writes == 0U); /* Failed reads retry only once a second. */
    fail_read = -1; before = reads_at[1];
    steps(3U); assert(reads_at[1] == before && writes == 0U);
    step();
    assert(writes == 1U && state().keep_5v_verified && state().power_config_before_valid);
    assert(state().power_config_before.raw02 == 0x6DU && regs[0x00] == 0x30U);
    steps(16U); assert(writes == 1U); /* Successful read recovery allows one setup, then reads only. */
    reset(0xFFU, 0U); regs[0x00] = regs[0x01] = regs[0x0e] = 0xFFU; step();
    assert(writes == 0U && !state().power_config_valid && !state().power_config_before_valid);
    assert(state().power_setup_result == IP5310_ERROR_DATA && state().snapshot_valid);
    regs[0x00] = 0x30U; regs[0x01] = 0x11U; regs[0x02] = 0x6DU; regs[0x0e] = 0x44U;
    steps(16U); assert(writes == 0U && !state().keep_5v_verified);
}
static void test_driver_preflight_read_can_retry_before_write(void)
{
    unsigned before;
    reset(0x6DU, 0U);
    fail_read = 0x01; fail_read_call = 2U; /* App read succeeds; driver's preflight NACKs. */
    step();
    assert(writes == 0U && state().power_config_before_valid && !state().power_config_valid);
    assert(state().power_setup_result == IP5310_ERROR_IO && state().snapshot_valid);
    before = reads_at[1];
    steps(3U); assert(reads_at[1] == before && writes == 0U);
    step();
    assert(writes == 1U && state().keep_5v_verified && state().power_setup_result == IP5310_OK);
    assert(regs[0x02] == 0xEDU && regs[0x00] == 0x30U);
    steps(16U); assert(writes == 1U);
}
static void test_snapshot_recovery_and_staleness(void)
{
    unsigned before_reads;
    reset(0x6DU, 0U); fail_write = 1; step();
    fail_write = 0; fail_read = 0x70; step();
    assert(state().link == BATTERY_LINK_BUS_ERROR && !state().power_config_valid);
    fail_read = -1; step();
    assert(writes == 2U && state().keep_5v_verified); /* One new attempt after real status-link recovery. */
    before_reads = reads;
    now += BATTERY_STALE_MS + 1U;
    assert(state().link == BATTERY_LINK_STALE && !state().keep_5v_verified && !state().power_config_valid);
    assert(reads == before_reads);
    Battery_Task(); assert(state().keep_5v_verified && writes == 2U);
}
static void test_sleep_and_mid_transaction_edges(void)
{
    unsigned before_reads, before_writes;
    int returns_high;
    reset(0x6DU, 0U); step();
    before_reads = reads; before_writes = writes;
    awake = 0U; steps(12U);
    assert(reads == before_reads && writes == before_writes && !state().keep_5v_verified);
    assert(state().link == BATTERY_LINK_STANDBY); /* No periodic wake or KEY operation. */
    regs[0x02] = 0x6DU; awake = 1U; step(); assert(reads == before_reads);
    step(); assert(writes == 2U && state().keep_5v_verified);
    for (returns_high = 0; returns_high <= 1; ++returns_high) {
        reset(0x6DU, 0U); sleep_read = 0x01; sleep_high = returns_high; step();
        assert(state().link == (returns_high ? BATTERY_LINK_STARTING : BATTERY_LINK_STANDBY));
        assert(!state().snapshot_valid && writes == 0U && state().bus_error_count == 0U);
        assert(state().last_io_failure.reg == 0x01U && state().last_io_failure.error == BSP_BATTERY_IO_ASLEEP);
        before_reads = reads;
        sleep_read = -1; awake = 1U; step(); assert(reads == before_reads);
        step(); assert(writes == 1U && state().keep_5v_verified);
    }
    reset(0x6DU, 0U); sleep_write = 0x02; step();
    assert(writes == 1U && state().link == BATTERY_LINK_STANDBY && !state().snapshot_valid);
    assert(regs[0x00] == 0x30U && !state().keep_5v_verified);
}
static void test_millisecond_wrap(void)
{
    unsigned before;
    reset(0x6DU, 0xFFFFFF00UL); step();
    assert(state().keep_5v_verified && writes == 1U);
    before = reads_at[1]; steps(3U); assert(reads_at[1] == before);
    step(); assert(reads_at[1] == before + 1U && state().keep_5v_verified && writes == 1U);
}
int main(void)
{
    test_setup_and_read_only_cadence(); test_unknown_and_drift_never_rewrite();
    test_failures_are_bounded_and_do_not_change_charge(); test_snapshot_recovery_and_staleness();
    test_driver_preflight_read_can_retry_before_write();
    test_sleep_and_mid_transaction_edges(); test_millisecond_wrap();
    puts("Battery keep-5V tests passed: one bounded setup, 1 s readback, unknown/drift/failure no replay, charge preservation, real wake/recovery and tick wrap.");
    return 0;
}
