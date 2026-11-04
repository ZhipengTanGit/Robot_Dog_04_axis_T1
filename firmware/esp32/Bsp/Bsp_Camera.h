#ifndef BSP_CAMERA_H
#define BSP_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool ready;                /* Sensor initialized; a later capture can still fail. */
    char state[20];            /* OFF, STARTING, READY, or an explicit failure stage. */
    esp_err_t error;
    uint16_t sensor_pid;
    uint32_t psram_bytes;
    uint16_t width, height;
    uint32_t frames;           /* Valid JPEG frames obtained, not guaranteed delivered. */
    uint32_t capture_failures;
    uint32_t send_failures;
    uint32_t last_frame_ms;    /* Sensor frame timestamp since ESP32 boot. */
    uint16_t http_port;
    bool http_ready;
    /* Read-only preflight evidence. Bits 0/1/2 of xl_valid identify valid
     * output/config/input snapshots respectively; PID fields need ESP_OK.
     */
    uint8_t xl_output0, xl_config0, xl_input0, xl_valid;
    esp_err_t xl_error;
    bool diag_complete;
    esp_err_t sensor_probe_error;
    esp_err_t id_restart_error, id_stop_error;
    uint16_t id_restart, id_stop;
    uint8_t sda_level, scl_level;
} Camera_Status;

/* Starts one initialization task. Camera failure never stops UART/network tasks.
 * Uses the new IDF I2C driver on I2C0, GPIO8 SDA / GPIO19 SCL, shared with SCCB.
 * Call once from app_main; repeated calls are harmless and do not reset hardware.
 */
esp_err_t Camera_Start(void);
void Camera_GetStatus(Camera_Status *out);

typedef struct {
    char id[17], state[24], stage[24];
    bool running, completed, usable, expander_ok, sccb_ack, id_ok, init_ok, frame_ok;
    uint32_t sequence, started_ms, completed_ms, frame_bytes, frame_timestamp_ms;
    uint16_t sensor_pid, width, height;
    uint8_t xl_output0, xl_config0, xl_input0, xl_valid;
    esp_err_t error, expander_error, sccb_error, id_error, init_error, frame_error;
} Camera_CheckStatus;
/* Explicit asynchronous re-check. Exactly 16 lower-case hex chars. Last ID is
 * idempotent; a different ID during a check returns ESP_ERR_INVALID_STATE.
 * Success requires a real, recent OV5640 JPEG. Does not transmit the image. */
esp_err_t Camera_RequestCheck(const char *id);
void Camera_GetCheckStatus(Camera_CheckStatus *out);
#define CAMERA_DIAGNOSTICS_CAPACITY 1024U
/* First bounded SDK ERROR messages from the current check's initialization,
 * followed by RAM capture-event counters after the first-frame validation.
 * Thread-safe, always NUL-terminated when out != NULL and capacity > 0. */
void Camera_GetDiagnostics(char *out, size_t capacity);
/* Runtime calibration; the frame mutex is bounded to 200 ms and BoardBus
 * retains its normal timeout. Results append actual register readback as
 * CAM_COLOR_RGB in diagnostics. SET is volatile until reset or camera recheck.
 * Modes: 0 SDK advanced auto, 1 simple auto, 2 office, 3 manual RGB (256..4095).
 * RGB arguments are ignored for modes 0..2. No color matrix/clock changes. */
esp_err_t Camera_ReadColor(void);
esp_err_t Camera_SetColor(uint8_t mode, uint16_t r, uint16_t g, uint16_t b);

typedef struct {
    const uint8_t *data;
    size_t length;
    uint32_t width, height, sequence, timestamp_ms;
    void *opaque;
} Camera_Frame;

/* Caller must release every successful acquire, including upload failures.
 * One lease at a time; call release on the task that acquired it. timeout_ms
 * bounds the lease mutex; the camera driver's fb_get can wait up to 4 seconds.
 * This module does not create sockets; Bsp_Wifi serves authenticated frames.
 */
esp_err_t Camera_AcquireFrame(Camera_Frame *out, uint32_t timeout_ms);
void Camera_ReleaseFrame(Camera_Frame *frame, bool sent_ok);
/* For a private frame copy sent after returning its lease: count a later
 * failed transfer exactly once without retaining or touching the driver FB. */
void Camera_RecordSendFailure(void);

#endif
