#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct {
    bool attempted, completed, consistent, raw_repeatable;
    uint8_t id[2][3], power[2];
    esp_err_t id_error[2], power_error[2];
    esp_err_t setup_error, cleanup_error, restore_error;
    uint32_t transport_errors, sampled_ms;
    char state[24];
} Display_RegisterReadback;
typedef struct {
    char state[24];
    char init_stage[32];
    char transport[24];
    uint32_t write_hz;
    bool readback_enabled;
    esp_err_t error;
    bool configured;
    uint32_t init_attempts;
    bool retry_pending;
    uint32_t frames, write_errors, te_edges;
    bool te_sync_enabled, te_signal_present;
    uint32_t te_period_us, te_age_ms, te_syncs, te_timeouts, te_late_skips, te_invalid_periods;
    uint32_t te_overruns, last_draw_us, max_draw_us;
    uint8_t pending_strips;
    char ui_mode[12], expression[12];
    uint32_t face_frames, screen_taps, head_touch_events, touch_empty_reports;
    bool head_touch;
    char touch_report[24];
    Display_RegisterReadback readback_reset, readback_initialized;
    char touch_state[24];
    esp_err_t touch_error;
    bool touch_present, touch_valid;
    uint8_t touch_count;
    uint16_t x[2], y[2];
    uint32_t touch_reads, touch_errors, last_touch_ms;
} Display_Status;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t Display_Start(void);
esp_err_t Display_RequestFace(bool visible);
/* Requests are consumed by the display task. Head touch temporarily overrides mood. */
void Display_SetVoiceEmotion(unsigned mood);
void Display_GetStatus(Display_Status *out);
void Display_UpdateRobot(const char *battery_line, const char *peripheral_line);

#ifdef __cplusplus
}
#endif
