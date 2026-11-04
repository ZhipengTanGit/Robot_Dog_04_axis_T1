#ifndef BATTERY_POLICY_H
#define BATTERY_POLICY_H

#include <stdint.h>

typedef enum {
    BATTERY_CHARGE_INHIBITED = 0,
    BATTERY_CHARGE_COOL,
    BATTERY_CHARGE_NORMAL
} Battery_ChargePolicy;

typedef struct {
    Battery_ChargePolicy mode;
    uint8_t recovery_count;
    uint8_t warm_count;
} Battery_PolicyContext;

void Battery_PolicyReset(Battery_PolicyContext *context);
Battery_ChargePolicy Battery_PolicyUpdate(Battery_PolicyContext *context,
    uint8_t temperature_valid, int16_t temperature_dc, uint8_t hardware_ntc_fault);
/* 100k NTC B3950 to GND, 100k pull-up to ADC reference supply; 12-bit ADC.
 * Returns 0 on success. No sensor or ADC failure may be replaced with 25 C.
 */
int Battery_TemperatureFromAdc(uint16_t raw, int16_t *temperature_dc);

#endif
