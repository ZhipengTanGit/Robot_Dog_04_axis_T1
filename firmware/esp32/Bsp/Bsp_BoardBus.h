#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_XL_CAMERA_RESET 0x01U
#define BOARD_XL_CAMERA_PWDN 0x02U
#define BOARD_XL_LCD_RESET 0x04U
#define BOARD_XL_TOUCH_RESET 0x08U
#define BOARD_XL_SPEAKER_SD 0x10U /* MS8002A: HIGH shuts down the amplifier. */

typedef struct {
    uint8_t sda_level, scl_level;
    bool valid, sda_stable, scl_stable;
} BoardBus_LineSample;

typedef struct {
    char state[32];
    bool attempted, completed, reset_attempted, release_attempted;
    bool restore_attempted, verify_attempted, verified;
    uint32_t attempted_ms;
    esp_err_t result, lock_error, reset_error, release_error, restore_error, probe_error;
    BoardBus_LineSample before, after_reset, released, after_restore, after_verify;
    /* Bit order is XL9555 registers 2 (output), 6 (configuration), 0 (input).
     * A zero value is meaningful only when its valid bit is set. */
    uint8_t read_attempted, valid, output0, config0, input0;
    esp_err_t read_errors[3];
} BoardBus_Recovery;

typedef struct {
    uint8_t output0, config0, input0, valid;
    uint32_t transactions, errors;
    esp_err_t last_error;
    /* GPIO samples while our bus lock excludes transactions. These are
     * digital input levels, not voltage measurements or proof of bus health. */
    uint8_t sda_level, scl_level;
    bool line_sample_valid, sda_stable, scl_stable;
    uint32_t line_sample_age_ms;
    esp_err_t line_sample_error;
    BoardBus_Recovery recovery;
} BoardBus_Status;

/* Idempotent, thread safe. Owns the single new-driver I2C0 instance. */
esp_err_t BoardBus_Init(void);
i2c_master_bus_handle_t BoardBus_Handle(void);
esp_err_t BoardBus_AddDevice(uint8_t address, i2c_master_dev_handle_t *device);
/* Per-device clock. These functions acquire the shared lock themselves. */
esp_err_t BoardBus_AddDeviceAtSpeed(uint8_t address, uint32_t scl_speed_hz,
                                  i2c_master_dev_handle_t *device);
esp_err_t BoardBus_RemoveDevice(i2c_master_dev_handle_t device);
esp_err_t BoardBus_Probe(uint8_t address);
esp_err_t BoardBus_Write(i2c_master_dev_handle_t device, const uint8_t *data, size_t length);
esp_err_t BoardBus_Read(i2c_master_dev_handle_t device, const uint8_t *reg, size_t reg_len, uint8_t *data, size_t length);
esp_err_t BoardBus_XlRead(uint8_t reg, uint8_t *value);
/* Atomic read/modify/write/readback of one XL9555 register. */
esp_err_t BoardBus_XlUpdate(uint8_t reg, uint8_t mask, uint8_t bits);
esp_err_t BoardBus_XlOutput(uint8_t mask, uint8_t bits);
esp_err_t BoardBus_XlConfigureOutput(uint8_t mask, uint8_t initial);
void BoardBus_GetStatus(BoardBus_Status *out);
void BoardBus_RefreshStatus(void);
/* One bounded attempt, under the shared lock. Does not power-cycle clients.
 * Success requires high idle lines AND verified XL9555 register reads. */
esp_err_t BoardBus_Recover(void);
/* For a third-party SCCB implementation that owns its own device handle.
 * Do not call BoardBus transaction methods between these calls. */
esp_err_t BoardBus_BeginExclusive(void);
void BoardBus_EndExclusive(void);
