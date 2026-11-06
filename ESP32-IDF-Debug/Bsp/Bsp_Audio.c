#include "Bsp_Audio.h"
#include "Bsp_Camera.h"
#include <string.h>

void Audio_PcmBufferDiscard(Audio_PcmBuffer *buffer)
{
    if (!buffer) return;
    buffer->dropped += buffer->count;
    buffer->count = buffer->head = 0;
    buffer->newest_ms = 0;
}
void Audio_PcmBufferStop(Audio_PcmBuffer *buffer)
{
    if (!buffer) return;
    Audio_PcmBufferDiscard(buffer);
    buffer->enabled = false;
    buffer->expires_ms = 0;
}
void Audio_PcmBufferArm(Audio_PcmBuffer *buffer, uint32_t expires_ms, bool reset)
{
    if (!buffer) return;
    if (reset || !buffer->enabled) Audio_PcmBufferDiscard(buffer);
    buffer->expires_ms = expires_ms;
    buffer->enabled = true;
}
static bool pcm_current(Audio_PcmBuffer *buffer, uint32_t now)
{
    if (!buffer || !buffer->enabled) return false;
    if ((int32_t)(now - buffer->expires_ms) >= 0) {
        Audio_PcmBufferStop(buffer);
        return false;
    }
    return true;
}
void Audio_PcmBufferPushLeft(Audio_PcmBuffer *buffer, const int16_t *stereo, size_t frames, uint32_t now)
{
    if (!stereo || !frames || !pcm_current(buffer, now)) return;
    if (frames > AUDIO_PCM_CAPACITY) {
        size_t skip = frames - AUDIO_PCM_CAPACITY;
        stereo += skip * 2;
        buffer->dropped += (uint32_t)skip;
        frames = AUDIO_PCM_CAPACITY;
    }
    for (size_t i = 0; i < frames; ++i) {
        buffer->samples[buffer->head] = stereo[i * 2];
        buffer->head = (buffer->head + 1) % AUDIO_PCM_CAPACITY;
        if (buffer->count < AUDIO_PCM_CAPACITY) ++buffer->count;
        else ++buffer->dropped;
    }
    buffer->newest_ms = now;
}
size_t Audio_PcmBufferReadLatest(Audio_PcmBuffer *buffer, uint8_t *out, size_t max_samples,
                                uint32_t now, uint32_t max_age_ms, uint32_t *oldest_age_ms)
{
    if (oldest_age_ms) *oldest_age_ms = UINT32_MAX;
    if (!out || !max_samples || !pcm_current(buffer, now) || !buffer->count) return 0;
    size_t count = buffer->count < max_samples ? buffer->count : max_samples;
    uint32_t age = now - buffer->newest_ms;
    if (age > max_age_ms || (count - 1) * 1000 / AUDIO_PCM_RATE > max_age_ms - age) {
        Audio_PcmBufferDiscard(buffer);
        return 0;
    }
    age += (uint32_t)((count - 1) * 1000 / AUDIO_PCM_RATE);
    buffer->dropped += buffer->count - (uint32_t)count;
    uint32_t index = (buffer->head + AUDIO_PCM_CAPACITY - (uint32_t)count) % AUDIO_PCM_CAPACITY;
    for (size_t i = 0; i < count; ++i) {
        uint16_t sample = (uint16_t)buffer->samples[index];
        out[i * 2] = (uint8_t)sample;
        out[i * 2 + 1] = (uint8_t)(sample >> 8);
        index = (index + 1) % AUDIO_PCM_CAPACITY;
    }
    buffer->count = 0;
    if (oldest_age_ms) *oldest_age_ms = age;
    return count * 2;
}

#ifndef AUDIO_PCM_HOST_TEST
#ifdef AUDIO_DRIVER_HOST_TEST
#include "audio_driver_stubs.h"
#else
#include "Bsp_BoardBus.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include "AudioToneControl.h"
#include "PeripheralMath.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define AUDIO_RATE 16000U
#define CODEC_I2C_HZ 50000U
#define CODEC_FALLBACK_I2C_HZ 10000U
#define BLOCK_FRAMES 256U
#define AUDIO_INIT_RETRY_MS 5000U
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static Audio_Status status = {
    .state = "OFF", .sample_rate = AUDIO_RATE, .init_stage = "OFF",
    .tone_state = "IDLE",
    .probe_before_mclk = ESP_ERR_INVALID_STATE, .probe_after_mclk = ESP_ERR_INVALID_STATE,
    .probe_alternate = ESP_ERR_INVALID_STATE, .failed_register = 0xff,
    .last_failure_register = 0xff,
};
static i2c_master_dev_handle_t codec;
static uint32_t codec_device_hz;
static uint32_t codec_target_hz = CODEC_I2C_HZ;
static i2s_chan_handle_t rx, tx;
static bool rx_enabled, tx_enabled;
static AudioToneControl tone_control;
static bool audio_task_alive;
static bool started;
static bool diagnostic_clock;
static bool diagnostic_timer;
/* Values are unchanged from the board's ADC configuration below. Masks cover
 * the configuration fields defined in Everest ES8388 Rev10 section 6. */
