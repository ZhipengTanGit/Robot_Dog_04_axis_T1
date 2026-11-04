#ifndef BSP_PERIPHERALS_H
#define BSP_PERIPHERALS_H
#include <stdint.h>
/* P-0090 pulse endpoints confirmed by the user's supplier, 2026-10-25.
 * Shared by command validation and the timer driver; no angle mapping. */
#define BSP_SERVO_MIN_PULSE_US 500U
#define BSP_SERVO_MAX_PULSE_US 2500U
void BSP_Peripherals_Init(void);
uint8_t BSP_Peripherals_Key(void);
uint8_t BSP_Peripherals_Touch(void);
void BSP_Peripherals_Led(uint8_t index, uint8_t on);
void BSP_Peripherals_Buzzer(uint8_t on);
/* 0 disables pulses, otherwise only BSP_SERVO_MIN/MAX_PULSE_US. Channel 0..3 maps to
 * right rear PB6, left rear PB7, right front PB8, left front PB9. */
void BSP_Peripherals_Servo(uint8_t channel, uint16_t pulse_us);
#endif
