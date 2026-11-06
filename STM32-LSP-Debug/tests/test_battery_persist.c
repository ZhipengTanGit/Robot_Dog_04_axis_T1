#include "settings_flash_model.h"
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


int main(void)
{
    unsigned before;
    reset_flash();
    reset(0xf8U, 0xa3U); step();
    assert(status().control_verified && status().requested_nominal_ma == BATTERY_VBUS_NOMINAL_MA);
    assert(status().saved_nominal_ma == 0 && status().settings_result == 0);
    assert(Battery_RequestNominalCurrent(1950U) == BATTERY_CURRENT_BOARD_LIMIT);
    assert(Battery_RequestNominalCurrent(1850U) == BATTERY_CURRENT_ACCEPTED); step();
    assert(status().control_verified && status().applied_nominal_ma == 1850U);
    assert(status().settings_result == 1 && status().saved_nominal_ma == 1850U);
    before = flash_programs;
    assert(Battery_RequestNominalCurrent(1850U) == BATTERY_CURRENT_ACCEPTED); step();
    assert(flash_programs == before);
    /* Full MCU/charger restart preserves Flash but loses charger registers. */
    reset(0xf8U, 0xa3U); step();
    assert(status().requested_nominal_ma == 1850U && status().applied_nominal_ma == 1850U);
    assert(status().settings_result == 1 && status().saved_nominal_ma == 1850U);
    awake = 0; step(); awake = 1; regs[0x26] = 0xa3U; step(); step();
    assert(status().control_verified && status().applied_nominal_ma == 1850U);
    /* Reconnection/chip register reset while MCU remains powered. */
    regs[0x26] = 0xa3U; step();
    assert(status().control_verified && status().applied_nominal_ma == 1850U);
    assert(flash_programs == before);
    /* A failed I2C request never replaces the persisted value or retries. */
    assert(Battery_RequestNominalCurrent(650U) == BATTERY_CURRENT_ACCEPTED);
    ignore_write_reg = 0x26; step();
    assert(status().current_request_state == BATTERY_CURRENT_FAILED);
    before = current_writes; ignore_write_reg = -1; step(); step();
    assert(current_writes == before && status().saved_nominal_ma == 1850U);
    reset(0xe8U, 0xa3U); step();
    assert(status().applied_nominal_ma == 1850U && !(regs[0x00] & 0x10U));
    /* Flash failure is visible; reset restores previous successful record. */
    flash_budget = 3;
    assert(Battery_RequestNominalCurrent(650U) == BATTERY_CURRENT_ACCEPTED); step();
    assert(status().control_verified && status().applied_nominal_ma == 650U);
    assert(status().settings_result == -1 && status().saved_nominal_ma == 1850U);
    flash_budget = -1;
    reset(0xe8U, 0xa3U); step();
    assert(status().applied_nominal_ma == 1850U && status().settings_result == 1);
    assert(Battery_RequestNominalCurrent(250U) == BATTERY_CURRENT_ACCEPTED); step();
    assert(status().saved_nominal_ma == 250U);
    reset(0xe8U, 0xadU); step();
    assert(status().applied_nominal_ma == 250U && !enable_writes);
    puts("Persistent current passed: 1850mA, restart, reconnect, flash/I2C failures, no charge-enable writes.");
    return 0;
}
