#include "IP5310.h"
#include <stddef.h>
#include <string.h>

static int readable(const IP5310_Device *dev)
{
    return dev != NULL && dev->read != NULL;
}

static int writable(const IP5310_Device *dev)
{
    return readable(dev) && dev->write != NULL;
}

static int read_reg(const IP5310_Device *dev, uint8_t reg, uint8_t *value)
{
    return dev->read(reg, value) == 0 ? IP5310_OK : IP5310_ERROR_IO;
}

/* Deliberately private: the application cannot pass arbitrary reserved masks. */
static int update_verified(const IP5310_Device *dev, uint8_t reg,
                           uint8_t mask, uint8_t bits)
{
    uint8_t before, after, check;
    int result = read_reg(dev, reg, &before);
    if (result != IP5310_OK) {
        return result;
    }
    after = (uint8_t)((before & (uint8_t)~mask) | (bits & mask));
    if (after != before && dev->write(reg, after) != 0) {
        return IP5310_ERROR_IO;
    }
    result = read_reg(dev, reg, &check);
    if (result != IP5310_OK) {
        return result;
    }
    return (check & mask) == (bits & mask) ? IP5310_OK : IP5310_ERROR_VERIFY;
}

static int profile_codes(uint16_t nominal_ma, uint8_t *vin, uint8_t *vbus)
{
    if (nominal_ma == 250u) {
        *vin = 2u;  /* 50 + 100 * 2 = 250 mA */
        *vbus = 2u; /* 50 + 200 = 250 mA */
    } else if (nominal_ma == 650u) {
        *vin = 6u;  /* 50 + 100 * 6 = 650 mA */
        *vbus = 5u; /* 50 + 100 + 500 = 650 mA */
    } else if (nominal_ma == 850u) {
        *vin = 8u;  /* 50 + 100 * 8 = 850 mA */
        *vbus = 7u; /* 50 + 100 + 200 + 500 = 850 mA */
    } else if (nominal_ma == 1250u) {
        *vin = 12u; /* 50 + 100 * 12 = 1250 mA */
        *vbus = 8u; /* 50 + 1200 = 1250 mA */
    } else if (nominal_ma == 1850u) {
        *vin = 18u;  /* 50 + 100 * 18 = 1850 mA */
        *vbus = 13u; /* 50 + 100 + 500 + 1200 = 1850 mA */
    } else {
        return IP5310_ERROR_ARGUMENT;
    }
    return IP5310_OK;
}

void IP5310_DecodeSnapshot(IP5310_Snapshot *s)
{
    uint8_t code;
    if (s == NULL) {
        return;
    }
    s->charge_enabled = (uint8_t)((s->raw00 >> 4) & 1u);
    s->charge_active = (uint8_t)((s->raw70 >> 3) & 1u);
    s->charge_full = (uint8_t)((s->raw71 >> 3) & 1u);
    s->light_load = (uint8_t)((s->raw72 >> 2) & 1u);
    s->ntc_high = (uint8_t)((s->raw76 >> 6) & 1u);
    s->ntc_mid = (uint8_t)((s->raw76 >> 5) & 1u);
    s->ntc_low = (uint8_t)((s->raw76 >> 4) & 1u);
    /* High and mid can coexist for threshold flags. Cold plus either hot flag
     * is contradictory and cannot authorize charge. No flag is a normal code,
     * but does NOT establish the narrower application 10..45 C window. */
    s->ntc_valid = (uint8_t)!(s->ntc_low && (s->ntc_high || s->ntc_mid));
    s->key_short = s->raw77 & 1u;
    s->key_long = (uint8_t)((s->raw77 >> 1) & 1u);
    s->typec_sink = (uint8_t)((s->raw83 >> 5) & 1u);
    s->typec_source = (uint8_t)((s->raw83 >> 4) & 1u);

    code = (uint8_t)(s->raw74 >> 4);
    s->soc_valid = 1u;
    s->soc_min_inclusive = 0u;
    switch (code) {
    case 0u:
        s->soc_min_pct = 0u;
        s->soc_max_pct = 3u;
        s->soc_min_inclusive = 1u;
        break;
    case 1u:
        s->soc_min_pct = 3u;
        s->soc_max_pct = 25u;
        break;
    case 3u:
        s->soc_min_pct = 25u;
        s->soc_max_pct = 50u;
        break;
    case 7u:
        s->soc_min_pct = 50u;
        s->soc_max_pct = 75u;
        break;
    case 15u:
        s->soc_min_pct = 75u;
        s->soc_max_pct = 100u;
        s->soc_min_inclusive = 1u;
        break;
    default:
        s->soc_min_pct = 0u;
        s->soc_max_pct = 0u;
        s->soc_valid = 0u;
        break;
    }
}

