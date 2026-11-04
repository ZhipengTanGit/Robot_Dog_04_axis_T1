#include "IP5310.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t regs[256];
static uint8_t written_reg[128], written_value[128];
static unsigned write_count, read_count;
static int fail_read_at, fail_write_reg, ignore_write_reg;
static int fail_write_at, fail_after_commit_at;
static int all_ff;
static unsigned ff_read_first, ff_read_last;
static int mutate_read_at, mutate_reg;
static uint8_t mutate_mask;

static int mock_read(uint8_t reg, uint8_t *value)
{
    ++read_count;
    if ((int)read_count == fail_read_at) { return -1; }
    if ((int)read_count == mutate_read_at) { regs[mutate_reg] ^= mutate_mask; }
    *value = (all_ff || (ff_read_first != 0u && read_count >= ff_read_first &&
                       read_count <= ff_read_last)) ? 0xffu : regs[reg];
    return 0;
}

static int mock_write(uint8_t reg, uint8_t value)
{
    assert(write_count < sizeof(written_reg));
    written_reg[write_count] = reg;
    written_value[write_count++] = value;
    if (reg == fail_write_reg || (int)write_count == fail_write_at) { return -1; }
    if (reg != ignore_write_reg) { regs[reg] = value; }
    if ((int)write_count == fail_after_commit_at) { return -1; }
    return 0;
}

static const IP5310_Device device = { mock_read, mock_write };

static void reset_mock(void)
{
    memset(regs, 0, sizeof(regs));
    memset(written_reg, 0, sizeof(written_reg));
    memset(written_value, 0, sizeof(written_value));
    write_count = 0u;
    read_count = 0u;
    fail_read_at = -1;
    fail_write_reg = -1;
    ignore_write_reg = -1;
    fail_write_at = -1;
    fail_after_commit_at = -1;
    all_ff = 0;
    ff_read_first = ff_read_last = 0u;
    mutate_read_at = mutate_reg = -1;
    mutate_mask = 0u;
    regs[0x00] = 0xffu;
    regs[0x0a] = 0xffu;
    regs[0x22] = 0xffu;
    regs[0x23] = 0xffu;
    regs[0x24] = 0xffu;
    regs[0x25] = 0xffu;
    regs[0x26] = 0xffu;
    regs[0x74] = 0x30u;
}

static void test_decode(void)
{
    static const uint8_t codes[] = { 0u, 1u, 3u, 7u, 15u };
    static const uint8_t mins[] = { 0u, 3u, 25u, 50u, 75u };
    static const uint8_t maxs[] = { 3u, 25u, 50u, 75u, 100u };
    IP5310_Snapshot s;
    unsigned i;
    reset_mock();
    regs[0x70] = 8u;
    regs[0x71] = 8u;
    regs[0x72] = 4u;
    regs[0x77] = 3u;
    regs[0x83] = 0x20u;
    for (i = 0; i < sizeof(codes); ++i) {
        regs[0x74] = (uint8_t)((codes[i] << 4) | 5u);
        assert(IP5310_ReadSnapshot(&device, &s) == IP5310_OK);
        assert(s.soc_valid && s.soc_min_pct == mins[i] && s.soc_max_pct == maxs[i]);
        assert(s.raw74 == regs[0x74]);
        assert(s.charge_enabled && s.charge_active && s.charge_full && s.light_load);
        assert(s.key_short && s.key_long && s.typec_sink && !s.typec_source);
    }
    for (i = 0; i < 16u; ++i) {
        if (i == 0u || i == 1u || i == 3u || i == 7u || i == 15u) { continue; }
        regs[0x74] = (uint8_t)(i << 4);
        assert(IP5310_ReadSnapshot(&device, &s) == IP5310_OK);
        assert(!s.soc_valid);
    }
    regs[0x76] = 0x70u;
    assert(IP5310_ReadSnapshot(&device, &s) == IP5310_OK && !s.ntc_valid);
    regs[0x76] = 0x60u;
    assert(IP5310_ReadSnapshot(&device, &s) == IP5310_OK && s.ntc_valid);
    assert(s.ntc_high && s.ntc_mid && !s.ntc_low);
    assert(write_count == 0u); /* Reading key flags has no W1C side effect. */
}

