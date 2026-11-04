#include "Battery_Policy.h"
#include "Battery_Config.h"

/* Rounded ADC codes at -20,-15,...,80 C, using the nominal B3950 equation.
 * Ratio measurement cancels supply voltage when pull-up and VDDA track.
 * Linear interpolation gives a nominal estimate, not per-probe calibration.
 */
static const uint16_t ntc_adc[] = {
    3740, 3629, 3495, 3337, 3156, 2955, 2738,
    2510, 2278, 2048, 1825, 1614, 1419, 1241,
    1081, 940, 815, 707, 613, 532, 462
};

void Battery_PolicyReset(Battery_PolicyContext *context)
{
    context->mode = BATTERY_CHARGE_INHIBITED;
    context->recovery_count = 0;
    context->warm_count = 0;
}

Battery_ChargePolicy Battery_PolicyUpdate(Battery_PolicyContext *context,
    uint8_t valid, int16_t temperature, uint8_t hardware_ntc_fault)
{
    if (!valid || hardware_ntc_fault || temperature < BATTERY_TEMP_STOP_COLD_DC ||
        temperature > BATTERY_TEMP_STOP_HOT_DC) {
        Battery_PolicyReset(context);
        return context->mode;
    }
    if (context->mode == BATTERY_CHARGE_INHIBITED) {
        if (temperature < BATTERY_TEMP_RESTART_COLD_DC ||
            temperature > BATTERY_TEMP_RESTART_HOT_DC) {
            context->recovery_count = 0;
            return context->mode;
        }
        if (++context->recovery_count < BATTERY_RECOVERY_SAMPLES) {
            return context->mode;
        }
        context->mode = temperature >= BATTERY_TEMP_NORMAL_RECOVER_DC ?
            BATTERY_CHARGE_NORMAL : BATTERY_CHARGE_COOL;
        context->recovery_count = 0;
    }
    if (temperature <= BATTERY_TEMP_REDUCE_DC) {
        context->mode = BATTERY_CHARGE_COOL;
        context->warm_count = 0;
    } else if (context->mode == BATTERY_CHARGE_COOL) {
        if (temperature >= BATTERY_TEMP_NORMAL_RECOVER_DC) {
            if (++context->warm_count >= BATTERY_RECOVERY_SAMPLES) {
                context->mode = BATTERY_CHARGE_NORMAL;
                context->warm_count = 0;
            }
        } else {
            context->warm_count = 0;
        }
    }
    return context->mode;
}

int Battery_TemperatureFromAdc(uint16_t raw, int16_t *temperature_dc)
{
    unsigned int i;
    unsigned int count = sizeof(ntc_adc) / sizeof(ntc_adc[0]);
    if (temperature_dc == 0 || raw < ntc_adc[count - 1U] || raw > ntc_adc[0]) {
        return -1;
    }
    for (i = 0; i < count - 1U; ++i) {
        if (raw >= ntc_adc[i + 1U]) {
            uint32_t span = ntc_adc[i] - ntc_adc[i + 1U];
            *temperature_dc = (int16_t)(-200 + (int32_t)i * 50 +
                (int32_t)(((ntc_adc[i] - raw) * 50U + span / 2U) / span));
            return 0;
        }
    }
    return -1;
}
