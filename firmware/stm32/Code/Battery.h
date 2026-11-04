#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include "IP5310.h"
#include "Battery_Policy.h"

typedef enum {
    BATTERY_LINK_STARTING = 0,
    BATTERY_LINK_STANDBY,
    BATTERY_LINK_READY,
    BATTERY_LINK_BUS_ERROR,
    BATTERY_LINK_DATA_ERROR,
    BATTERY_LINK_STALE
} Battery_LinkState;

typedef enum {
    BATTERY_MOTION_STOP_REQUEST = 0,
    BATTERY_MOTION_LIMIT_REQUEST,
    BATTERY_MOTION_NORMAL
} Battery_MotionRequest;

typedef enum {
    BATTERY_CURRENT_IDLE = 0,
    BATTERY_CURRENT_PENDING,
    BATTERY_CURRENT_APPLIED,
    BATTERY_CURRENT_FAILED
} Battery_CurrentRequestState;

#define BATTERY_CURRENT_ACCEPTED       0
#define BATTERY_CURRENT_UNSUPPORTED   -1
#define BATTERY_CURRENT_WRONG_MODE    -2
#define BATTERY_CURRENT_NOT_READY     -3
#define BATTERY_CURRENT_BUSY          -4
#define BATTERY_CURRENT_BOARD_LIMIT   -5

#define BATTERY_FAULT_BUS                (1UL << 0)
#define BATTERY_FAULT_DATA               (1UL << 1)
#define BATTERY_FAULT_TEMP_UNCONNECTED   (1UL << 2)
#define BATTERY_FAULT_TEMP_SENSOR        (1UL << 3)
#define BATTERY_FAULT_NTC                (1UL << 4)
#define BATTERY_FAULT_CONTROL            (1UL << 5)
#define BATTERY_FAULT_STALE              (1UL << 6)

/* Original failed transaction, captured before any best-effort OFF operation.
 * Codes/stages/pins use the BSP I/O diagnostic definitions. Survives recovery. */
typedef struct {
    int error;
    uint32_t at_ms;
    uint8_t stage, reg, write, pins;
} Battery_IoFailure;

typedef struct {
    Battery_LinkState link;
    Battery_ChargePolicy charge_policy;
    Battery_MotionRequest motion_request;
    uint32_t faults;
    uint32_t updated_ms;
    uint32_t last_good_ms;
    uint32_t bus_error_count;
    Battery_IoFailure last_io_failure;
    uint16_t temperature_adc;
    int16_t temperature_dc;
    uint16_t applied_nominal_ma;
    uint16_t requested_nominal_ma;
    uint16_t saved_nominal_ma; /* Last valid Flash record; zero means none. */
    int settings_result; /* 0 no saved record, 1 saved/loaded, -1 save failed. */
    Battery_CurrentRequestState current_request_state;
    int current_request_result; /* IP5310 result; valid after APPLIED/FAILED. */
    int power_setup_result; /* Latest keep-awake setup/readback result. */
    uint8_t awake;
    uint8_t snapshot_valid;
    uint8_t temperature_valid;
    uint8_t critical_battery;
    uint8_t monitor_only;
    uint8_t vbus_limit_only;
    uint8_t register_map_confirmed;
    uint8_t charge_requested;
    uint8_t control_verified;
    uint8_t charge_stop_verified;
    uint8_t charge_config_valid;
    uint8_t charge_config_before_valid;
    uint8_t power_config_valid, keep_5v_verified, power_config_before_valid;
    IP5310_ChargeConfiguration charge_config_before;
    IP5310_ChargeConfiguration charge_config;
    IP5310_PowerConfiguration power_config_before, power_config;
    IP5310_Snapshot chip;
} Battery_Status;

/* Watch this variable in Keil; application code should use Battery_GetStatus.
 * APIs execute in the main-loop context, never in an ISR.
 */
extern volatile Battery_Status g_battery_status;
void Battery_Init(void);
void Battery_Task(void);
void Battery_GetStatus(Battery_Status *status);
/* Full temperature-control mode only: applied at the next 250 ms poll.
 * Monitor/current-only modes never use this request to enable charging.
 * A request is not a readback confirmation.
 */
void Battery_RequestCharging(uint8_t enable);
/* Main-loop request. Verified settings are saved when persistence is enabled.
 * Accepts exact VBUS charging encodings within
 * BATTERY_BOARD_MAX_NOMINAL_MA, only when
 * READY/awake in VBUS-current-only mode. No I2C transaction here. Poll status
 * for APPLIED (masked configuration readback passed) or FAILED. A failed
 * explicit request is never automatically written again; use a NEW request.
 * Encoding support does not certify a cell or hardware current rating. The
 * caller must validate its installed battery profile before sending a request.
 * Never changes charge enable, voltage or NTC configuration. */
int Battery_RequestNominalCurrent(uint16_t nominal_ma);

#endif