int IP5310_ReadSnapshot(const IP5310_Device *dev, IP5310_Snapshot *snapshot)
{
    IP5310_Snapshot next;
    if (!readable(dev) || snapshot == NULL) {
        return IP5310_ERROR_ARGUMENT;
    }
    memset(&next, 0, sizeof(next));
    if (read_reg(dev, 0x00u, &next.raw00) != IP5310_OK ||
        read_reg(dev, 0x70u, &next.raw70) != IP5310_OK ||
        read_reg(dev, 0x71u, &next.raw71) != IP5310_OK ||
        read_reg(dev, 0x72u, &next.raw72) != IP5310_OK ||
        read_reg(dev, 0x74u, &next.raw74) != IP5310_OK ||
        read_reg(dev, 0x76u, &next.raw76) != IP5310_OK ||
        read_reg(dev, 0x77u, &next.raw77) != IP5310_OK ||
        read_reg(dev, 0x83u, &next.raw83) != IP5310_OK) {
        return IP5310_ERROR_IO;
    }
    if ((next.raw00 & next.raw70 & next.raw71 & next.raw72 & next.raw74 &
         next.raw76 & next.raw77 & next.raw83) == 0xffu) {
        return IP5310_ERROR_DATA;
    }
    IP5310_DecodeSnapshot(&next);
    *snapshot = next;
    return IP5310_OK;
}

int IP5310_ReadChargeConfiguration(const IP5310_Device *dev,
                                   IP5310_ChargeConfiguration *config)
{
    IP5310_ChargeConfiguration next;
    if (!readable(dev) || config == NULL) {
        return IP5310_ERROR_ARGUMENT;
    }
    if (read_reg(dev, 0x00u, &next.raw00) != IP5310_OK ||
        read_reg(dev, 0x0au, &next.raw0a) != IP5310_OK ||
        read_reg(dev, 0x22u, &next.raw22) != IP5310_OK ||
        read_reg(dev, 0x23u, &next.raw23) != IP5310_OK ||
        read_reg(dev, 0x24u, &next.raw24) != IP5310_OK ||
        read_reg(dev, 0x25u, &next.raw25) != IP5310_OK ||
        read_reg(dev, 0x26u, &next.raw26) != IP5310_OK) {
        return IP5310_ERROR_IO;
    }
    if ((next.raw00 & next.raw0a & next.raw22 & next.raw23 & next.raw24 &
         next.raw25 & next.raw26) == 0xffu) {
        return IP5310_ERROR_DATA;
    }
    *config = next;
    return IP5310_OK;
}

int IP5310_ReadPowerConfiguration(const IP5310_Device *dev,
                                IP5310_PowerConfiguration *config)
{
    IP5310_PowerConfiguration next;
    if (!readable(dev) || config == NULL) return IP5310_ERROR_ARGUMENT;
    if (read_reg(dev, 0x00u, &next.raw00) != IP5310_OK ||
        read_reg(dev, 0x01u, &next.raw01) != IP5310_OK ||
        read_reg(dev, 0x02u, &next.raw02) != IP5310_OK ||
        read_reg(dev, 0x0eu, &next.raw0e) != IP5310_OK) return IP5310_ERROR_IO;
    if ((next.raw00 & next.raw01 & next.raw02 & next.raw0e) == 0xffu)
        return IP5310_ERROR_DATA;
    *config = next;
    return IP5310_OK;
}

int IP5310_PowerConfigurationKeepsAwake(const IP5310_PowerConfiguration *config)
{
    return config != NULL && (config->raw02 & 0xe0u) == 0xe0u;
}

