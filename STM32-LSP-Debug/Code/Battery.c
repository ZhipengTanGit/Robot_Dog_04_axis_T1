#include "Battery.h"
#include "Battery_Config.h"
#if BATTERY_CURRENT_PERSIST_ENABLED
#include "Battery_Settings.h"
#endif
#include "BSP_Battery.h"
#include "BSP_SysTick.h"
#include <string.h>

volatile Battery_Status g_battery_status;
static Battery_Status status;
static Battery_PolicyContext policy;
static int battery_read_reg(uint8_t reg, uint8_t *value);
static int battery_write_reg(uint8_t reg, uint8_t value);
static IP5310_Device chip = { battery_read_reg, battery_write_reg };
static uint32_t last_poll_ms;
static uint32_t awake_since_ms;
static uint8_t was_awake;
static uint8_t critical_count;
static uint8_t critical_recovery;
static uint8_t poll_io_failed;
static uint8_t poll_sleep_seen;
#if BATTERY_KEEP_5V_ON_ENABLED
static uint8_t power_attempted, power_poll_started;
static uint32_t power_checked_ms;
#endif
#if BATTERY_VBUS_CURRENT_SETUP_ENABLED
static uint8_t vbus_manual_target;
static uint8_t vbus_write_blocked;
#endif
#if BATTERY_CONTROL_ENABLED
static uint16_t configured_ma;
#endif

static void publish(void)
{
    g_battery_status = status;
}

static void record_io_failure(int result)
{
    BSP_Battery_IoDiagnostics io;
    if (result == BSP_BATTERY_IO_OK) {
        return;
    }
    BSP_Battery_GetIoDiagnostics(&io);
    if (!poll_io_failed) {
        status.last_io_failure.error = io.last_error;
        status.last_io_failure.at_ms = io.last_error_ms;
        status.last_io_failure.stage = io.last_stage;
        status.last_io_failure.reg = io.last_reg;
        status.last_io_failure.write = io.last_write;
        status.last_io_failure.pins = io.last_pins;
        poll_io_failed = 1U;
    }
    if (result == BSP_BATTERY_IO_ASLEEP) {
        poll_sleep_seen = 1U;
    }
}

/* The protocol driver also attempts OFF internally after a failed profile.
 * Latch the original failure here, before those cleanup callbacks can run.
 * Once a sleep edge occurs, no further transaction is allowed this poll,
 * even if L3 rises again before the protocol operation returns. */
static int battery_read_reg(uint8_t reg, uint8_t *value)
{
    int result;
    if (poll_sleep_seen) {
        return BSP_BATTERY_IO_ASLEEP;
    }
    result = BSP_Battery_ReadReg(reg, value);
    record_io_failure(result);
    return result;
}

static int battery_write_reg(uint8_t reg, uint8_t value)
{
    int result;
    if (poll_sleep_seen) {
        return BSP_BATTERY_IO_ASLEEP;
    }
    result = BSP_Battery_WriteReg(reg, value);
    record_io_failure(result);
    return result;
}

static void reset_policy(void)
{
    Battery_PolicyReset(&policy);
    status.charge_policy = BATTERY_CHARGE_INHIBITED;
    status.control_verified = 0;
    status.charge_stop_verified = 0;
    status.applied_nominal_ma = 0;
#if BATTERY_CONTROL_ENABLED
    configured_ma = 0;
#endif
}

static void fail_pending_current(int result)
{
    if (status.current_request_state == BATTERY_CURRENT_PENDING) {
        status.current_request_state = BATTERY_CURRENT_FAILED;
        status.current_request_result = result;
#if BATTERY_VBUS_CURRENT_SETUP_ENABLED
        vbus_write_blocked = 1U;
#endif
    }
}

static void invalidate_snapshot(Battery_LinkState link, uint32_t fault)
{
    fail_pending_current(IP5310_ERROR_IO);
    status.link = link;
    status.faults |= fault;
    status.snapshot_valid = 0;
    status.charge_config_valid = 0;
    status.temperature_valid = 0;
    status.power_config_valid = status.keep_5v_verified = 0U;
#if BATTERY_KEEP_5V_ON_ENABLED
    /* A real wake/status-link recovery permits one new setup attempt. The
     * optional power-only verifier never calls this on an ordinary failure. */
    power_attempted = power_poll_started = 0U;
#endif
    status.motion_request = BATTERY_MOTION_STOP_REQUEST;
    critical_count = 0;
    critical_recovery = 0;
    reset_policy();
}

