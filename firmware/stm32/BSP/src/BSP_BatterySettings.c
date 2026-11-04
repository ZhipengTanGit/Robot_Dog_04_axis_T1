#include "Battery_Settings.h"
#include "stm32f10x.h"
#include "stm32f10x_flash.h"

uint16_t BSP_SettingsRead(uint32_t address)
{
    if (address < BATTERY_SETTINGS_BASE || address >= BATTERY_SETTINGS_END || (address & 1UL)) { return 0; }
    return *(volatile const uint16_t *)address;
}

int BSP_SettingsErase(uint32_t page_address)
{
    FLASH_Status result;
    uint32_t irq;
    if (page_address != BATTERY_SETTINGS_BASE &&
        page_address != BATTERY_SETTINGS_BASE + BATTERY_SETTINGS_PAGE_BYTES) { return -1; }
    irq = __get_PRIMASK();
    __disable_irq();
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    result = FLASH_ErasePage(page_address);
    FLASH_Lock();
    __set_PRIMASK(irq);
    return result == FLASH_COMPLETE ? 0 : -1;
}

int BSP_SettingsProgram(uint32_t address, uint16_t value)
{
    FLASH_Status result;
    uint32_t irq;
    if (address < BATTERY_SETTINGS_BASE || address >= BATTERY_SETTINGS_END || (address & 1UL) ||
        BSP_SettingsRead(address) != 0xFFFFU) { return -1; }
    irq = __get_PRIMASK();
    __disable_irq();
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    result = FLASH_ProgramHalfWord(address, value);
    FLASH_Lock();
    __set_PRIMASK(irq);
    return result == FLASH_COMPLETE && BSP_SettingsRead(address) == value ? 0 : -1;
}