static void test_atomic_snapshot(void)
{
    IP5310_Snapshot before, after;
    unsigned i;
    for (i = 1u; i <= 8u; ++i) {
        reset_mock();
        memset(&before, 0xa5, sizeof(before));
        after = before;
        fail_read_at = (int)i;
        assert(IP5310_ReadSnapshot(&device, &after) == IP5310_ERROR_IO);
        assert(memcmp(&before, &after, sizeof(before)) == 0);
    }
    reset_mock();
    all_ff = 1;
    before = after;
    assert(IP5310_ReadSnapshot(&device, &after) == IP5310_ERROR_DATA);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
}

static void test_profiles_and_preservation(void)
{
    IP5310_ChargeConfiguration config;
    unsigned i;
    reset_mock();
    assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_OK);
    assert(written_reg[0] == 0x00u && !(written_value[0] & 0x10u));
    assert(written_reg[write_count - 1u] == 0x00u &&
           (written_value[write_count - 1u] & 0x10u));
    for (i = 1u; i + 1u < write_count; ++i) {
        assert(written_reg[i] != 0x00u); /* No enable before profile completion. */
    }
    assert(regs[0x00] == 0xffu); /* Boost, key-related, reserved bits preserved. */
    assert(regs[0x0a] == 0x7fu && regs[0x22] == 0xf0u);
    assert(regs[0x25] == 0xefu && regs[0x23] == 0xdfu);
    assert(regs[0x24] == 0xe6u && regs[0x26] == 0xe5u);
    assert(IP5310_ReadChargeConfiguration(&device, &config) == IP5310_OK);
    assert(IP5310_ChargeConfigurationMatches(&config, 650u, 1u));
    assert(IP5310_VerifyChargeProfile(&device, 650u, 1u) == IP5310_OK);
    assert(IP5310_VerifyChargeProfile(&device, 250u, 1u) == IP5310_ERROR_VERIFY);
    assert(!IP5310_ChargeConfigurationMatches(&config, 250u, 1u));

    assert(IP5310_ApplyChargeProfile(&device, 850u, 1u) == IP5310_OK);
    assert(regs[0x24] == 0xe8u && regs[0x26] == 0xe7u);
    assert(IP5310_VerifyChargeProfile(&device, 850u, 1u) == IP5310_OK);

    assert(IP5310_ApplyChargeProfile(&device, 1250u, 1u) == IP5310_OK);
    assert(regs[0x24] == 0xecu && regs[0x26] == 0xe8u);
    assert(IP5310_VerifyChargeProfile(&device, 1250u, 1u) == IP5310_OK);

    assert(IP5310_ApplyChargeProfile(&device, 1850u, 1u) == IP5310_OK);
    assert(regs[0x24] == 0xf2u && regs[0x26] == 0xedu);
    assert(IP5310_VerifyChargeProfile(&device, 1850u, 1u) == IP5310_OK);

    assert(IP5310_ApplyChargeProfile(&device, 250u, 0u) == IP5310_OK);
    assert(regs[0x24] == 0xe2u && regs[0x26] == 0xe2u);
    assert(!(regs[0x00] & 0x10u));
    assert(IP5310_ReadChargeConfiguration(&device, &config) == IP5310_OK);
    assert(IP5310_ChargeConfigurationMatches(&config, 250u, 0u));
    config.raw25 |= 0x10u;
    assert(!IP5310_ChargeConfigurationMatches(&config, 250u, 0u));
    config.raw25 &= (uint8_t)~0x10u;
    config.raw23 |= 0x20u;
    assert(!IP5310_ChargeConfigurationMatches(&config, 250u, 0u));
}