int IP5310_DisableLightLoadShutdownTracked(const IP5310_Device *dev,
                                         uint8_t *write_attempted)
{
    IP5310_PowerConfiguration before;
    uint8_t after, check, code;
    int result;
    if (write_attempted != NULL) *write_attempted = 0u;
    if (!writable(dev)) return IP5310_ERROR_ARGUMENT;
    result = IP5310_ReadPowerConfiguration(dev, &before);
    if (result != IP5310_OK) return result;
    code = (uint8_t)(before.raw02 & 0xe0u);
    if (code != 0x60u && code != 0xe0u) return IP5310_ERROR_DATA;
    after = (uint8_t)((before.raw02 & 0x1fu) | 0xe0u);
    if (after != before.raw02) {
        if (write_attempted != NULL) *write_attempted = 1u;
        if (dev->write(0x02u, after) != 0) return IP5310_ERROR_IO;
    }
    result = read_reg(dev, 0x02u, &check);
    if (result != IP5310_OK) return result;
    return check == after ? IP5310_OK : IP5310_ERROR_VERIFY;
}

int IP5310_DisableLightLoadShutdown(const IP5310_Device *dev)
{
    return IP5310_DisableLightLoadShutdownTracked(dev, NULL);
}

int IP5310_ChargeConfigurationMatches(const IP5310_ChargeConfiguration *c,
                                    uint16_t nominal_ma, uint8_t allow_charge)
{
    uint8_t vin, vbus;
    if (c == NULL || allow_charge > 1u ||
        profile_codes(nominal_ma, &vin, &vbus) != IP5310_OK) {
        return 0;
    }
    return ((c->raw00 & 0x10u) == (allow_charge ? 0x10u : 0u)) &&
           ((c->raw0a & 0x80u) == 0u) &&
           ((c->raw22 & 0x0fu) == 0u) &&
           ((c->raw25 & 0x10u) == 0u) &&
           ((c->raw23 & 0x20u) == 0u) &&
           ((c->raw24 & 0x1fu) == vin) &&
           ((c->raw26 & 0x1fu) == vbus);
}

int IP5310_SetChargeEnabled(const IP5310_Device *dev, uint8_t enabled)
{
    if (!writable(dev) || enabled > 1u) {
        return IP5310_ERROR_ARGUMENT;
    }
    return update_verified(dev, 0x00u, 0x10u, enabled ? 0x10u : 0u);
}

uint16_t IP5310_VbusNominalFromCode(uint8_t code)
{
    if (code > 31U) { return 0U; }
    return (uint16_t)(50U + ((code & 1U) ? 100U : 0U) + ((code & 2U) ? 200U : 0U) +
        ((code & 4U) ? 500U : 0U) + ((code & 8U) ? 1200U : 0U) + ((code & 16U) ? 2300U : 0U));
}

int IP5310_VbusCodeFromNominal(uint16_t nominal_ma, uint8_t *code)
{
    uint8_t candidate;
    if (!code) { return IP5310_ERROR_ARGUMENT; }
    for (candidate = 0U; candidate < 32U; ++candidate) {
        if (IP5310_VbusNominalFromCode(candidate) == nominal_ma) {
            *code = candidate;
            return IP5310_OK;
        }
    }
    return IP5310_ERROR_ARGUMENT;
}

int IP5310_VbusCurrentMatches(const IP5310_ChargeConfiguration *config,
                             uint16_t nominal_ma)
{
    uint8_t vbus;
    if (config == NULL || IP5310_VbusCodeFromNominal(nominal_ma, &vbus) != IP5310_OK) {
        return 0;
    }
    return (config->raw26 & 0x1fu) == vbus;
}

