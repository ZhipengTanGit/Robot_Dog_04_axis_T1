#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define I2C_DIAGNOSTICS_FIRST_ADDRESS 0x08U
#define I2C_DIAGNOSTICS_LAST_ADDRESS 0x77U
#define I2C_DIAGNOSTICS_ADDRESS_COUNT 112U
#define I2C_DIAGNOSTICS_CODEC_HZ 50000U
#define I2C_DIAGNOSTICS_CODEC_REGISTER 0x00U
typedef struct {
    char id[17], state[24];
    bool running, completed, codec_ack, codec_alternate_ack, audio_restart_required;
    bool codec_read_attempted, codec_alternate_read_attempted;
    uint8_t codec_reg00, codec_alternate_reg00, codec_unconfirmed_count;
    uint8_t addresses[I2C_DIAGNOSTICS_ADDRESS_COUNT], count, last_address, sda, scl;
    uint16_t scanned, nack_count, timeout_count, error_count;
    uint32_t sequence, started_ms, completed_ms;
    esp_err_t error, last_probe_error;
    esp_err_t codec_read_error, codec_alternate_read_error;
} I2cDiagnostics_Status;
/* Explicit asynchronous scan: ordinary addresses use a 100 kHz ACK probe;
 * 0x10/0x11 use a 50 kHz register-0 read. No configuration writes or codec reinit.
 * id is exactly 16 ASCII hexadecimal characters. The last id is idempotent;
 * a different id during a scan returns ESP_ERR_INVALID_STATE (BUSY). */
esp_err_t I2cDiagnostics_RequestScan(const char *id);
void I2cDiagnostics_GetStatus(I2cDiagnostics_Status *out);

/* Pure result accumulation, also used by the host regression test. */
esp_err_t I2cDiagnostics_ResultRequest(I2cDiagnostics_Status *result, const char *id, uint32_t now, bool *start_scan);
void I2cDiagnostics_ResultRecord(I2cDiagnostics_Status *result, uint8_t address, esp_err_t error);
void I2cDiagnostics_ResultRecordCodec(I2cDiagnostics_Status *result, uint8_t address, esp_err_t error, bool read_attempted, uint8_t value);
void I2cDiagnostics_ResultFinish(I2cDiagnostics_Status *result, uint32_t now, bool audio_configured);