static void test_failures_never_enable(void)
{
    static const uint8_t targets[] = { 0x22u, 0x0au, 0x25u, 0x23u, 0x24u, 0x26u };
    unsigned i, j;
    for (i = 0u; i < sizeof(targets); ++i) {
        reset_mock();
        fail_write_reg = targets[i];
        assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_ERROR_IO);
        assert(!(regs[0x00] & 0x10u));
        for (j = 0u; j < write_count; ++j) {
            assert(written_reg[j] != 0x00u || !(written_value[j] & 0x10u));
        }
        reset_mock();
        ignore_write_reg = targets[i];
        assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_ERROR_VERIFY);
        assert(!(regs[0x00] & 0x10u));
    }
    reset_mock();
    ignore_write_reg = 0x00u;
    assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_ERROR_VERIFY);
    assert(regs[0x00] & 0x10u); /* Failure is NOT a guarantee that charging stopped. */
    assert(write_count == 2u); /* Initial stop and best-effort retry; no profile writes. */
    reset_mock();
    fail_read_at = 1;
    assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_ERROR_IO);
    assert(!(regs[0x00] & 0x10u)); /* Best effort can recover a transient read error. */
}

static void test_arguments_and_config_atomicity(void)
{
    IP5310_ChargeConfiguration before, after;
    IP5310_Device readonly = { mock_read, NULL };
    unsigned i;
    reset_mock();
    assert(IP5310_ApplyChargeProfile(&device, 700u, 1u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ApplyChargeProfile(&device, 280u, 1u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ApplyChargeProfile(&device, 650u, 2u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_SetChargeEnabled(&readonly, 0u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_SetChargeEnabled(NULL, 0u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ReadSnapshot(&device, NULL) == IP5310_ERROR_ARGUMENT);
    assert(write_count == 0u && read_count == 0u);
    memset(&before, 0xa5, sizeof(before));
    for (i = 1u; i <= 7u; ++i) {
        reset_mock();
        after = before;
        fail_read_at = (int)i;
        assert(IP5310_ReadChargeConfiguration(&device, &after) == IP5310_ERROR_IO);
        assert(memcmp(&before, &after, sizeof(before)) == 0);
    }
    reset_mock();
    all_ff = 1;
    after = before;
    assert(IP5310_ReadChargeConfiguration(&device, &after) == IP5310_ERROR_DATA);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
}

static void test_every_profile_read_failure(void)
{
    unsigned total_reads, total_writes, i;
    reset_mock();
    assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_OK);
    total_reads = read_count;
    total_writes = write_count;
    for (i = 1u; i <= total_reads; ++i) {
        reset_mock();
        fail_read_at = (int)i;
        assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_ERROR_IO);
        assert(!(regs[0x00] & 0x10u));
    }
    reset_mock();
    fail_write_at = (int)total_writes; /* Failure of the final enable write. */
    assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_ERROR_IO);
    assert(!(regs[0x00] & 0x10u));
    reset_mock();
    fail_after_commit_at = (int)total_writes; /* IC accepted it, ACK was lost. */
    assert(IP5310_ApplyChargeProfile(&device, 650u, 1u) == IP5310_ERROR_IO);
    assert(!(regs[0x00] & 0x10u)); /* Recovery explicitly revokes permission. */
}

static void reset_vbus_mock(void)
{
    reset_mock();
    /* Deliberately non-default settings must survive a VBUS-only update. */
    regs[0x00] = 0xf8u;
    regs[0x22] = 0xa6u;
    regs[0x23] = 0xbdu;
    regs[0x24] = 0xcfu;
    regs[0x25] = 0xd9u;
    regs[0x20] = 0x53u;
    regs[0x76] = 0x60u;
}

static void test_vbus_current_and_preservation(void)
{
    static const uint16_t currents[] = { 250u, 650u, 850u, 1250u, 1850u };
    static const uint8_t codes[] = { 2u, 5u, 7u, 8u, 13u };
    uint8_t before[256];
    IP5310_ChargeConfiguration config;
    unsigned i, allow, reg, writes_before;
    for (i = 0u; i < sizeof(currents) / sizeof(currents[0]); ++i) {
        for (allow = 0u; allow <= 1u; ++allow) {
            reset_vbus_mock();
            if (!allow) { regs[0x00] &= (uint8_t)~0x10u; }
            memcpy(before, regs, sizeof(before));
            assert(IP5310_ApplyVbusCurrent(&device, currents[i]) == IP5310_OK);
            assert(write_count == 1u && written_reg[0] == 0x26u);
            assert(regs[0x26] == (uint8_t)(0xe0u | codes[i]));
            for (reg = 0u; reg < sizeof(regs); ++reg) {
                if (reg != 0x26u) { assert(regs[reg] == before[reg]); }
            }
            assert(IP5310_ReadChargeConfiguration(&device, &config) == IP5310_OK);
            assert(IP5310_VbusCurrentMatches(&config, currents[i]));
            assert(!IP5310_VbusCurrentMatches(&config,
                currents[(i + 1u) % (sizeof(currents) / sizeof(currents[0]))]));
            writes_before = write_count;
            assert(IP5310_ApplyVbusCurrent(&device, currents[i]) == IP5310_OK);
            assert(write_count == writes_before); /* Already applied: read only. */
        }
    }
}

static void test_all_vbus_encodings(void)
{
    unsigned code, previous, value, matches;
    uint8_t encoded = 0xffu;
    uint8_t before[256];
    IP5310_ChargeConfiguration config;
    for (code = 0u; code < 32u; ++code) {
        uint16_t nominal = (uint16_t)(50u + ((code & 1u) ? 100u : 0u) +
            ((code & 2u) ? 200u : 0u) + ((code & 4u) ? 500u : 0u) +
            ((code & 8u) ? 1200u : 0u) + ((code & 16u) ? 2300u : 0u));
        assert(IP5310_VbusNominalFromCode((uint8_t)code) == nominal);
        assert(IP5310_VbusCodeFromNominal(nominal, &encoded) == IP5310_OK && encoded == code);
        for (previous = 0u; previous < code; ++previous) {
            assert(IP5310_VbusNominalFromCode((uint8_t)previous) != nominal);
        }
        reset_vbus_mock();
        regs[0x26] = (uint8_t)(0xe0u | ((code + 1u) & 31u));
        memcpy(before, regs, sizeof(before));
        assert(IP5310_ApplyVbusCurrent(&device, nominal) == IP5310_OK);
        assert(write_count == 1u && written_reg[0] == 0x26u && regs[0x26] == (0xe0u | code));
        for (value = 0u; value < sizeof(regs); ++value) { if (value != 0x26u) { assert(regs[value] == before[value]); } }
        assert(IP5310_ReadChargeConfiguration(&device, &config) == IP5310_OK);
        assert(IP5310_VbusCurrentMatches(&config, nominal));
    }
    for (value = 0u; value <= 4400u; ++value) {
        matches = 0u;
        for (code = 0u; code < 32u; ++code) { if (IP5310_VbusNominalFromCode((uint8_t)code) == value) { ++matches; } }
        assert((IP5310_VbusCodeFromNominal((uint16_t)value, &encoded) == IP5310_OK) == (matches == 1u));
    }
    assert(IP5310_VbusNominalFromCode(32u) == 0u);
    assert(IP5310_VbusNominalFromCode(255u) == 0u);
    assert(IP5310_VbusCodeFromNominal(650u, NULL) == IP5310_ERROR_ARGUMENT);
    reset_vbus_mock();
    assert(IP5310_ApplyChargeProfile(&device, 4350u, 1u) == IP5310_ERROR_ARGUMENT && write_count == 0u);
}

static void test_vbus_failures(void)
{
    unsigned total_reads, i;
    reset_vbus_mock();
    assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_OK);
    total_reads = read_count;
    for (i = 1u; i <= total_reads; ++i) {
        reset_vbus_mock();
        fail_read_at = (int)i;
        assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_ERROR_IO);
        assert(!(regs[0x00] & 0x10u));
    }
    reset_vbus_mock();
    fail_write_at = 1;
    assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_ERROR_IO);
    assert(!(regs[0x00] & 0x10u));
    assert(regs[0x26] == 0xffu);
    reset_vbus_mock();
    fail_after_commit_at = 1;
    assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_ERROR_IO);
    assert(!(regs[0x00] & 0x10u));
    assert(regs[0x26] == 0xe5u); /* ACK lost after IC accepted the write. */
    reset_vbus_mock();
    ignore_write_reg = 0x26;
    assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_ERROR_VERIFY);
    assert(!(regs[0x00] & 0x10u));
    reset_vbus_mock();
    fail_write_reg = 0x26;
    ignore_write_reg = 0x00;
    assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_ERROR_IO);
    assert(regs[0x00] & 0x10u); /* Original failure retained; OFF not guaranteed. */
    for (i = 0u; i < write_count; ++i) {
        assert(written_reg[i] != 0x00u || !(written_value[i] & 0x10u));
    }

    reset_vbus_mock();
    ff_read_first = 1u;
    ff_read_last = 7u;
    assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_ERROR_DATA);
    assert(write_count == 1u && written_reg[0] == 0x00u);
    assert(!(regs[0x00] & 0x10u));
    reset_vbus_mock();
    ff_read_first = 10u; /* Complete final configuration read returns 0xFF. */
    ff_read_last = 16u;
    assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_ERROR_DATA);
    assert(!(regs[0x00] & 0x10u));
}