static int handle_sleep_edge(void)
{
    uint8_t awake = BSP_Battery_IsAwake();
    if (!poll_sleep_seen && awake) {
        return 0;
    }
    poll_sleep_seen = 1U;
    status.awake = awake;
    was_awake = 0U;
    status.faults &= ~(BATTERY_FAULT_BUS | BATTERY_FAULT_CONTROL);
    invalidate_snapshot(awake ? BATTERY_LINK_STARTING : BATTERY_LINK_STANDBY, 0);
    BSP_Battery_ReleaseBus();
    return 1;
}

static void try_stop_charge(void)
{
#if BATTERY_CONTROL_ENABLED || BATTERY_VBUS_CURRENT_SETUP_ENABLED
    int result;
    if (handle_sleep_edge()) {
        return;
    }
#if BATTERY_CONTROL_ENABLED
    configured_ma = 0;
#endif
    status.applied_nominal_ma = 0;
    status.charge_stop_verified = 0;
    status.control_verified = 0;
    result = IP5310_SetChargeEnabled(&chip, 0);
    if (result == IP5310_OK) {
        status.charge_stop_verified = 1;
#if BATTERY_CONTROL_ENABLED
        status.control_verified = 1;
#endif
    } else if (!handle_sleep_edge()) {
        status.faults |= BATTERY_FAULT_CONTROL;
    }
#endif
}

void Battery_Init(void)
{
    uint32_t now = BSP_Millis();
    memset(&status, 0, sizeof(status));
    status.link = BATTERY_LINK_STARTING;
    status.monitor_only = !(BATTERY_CONTROL_ENABLED || BATTERY_VBUS_CURRENT_SETUP_ENABLED);
    status.vbus_limit_only = BATTERY_VBUS_CURRENT_SETUP_ENABLED;
    status.register_map_confirmed = BATTERY_REGISTER_MAP_CONFIRMED;
    status.charge_requested = 1;
    status.requested_nominal_ma = BATTERY_VBUS_NOMINAL_MA;
#if BATTERY_CURRENT_PERSIST_ENABLED && BATTERY_VBUS_CURRENT_SETUP_ENABLED && !BATTERY_CONTROL_ENABLED
    if (Battery_SettingsLoad(&status.saved_nominal_ma)) {
        status.requested_nominal_ma = status.saved_nominal_ma;
        status.settings_result = 1;
    }
#endif
    status.power_setup_result = IP5310_ERROR_DATA;
#if BATTERY_KEEP_5V_ON_ENABLED
    power_attempted = power_poll_started = 0U;
    power_checked_ms = now;
#endif
#if BATTERY_VBUS_CURRENT_SETUP_ENABLED
    vbus_manual_target = vbus_write_blocked = 0U;
#endif
    status.motion_request = BATTERY_MOTION_STOP_REQUEST;
#if !BATTERY_TEMP_ADC_CONNECTED
    status.faults = BATTERY_FAULT_TEMP_UNCONNECTED;
#endif
    was_awake = 0;
    critical_count = 0;
    critical_recovery = 0;
    poll_io_failed = 0U;
    poll_sleep_seen = 0U;
    reset_policy();
    BSP_Battery_Init();
    last_poll_ms = now - BATTERY_POLL_MS;
    awake_since_ms = now;
    publish();
}

void Battery_RequestCharging(uint8_t enable)
{
    status.charge_requested = enable ? 1U : 0U;
    /* Apply on the next scheduled poll. Do not advance sample counters when
     * a caller repeats requests within the same sampling interval. */
    publish();
}

int Battery_RequestNominalCurrent(uint16_t nominal_ma)
{
    uint8_t code;
    if (IP5310_VbusCodeFromNominal(nominal_ma, &code) != IP5310_OK) { return BATTERY_CURRENT_UNSUPPORTED; }
    if (nominal_ma > BATTERY_BOARD_MAX_NOMINAL_MA) { return BATTERY_CURRENT_BOARD_LIMIT; }
#if BATTERY_VBUS_CURRENT_SETUP_ENABLED && !BATTERY_CONTROL_ENABLED
    if (status.current_request_state == BATTERY_CURRENT_PENDING) { return BATTERY_CURRENT_BUSY; }
    if (status.link != BATTERY_LINK_READY || !status.snapshot_valid || !status.awake ||
        !BSP_Battery_IsAwake() || (uint32_t)(BSP_Millis() - status.last_good_ms) > BATTERY_STALE_MS) {
        return BATTERY_CURRENT_NOT_READY;
    }
    status.requested_nominal_ma = nominal_ma;
    status.current_request_state = BATTERY_CURRENT_PENDING;
    status.current_request_result = IP5310_OK;
    vbus_manual_target = 1U;
    vbus_write_blocked = 0U;
    publish();
    return BATTERY_CURRENT_ACCEPTED;
#else
    return BATTERY_CURRENT_WRONG_MODE;
#endif
}

