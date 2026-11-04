#include "Bsp_I2cDiagnostics.h"
#include <string.h>

esp_err_t I2cDiagnostics_ResultRequest(I2cDiagnostics_Status *result, const char *id, uint32_t now, bool *start_scan)
{
    if (start_scan) *start_scan = false;
    if (!result || !id || !start_scan) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < 16; ++i) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return ESP_ERR_INVALID_ARG;
    }
    if (id[16]) return ESP_ERR_INVALID_ARG;
    if (!strcmp(id, result->id)) return ESP_OK;
    if (result->running) return ESP_ERR_INVALID_STATE;
    uint32_t sequence = result->sequence + 1;
    memset(result, 0, sizeof(*result));
    memcpy(result->id, id, sizeof(result->id));
    strcpy(result->state, "SCANNING");
    result->running = true;
    result->sequence = sequence;
    result->started_ms = now;
    result->sda = 8;
    result->scl = 19;
    result->codec_read_error = result->codec_alternate_read_error = ESP_ERR_INVALID_STATE;
    *start_scan = true;
    return ESP_OK;
}
void I2cDiagnostics_ResultRecord(I2cDiagnostics_Status *result, uint8_t address, esp_err_t error)
{
    if (!result || !result->running || address < I2C_DIAGNOSTICS_FIRST_ADDRESS ||
        address > I2C_DIAGNOSTICS_LAST_ADDRESS) return;
    ++result->scanned;
    result->last_address = address;
    result->last_probe_error = error;
    if (error == ESP_OK) {
        if (result->count < I2C_DIAGNOSTICS_ADDRESS_COUNT)
            result->addresses[result->count++] = address;
        if (address == 0x10) result->codec_ack = true;
        if (address == 0x11) result->codec_alternate_ack = true;
    } else if (error == ESP_ERR_NOT_FOUND) {
        ++result->nack_count; /* Normal absent-device result, not a bus failure. */
    } else {
        if (error == ESP_ERR_TIMEOUT) ++result->timeout_count;
        else ++result->error_count;
        if (result->error == ESP_OK) result->error = error;
    }
}
void I2cDiagnostics_ResultRecordCodec(I2cDiagnostics_Status *result, uint8_t address, esp_err_t error, bool read_attempted, uint8_t value)
{
    if (!result || !result->running || (address != 0x10 && address != 0x11)) return;
    if (!read_attempted && error == ESP_OK) error = ESP_ERR_INVALID_STATE;
    if (address == 0x10) {
        result->codec_read_attempted = read_attempted;
        result->codec_read_error = error;
        result->codec_reg00 = error == ESP_OK ? value : 0;
    } else {
        result->codec_alternate_read_attempted = read_attempted;
        result->codec_alternate_read_error = error;
        result->codec_alternate_reg00 = error == ESP_OK ? value : 0;
    }
    /* IDF 5.5 register reads report an absent address as INVALID_STATE rather
     * than probe's NOT_FOUND. Keep that raw result local to this candidate;
     * it neither proves a 100 kHz NACK nor a broken shared bus. */
    if (read_attempted && error == ESP_ERR_INVALID_STATE) {
        ++result->scanned;
        result->last_address = address;
        ++result->codec_unconfirmed_count;
    } else {
        esp_err_t previous_probe_error = result->last_probe_error;
        I2cDiagnostics_ResultRecord(result, address, error);
        result->last_probe_error = previous_probe_error;
    }
}
void I2cDiagnostics_ResultFinish(I2cDiagnostics_Status *result, uint32_t now, bool audio_configured)
{
    if (!result) return;
    result->running = false;
    result->completed = result->scanned == I2C_DIAGNOSTICS_ADDRESS_COUNT;
    result->completed_ms = now;
    result->audio_restart_required = result->codec_ack && !audio_configured;
    strcpy(result->state, result->completed ? (result->error == ESP_OK ? "COMPLETE" : "SCAN_ERRORS") : "ABORTED");
}