static void test_vbus_unexpected_change_and_arguments(void)
{
    static const uint8_t config_regs[] = { 0x00u, 0x0au, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u };
    IP5310_Device readonly = { mock_read, NULL };
    IP5310_ChargeConfiguration config;
    unsigned i;
    for (i = 0u; i < sizeof(config_regs); ++i) {
        reset_vbus_mock();
        mutate_read_at = 10; /* Before final complete read, after immediate verify. */
        mutate_reg = config_regs[i];
        mutate_mask = 0x80u;
        assert(IP5310_ApplyVbusCurrent(&device, 650u) == IP5310_ERROR_VERIFY);
        assert(!(regs[0x00] & 0x10u));
    }
    reset_vbus_mock();
    memset(&config, 0, sizeof(config));
    assert(!IP5310_VbusCurrentMatches(NULL, 650u));
    assert(!IP5310_VbusCurrentMatches(&config, 700u));
    assert(IP5310_ApplyVbusCurrent(NULL, 650u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ApplyVbusCurrent(&readonly, 650u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ApplyVbusCurrent(&device, 700u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ApplyVbusCurrent(&device, 280u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ApplyVbusCurrent(&device, 1000u) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ApplyVbusCurrent(&device, 2000u) == IP5310_ERROR_ARGUMENT);
    assert(read_count == 0u && write_count == 0u);
}

static void reset_power_mock(uint8_t value)
{
    reset_mock();
    regs[0x00] = 0x38u;
    regs[0x01] = 0x15u;
    regs[0x02] = value;
    regs[0x0e] = 0x12u;
}

static void test_power_read_and_preservation(void)
{
    IP5310_PowerConfiguration before, after;
    uint8_t original[256];
    unsigned low, i;
    for (low = 0; low < 32u; ++low) {
        reset_power_mock((uint8_t)(0x60u | low));
        memcpy(original, regs, sizeof(original));
        assert(IP5310_ReadPowerConfiguration(&device, &before) == IP5310_OK);
        assert(!IP5310_PowerConfigurationKeepsAwake(&before));
        assert(IP5310_DisableLightLoadShutdown(&device) == IP5310_OK);
        assert(write_count == 1u && written_reg[0] == 0x02u);
        assert(regs[0x02] == (uint8_t)(0xe0u | low));
        for (i = 0; i < 256u; ++i) if (i != 2u) assert(regs[i] == original[i]);
        assert(IP5310_ReadPowerConfiguration(&device, &after) == IP5310_OK);
        assert(IP5310_PowerConfigurationKeepsAwake(&after));
        assert(IP5310_DisableLightLoadShutdown(&device) == IP5310_OK);
        assert(write_count == 1u); /* Verification never writes an unchanged code. */
    }
    for (i = 1; i <= 4; ++i) {
        reset_power_mock(0x60u); memset(&before, 0xa5, sizeof(before)); after = before;
        fail_read_at = (int)i;
        assert(IP5310_ReadPowerConfiguration(&device, &after) == IP5310_ERROR_IO);
        assert(!memcmp(&before, &after, sizeof(before)) && write_count == 0);
    }
    reset_power_mock(0x60u); all_ff = 1; after = before;
    assert(IP5310_ReadPowerConfiguration(&device, &after) == IP5310_ERROR_DATA);
    assert(!memcmp(&before, &after, sizeof(before)));
    assert(IP5310_DisableLightLoadShutdown(&device) == IP5310_ERROR_DATA && !write_count);
}

static void test_power_failure_never_changes_other_settings(void)
{
    IP5310_Device readonly = {mock_read, NULL};
    uint8_t write_attempted;
    unsigned code, i;
    for (code = 0; code < 8; ++code) {
        if (code == 3 || code == 7) continue;
        reset_power_mock((uint8_t)(code << 5));
        assert(IP5310_DisableLightLoadShutdown(&device) == IP5310_ERROR_DATA && !write_count);
    }
    for (i = 1; i <= 5; ++i) {
        reset_power_mock(0x73u); fail_read_at = (int)i;
        write_attempted = 99u;
        assert(IP5310_DisableLightLoadShutdownTracked(&device, &write_attempted) == IP5310_ERROR_IO);
        assert(write_attempted == (i == 5 ? 1u : 0u));
        assert(write_count == (i == 5 ? 1u : 0u));
        assert(regs[0] == 0x38u && regs[1] == 0x15u && regs[0x26] == 0xffu);
    }
    reset_power_mock(0x73u); fail_write_reg = 2;
    assert(IP5310_DisableLightLoadShutdownTracked(&device, &write_attempted) == IP5310_ERROR_IO && write_count == 1u);
    assert(write_attempted == 1u);
    reset_power_mock(0x73u); fail_after_commit_at = 1;
    assert(IP5310_DisableLightLoadShutdown(&device) == IP5310_ERROR_IO && write_count == 1u);
    reset_power_mock(0x73u); ignore_write_reg = 2;
    assert(IP5310_DisableLightLoadShutdown(&device) == IP5310_ERROR_VERIFY && write_count == 1u);
    reset_power_mock(0x73u); mutate_read_at = 5; mutate_reg = 2; mutate_mask = 1;
    assert(IP5310_DisableLightLoadShutdown(&device) == IP5310_ERROR_VERIFY); /* Reserved-bit corruption is visible. */
    assert(IP5310_DisableLightLoadShutdown(NULL) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_DisableLightLoadShutdown(&readonly) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ReadPowerConfiguration(NULL, NULL) == IP5310_ERROR_ARGUMENT);
    assert(IP5310_ReadPowerConfiguration(&device, NULL) == IP5310_ERROR_ARGUMENT);
    assert(!IP5310_PowerConfigurationKeepsAwake(NULL));
}

int main(void)
{
    test_decode();
    test_atomic_snapshot();
    test_profiles_and_preservation();
    test_failures_never_enable();
    test_arguments_and_config_atomicity();
    test_every_profile_read_failure();
    test_vbus_current_and_preservation();
    test_all_vbus_encodings();
    test_vbus_failures();
    test_vbus_unexpected_change_and_arguments();
    test_power_read_and_preservation();
    test_power_failure_never_changes_other_settings();
    puts("IP5310 mock tests passed");
    return 0;
}
