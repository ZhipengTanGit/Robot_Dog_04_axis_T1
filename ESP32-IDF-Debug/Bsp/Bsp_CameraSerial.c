#include "Bsp_CameraSerial.h"
#include "Bsp_Camera.h"
#include "CameraSerialLogic.h"
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct { char id[17]; uint32_t started_ms; } Job;
typedef struct {
    Job job;
    CameraSerial_Metadata meta;
    uint8_t *data;
    esp_err_t error;
} Result;
typedef struct { char id[17]; esp_err_t error; } Rejection;
static QueueHandle_t jobs, results, rejections;
static TaskHandle_t worker;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool busy;
static char last_id[17];
/* Active result and line belong exclusively to the STM->PC writer. */
static Result active;
static bool have_active, began;
static uint32_t offset, chunk_index;
static char line[CAMERA_SERIAL_LINE_BYTES];
static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void capture_task(void *unused)
{
    (void)unused;
    Job job;
    for (;;) {
        if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        Result result = {.job = job};
        Camera_Frame frame;
        result.error = Camera_AcquireFrame(&frame, 200);
        if (result.error == ESP_OK) {
            if (frame.length < 4 || frame.length > CAMERA_SERIAL_MAX_BYTES ||
                !frame.width || !frame.height || frame.width > 65535 || frame.height > 65535 ||
                frame.data[0] != 0xff || frame.data[1] != 0xd8 ||
                frame.data[frame.length - 2] != 0xff || frame.data[frame.length - 1] != 0xd9) {
                result.error = ESP_ERR_INVALID_SIZE;
            } else {
                result.data = heap_caps_malloc(frame.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!result.data) result.error = ESP_ERR_NO_MEM;
                else {
                    memcpy(result.data, frame.data, frame.length);
                    result.meta = (CameraSerial_Metadata){.bytes = frame.length, .width = frame.width,
                        .height = frame.height, .sequence = frame.sequence, .timestamp_ms = frame.timestamp_ms,
                        .crc32 = CameraSerial_Crc32(result.data, frame.length)};
                }
            }
            /* Return the driver FB on its owning task before serial transfer. */
            Camera_ReleaseFrame(&frame, result.error == ESP_OK);
        }
        if (xQueueSend(results, &result, 0) != pdTRUE) {
            free(result.data);
            portENTER_CRITICAL(&lock); busy = false; portEXIT_CRITICAL(&lock);
        }
    }
}
esp_err_t CameraSerial_Start(void)
{
    if (worker) return ESP_OK;
    jobs = xQueueCreate(1, sizeof(Job));
    results = xQueueCreate(1, sizeof(Result));
    rejections = xQueueCreate(2, sizeof(Rejection));
    if (jobs && results && rejections &&
        xTaskCreate(capture_task, "usb_snapshot", 4096, NULL, 1, &worker) == pdPASS) return ESP_OK;
    if (jobs) vQueueDelete(jobs);
    if (results) vQueueDelete(results);
    if (rejections) vQueueDelete(rejections);
    jobs = results = rejections = NULL; worker = NULL;
    return ESP_ERR_NO_MEM;
}
bool CameraSerial_Available(void) { return worker != NULL; }
bool CameraSerial_IsBusy(void)
{
    portENTER_CRITICAL(&lock); bool value = busy; portEXIT_CRITICAL(&lock); return value;
}
esp_err_t CameraSerial_Request(const char *id)
{
    if (!CameraSerial_ValidId(id)) return ESP_ERR_INVALID_ARG;
    if (!CameraSerial_Available()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&lock);
    bool duplicate = !strcmp(last_id, id), occupied = busy;
    if (!duplicate && !occupied) { busy = true; memcpy(last_id, id, 17); }
    portEXIT_CRITICAL(&lock);
    if (duplicate) return ESP_OK;
    if (occupied) {
        Rejection rejection = {.error = ESP_ERR_INVALID_STATE}; memcpy(rejection.id, id, 17);
        (void)xQueueSend(rejections, &rejection, 0);
        return ESP_ERR_INVALID_STATE;
    }
    Job job = {.started_ms = millis()}; memcpy(job.id, id, 17);
    if (xQueueSend(jobs, &job, 0) == pdTRUE) return ESP_OK;
    portENTER_CRITICAL(&lock); busy = false; portEXIT_CRITICAL(&lock);
    return ESP_ERR_NO_MEM;
}
static bool write_line(size_t length)
{
    if (!length) return false;
    if (uart_write_bytes(UART_NUM_0, line, length) == (int)length) return true;
    (void)uart_write_bytes(UART_NUM_0, "\r\n", 2);
    return false;
}
static void finish(bool delivered)
{
    if (!delivered && active.data) Camera_RecordSendFailure();
    free(active.data); memset(&active, 0, sizeof(active));
    have_active = began = false; offset = chunk_index = 0;
    portENTER_CRITICAL(&lock); busy = false; portEXIT_CRITICAL(&lock);
}
bool CameraSerial_WriteNext(void)
{
    if (!CameraSerial_Available()) return true;
    if (!have_active) {
        if (xQueueReceive(results, &active, 0) == pdTRUE) have_active = true;
    }
    /* Timeouts outrank BUSY replies. A stream of rejected new IDs must not
     * retain the active PSRAM image or the request slot beyond its deadline. */
    if (have_active && (uint32_t)(millis() - active.job.started_ms) >= CAMERA_SERIAL_TIMEOUT_MS)
        active.error = ESP_ERR_TIMEOUT;
    if (have_active && active.error != ESP_OK) {
        bool ok = write_line(CameraSerial_FormatError(line, sizeof(line), active.job.id, active.error,
            active.error == ESP_ERR_TIMEOUT ? "TIMEOUT" : "CAPTURE_FAILED"));
        finish(false); return ok;
    }
    Rejection rejection;
    if (xQueueReceive(rejections, &rejection, 0) == pdTRUE)
        return write_line(CameraSerial_FormatError(line, sizeof(line), rejection.id, rejection.error, "BUSY"));
    if (!have_active) return true;
    bool ok;
    if (!began) {
        ok = write_line(CameraSerial_FormatBegin(line, sizeof(line), active.job.id, &active.meta));
        began = true;
    } else if (offset < active.meta.bytes) {
        size_t count = active.meta.bytes - offset;
        if (count > CAMERA_SERIAL_CHUNK_BYTES) count = CAMERA_SERIAL_CHUNK_BYTES;
        ok = write_line(CameraSerial_FormatData(line, sizeof(line), active.job.id, chunk_index, active.data + offset, count));
        offset += count; ++chunk_index;
    } else {
        ok = write_line(CameraSerial_FormatEnd(line, sizeof(line), active.job.id, &active.meta));
        finish(ok); return ok;
    }
    if (!ok) finish(false);
    return ok;
}