static void update_motion_request(void)
{
    if (status.chip.soc_max_pct <= 3U) {
        critical_recovery = 0;
        if (critical_count < BATTERY_CRITICAL_SAMPLES) {
            ++critical_count;
        }
        if (critical_count >= BATTERY_CRITICAL_SAMPLES) {
            status.critical_battery = 1;
        }
    } else {
        critical_count = 0;
        /* Require a clearly higher bucket, not a single borderline sample. */
        if (status.chip.soc_min_pct >= 25U) {
            if (critical_recovery < BATTERY_RECOVERY_SAMPLES) {
                ++critical_recovery;
            }
            if (critical_recovery >= BATTERY_RECOVERY_SAMPLES) {
                status.critical_battery = 0;
            }
        } else {
            critical_recovery = 0;
        }
    }
    if (status.critical_battery || status.chip.ntc_high || status.chip.ntc_low) {
        status.motion_request = BATTERY_MOTION_STOP_REQUEST;
    } else if (status.chip.soc_max_pct <= 25U || status.chip.charge_active ||
               status.chip.typec_sink || status.chip.ntc_mid) {
        status.motion_request = BATTERY_MOTION_LIMIT_REQUEST;
    } else {
        status.motion_request = BATTERY_MOTION_NORMAL;
    }
}

static void update_charge_control(void)
{
#if BATTERY_CONTROL_ENABLED
    uint16_t desired_ma;
    if (!status.charge_requested || status.charge_policy == BATTERY_CHARGE_INHIBITED) {
        try_stop_charge();
        return;
    }
    desired_ma = status.charge_policy == BATTERY_CHARGE_COOL ?
        BATTERY_COOL_NOMINAL_MA : BATTERY_NORMAL_NOMINAL_MA;
    if (configured_ma == desired_ma) {
        /* Check every poll; chip resets may silently discard configuration. */
        if (IP5310_VerifyChargeProfile(&chip, desired_ma, 1) != 0) {
            if (handle_sleep_edge()) { return; }
            status.faults |= BATTERY_FAULT_CONTROL;
            reset_policy();
            try_stop_charge();
            return;
        }
    } else if (IP5310_ApplyChargeProfile(&chip, desired_ma, 1) != 0) {
        if (handle_sleep_edge()) { return; }
        status.faults |= BATTERY_FAULT_CONTROL;
        reset_policy();
        try_stop_charge();
        return;
    }
    configured_ma = desired_ma;
    status.applied_nominal_ma = desired_ma;
    status.control_verified = 1;
    status.charge_stop_verified = 0;
#endif
}

/* Current-only mode leaves SYS_CTL0 permission, VSET and hardware NTC intact.
 * A chip reset can discard the field, so inspect it on every valid poll.
 * Explicit requests replace the RAM target and allow one attempt only. After
 * success, persisted targets may be restored if the charger loses its field.
 * A failed attempt is latched until a NEW request or an MCU restart.
 * On a failed transaction we attempt OFF, but never automatically turn it ON.
 */
