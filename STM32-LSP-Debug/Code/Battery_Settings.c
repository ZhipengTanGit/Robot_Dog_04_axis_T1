#include "Battery_Settings.h"
#include "Battery_Config.h"
#include "IP5310.h"

#define RECORD_BYTES 16UL
#define RECORD_WORDS 8U
#define RECORD_MAGIC 0xB185U
#define RECORD_VERSION 1U
#define RECORD_COMMIT 0xA55AU

static int supported(uint16_t ma)
{
    uint8_t code;
    return ma <= BATTERY_BOARD_MAX_NOMINAL_MA &&
        IP5310_VbusCodeFromNominal(ma, &code) == IP5310_OK;
}

static uint16_t crc(const uint16_t *record)
{
    uint16_t value = 0xFFFFU;
    unsigned i, bit;
    for (i = 0; i < 6U; ++i) {
        value ^= record[i];
        for (bit = 0; bit < 16U; ++bit) {
            value = (uint16_t)((value & 0x8000U) ?
                ((uint32_t)value << 1) ^ 0x1021U : (uint32_t)value << 1);
        }
    }
    return value;
}

static void read_record(uint32_t address, uint16_t *record)
{
    unsigned i;
    for (i = 0; i < RECORD_WORDS; ++i) {
        record[i] = BSP_SettingsRead(address + i * 2UL);
    }
}

static int valid(const uint16_t *r)
{
    return r[0] == RECORD_MAGIC && r[1] == RECORD_VERSION &&
        (uint32_t)r[3] + (uint32_t)r[2] == 0xFFFFUL && supported(r[2]) &&
        r[6] == crc(r) && r[7] == RECORD_COMMIT;
}

static uint32_t sequence(const uint16_t *r)
{
    return (uint32_t)r[4] | ((uint32_t)r[5] << 16);
}

static uint32_t find_latest(uint16_t *ma, uint32_t *seq)
{
    uint32_t address, latest = 0, candidate;
    uint16_t r[RECORD_WORDS];
    *seq = 0;
    for (address = BATTERY_SETTINGS_BASE; address < BATTERY_SETTINGS_END; address += RECORD_BYTES) {
        read_record(address, r);
        candidate = sequence(r);
        if (valid(r) && (!latest || (candidate != *seq && (uint32_t)(candidate - *seq) < 0x80000000UL))) {
            latest = address;
            *seq = candidate;
            *ma = r[2];
        }
    }
    return latest;
}

int Battery_SettingsLoad(uint16_t *nominal_ma)
{
    uint32_t seq;
    if (!nominal_ma) { return 0; }
    return find_latest(nominal_ma, &seq) != 0;
}

static uint32_t empty_slot(uint32_t page)
{
    uint32_t address;
    unsigned i;
    uint16_t r[RECORD_WORDS];
    for (address = page; address < page + BATTERY_SETTINGS_PAGE_BYTES; address += RECORD_BYTES) {
        read_record(address, r);
        for (i = 0; i < RECORD_WORDS && r[i] == 0xFFFFU; ++i) { }
        if (i == RECORD_WORDS) { return address; }
    }
    return 0;
}

int Battery_SettingsSave(uint16_t nominal_ma)
{
    uint32_t latest, seq, page, address;
    uint16_t old_ma = 0, r[RECORD_WORDS], check[RECORD_WORDS];
    unsigned i;
    if (!supported(nominal_ma)) { return -1; }
    latest = find_latest(&old_ma, &seq);
    if (latest && old_ma == nominal_ma) { return 0; } /* No wear for repeats. */
    page = latest ? latest - (latest % BATTERY_SETTINGS_PAGE_BYTES) : BATTERY_SETTINGS_BASE;
    address = empty_slot(page);
    if (!address) {
        page = (page == BATTERY_SETTINGS_BASE) ? page + BATTERY_SETTINGS_PAGE_BYTES : BATTERY_SETTINGS_BASE;
        /* The newest committed record remains intact on the other page. */
        if (BSP_SettingsErase(page) != 0) { return -1; }
        address = page;
    }
    r[0] = RECORD_MAGIC; r[1] = RECORD_VERSION;
    r[2] = nominal_ma; r[3] = (uint16_t)(nominal_ma ^ 0xFFFFU);
    ++seq;
    r[4] = (uint16_t)seq; r[5] = (uint16_t)(seq >> 16);
    r[6] = crc(r); r[7] = RECORD_COMMIT;
    for (i = 0; i < RECORD_WORDS; ++i) {
        if (BSP_SettingsProgram(address + i * 2UL, r[i]) != 0) { return -1; }
    }
    read_record(address, check);
    return valid(check) && sequence(check) == seq && check[2] == nominal_ma ? 0 : -1;
}
