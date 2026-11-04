#ifndef SETTINGS_FLASH_MODEL_H
#define SETTINGS_FLASH_MODEL_H
#include "Battery_Settings.h"
#include <assert.h>
#include <string.h>
static uint16_t settings_flash[1024];
static int flash_budget = -1;
static unsigned flash_programs, flash_erases;
static void reset_flash(void)
{
    memset(settings_flash, 0xFF, sizeof(settings_flash));
    flash_budget = -1;
    flash_programs = flash_erases = 0;
}
uint16_t BSP_SettingsRead(uint32_t address)
{
    assert(address >= BATTERY_SETTINGS_BASE && address < BATTERY_SETTINGS_END && !(address & 1UL));
    return settings_flash[(address - BATTERY_SETTINGS_BASE) / 2UL];
}
static int flash_operation(void)
{
    if (!flash_budget) { return -1; }
    if (flash_budget > 0) { --flash_budget; }
    return 0;
}
int BSP_SettingsErase(uint32_t address)
{
    assert(address == BATTERY_SETTINGS_BASE || address == BATTERY_SETTINGS_BASE + 1024UL);
    if (flash_operation()) { return -1; }
    ++flash_erases;
    memset(&settings_flash[(address - BATTERY_SETTINGS_BASE) / 2UL], 0xFF, 1024);
    return 0;
}
int BSP_SettingsProgram(uint32_t address, uint16_t value)
{
    unsigned index = (unsigned)((address - BATTERY_SETTINGS_BASE) / 2UL);
    assert(BSP_SettingsRead(address) == 0xFFFFU);
    if (flash_operation()) { return -1; }
    ++flash_programs;
    settings_flash[index] = value;
    return 0;
}
#endif