static void update_vbus_current(void)
{
#if BATTERY_VBUS_CURRENT_SETUP_ENABLED
    IP5310_ChargeConfiguration config;
    int result;
    status.control_verified = 0;
    status.applied_nominal_ma = 0;
    status.charge_config_valid = 0;
    result = IP5310_ReadChargeConfiguration(&chip, &config);
    if (result == IP5310_OK && !status.charge_config_before_valid) {
        status.charge_config_before = config;
        status.charge_config_before_valid = 1;
    }
    if (result == IP5310_OK &&
        !IP5310_VbusCurrentMatches(&config, status.requested_nominal_ma)) {
        result = vbus_write_blocked ? IP5310_ERROR_VERIFY :
            IP5310_ApplyVbusCurrent(&chip, status.requested_nominal_ma);
        if (result == IP5310_OK) {
            result = IP5310_ReadChargeConfiguration(&chip, &config);
        }
    }
    if (result != IP5310_OK ||
        !IP5310_VbusCurrentMatches(&config, status.requested_nominal_ma)) {
        fail_pending_current(result == IP5310_OK ? IP5310_ERROR_VERIFY : result);
        if (vbus_manual_target) { vbus_write_blocked = 1U; }
#if BATTERY_CURRENT_PERSIST_ENABLED
        vbus_write_blocked = 1U;
#endif
        if (handle_sleep_edge()) { return; }
        status.faults |= BATTERY_FAULT_CONTROL;
        try_stop_charge();
        /* OFF verification must not masquerade as current verification. */
        status.control_verified = 0;
        return;
    }
    status.charge_config = config;
    status.charge_config_valid = 1;
    status.applied_nominal_ma = status.requested_nominal_ma;
    status.control_verified = 1;
    status.charge_stop_verified = (config.raw00 & 0x10u) == 0u;
    if (status.current_request_state == BATTERY_CURRENT_PENDING) {
        status.current_request_state = BATTERY_CURRENT_APPLIED;
        status.current_request_result = IP5310_OK;
        vbus_write_blocked = 1U;
#if BATTERY_CURRENT_PERSIST_ENABLED
        if (Battery_SettingsSave(status.requested_nominal_ma) == 0) {
            status.saved_nominal_ma = status.requested_nominal_ma;
            status.settings_result = 1;
            vbus_manual_target = vbus_write_blocked = 0U;
        } else {
            status.settings_result = -1;
        }
#endif
    }
#endif
}

static void update_keep_5v(uint32_t now)
{
#if BATTERY_KEEP_5V_ON_ENABLED
    IP5310_PowerConfiguration config;
    int result;
    uint8_t first, write_attempted = 0U;
    if (!status.snapshot_valid || status.link != BATTERY_LINK_READY || !status.awake || poll_sleep_seen) { return; }
    if (power_poll_started && (uint32_t)(now - power_checked_ms) < BATTERY_KEEP_5V_VERIFY_MS) { return; }
    first = !power_attempted;
    /* A failed initial read has made no configuration attempt. Retry that
     * read at 1 s intervals, without turning the 250 ms poll into a retry loop. */
    power_poll_started = 1U;
    power_checked_ms = now;
    status.power_config_valid = status.keep_5v_verified = 0U;
    result = IP5310_ReadPowerConfiguration(&chip, &config);
    if (result == IP5310_OK && !status.power_config_before_valid) {
        status.power_config_before = config;
        status.power_config_before_valid = 1U;
    }
    /* Only a read-only I/O failure may retry. Invalid data consumes the setup
     * opportunity too; failed/uncertain writes and later drift never replay. */
    if (first && result != IP5310_ERROR_IO) { power_attempted = 1U; }
    if (result == IP5310_OK && first && !IP5310_PowerConfigurationKeepsAwake(&config)) {
        result = IP5310_DisableLightLoadShutdownTracked(&chip, &write_attempted);
        if (result == IP5310_ERROR_IO && !write_attempted) { power_attempted = 0U; }
        /* This final read cannot reopen the opportunity: the setup API already
         * succeeded, so a write may have committed before this read fails. */
        if (result == IP5310_OK) { result = IP5310_ReadPowerConfiguration(&chip, &config); }
    }
    if (result == IP5310_OK) {
        status.power_config = config;
        status.power_config_valid = 1U;
        status.keep_5v_verified = (uint8_t)IP5310_PowerConfigurationKeepsAwake(&config);
        if (!status.keep_5v_verified) {
            result = (config.raw02 & 0xE0U) == 0x60U ? IP5310_ERROR_VERIFY : IP5310_ERROR_DATA;
        }
    }
    status.power_setup_result = result;
    /* Do not touch charge enable/profile as a fallback for this optional
     * setting. A genuine L3 sleep edge still takes the normal standby path. */
    if (result != IP5310_OK) { (void)handle_sleep_edge(); }
#else
    (void)now;
#endif
}

