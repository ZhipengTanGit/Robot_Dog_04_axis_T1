#ifndef BATTERY_SETTINGS_H
#define BATTERY_SETTINGS_H
#include <stdint.h>

/* STM32F103RB: last two 1 KiB pages, excluded from the application linker.
 * Append-only records, CRC and a commit halfword written last. A page holding
 * the newest record is never erased while committing its replacement. */
#define BATTERY_SETTINGS_BASE 0x0801F800UL
#define BATTERY_SETTINGS_PAGE_BYTES 1024UL
#define BATTERY_SETTINGS_END 0x08020000UL
int Battery_SettingsLoad(uint16_t *nominal_ma); /* 1 found, 0 no valid record */
int Battery_SettingsSave(uint16_t nominal_ma); /* 0 verified, -1 failed */

/* Main-loop only, implemented by BSP; writes cannot touch application flash. */
uint16_t BSP_SettingsRead(uint32_t address);
int BSP_SettingsErase(uint32_t page_address);
int BSP_SettingsProgram(uint32_t address, uint16_t value);
#endif