static const uint8_t adc_checks[AUDIO_ADC_REGISTER_COUNT][3] = {
    {0x03, 0x58, 0xff}, {0x09, 0x60, 0xff}, {0x0b, 0x02, 0x9c},
    {0x0d, 0x02, 0x3f}, {0x0e, 0x30, 0xf0}, {0x0f, 0x20, 0xec},
    {0x10, 0x00, 0xff}, {0x11, 0xc0, 0xff}, {0x12, 0x38, 0xff},
};
static portMUX_TYPE pcm_lock = portMUX_INITIALIZER_UNLOCKED;
static Audio_PcmBuffer pcm_buffer;
void Audio_MonitorArm(uint32_t expires_ms, bool reset_buffer)
{
    portENTER_CRITICAL(&pcm_lock);
    Audio_PcmBufferArm(&pcm_buffer, expires_ms, reset_buffer);
    portEXIT_CRITICAL(&pcm_lock);
}
void Audio_MonitorStop(void)
{
    portENTER_CRITICAL(&pcm_lock); Audio_PcmBufferStop(&pcm_buffer); portEXIT_CRITICAL(&pcm_lock);
}
void Audio_MonitorDiscard(void)
{
    portENTER_CRITICAL(&pcm_lock); Audio_PcmBufferDiscard(&pcm_buffer); portEXIT_CRITICAL(&pcm_lock);
}
size_t Audio_MonitorReadLatest(uint8_t *out, size_t max_samples, uint32_t max_age_ms, uint32_t *oldest_age_ms)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&pcm_lock);
    size_t bytes = Audio_PcmBufferReadLatest(&pcm_buffer, out, max_samples, now, max_age_ms, oldest_age_ms);
    portEXIT_CRITICAL(&pcm_lock);
    return bytes;
}
void Audio_MonitorGetBufferStatus(Audio_PcmBufferStatus *out)
{
    if (!out) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&pcm_lock);
    out->enabled = pcm_current(&pcm_buffer, now);
    out->buffered_samples = pcm_buffer.count;
    out->dropped_samples = pcm_buffer.dropped;
    out->newest_age_ms = pcm_buffer.count ? now - pcm_buffer.newest_ms : UINT32_MAX;
    portEXIT_CRITICAL(&pcm_lock);
}
static void stage(const char *name, esp_err_t error)
{
    portENTER_CRITICAL(&lock);
    snprintf(status.state, sizeof(status.state), "%s", name); status.error = error;
    portEXIT_CRITICAL(&lock);
}
static void init_stage(const char *name)
{
    portENTER_CRITICAL(&lock);
    snprintf(status.init_stage, sizeof(status.init_stage), "%s", name);
    status.last_init_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&lock);
}
void Audio_GetStatus(Audio_Status *out)
{
    if (!out) return;
    portENTER_CRITICAL(&lock); *out = status; portEXIT_CRITICAL(&lock);
}
static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t packet[] = {reg, value};
    esp_err_t error = BoardBus_Write(codec, packet, sizeof(packet));
    if (error != ESP_OK) {
        portENTER_CRITICAL(&lock); status.failed_register = reg; portEXIT_CRITICAL(&lock);
    }
    return error;
}
static esp_err_t codec_check_access(void)
{
    portENTER_CRITICAL(&lock); status.failed_register = 0xff; portEXIT_CRITICAL(&lock);
    /* Only this audio task owns this handle. A single 50 -> 10 kHz fallback
     * never alters the shared bus or another client's per-device timing. */
    if (codec && codec_device_hz != codec_target_hz) {
        init_stage("DEVICE_REMOVE");
        esp_err_t remove_error = BoardBus_RemoveDevice(codec);
        if (remove_error != ESP_OK) return remove_error;
        codec = NULL; codec_device_hz = 0;
        portENTER_CRITICAL(&lock); status.codec_i2c_hz = 0; portEXIT_CRITICAL(&lock);
    }
    init_stage("DEVICE_HANDLE");
    esp_err_t error = ESP_OK;
    if (!codec) {
        error = BoardBus_AddDeviceAtSpeed(0x10, codec_target_hz, &codec);
        if (error == ESP_OK) {
            codec_device_hz = codec_target_hz;
            portENTER_CRITICAL(&lock); status.codec_i2c_hz = codec_device_hz; portEXIT_CRITICAL(&lock);
        }
    }
    if (error != ESP_OK) return error;
    /* Isolated board tests read both registers reliably at 10/50 kHz, while
     * 100 kHz failed. IDF's address-only probe always uses 100 kHz; it cannot
     * gate this device. No configuration values are written by this check. */
    init_stage(codec_device_hz == CODEC_FALLBACK_I2C_HZ ? "READ_10KHZ" : "READ_50KHZ");
    const uint8_t registers[] = {0x00, 0x08};
    for (unsigned i = 0; i < sizeof(registers); ++i) {
        uint8_t value = 0;
        error = BoardBus_Read(codec, &registers[i], 1, &value, 1);
        if (error != ESP_OK) {
            portENTER_CRITICAL(&lock); status.failed_register = registers[i]; portEXIT_CRITICAL(&lock);
            return error;
        }
    }
    return ESP_OK;
}
static esp_err_t codec_init(void)
{
    portENTER_CRITICAL(&lock);
    status.failed_register = 0xff;
    for (unsigned i = 0; i < AUDIO_ADC_REGISTER_COUNT; ++i) {
        status.adc_registers[i] = (Audio_RegisterReadback) {
            .address = adc_checks[i][0], .expected = adc_checks[i][1], .mask = adc_checks[i][2],
            .read_error = ESP_ERR_INVALID_STATE,
        };
    }
    portEXIT_CRITICAL(&lock);
    init_stage("AMP_SHUTDOWN");
    esp_err_t error = BoardBus_XlConfigureOutput(BOARD_XL_SPEAKER_SD, BOARD_XL_SPEAKER_SD);
    if (error != ESP_OK) return error;
    error = codec_check_access();
    if (error != ESP_OK) return error;
    init_stage("CODEC_RESET");
    if ((error = write_reg(0x00, 0x80)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(10));
    /* Everest ES8388 register map: ESP is I2S master, codec slave, 16-bit
     * Philips, 16 kHz, MCLK=256fs. LIN1 is the only populated input. Only
     * LOUT2 is wired; analog bypass is disabled (no microphone feedback).
     */
    static const uint8_t config[][2] = {
        /* Release SCPReset before programming other registers. Writes made
         * while CONTROL1 bit 7 is set can be lost when reset is released. */
        {0x00, 0x16},
        {0x19, 0x26}, /* DAC mute + soft ramp. */
        {0x01, 0x50}, {0x02, 0x00},
        {0x03, 0x58}, /* Right ADC/input and unused MIC bias off. */
        {0x04, 0x48}, /* Left DAC and LOUT2 only. */
        {0x05, 0x00}, {0x06, 0x00}, {0x08, 0x00},
        {0x09, 0x60}, /* LIN1 +18 dB fixed PGA, right PGA 0 dB. */
        {0x0a, 0x00}, {0x0b, 0x02}, {0x0c, 0x0c}, {0x0d, 0x02},
        {0x0e, 0x30}, {0x0f, 0x20}, {0x10, 0x00}, {0x11, 0xc0},
        {0x12, 0x38}, /* ALC off: reported signal level has fixed gain. */
        {0x17, 0x18}, {0x18, 0x02}, {0x1a, 0x18}, {0x1b, 0xc0},
        {0x26, 0x00}, {0x27, 0xb8}, {0x2a, 0x38}, {0x2b, 0x80},
        {0x2e, 0x00}, {0x2f, 0x00}, {0x30, 0x18}, {0x31, 0x00},
    };
    init_stage("REGISTER_CONFIG");
    for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); ++i) {
        if ((error = write_reg(config[i][0], config[i][1])) != ESP_OK) return error;
    }
    const uint8_t registers[] = {0x08, 0x0a, 0x0c, 0x17, 0x19, 0x30};
    const uint8_t expected[]  = {0x00, 0x00, 0x0c, 0x18, 0x26, 0x18};
    /* Everest Rev10 register definitions: ignore only undocumented bits.
     * DACCONTROL3 (0x19) bit 2 is DACMute and MUST still compare equal. */
    const uint8_t masks[]     = {0xff, 0xfc, 0xff, 0xfe, 0xec, 0x3f};
    init_stage("REGISTER_VERIFY");
    for (unsigned i = 0; i < sizeof(registers); ++i) {
        uint8_t value = 0;
        error = BoardBus_Read(codec, &registers[i], 1, &value, 1);
        if (error == ESP_OK) {
            portENTER_CRITICAL(&lock);
            status.verify_register = registers[i]; status.verify_value = value;
            status.verify_expected = expected[i]; status.verify_mask = masks[i];
            status.verify_valid = true;
            if (registers[i] == 0x19) {
                status.dac_control3_readback = value;
                status.dac_control3_readback_valid = true;
            }
            portEXIT_CRITICAL(&lock);
            if ((value & masks[i]) != (expected[i] & masks[i])) error = ESP_ERR_INVALID_RESPONSE;
        }
        if (error != ESP_OK) {
            portENTER_CRITICAL(&lock); status.failed_register = registers[i]; portEXIT_CRITICAL(&lock);
            return error;
        }
    }
    /* Read the whole ADC diagnostic group, preserving every raw value even if
     * one comparison fails. No ADC setting is changed by this diagnostic. */
    init_stage("ADC_REGISTER_VERIFY");
    esp_err_t first_error = ESP_OK;
    uint8_t first_register = 0xff;
    for (unsigned i = 0; i < AUDIO_ADC_REGISTER_COUNT; ++i) {
        Audio_RegisterReadback result = {
            .address = adc_checks[i][0], .expected = adc_checks[i][1], .mask = adc_checks[i][2],
        };
        result.read_error = BoardBus_Read(codec, &result.address, 1, &result.value, 1);
        result.valid = result.read_error == ESP_OK;
        result.matches = result.valid && (result.value & result.mask) == (result.expected & result.mask);
        esp_err_t check_error = result.valid ? (result.matches ? ESP_OK : ESP_ERR_INVALID_RESPONSE) : result.read_error;
        portENTER_CRITICAL(&lock);
        status.adc_registers[i] = result;
        if (result.valid) {
            status.verify_register = result.address; status.verify_value = result.value;
            status.verify_expected = result.expected; status.verify_mask = result.mask;
            status.verify_valid = true;
        }
        portEXIT_CRITICAL(&lock);
        if (first_error == ESP_OK && check_error != ESP_OK) {
            first_error = check_error; first_register = result.address;
        }
    }
    if (first_error != ESP_OK) {
        portENTER_CRITICAL(&lock); status.failed_register = first_register; portEXIT_CRITICAL(&lock);
        return first_error;
    }
    return ESP_OK;
}
static esp_err_t mclk_test_start(void)
{
    /* Supply only MCLK. Never drive BCLK/LRCK until register 8 has selected
     * codec slave mode: its reset default is master mode.
     * Camera uses external XCLK, so LEDC timer3/channel7 are free on V0.24.
     */
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_3,
        .duty_resolution = LEDC_TIMER_1_BIT, .freq_hz = AUDIO_RATE * 256,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t error = ledc_timer_config(&timer);
    if (error != ESP_OK) return error;
    diagnostic_timer = true;
    const ledc_channel_config_t channel = {
        .gpio_num = 6, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_7,
        .intr_type = LEDC_INTR_DISABLE, .timer_sel = LEDC_TIMER_3, .duty = 1, .hpoint = 0,
    };
    error = ledc_channel_config(&channel);
    if (error == ESP_OK) {
        diagnostic_clock = true;
        uint32_t hz = ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3);
        portENTER_CRITICAL(&lock);
        status.mclk_test_performed = true; status.mclk_test_hz = hz;
        portEXIT_CRITICAL(&lock);
    }
    return error;
}
static esp_err_t mclk_test_stop(void)
{
    esp_err_t error = ESP_OK;
    if (diagnostic_clock) {
        error = ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
        if (error == ESP_OK) diagnostic_clock = false;
    }
    if (diagnostic_timer) {
        esp_err_t pause_error = ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3);
        if (pause_error == ESP_OK) diagnostic_timer = false;
        if (error == ESP_OK) error = pause_error;
    }
    return error;
}
static esp_err_t i2s_cleanup(void)
{
    /* Retain failed handles/flags so the next recovery iteration can retry
     * cleanup. Never allocate another channel over an unresolved resource. */
    esp_err_t first_error = ESP_OK;
    i2s_chan_handle_t *handles[] = {&rx, &tx};
    bool *enabled[] = {&rx_enabled, &tx_enabled};
    for (unsigned i = 0; i < 2; ++i) {
        if (!*handles[i]) continue;
        esp_err_t error = ESP_OK;
        if (*enabled[i]) {
            error = i2s_channel_disable(*handles[i]);
            if (error == ESP_OK) *enabled[i] = false;
        }
        if (error == ESP_OK) {
            error = i2s_del_channel(*handles[i]);
            if (error == ESP_OK) *handles[i] = NULL;
        }
        if (first_error == ESP_OK) first_error = error;
    }
    return first_error;
}
static esp_err_t i2s_init(void)
{
    if (rx || tx) return ESP_ERR_INVALID_STATE;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6; channel.dma_frame_num = BLOCK_FRAMES;
    channel.auto_clear = true;
    esp_err_t error = i2s_new_channel(&channel, &tx, &rx);
    if (error != ESP_OK) return error;
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = 6, .bclk = 7, .ws = 15, .dout = 16, .din = 17 },
    };
    if ((error = i2s_channel_init_std_mode(tx, &config)) != ESP_OK) return error;
    if ((error = i2s_channel_init_std_mode(rx, &config)) != ESP_OK) return error;
    if ((error = i2s_channel_enable(tx)) != ESP_OK) return error;
    tx_enabled = true;
    error = i2s_channel_enable(rx);
    if (error == ESP_OK) rx_enabled = true;
    return error;
}
static esp_err_t codec_set_mute(bool mute)
{
    if (!codec) return ESP_ERR_INVALID_STATE;
    const uint8_t reg = 0x19, expected = mute ? 0x26 : 0x22;
    esp_err_t error = write_reg(reg, expected);
    uint8_t value = 0;
    if (error == ESP_OK) error = BoardBus_Read(codec, &reg, 1, &value, 1);
    portENTER_CRITICAL(&lock);
    status.dac_control3_readback_valid = error == ESP_OK;
    if (error == ESP_OK) status.dac_control3_readback = value;
    portEXIT_CRITICAL(&lock);
    if (error == ESP_OK && (value & 0xec) != (expected & 0xec))
        error = ESP_ERR_INVALID_RESPONSE;
    if (error != ESP_OK) {
        portENTER_CRITICAL(&lock); status.failed_register = reg; portEXIT_CRITICAL(&lock);
    }
    return error;
}
static esp_err_t speaker_off(void)
{
    /* XL update already performs register readback. Do not report shutdown
     * when an I2C failure leaves the amplifier's actual state unknown. */
    esp_err_t amp_error = BoardBus_XlOutput(BOARD_XL_SPEAKER_SD, BOARD_XL_SPEAKER_SD);
    esp_err_t mute_error = codec_set_mute(true);
    portENTER_CRITICAL(&lock);
    if (amp_error == ESP_OK) status.speaker_active = false;
    status.speaker_shutdown_confirmed = amp_error == ESP_OK && mute_error == ESP_OK;
    portEXIT_CRITICAL(&lock);
    return amp_error != ESP_OK ? amp_error : mute_error;
}
static void tone_state(uint32_t generation, const char *state_name)
{
    portENTER_CRITICAL(&lock);
    if (AudioTone_Current(&tone_control, generation))
        snprintf(status.tone_state, sizeof(status.tone_state), "%s", state_name);
    portEXIT_CRITICAL(&lock);
}
static void tone_finish(uint32_t generation, const char *state_name, esp_err_t error)
{
    portENTER_CRITICAL(&lock);
    if (AudioTone_Complete(&tone_control, generation)) {
        snprintf(status.tone_state, sizeof(status.tone_state), "%s", state_name);
        status.tone_error = error;
        if (error != ESP_OK) ++status.tone_errors;
        else if (!strcmp(state_name, "COMPLETED")) ++status.tone_completed;
        else if (!strcmp(state_name, "STOPPED")) ++status.tone_stopped;
    }
    portEXIT_CRITICAL(&lock);
}
static esp_err_t audio_initialize_once(void)
{
    stage("INITIALIZING", ESP_OK);
    portENTER_CRITICAL(&lock);
    bool first_attempt = status.init_attempts == 0;
    ++status.init_attempts;
    status.configured = status.valid = status.retry_pending = false;
    status.next_retry_ms = 0;
    status.failed_register = 0xff;
    status.verify_valid = status.dac_control3_readback_valid = false;
    portEXIT_CRITICAL(&lock);
    /* A previous partial setup must be gone before touching the codec again. */
    init_stage("I2S_CLEANUP");
    esp_err_t error = i2s_cleanup();
    if (error != ESP_OK) return error;
    init_stage("MCLK_CLEANUP");
    error = mclk_test_stop();
    if (error != ESP_OK) return error;
    init_stage("AMP_SHUTDOWN");
    error = BoardBus_XlConfigureOutput(BOARD_XL_SPEAKER_SD, BOARD_XL_SPEAKER_SD);
    if (error != ESP_OK) return error;
    if (first_attempt) {
        /* A short quiet supply-settle interval also separates this probe from
         * reset release. The alternate CE address is probed only, never written.
         */
        vTaskDelay(pdMS_TO_TICKS(200));
        init_stage("BASELINE_PROBE");
        esp_err_t before = BoardBus_Probe(0x10);
        esp_err_t alternate = before == ESP_OK ? ESP_ERR_INVALID_STATE : BoardBus_Probe(0x11);
        portENTER_CRITICAL(&lock);
        status.probe_before_mclk = before; status.probe_alternate = alternate;
        portEXIT_CRITICAL(&lock);
    }
    /* Missing/unready codec: only bounded low-speed reads and an optional
     * MCLK-only test. Never reset/program it before both access reads succeed.
     * Retry MCLK as well, so a device powered later can recover without reboot. */
    error = codec_check_access();
    if (error != ESP_OK && codec_target_hz == CODEC_I2C_HZ && status.failed_register != 0xff) {
        /* At most one direction change per boot. Subsequent attempts keep the
         * lower rate, including after missing-device or handle-cleanup errors. */
        codec_target_hz = CODEC_FALLBACK_I2C_HZ;
        error = codec_check_access();
    }
    if (error != ESP_OK) {
        if (status.failed_register == 0xff) return error;
        init_stage("MCLK_TEST");
        portENTER_CRITICAL(&lock); status.failed_register = 0xff; portEXIT_CRITICAL(&lock);
        error = mclk_test_start();
        if (error != ESP_OK) return error;
        vTaskDelay(pdMS_TO_TICKS(100));
        if (first_attempt) {
            esp_err_t probe = BoardBus_Probe(0x10);
            portENTER_CRITICAL(&lock); status.probe_after_mclk = probe; portEXIT_CRITICAL(&lock);
        }
        error = codec_check_access();
        if (error != ESP_OK) return error;
    }
    error = codec_init();
    if (error != ESP_OK) return error;
    init_stage("MCLK_CLEANUP");
    error = mclk_test_stop();
    if (error != ESP_OK) return error;
    init_stage("I2S_START");
    error = i2s_init();
    if (error != ESP_OK) return error;
    init_stage("FINAL_SHUTDOWN");
    return speaker_off();
}
static void audio_initialize_until_ready(void)
{
    for (;;) {
        while(Camera_Calibrating())vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t error = audio_initialize_once();
        if (error == ESP_OK) break;
        /* Cleanup writes (notably DAC mute register 0x19) must never replace
         * the original initialization stage/register with a secondary error. */
        char failed_stage[sizeof(status.init_stage)];
        portENTER_CRITICAL(&lock);
        memcpy(failed_stage, status.init_stage, sizeof(failed_stage));
        uint8_t failed_register = status.failed_register;
        memcpy(status.last_failure_stage, failed_stage, sizeof(failed_stage));
        status.last_failure_register = failed_register;
        status.last_failure_error = error;
        status.last_failure_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ++status.init_failures;
        snprintf(status.tone_state, sizeof(status.tone_state), "ERROR");
        status.tone_error = error;
        portEXIT_CRITICAL(&lock);
        Audio_MonitorDiscard();
        esp_err_t shutdown_error = speaker_off();
        esp_err_t cleanup_error = i2s_cleanup();
        esp_err_t clock_error = mclk_test_stop();
        if (cleanup_error == ESP_OK) cleanup_error = clock_error;
        portENTER_CRITICAL(&lock);
        memcpy(status.init_stage, failed_stage, sizeof(failed_stage));
        status.failed_register = failed_register;
        status.shutdown_error = shutdown_error;
        status.cleanup_error = cleanup_error;
        status.retry_pending = true;
        status.next_retry_ms = (uint32_t)(esp_timer_get_time() / 1000) + AUDIO_INIT_RETRY_MS;
        portEXIT_CRITICAL(&lock);
        stage("INIT_ERROR", error);
        /* No shared bus lock is held while waiting. No sound command is
         * accepted until a complete initialization and shutdown readback. */
        vTaskDelay(pdMS_TO_TICKS(AUDIO_INIT_RETRY_MS));
    }
    init_stage(codec_device_hz == CODEC_FALLBACK_I2C_HZ ? "COMPLETE_10KHZ" : "COMPLETE_50KHZ");
    vTaskDelay(pdMS_TO_TICKS(100));
    portENTER_CRITICAL(&lock);
    status.configured = true;
    status.failed_register = 0xff;
    status.shutdown_error = status.cleanup_error = ESP_OK;
    snprintf(status.tone_state, sizeof(status.tone_state), "IDLE");
    status.tone_error = ESP_OK;
    portEXIT_CRITICAL(&lock);
    stage("WAITING_SAMPLES", ESP_OK);
}
static void audio_task(void *unused)
{
    (void)unused;
    audio_initialize_until_ready();
    esp_err_t error = ESP_OK;
    int16_t input[BLOCK_FRAMES * 2], output[BLOCK_FRAMES * 2];
    PcmMetrics metrics = {0}, right_metrics = {0};
    AudioToneRequest tone = {0}; unsigned remaining = 0, played = 0;
    int64_t drain_until = 0, quiet_until = 0, tone_deadline = 0;
    while (true) {
        uint32_t stop_generation = 0;
        portENTER_CRITICAL(&lock);
        bool stop = AudioTone_TakeStop(&tone_control, &stop_generation);
        portEXIT_CRITICAL(&lock);
        if (stop) {
            remaining = 0; drain_until = 0; tone_deadline = 0;
            error = speaker_off();
            /* Keep sending zeros with the amplifier shut down until every
             * old DMA block has left the six-descriptor TX queue. */
            quiet_until = esp_timer_get_time() + 110000;
            tone_finish(stop_generation, error == ESP_OK ? "STOPPED" : "SHUTDOWN_ERROR", error);
        }
        if (tone_deadline && esp_timer_get_time() >= tone_deadline) {
            remaining = 0; drain_until = 0; tone_deadline = 0;
            error = speaker_off();
            quiet_until = esp_timer_get_time() + 110000;
            tone_finish(tone.generation, error == ESP_OK ? "ERROR" : "SHUTDOWN_ERROR",
                        error == ESP_OK ? ESP_ERR_TIMEOUT : error);
        }
        if (drain_until && esp_timer_get_time() >= drain_until) {
            error = speaker_off(); drain_until = 0; tone_deadline = 0;
            tone_finish(tone.generation, error == ESP_OK ? "COMPLETED" : "SHUTDOWN_ERROR", error);
        }
        bool take_tone = false;
        if (!remaining && !drain_until && esp_timer_get_time() >= quiet_until) {
            portENTER_CRITICAL(&lock);
            take_tone = AudioTone_Take(&tone_control, &tone);
            if (take_tone) snprintf(status.tone_state, sizeof(status.tone_state), "STARTING");
            portEXIT_CRITICAL(&lock);
        }
        if (take_tone) {
            portENTER_CRITICAL(&lock); status.speaker_shutdown_confirmed = false; portEXIT_CRITICAL(&lock);
            error = codec_set_mute(false);
            if (error == ESP_OK) {
                /* An unacknowledged write may still have enabled the amp.
                 * Preserve this conservative flag until shutdown is read back. */
                portENTER_CRITICAL(&lock); status.speaker_active = true; portEXIT_CRITICAL(&lock);
                error = BoardBus_XlOutput(BOARD_XL_SPEAKER_SD, 0);
            }
            portENTER_CRITICAL(&lock);
            bool current = AudioTone_Current(&tone_control, tone.generation);
            if (error == ESP_OK && current) {
                ++status.tone_started;
                snprintf(status.tone_state, sizeof(status.tone_state), "PLAYING");
            }
            portEXIT_CRITICAL(&lock);
            if (!current) {
                /* STOP arrived while I2C was in flight. Never emit this tone. */
                (void)speaker_off();
                quiet_until = esp_timer_get_time() + 110000;
            } else if (error == ESP_OK) {
                remaining = tone.duration_ms * AUDIO_RATE / 1000; played = 0;
                tone_deadline = esp_timer_get_time() + (tone.duration_ms + 250) * 1000;
            } else {
                esp_err_t shutdown = speaker_off();
                quiet_until = esp_timer_get_time() + 110000;
                tone_finish(tone.generation, shutdown == ESP_OK ? "ERROR" : "SHUTDOWN_ERROR",
                            shutdown == ESP_OK ? error : shutdown);
            }
        }
        size_t received = 0;
        error = i2s_channel_read(rx, input, sizeof(input), &received, 100);
        if (error == ESP_OK && received >= 4 && received % 4 == 0) {
            portENTER_CRITICAL(&pcm_lock);
            Audio_PcmBufferPushLeft(&pcm_buffer, input, received / 4,
                (uint32_t)(esp_timer_get_time() / 1000));
            portEXIT_CRITICAL(&pcm_lock);
            PcmMetrics_AddStereoLeft(&metrics, input, received / 4);
            /* Same interleaved frames/window; offset one sample selects right. */
            PcmMetrics_AddStereoLeft(&right_metrics, input + 1, received / 4);
            if (metrics.samples >= 1600) {
                float rms = (float)PcmMetrics_Rms(&metrics);
                float ac = (float)PcmMetrics_AcRms(&metrics);
                float right_rms = (float)PcmMetrics_Rms(&right_metrics);
                float right_ac = (float)PcmMetrics_AcRms(&right_metrics);
                portENTER_CRITICAL(&lock);
                status.valid = true; status.rms = rms; status.ac_rms = ac;
                status.dc = (float)metrics.sum / metrics.samples;
                status.right_rms = right_rms; status.right_ac_rms = right_ac;
                status.right_dc = (float)right_metrics.sum / right_metrics.samples;
                status.right_peak = right_metrics.peak;
                status.peak = metrics.peak; status.clipped += metrics.clipped;
                status.samples += metrics.samples; ++status.blocks;
                status.last_sample_ms = (uint32_t)(esp_timer_get_time() / 1000);
                portEXIT_CRITICAL(&lock);
                memset(&metrics, 0, sizeof(metrics)); memset(&right_metrics, 0, sizeof(right_metrics));
                stage("SAMPLING", ESP_OK);
            }
        } else {
            Audio_MonitorDiscard();
            portENTER_CRITICAL(&lock); ++status.read_errors; status.valid = false; portEXIT_CRITICAL(&lock);
            memset(&metrics, 0, sizeof(metrics));
            memset(&right_metrics, 0, sizeof(right_metrics));
            stage("READ_ERROR", error == ESP_OK ? ESP_ERR_INVALID_SIZE : error);
        }
        memset(output, 0, sizeof(output));
        bool was_playing = remaining != 0;
        for (unsigned i = 0; i < BLOCK_FRAMES && remaining; ++i) {
            /* 5 ms rise/fall envelope avoids a full-scale step. */
            unsigned ramp = played < 80 ? played : 80;
            if (remaining < ramp) ramp = remaining;
            float phase = (float)(played % AUDIO_RATE) * (6.28318530718f * tone.hz / AUDIO_RATE);
            output[i * 2] = (int16_t)(sinf(phase) * (32767.0f * tone.volume / 100) * ramp / 80);
            ++played; --remaining;
        }
        size_t written = 0;
        esp_err_t tx_error = i2s_channel_write(tx, output, sizeof(output), &written, 100);
        if (tx_error != ESP_OK || written != sizeof(output)) {
            if (was_playing || drain_until) {
                esp_err_t shutdown = speaker_off();
                quiet_until = esp_timer_get_time() + 110000;
                tone_finish(tone.generation, shutdown == ESP_OK ? "ERROR" : "SHUTDOWN_ERROR",
                    shutdown != ESP_OK ? shutdown : (tx_error != ESP_OK ? tx_error : ESP_ERR_INVALID_SIZE));
            }
            remaining = 0; drain_until = 0;
            tone_deadline = 0;
        } else if (was_playing && !remaining) {
            /* Keep receiving microphone data while the queued DAC DMA drains. */
            drain_until = esp_timer_get_time() + 110000;
            tone_state(tone.generation, "DRAINING");
        }
    }
}
esp_err_t Audio_Start(void)
{
    portENTER_CRITICAL(&lock);
    if (started) { portEXIT_CRITICAL(&lock); return ESP_OK; }
    started = true;
    audio_task_alive = true;
    portEXIT_CRITICAL(&lock);
    if (xTaskCreate(audio_task, "periph_audio", 6144, NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&lock);
        audio_task_alive = false;
        started = false;
        portEXIT_CRITICAL(&lock);
        stage("TASK_ERROR", ESP_ERR_NO_MEM); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t Audio_PlayTone(unsigned hz, unsigned duration_ms, unsigned volume_percent)
{
    return Audio_PlayToneWithId(hz, duration_ms, volume_percent, NULL);
}
esp_err_t Audio_PlayToneWithId(unsigned hz, unsigned duration_ms, unsigned volume_percent,
                             const char *request_id)
{
    if (!AudioTone_ValidArgs(hz, duration_ms, volume_percent) || !AudioTone_ValidId(request_id))
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    bool accepted = audio_task_alive && status.configured && status.speaker_shutdown_confirmed &&
        AudioTone_Request(&tone_control, hz, duration_ms, volume_percent, request_id);
    if (accepted) {
        snprintf(status.tone_request_id, sizeof(status.tone_request_id), "%s", request_id ? request_id : "");
        snprintf(status.tone_state, sizeof(status.tone_state), "QUEUED");
        status.tone_error = ESP_OK; ++status.tone_requested;
        status.tone_hz = hz; status.tone_duration_ms = duration_ms; status.tone_volume_percent = volume_percent;
    }
    portEXIT_CRITICAL(&lock);
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t Audio_StopTone(void)
{
    return Audio_StopToneWithId(NULL);
}
esp_err_t Audio_StopToneWithId(const char *request_id)
{
    if (!AudioTone_ValidId(request_id)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    bool accepted = audio_task_alive && status.configured && AudioTone_Stop(&tone_control, request_id);
    if (accepted) {
        snprintf(status.tone_request_id, sizeof(status.tone_request_id), "%s", request_id ? request_id : "");
        snprintf(status.tone_state, sizeof(status.tone_state), "STOPPING");
        status.tone_error = ESP_OK;
    }
    portEXIT_CRITICAL(&lock);
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}
#endif /* AUDIO_PCM_HOST_TEST */