int IP5310_ApplyVbusCurrent(const IP5310_Device *dev, uint16_t nominal_ma)
{
    uint8_t vbus;
    IP5310_ChargeConfiguration before, check;
    int result;
    if (!writable(dev) || IP5310_VbusCodeFromNominal(nominal_ma, &vbus) != IP5310_OK) {
        return IP5310_ERROR_ARGUMENT;
    }
    result = IP5310_ReadChargeConfiguration(dev, &before);
    if (result != IP5310_OK) { goto failed; }
    /* V0.24 has USB on VBUS. Do not switch VIN loops or take over VSET. */
    result = update_verified(dev, 0x26u, 0x1fu, vbus);
    if (result != IP5310_OK) { goto failed; }
    result = IP5310_ReadChargeConfiguration(dev, &check);
    if (result != IP5310_OK) { goto failed; }
    /* Also detect a chip reset or unexpected configuration change mid-update.
     * Charge permission is observed, never asserted by this operation. */
    if (!IP5310_VbusCurrentMatches(&check, nominal_ma) ||
        before.raw00 != check.raw00 || before.raw0a != check.raw0a ||
        before.raw22 != check.raw22 || before.raw23 != check.raw23 ||
        before.raw24 != check.raw24 || before.raw25 != check.raw25 ||
        (before.raw26 & 0xe0u) != (check.raw26 & 0xe0u)) {
        result = IP5310_ERROR_VERIFY;
        goto failed;
    }
    return IP5310_OK;

failed:
    (void)IP5310_SetChargeEnabled(dev, 0u);
    return result;
}

int IP5310_VerifyChargeProfile(const IP5310_Device *dev, uint16_t nominal_ma,
                              uint8_t allow_charge)
{
    IP5310_ChargeConfiguration config;
    uint8_t vin, vbus;
    int result;
    if (allow_charge > 1u ||
        profile_codes(nominal_ma, &vin, &vbus) != IP5310_OK) {
        return IP5310_ERROR_ARGUMENT;
    }
    result = IP5310_ReadChargeConfiguration(dev, &config);
    if (result != IP5310_OK) {
        return result;
    }
    return IP5310_ChargeConfigurationMatches(&config, nominal_ma, allow_charge)
               ? IP5310_OK : IP5310_ERROR_VERIFY;
}

int IP5310_ApplyChargeProfile(const IP5310_Device *dev, uint16_t nominal_ma,
                            uint8_t allow_charge)
{
    uint8_t vin, vbus;
    IP5310_ChargeConfiguration check;
    int result;
    if (!writable(dev) || allow_charge > 1u ||
        profile_codes(nominal_ma, &vin, &vbus) != IP5310_OK) {
        return IP5310_ERROR_ARGUMENT;
    }
    result = IP5310_SetChargeEnabled(dev, 0u);
    if (result != IP5310_OK) {
        goto failed;
    }
    /* Set 4.2 V with zero offset BEFORE transferring VSET control to I2C. */
    result = update_verified(dev, 0x22u, 0x0fu, 0u);
    if (result != IP5310_OK) { goto failed; }
    result = update_verified(dev, 0x0au, 0x80u, 0u);
    if (result != IP5310_OK) { goto failed; }
    /* 0x25[4] must be zero before 0x23[5] can select the BAT CC loop. */
    result = update_verified(dev, 0x25u, 0x10u, 0u);
    if (result != IP5310_OK) { goto failed; }
    result = update_verified(dev, 0x23u, 0x20u, 0u);
    if (result != IP5310_OK) { goto failed; }
    result = update_verified(dev, 0x24u, 0x1fu, vin);
    if (result != IP5310_OK) { goto failed; }
    result = update_verified(dev, 0x26u, 0x1fu, vbus);
    if (result != IP5310_OK) { goto failed; }
    /* Detect a reset or lost write before charging is permitted. */
    result = IP5310_ReadChargeConfiguration(dev, &check);
    if (result != IP5310_OK) { goto failed; }
    if (!IP5310_ChargeConfigurationMatches(&check, nominal_ma, 0u)) {
        result = IP5310_ERROR_VERIFY;
        goto failed;
    }
    if (allow_charge) {
        result = IP5310_SetChargeEnabled(dev, 1u);
        if (result != IP5310_OK) { goto failed; }
    }
    return IP5310_OK;

failed:
    /* Best effort only. Preserve the original error; caller must not claim
     * charging is off if communication or verification failed. */
    (void)IP5310_SetChargeEnabled(dev, 0u);
    return result;
}