#ifndef I2C_DIAGNOSTICS_HOST_TEST
#include "Bsp_BoardBus.h"
#include "Bsp_Audio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static I2cDiagnostics_Status status = {.state = "IDLE", .sda = 8, .scl = 19};
static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void scan_task(void *unused)
{
    (void)unused;
    esp_err_t error = BoardBus_Init();
    if (error != ESP_OK) {
        portENTER_CRITICAL(&lock);
        status.error = error; status.running = false; status.completed_ms = millis();
        strcpy(status.state, "INIT_ERROR");
        portEXIT_CRITICAL(&lock);
        vTaskDelete(NULL);
        return;
    }
    for (unsigned address = I2C_DIAGNOSTICS_FIRST_ADDRESS; address <= I2C_DIAGNOSTICS_LAST_ADDRESS; ++address) {
        /* Serialize with camera SCCB, touch, codec and XL9555. The existing
         * shared lock has a bounded wait; never hold it for the whole scan. */
        bool codec = address == 0x10 || address == 0x11;
        bool acquired, read_attempted = false;
        uint8_t value = 0;
        esp_err_t cleanup_error = ESP_OK;
        if (codec) {
            i2c_master_dev_handle_t device = NULL;
            error = BoardBus_AddDeviceAtSpeed((uint8_t)address, I2C_DIAGNOSTICS_CODEC_HZ, &device);
            acquired = error == ESP_OK;
            if (acquired) {
                const uint8_t reg = I2C_DIAGNOSTICS_CODEC_REGISTER;
                read_attempted = true;
                error = BoardBus_Read(device, &reg, 1, &value, 1);
                cleanup_error = BoardBus_RemoveDevice(device);
            }
        } else {
            error = BoardBus_BeginExclusive();
            acquired = error == ESP_OK;
            if (acquired) {
                error = i2c_master_probe(BoardBus_Handle(), (uint16_t)address, 20);
                BoardBus_EndExclusive(); /* Also supplies CST9217 bus-free time. */
            }
        }
        portENTER_CRITICAL(&lock);
        if (codec) I2cDiagnostics_ResultRecordCodec(&status, (uint8_t)address, error, read_attempted, value);
        else I2cDiagnostics_ResultRecord(&status, (uint8_t)address, error);
        if (cleanup_error != ESP_OK) {
            ++status.error_count;
            if (status.error == ESP_OK) status.error = cleanup_error;
        }
        portEXIT_CRITICAL(&lock);
        if (!acquired || cleanup_error != ESP_OK) break; /* Avoid repeated lock failures or leaking scan handles. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    Audio_Status audio;
    Audio_GetStatus(&audio);
    portENTER_CRITICAL(&lock);
    I2cDiagnostics_ResultFinish(&status, millis(), audio.configured);
    portEXIT_CRITICAL(&lock);
    vTaskDelete(NULL);
}
esp_err_t I2cDiagnostics_RequestScan(const char *id)
{
    bool start_scan;
    portENTER_CRITICAL(&lock);
    esp_err_t error = I2cDiagnostics_ResultRequest(&status, id, millis(), &start_scan);
    portEXIT_CRITICAL(&lock);
    if (error != ESP_OK || !start_scan) return error;
    if (xTaskCreate(scan_task, "i2c_scan", 3072, NULL, 1, NULL) != pdPASS) {
        portENTER_CRITICAL(&lock);
        status.running = false; status.error = ESP_ERR_NO_MEM; status.completed_ms = millis();
        strcpy(status.state, "TASK_ERROR");
        portEXIT_CRITICAL(&lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void I2cDiagnostics_GetStatus(I2cDiagnostics_Status *out)
{
    if (!out) return;
    portENTER_CRITICAL(&lock); *out = status; portEXIT_CRITICAL(&lock);
}
#endif /* I2C_DIAGNOSTICS_HOST_TEST */