void Battery_Task(void)
{
    IP5310_Snapshot sample;
    uint16_t adc = 0;
    int16_t temperature = 0;
    int result;
    uint32_t now = BSP_Millis();
    uint32_t elapsed = now - last_poll_ms;
    if (elapsed < BATTERY_POLL_MS) {
        return;
    }
    last_poll_ms = now;
    status.updated_ms = now;
    poll_io_failed = 0U;
    poll_sleep_seen = 0U;
    status.faults = 0;
#if !BATTERY_TEMP_ADC_CONNECTED
    status.faults |= BATTERY_FAULT_TEMP_UNCONNECTED;
#endif
    status.awake = BSP_Battery_IsAwake();
    if (!status.awake) {
        was_awake = 0;
        invalidate_snapshot(BATTERY_LINK_STANDBY, 0);
        BSP_Battery_ReleaseBus();
        publish();
        return;
    }
    if (!was_awake) {
        was_awake = 1;
        awake_since_ms = now;
        invalidate_snapshot(BATTERY_LINK_STARTING, 0);
    }
    if ((uint32_t)(now - awake_since_ms) < BATTERY_WAKE_SETTLE_MS) {
        publish();
        return;
    }
    if (elapsed > BATTERY_STALE_MS) {
        fail_pending_current(IP5310_ERROR_IO);
        reset_policy();
        critical_count = 0;
        critical_recovery = 0;
    }
#if BATTERY_CONTROL_ENABLED
    /* At boot/wake/recovery do not wait for a good temperature to request OFF. */
    if (configured_ma == 0 && !status.charge_stop_verified) {
        try_stop_charge();
        if (!status.charge_stop_verified) {
            if (poll_sleep_seen) {
                publish();
                return;
            }
            invalidate_snapshot(BATTERY_LINK_BUS_ERROR, BATTERY_FAULT_CONTROL);
            publish();
            return;
        }
    }
#endif
    result = IP5310_ReadSnapshot(&chip, &sample);
    if (result != 0) {
        if (result == IP5310_ERROR_IO && handle_sleep_edge()) {
            publish();
            return;
        }
        if (result == IP5310_ERROR_DATA) {
            invalidate_snapshot(BATTERY_LINK_DATA_ERROR, BATTERY_FAULT_DATA);
        } else {
            ++status.bus_error_count;
            invalidate_snapshot(BATTERY_LINK_BUS_ERROR, BATTERY_FAULT_BUS);
        }
        try_stop_charge();
        publish();
        return;
    }
    status.chip = sample;
    if (!sample.soc_valid || !sample.ntc_valid) {
        invalidate_snapshot(BATTERY_LINK_DATA_ERROR, BATTERY_FAULT_DATA);
        try_stop_charge();
        publish();
        return;
    }
    status.snapshot_valid = 1;
    status.last_good_ms = now;
    status.link = BATTERY_LINK_READY;
    status.temperature_valid = 0;
    if (BSP_Battery_ReadTemperatureAdc(&adc) == 0 &&
        Battery_TemperatureFromAdc(adc, &temperature) == 0) {
        status.temperature_adc = adc;
        status.temperature_dc = temperature;
        status.temperature_valid = 1;
    } else {
#if BATTERY_TEMP_ADC_CONNECTED
        status.faults |= BATTERY_FAULT_TEMP_SENSOR;
#endif
    }
    if (sample.ntc_high || sample.ntc_mid || sample.ntc_low) {
        status.faults |= BATTERY_FAULT_NTC;
    }
    status.charge_policy = Battery_PolicyUpdate(&policy,
        status.temperature_valid, status.temperature_dc,
        (uint8_t)(sample.ntc_high || sample.ntc_mid || sample.ntc_low));
    update_motion_request();
    update_charge_control();
    update_vbus_current();
    update_keep_5v(now);
    publish();
}

void Battery_GetStatus(Battery_Status *output)
{
    if (output == 0) {
        return;
    }
    *output = status;
    if (output->snapshot_valid &&
        (uint32_t)(BSP_Millis() - output->last_good_ms) > BATTERY_STALE_MS) {
        output->snapshot_valid = 0;
        output->temperature_valid = 0;
        output->link = BATTERY_LINK_STALE;
        output->faults |= BATTERY_FAULT_STALE;
        output->motion_request = BATTERY_MOTION_STOP_REQUEST;
        output->charge_policy = BATTERY_CHARGE_INHIBITED;
        output->control_verified = 0;
        output->charge_stop_verified = 0;
        output->applied_nominal_ma = 0;
        output->charge_config_valid = 0;
        output->power_config_valid = output->keep_5v_verified = 0U;
    }
}
