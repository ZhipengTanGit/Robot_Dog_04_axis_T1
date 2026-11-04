#ifndef IP5310_H
#define IP5310_H

#include <stdint.h>

/*
 * Register definitions: Injoinic IP5310 I2C manual V1.28 (local copy).
 * Full voltage/temperature control requires checking the fitted custom IC map.
 * V0.24 current-only setup uses the single VBUS field at the user's request;
 * readback confirms stored bits, not batch equivalence or physical current.
 * Call from one task only; the BSP owns bus serialization and finite timeouts.
 */
#define IP5310_ADDRESS_7BIT 0x75u

typedef enum {
    IP5310_OK = 0,
    IP5310_ERROR_IO = -1,
    IP5310_ERROR_DATA = -2,
    IP5310_ERROR_VERIFY = -3,
    IP5310_ERROR_ARGUMENT = -4
} IP5310_Result;

typedef struct {
    int (*read)(uint8_t reg, uint8_t *value);
    int (*write)(uint8_t reg, uint8_t value);
} IP5310_Device;

typedef struct {
    uint8_t raw00, raw70, raw71, raw72, raw74, raw76, raw77, raw83;
    uint8_t charge_enabled; /* SYS_CTL0 permission, not measured current. */
    uint8_t charge_active;  /* Legacy field name: REG_READ0 charge_en status,
                            * not a current-flow measurement. */
    uint8_t charge_full;
    uint8_t light_load;
    uint8_t soc_min_pct, soc_max_pct, soc_min_inclusive, soc_valid;
    uint8_t ntc_high, ntc_mid, ntc_low, ntc_valid;
    uint8_t key_short, key_long; /* Latched; this driver does not clear them. */
    uint8_t typec_sink, typec_source;
} IP5310_Snapshot;

typedef struct {
    uint8_t raw00, raw0a, raw22, raw23, raw24, raw25, raw26;
} IP5310_ChargeConfiguration;

typedef struct {
    uint8_t raw00, raw01, raw02, raw0e;
} IP5310_PowerConfiguration;

/* V1.28 SYS_CTRL6 0x02[7:5]: 011 allows light-load shutdown, 111
 * disables it. Readback verifies stored configuration, not measured 5 V.
 * The separate 0x00 bit2 mode may still sleep I2C and is not used here. */
int IP5310_ReadPowerConfiguration(const IP5310_Device *dev,
                                IP5310_PowerConfiguration *config);
int IP5310_PowerConfigurationKeepsAwake(const IP5310_PowerConfiguration *config);
/* Only the two documented encodings are accepted. Preserve all reserved
 * bits; never alter boost/charge permissions, key behavior or protections.
 * A failed write is not replayed by this driver; the caller owns retry policy. */
int IP5310_DisableLightLoadShutdown(const IP5310_Device *dev);
/* Reports whether a write was attempted, even if its acknowledgement failed.
 * Lets the app retry a purely failed read without replaying an uncertain write. */
int IP5310_DisableLightLoadShutdownTracked(const IP5310_Device *dev,
                                         uint8_t *write_attempted);

/* Outputs are unchanged after an I/O failure or an all-0xFF response. */
int IP5310_ReadSnapshot(const IP5310_Device *dev, IP5310_Snapshot *snapshot);
int IP5310_ReadChargeConfiguration(const IP5310_Device *dev,
                                    IP5310_ChargeConfiguration *config);

/* Invalid SOC thermometer codes and contradictory NTC flags have valid=0. */
void IP5310_DecodeSnapshot(IP5310_Snapshot *snapshot);

/* Only 250, 650, 850, 1250 and 1850 mA nominal profiles are supported; these are NOT
 * measured currents or guaranteed physical ceilings. Exact-IC tolerance, precharge,
 * termination and independent hardware protection require board validation.
 * VIN is selected to the BAT CC loop; both input-path settings are written.
 * Successful application requires masked readback, then enables charging last.
 * Any failure attempts to disable charging but cannot guarantee it succeeded.
 */
int IP5310_ApplyChargeProfile(const IP5310_Device *dev, uint16_t nominal_ma,
                            uint8_t allow_charge);
int IP5310_ChargeConfigurationMatches(const IP5310_ChargeConfiguration *config,
                                    uint16_t nominal_ma, uint8_t allow_charge);
int IP5310_VerifyChargeProfile(const IP5310_Device *dev, uint16_t nominal_ma,
                              uint8_t allow_charge);

/* V0.24 charges through VBUS. 0x26[4:0] sets BAT-side charger current for this
 * charging path; it is not a VBUS-input current-limit register.
 * Change only 0x26[4:0], preserving its other
 * bits and the existing charge permission, voltage, VIN and thermal settings.
 * Uses the V1.28 map; the fitted batch map still needs independent confirmation.
 * All 32 exact V1.28 nominal encodings are supported (50..4350 mA, with gaps).
 * These are register encodings, NOT hardware ratings or measured current.
 * The caller must enforce the installed battery/board limits before requesting
 * a code. There is no exact 1000 mA VBUS code. Readback is not measured current.
 * A failure attempts to revoke charge permission, never to restore or enable it.
 * The original failure is returned even if that best-effort stop also fails.
 */
int IP5310_VbusCurrentMatches(const IP5310_ChargeConfiguration *config,
                             uint16_t nominal_ma);
int IP5310_ApplyVbusCurrent(const IP5310_Device *dev, uint16_t nominal_ma);
/* VBUS-only mapping: 50+100*b0+200*b1+500*b2+1200*b3+2300*b4.
 * Decode returns 0 for a code outside 0..31; encode rejects non-exact values. */
uint16_t IP5310_VbusNominalFromCode(uint8_t code);
int IP5310_VbusCodeFromNominal(uint16_t nominal_ma, uint8_t *code);

/* Use enable only after the application has validated temperature and profile.
 * Returns success only when the requested permission bit has been read back.
 * A successful disable confirms the permission bit, not physical zero current.
 */
int IP5310_SetChargeEnabled(const IP5310_Device *dev, uint8_t enabled);

#endif
