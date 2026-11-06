#ifndef BATTERY_CONFIG_H
#define BATTERY_CONFIG_H

/* V0.24: two protected 802440P 700 mAh packs in parallel (1S2P).
 * User confirmed I2C support on 2026-10-23. The batch register map is pending.
 * User authorized a 1.85 A total trial on 2026-10-23, citing seller guidance
 * of 1 A per pack. This is a trial setting, not a validated pack safety limit:
 * branch currents, single-pack dropout and battery temperature are unmeasured.
 * V1.28 code 0x0D selects nominal 1850 mA. Charging setup writes only the
 * VBUS field; optional keep-awake configuration is separate below.
 * Full temperature-controlled charging still requires the exact batch map.
 */
#define BATTERY_PACK_CAPACITY_MAH             1400U
#define BATTERY_PACK_NOMINAL_MV               3700U
#define BATTERY_PACK_CHARGE_MV                4200U
#define BATTERY_PROJECT_NORMAL_LIMIT_MA 1850U
#define BATTERY_PROJECT_COOL_LIMIT_MA         280U
#define BATTERY_NORMAL_NOMINAL_MA             650U
#define BATTERY_COOL_NOMINAL_MA               250U

#ifndef BATTERY_CONTROL_ENABLED
#define BATTERY_CONTROL_ENABLED 0
#endif
/* V0.24: USB is connected to VBUS; VIN is not an external charge input.
 * This mode never enables charging or changes voltage/NTC configuration.
 * It does not establish temperature validity or confirm the IC batch map.
 */
#ifndef BATTERY_VBUS_CURRENT_SETUP_ENABLED
#define BATTERY_VBUS_CURRENT_SETUP_ENABLED   1
#endif
/* Keep the battery-powered robot awake while its 5 V loads are idle. Only
 * V1.28 SYS_CTRL6 0x02[7:5] is changed; KEY and protection settings are kept.
 * A fully read-only monitor build must explicitly disable this as well. */
#ifndef BATTERY_KEEP_5V_ON_ENABLED
#define BATTERY_KEEP_5V_ON_ENABLED 1
#endif
#define BATTERY_KEEP_5V_VERIFY_MS             1000U
#define BATTERY_VBUS_REQUESTED_MA 350U
#define BATTERY_VBUS_NOMINAL_MA 350U
/* Board/IP5310 BAT charging rating; exact encoded requests above this are
 * rejected even if a future battery profile would permit a higher current.
 * All 32 register encodings remain representable in the protocol driver. */
#define BATTERY_BOARD_MAX_NOMINAL_MA 1850U
#ifndef BATTERY_CURRENT_PERSIST_ENABLED
#define BATTERY_CURRENT_PERSIST_ENABLED 1
#endif
#if BATTERY_VBUS_NOMINAL_MA > BATTERY_BOARD_MAX_NOMINAL_MA
#error Startup nominal current must not exceed the board charging rating
#endif
#if BATTERY_VBUS_NOMINAL_MA > BATTERY_VBUS_REQUESTED_MA
#error VBUS nominal current must not exceed the user request
#endif
#if BATTERY_CONTROL_ENABLED && BATTERY_VBUS_CURRENT_SETUP_ENABLED
#error Select either VBUS current setup or full temperature control
#endif
#ifndef BATTERY_REGISTER_MAP_CONFIRMED
#define BATTERY_REGISTER_MAP_CONFIRMED        0
#endif
#if BATTERY_CONTROL_ENABLED && !BATTERY_REGISTER_MAP_CONFIRMED
#error Confirm the installed IP5310 register map before enabling charger control
#endif

/* STM_BAT_TEMP is NOT connected in V0.24. PC0/ADC10 is only a candidate.
 * Change these definitions only after connecting and verifying the actual pin.
 */
#ifndef BATTERY_TEMP_ADC_CONNECTED
#define BATTERY_TEMP_ADC_CONNECTED           0
#endif
#define BATTERY_TEMP_ADC_CHANNEL             ADC_Channel_10
#define BATTERY_TEMP_ADC_GPIO_PIN            GPIO_Pin_0
#define BATTERY_TEMP_ADC_GPIO_PORT           GPIOC
#define BATTERY_TEMP_ADC_GPIO_CLOCK          RCC_APB2Periph_GPIOC

/* Default diagnostics: USART3 PC10 TX / PC11 RX via the ESP32 USB bridge.
 * Select 1 to use H1 PA9/PA10 with a separate 3.3 V USB-UART adapter.
 * Both links use 115200 8N1; USART3 must use partial remap.
 */
#ifndef BATTERY_UART_DIAGNOSTICS
#define BATTERY_UART_DIAGNOSTICS             1
#endif
#ifndef BATTERY_DIAGNOSTIC_UART
#define BATTERY_DIAGNOSTIC_UART              3
#endif
#if BATTERY_DIAGNOSTIC_UART != 1 && BATTERY_DIAGNOSTIC_UART != 3
#error BATTERY_DIAGNOSTIC_UART must be 1 or 3
#endif
#define BATTERY_POLL_MS                      250U
#define BATTERY_STALE_MS                     1500U
#define BATTERY_WAKE_SETTLE_MS               100U
#define BATTERY_RECOVERY_SAMPLES             5U
#define BATTERY_CRITICAL_SAMPLES             3U

/* Project window: 10..45 C. Software uses 1 C nominal guard bands.
 * These margins are engineering defaults, not a calibrated error guarantee.
 * All temperatures below are 0.1 C; restrictive transitions are immediate.
 */
#define BATTERY_TEMP_STOP_COLD_DC            110
#define BATTERY_TEMP_STOP_HOT_DC             440
#define BATTERY_TEMP_RESTART_COLD_DC         120
#define BATTERY_TEMP_RESTART_HOT_DC          430
#define BATTERY_TEMP_REDUCE_DC               210
#define BATTERY_TEMP_NORMAL_RECOVER_DC       220

#endif
