#include "Bsp_Peripherals.h"
#include "Bsp_BoardBus.h"
#include "Bsp_Display.h"
#include "Bsp_Audio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>

static bool started;
static void diagnostic_task(void *unused)
{
    (void)unused;
    unsigned bad_rounds = 0;
    bool recovery_attempted = false;
    for (;;) {
        BoardBus_RefreshStatus();
        BoardBus_Status bus;
        BoardBus_GetStatus(&bus);
        bool bad = bus.valid != 7 || (bus.line_sample_valid &&
            (bus.sda_level == 0 || bus.scl_level == 0));
        if (!bad) bad_rounds = 0;
        else if (bad_rounds < 3) ++bad_rounds;
        if (!recovery_attempted && bad_rounds >= 3) {
            /* Even a lock/setup failure consumes this boot's one attempt. */
            recovery_attempted = true;
            BoardBus_Recover();
            BoardBus_RefreshStatus();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
esp_err_t Peripherals_Start(void)
{
    if (started) return ESP_OK;
    esp_err_t bus = BoardBus_Init();
    if (bus != ESP_OK) return bus;
    started = true;
    /* Each child owns its state. A missing screen/codec never aborts boot. */
    esp_err_t audio = Audio_Start();
    esp_err_t display = Display_Start();
    if (xTaskCreate(diagnostic_task, "periph_diag", 2048, NULL, 1, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return audio != ESP_OK ? audio : display;
}
void Peripherals_UpdateRobot(const char *battery_line, const char *peripheral_line)
{
    Display_UpdateRobot(battery_line, peripheral_line);
}
esp_err_t Peripherals_PlayTone(unsigned hz, unsigned duration_ms, unsigned volume_percent)
{
    return Audio_PlayTone(hz, duration_ms, volume_percent);
}

static void add_line_sample(cJSON *json, const char *name, const BoardBus_LineSample *sample)
{
    cJSON *item = cJSON_AddObjectToObject(json, name);
    if (!item) return;
    cJSON_AddBoolToObject(item, "valid", sample->valid);
    if (sample->valid) {
        cJSON_AddNumberToObject(item, "sda", sample->sda_level);
        cJSON_AddNumberToObject(item, "scl", sample->scl_level);
        cJSON_AddBoolToObject(item, "sda_stable", sample->sda_stable);
        cJSON_AddBoolToObject(item, "scl_stable", sample->scl_stable);
    } else {
        cJSON_AddNullToObject(item, "sda"); cJSON_AddNullToObject(item, "scl");
        cJSON_AddNullToObject(item, "sda_stable"); cJSON_AddNullToObject(item, "scl_stable");
    }
}

static void add_step_error(cJSON *json, const char *name, bool attempted, esp_err_t error)
{
    if (attempted) cJSON_AddNumberToObject(json, name, error);
    else cJSON_AddNullToObject(json, name);
}

static void add_recovery_status(cJSON *xl, const BoardBus_Recovery *recovery)
{
    cJSON *item = cJSON_AddObjectToObject(xl, "recovery");
    if (!item) return;
    cJSON_AddStringToObject(item, "state", recovery->state);
    cJSON_AddBoolToObject(item, "attempted", recovery->attempted);
    cJSON_AddBoolToObject(item, "completed", recovery->completed);
    cJSON_AddBoolToObject(item, "verified", recovery->verified);
    if (recovery->attempted) cJSON_AddNumberToObject(item, "attempted_ms", recovery->attempted_ms);
    else cJSON_AddNullToObject(item, "attempted_ms");
    add_step_error(item, "result", recovery->completed, recovery->result);
    add_step_error(item, "lock_error", recovery->completed, recovery->lock_error);
    add_step_error(item, "reset_error", recovery->reset_attempted, recovery->reset_error);
    add_step_error(item, "release_error", recovery->release_attempted, recovery->release_error);
    add_step_error(item, "restore_error", recovery->restore_attempted, recovery->restore_error);
    add_step_error(item, "probe_error", recovery->verify_attempted, recovery->probe_error);
    add_line_sample(item, "before", &recovery->before);
    add_line_sample(item, "after_reset", &recovery->after_reset);
    add_line_sample(item, "released", &recovery->released);
    add_line_sample(item, "after_restore", &recovery->after_restore);
    add_line_sample(item, "after_verify", &recovery->after_verify);
    cJSON_AddNumberToObject(item, "read_attempted_mask", recovery->read_attempted);
    cJSON_AddNumberToObject(item, "valid_mask", recovery->valid);
    const char *error_names[] = {"output0_error", "config0_error", "input0_error"};
    const char *value_names[] = {"output0", "config0", "input0"};
    const uint8_t values[] = {recovery->output0, recovery->config0, recovery->input0};
    for (unsigned i = 0; i < 3; ++i) {
        add_step_error(item, error_names[i], (recovery->read_attempted & (1U << i)) != 0,
                       recovery->read_errors[i]);
        if (recovery->valid & (1U << i)) cJSON_AddNumberToObject(item, value_names[i], values[i]);
        else cJSON_AddNullToObject(item, value_names[i]);
    }
}

cJSON *Peripherals_CreateJson(void)
{
    BoardBus_Status bus; BoardBus_GetStatus(&bus);
    Display_Status display; Display_GetStatus(&display);
    Audio_Status audio; Audio_GetStatus(&audio);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON *xl = cJSON_AddObjectToObject(json, "expander");
    cJSON *lcd = cJSON_AddObjectToObject(json, "display");
    cJSON *tp = cJSON_AddObjectToObject(json, "touch");
    cJSON *mic = cJSON_AddObjectToObject(json, "audio");
    if (!xl || !lcd || !tp || !mic) { cJSON_Delete(json); return NULL; }
    cJSON_AddStringToObject(xl, "model", "XL9555");
    cJSON_AddNumberToObject(xl, "valid_mask", bus.valid);
    cJSON_AddNumberToObject(xl, "output0", bus.output0);
    cJSON_AddNumberToObject(xl, "config0", bus.config0);
    cJSON_AddNumberToObject(xl, "input0", bus.input0);
    cJSON_AddNumberToObject(xl, "bus_errors", bus.errors);
    cJSON_AddNumberToObject(xl, "bus_transactions", bus.transactions);
    cJSON_AddNumberToObject(xl, "last_error", bus.last_error);
    cJSON_AddBoolToObject(xl, "line_sample_valid", bus.line_sample_valid);
    cJSON_AddNumberToObject(xl, "line_sample_error", bus.line_sample_error);
    if (bus.line_sample_valid) {
        cJSON_AddNumberToObject(xl, "sda_level", bus.sda_level);
        cJSON_AddNumberToObject(xl, "scl_level", bus.scl_level);
        cJSON_AddBoolToObject(xl, "sda_stable", bus.sda_stable);
        cJSON_AddBoolToObject(xl, "scl_stable", bus.scl_stable);
    } else {
        cJSON_AddNullToObject(xl, "sda_level"); cJSON_AddNullToObject(xl, "scl_level");
        cJSON_AddNullToObject(xl, "sda_stable"); cJSON_AddNullToObject(xl, "scl_stable");
    }
    if (bus.line_sample_age_ms != UINT32_MAX)
        cJSON_AddNumberToObject(xl, "line_sample_age_ms", bus.line_sample_age_ms);
    else cJSON_AddNullToObject(xl, "line_sample_age_ms");
    add_recovery_status(xl, &bus.recovery);
    cJSON_AddStringToObject(lcd, "model", "PY206 CO5300");
    cJSON_AddStringToObject(lcd, "state", display.state);
    cJSON_AddStringToObject(lcd, "init_stage", display.init_stage);
    cJSON_AddNumberToObject(lcd, "init_attempts", display.init_attempts);
    cJSON_AddBoolToObject(lcd, "retry_pending", display.retry_pending);
    cJSON_AddStringToObject(lcd, "transport", display.transport);
    cJSON_AddNumberToObject(lcd, "write_hz", display.write_hz);
    cJSON_AddBoolToObject(lcd, "configured", display.configured);
    cJSON_AddBoolToObject(lcd, "visual_verified", false);
    cJSON_AddNumberToObject(lcd, "error", display.error);
    cJSON_AddNumberToObject(lcd, "width", 410); cJSON_AddNumberToObject(lcd, "height", 502);
    cJSON_AddNumberToObject(lcd, "refreshes", display.frames);
    cJSON_AddNumberToObject(lcd, "write_errors", display.write_errors);
    cJSON_AddNumberToObject(lcd, "te_edges", display.te_edges);
    cJSON_AddBoolToObject(lcd, "te_sync_enabled", display.te_sync_enabled);
    cJSON_AddBoolToObject(lcd, "te_signal_present", display.te_signal_present);
    cJSON_AddNumberToObject(lcd, "te_gpio", 13);
    cJSON_AddNumberToObject(lcd, "te_period_us", display.te_period_us);
    if (display.te_age_ms != UINT32_MAX) cJSON_AddNumberToObject(lcd, "te_age_ms", display.te_age_ms);
    else cJSON_AddNullToObject(lcd, "te_age_ms");
    cJSON_AddNumberToObject(lcd, "te_syncs", display.te_syncs);
    cJSON_AddNumberToObject(lcd, "te_timeouts", display.te_timeouts);
    cJSON_AddNumberToObject(lcd, "te_late_skips", display.te_late_skips);
    cJSON_AddNumberToObject(lcd, "te_overruns", display.te_overruns);
    cJSON_AddNumberToObject(lcd, "te_invalid_periods", display.te_invalid_periods);
    cJSON_AddNumberToObject(lcd, "last_draw_us", display.last_draw_us);
    cJSON_AddNumberToObject(lcd, "max_draw_us", display.max_draw_us);
    cJSON_AddNumberToObject(lcd, "pending_strips", display.pending_strips);
    cJSON *readback = cJSON_AddObjectToObject(lcd, "register_readback");
    if (readback) {
        cJSON_AddBoolToObject(readback, "enabled", display.readback_enabled);
        cJSON_AddNumberToObject(readback, "write_hz", display.write_hz);
        cJSON_AddNullToObject(readback, "read_hz");
        cJSON_AddNumberToObject(readback, "data_gpio", 3);
        cJSON_AddStringToObject(readback, "method", "DISABLED_REFERENCE_PATH");
        const Display_RegisterReadback *results[] = {&display.readback_reset, &display.readback_initialized};
        const char *names[] = {"reset", "initialized"};
        const char *stages[] = {"AFTER_RESET", "AFTER_INIT"};
        for (unsigned phase = 0; phase < 2; ++phase) {
            const Display_RegisterReadback *result = results[phase];
            cJSON *item = cJSON_AddObjectToObject(readback, names[phase]);
            if (!item) continue;
            cJSON_AddStringToObject(item, "stage", stages[phase]);
            cJSON_AddStringToObject(item, "state", result->attempted ? result->state : "NOT_ATTEMPTED");
            cJSON_AddBoolToObject(item, "attempted", result->attempted);
            cJSON_AddBoolToObject(item, "completed", result->completed);
            cJSON_AddBoolToObject(item, "consistent", result->consistent);
            /* Repeatable raw values are evidence only, not panel identification. */
            cJSON_AddBoolToObject(item, "raw_repeatable", result->raw_repeatable);
            cJSON_AddNumberToObject(item, "transport_errors", result->transport_errors);
            if (result->attempted) {
                cJSON_AddNumberToObject(item, "setup_error", result->setup_error);
                cJSON_AddNumberToObject(item, "cleanup_error", result->cleanup_error);
                cJSON_AddNumberToObject(item, "restore_error", result->restore_error);
            } else {
                cJSON_AddNullToObject(item, "setup_error");
                cJSON_AddNullToObject(item, "cleanup_error");
                cJSON_AddNullToObject(item, "restore_error");
            }
            if (result->completed) cJSON_AddNumberToObject(item, "sampled_ms", result->sampled_ms);
            else cJSON_AddNullToObject(item, "sampled_ms");
            cJSON *attempts = cJSON_AddArrayToObject(item, "attempts");
            if (!attempts || !result->attempted) continue;
            for (unsigned i = 0; i < 2; ++i) {
                cJSON *sample = cJSON_CreateObject();
                if (!sample) continue;
                if (!cJSON_AddItemToArray(attempts, sample)) { cJSON_Delete(sample); continue; }
                cJSON_AddNumberToObject(sample, "id_error", result->id_error[i]);
                cJSON_AddNumberToObject(sample, "power_error", result->power_error[i]);
                if (result->completed && result->id_error[i] == ESP_OK) {
                    char hex[7];
                    snprintf(hex, sizeof(hex), "%02X%02X%02X", result->id[i][0], result->id[i][1], result->id[i][2]);
                    cJSON_AddStringToObject(sample, "id_hex", hex);
                } else cJSON_AddNullToObject(sample, "id_hex");
                if (result->completed && result->power_error[i] == ESP_OK)
                    cJSON_AddNumberToObject(sample, "power_raw", result->power[i]);
                else cJSON_AddNullToObject(sample, "power_raw");
            }
        }
    }
    cJSON_AddStringToObject(tp, "model", "CST9217");
    cJSON_AddStringToObject(tp, "state", display.touch_state);
    cJSON_AddBoolToObject(tp, "present", display.touch_present);
    cJSON_AddBoolToObject(tp, "valid", display.touch_valid && now - display.last_touch_ms < 1000);
    cJSON_AddBoolToObject(tp, "chip_id_verified", false);
    cJSON_AddNumberToObject(tp, "error", display.touch_error);
    cJSON_AddNumberToObject(tp, "count", display.touch_count);
    cJSON_AddNumberToObject(tp, "x", display.x[0]); cJSON_AddNumberToObject(tp, "y", display.y[0]);
    cJSON_AddNumberToObject(tp, "x2", display.x[1]); cJSON_AddNumberToObject(tp, "y2", display.y[1]);
    cJSON_AddNumberToObject(tp, "reads", display.touch_reads);
    cJSON_AddNumberToObject(tp, "read_errors", display.touch_errors);
    if (display.last_touch_ms) cJSON_AddNumberToObject(tp, "age_ms", now - display.last_touch_ms);
    else cJSON_AddNullToObject(tp, "age_ms");
    cJSON_AddStringToObject(mic, "model", "ES8388 + LCA3729T381");
    cJSON_AddStringToObject(mic, "state", audio.state);
    cJSON_AddBoolToObject(mic, "configured", audio.configured);
    cJSON_AddBoolToObject(mic, "valid", audio.valid && now - audio.last_sample_ms < 1000);
    cJSON_AddNumberToObject(mic, "error", audio.error);
    cJSON_AddNumberToObject(mic, "sample_rate", audio.sample_rate);
    cJSON_AddNumberToObject(mic, "samples", audio.samples); cJSON_AddNumberToObject(mic, "blocks", audio.blocks);
    cJSON_AddNumberToObject(mic, "rms", audio.rms); cJSON_AddNumberToObject(mic, "ac_rms", audio.ac_rms);
    cJSON_AddNumberToObject(mic, "peak", audio.peak); cJSON_AddNumberToObject(mic, "dc", audio.dc);
    cJSON_AddNumberToObject(mic, "right_rms", audio.right_rms);
    cJSON_AddNumberToObject(mic, "right_ac_rms", audio.right_ac_rms);
    cJSON_AddNumberToObject(mic, "right_peak", audio.right_peak);
    cJSON_AddNumberToObject(mic, "right_dc", audio.right_dc);
    cJSON_AddStringToObject(mic, "units", "PCM16 counts (not dB SPL)");
    cJSON_AddNumberToObject(mic, "clipped", audio.clipped);
    cJSON_AddNumberToObject(mic, "read_errors", audio.read_errors);
    cJSON_AddBoolToObject(mic, "speaker_active", audio.speaker_active);
    cJSON_AddNumberToObject(mic, "tone_completed", audio.tone_completed);
    cJSON_AddNumberToObject(mic, "tone_errors", audio.tone_errors);
    cJSON_AddStringToObject(mic, "tone_request_id", audio.tone_request_id);
    cJSON_AddStringToObject(mic, "tone_state", audio.tone_state);
    cJSON_AddNumberToObject(mic, "tone_error", audio.tone_error);
    cJSON_AddBoolToObject(mic, "speaker_shutdown_confirmed", audio.speaker_shutdown_confirmed);
    cJSON_AddNumberToObject(mic, "tone_requested", audio.tone_requested);
    cJSON_AddNumberToObject(mic, "tone_started", audio.tone_started);
    cJSON_AddNumberToObject(mic, "tone_stopped", audio.tone_stopped);
    cJSON_AddNumberToObject(mic, "tone_hz", audio.tone_hz);
    cJSON_AddNumberToObject(mic, "tone_duration_ms", audio.tone_duration_ms);
    cJSON_AddNumberToObject(mic, "tone_volume_percent", audio.tone_volume_percent);
    cJSON_AddStringToObject(mic, "init_stage", audio.init_stage);
    cJSON_AddNumberToObject(mic, "init_attempts", audio.init_attempts);
    cJSON_AddNumberToObject(mic, "last_init_ms", audio.last_init_ms);
    cJSON_AddNumberToObject(mic, "codec_i2c_hz", audio.codec_i2c_hz);
    cJSON_AddBoolToObject(mic, "retry_pending", audio.retry_pending);
    cJSON_AddNumberToObject(mic, "next_retry_ms", audio.next_retry_ms);
    cJSON_AddNumberToObject(mic, "init_failures", audio.init_failures);
    cJSON_AddNumberToObject(mic, "last_failure_ms", audio.last_failure_ms);
    cJSON_AddStringToObject(mic, "last_failure_stage", audio.last_failure_stage);
    cJSON_AddNumberToObject(mic, "last_failure_register", audio.last_failure_register);
    cJSON_AddNumberToObject(mic, "last_failure_error", audio.last_failure_error);
    cJSON_AddNumberToObject(mic, "shutdown_error", audio.shutdown_error);
    cJSON_AddNumberToObject(mic, "cleanup_error", audio.cleanup_error);
    cJSON_AddBoolToObject(mic, "mclk_test_performed", audio.mclk_test_performed);
    cJSON_AddNumberToObject(mic, "mclk_test_hz", audio.mclk_test_hz);
    cJSON_AddNumberToObject(mic, "probe_before_mclk", audio.probe_before_mclk);
    cJSON_AddNumberToObject(mic, "probe_after_mclk", audio.probe_after_mclk);
    cJSON_AddNumberToObject(mic, "probe_alternate", audio.probe_alternate);
    cJSON_AddNumberToObject(mic, "failed_register", audio.failed_register);
    cJSON_AddBoolToObject(mic, "verify_valid", audio.verify_valid);
    if (audio.verify_valid) {
        cJSON_AddNumberToObject(mic, "verify_register", audio.verify_register);
        cJSON_AddNumberToObject(mic, "verify_value", audio.verify_value);
        cJSON_AddNumberToObject(mic, "verify_expected", audio.verify_expected);
        cJSON_AddNumberToObject(mic, "verify_mask", audio.verify_mask);
    } else {
        cJSON_AddNullToObject(mic, "verify_register");
        cJSON_AddNullToObject(mic, "verify_value");
        cJSON_AddNullToObject(mic, "verify_expected");
        cJSON_AddNullToObject(mic, "verify_mask");
    }
    if (audio.dac_control3_readback_valid)
        cJSON_AddNumberToObject(mic, "dac_control3_readback", audio.dac_control3_readback);
    else cJSON_AddNullToObject(mic, "dac_control3_readback");
    cJSON *adc = cJSON_AddObjectToObject(mic, "adc_registers");
    for (unsigned i = 0; i < AUDIO_ADC_REGISTER_COUNT; ++i) {
        const Audio_RegisterReadback *value = &audio.adc_registers[i];
        if (!value->address) continue; /* Audio task has not begun its first attempt. */
        char name[3]; snprintf(name, sizeof(name), "%02X", value->address);
        cJSON *entry = cJSON_AddObjectToObject(adc, name);
        cJSON_AddNumberToObject(entry, "expected", value->expected);
        cJSON_AddNumberToObject(entry, "mask", value->mask);
        cJSON_AddNumberToObject(entry, "read_error", value->read_error);
        if (value->valid) {
            cJSON_AddNumberToObject(entry, "value", value->value);
            cJSON_AddBoolToObject(entry, "matches", value->matches);
        } else {
            cJSON_AddNullToObject(entry, "value");
            cJSON_AddNullToObject(entry, "matches");
        }
    }
    if (audio.last_sample_ms) cJSON_AddNumberToObject(mic, "age_ms", now - audio.last_sample_ms);
    else cJSON_AddNullToObject(mic, "age_ms");
    return json;
}
