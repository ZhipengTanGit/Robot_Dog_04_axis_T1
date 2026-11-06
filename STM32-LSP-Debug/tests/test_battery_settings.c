#include "settings_flash_model.h"
#include <stdio.h>

static void expect(uint16_t ma)
{
    uint16_t actual = 0;
    assert(Battery_SettingsLoad(&actual) == 1 && actual == ma);
}

int main(void)
{
    uint16_t actual, snapshot[1024];
    int budget;
    unsigned i, before;
    reset_flash();
    assert(!Battery_SettingsLoad(&actual));
    assert(Battery_SettingsSave(1000U) == -1);
    assert(Battery_SettingsSave(1950U) == -1);
    assert(Battery_SettingsSave(350U) == 0); expect(350U);
    before = flash_programs;
    assert(Battery_SettingsSave(350U) == 0 && flash_programs == before);
    memcpy(snapshot, settings_flash, sizeof(snapshot));
    /* Interruption at each halfword (including final commit) keeps old data. */
    for (budget = 0; budget < 8; ++budget) {
        memcpy(settings_flash, snapshot, sizeof(snapshot));
        flash_budget = budget;
        assert(Battery_SettingsSave(1850U) == -1); expect(350U);
        flash_budget = -1;
        assert(Battery_SettingsSave(1850U) == 0); expect(1850U);
    }
    reset_flash();
    for (i = 0; i < 64U; ++i) { assert(Battery_SettingsSave(i & 1U ? 1850U : 350U) == 0); }
    expect(1850U);
    memcpy(snapshot, settings_flash, sizeof(snapshot));
    /* Rollover: failure at erase or any program step cannot lose newest page. */
    for (budget = 0; budget < 9; ++budget) {
        memcpy(settings_flash, snapshot, sizeof(snapshot));
        flash_budget = budget;
        assert(Battery_SettingsSave(650U) == -1); expect(1850U);
        flash_budget = -1;
        assert(Battery_SettingsSave(650U) == 0); expect(650U);
    }
    reset_flash();
    /* Multiple rotations, then corrupt newest record: CRC rejects it. */
    for (i = 0; i < 192U; ++i) { assert(Battery_SettingsSave(i & 1U ? 1850U : 350U) == 0); }
    expect(1850U);
    settings_flash[63U * 8U + 6U] ^= 1U;
    expect(350U);
    puts("Settings journal passed: torn writes, rollover, corruption, deduplication.");
    return 0;
}
