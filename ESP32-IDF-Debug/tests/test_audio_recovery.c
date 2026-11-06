/* Exercise the real driver initialization/retry/cleanup code with SDK faults.
 * No production policy is reimplemented in this test. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#define AUDIO_DRIVER_HOST_TEST 1
#include "../Bsp/Bsp_Audio.c"
bool Camera_Calibrating(void) { return false; }

static uint64_t fake_us;
static uint8_t registers[256];
static unsigned unavailable_attempts, retry_waits, stop_after_waits;
static unsigned reset_writes, mute_failures, amp_enables, created_tasks;
static unsigned alloc_calls, deleted_channels, disabled_channels, live_channels;
static unsigned fail_i2s_stage, disable_failures, fail_task_create;
static unsigned fail_mclk_channel, timer_pauses;
static bool reject_50khz, codec_live;
static unsigned mock_codec_hz, codec_adds, codec_removes, codec_remove_failures, codec_add_failures;
static struct FakeI2sChannel channels[2];
static jmp_buf exit_wait;

static void reset_fixture(void)
{
    assert(!live_channels);
    fake_us = 0;
    memset(registers, 0, sizeof(registers));
    memset(channels, 0, sizeof(channels));
    unavailable_attempts = retry_waits = stop_after_waits = 0;
    reset_writes = mute_failures = amp_enables = created_tasks = 0;
    alloc_calls = deleted_channels = disabled_channels = 0;
    fail_i2s_stage = disable_failures = fail_task_create = 0;
    fail_mclk_channel = timer_pauses = 0;
    reject_50khz = codec_live = false;
    mock_codec_hz = codec_adds = codec_removes = codec_remove_failures = codec_add_failures = 0;
    codec = NULL; rx = tx = NULL;
    codec_device_hz = 0; codec_target_hz = CODEC_I2C_HZ;
    rx_enabled = tx_enabled = diagnostic_clock = diagnostic_timer = false;
    started = audio_task_alive = false;
    memset(&tone_control, 0, sizeof(tone_control));
    memset(&pcm_buffer, 0, sizeof(pcm_buffer));
    memset(&status, 0, sizeof(status));
    status.failed_register = status.last_failure_register = 0xff;
    status.sample_rate = AUDIO_RATE;
}
static void prepare(void)
{
    assert(Audio_Start() == ESP_OK);
    audio_initialize_until_ready();
}
static void stop_channels(void)
{
    assert(i2s_cleanup() == ESP_OK);
    assert(!rx && !tx && !rx_enabled && !tx_enabled && !live_channels);
}
static void test_codec_returns_after_old_three_attempt_limit(void)
{
    reset_fixture();
    unavailable_attempts = 3;
    prepare();
    assert(status.init_attempts == 4 && status.init_failures == 3 && retry_waits == 3);
    assert(status.configured && audio_task_alive && !status.retry_pending);
    assert(!strcmp(status.init_stage, "COMPLETE_10KHZ"));
    assert(status.last_failure_error == ESP_ERR_INVALID_STATE);
    assert(!strcmp(status.last_failure_stage, "READ_10KHZ"));
    assert(status.last_failure_register == 0x00);
    assert(mute_failures == 3); /* Cleanup really failed at 0x19. */
    assert(reset_writes == 1 && amp_enables == 0);
    assert(status.failed_register == 0xff && status.speaker_shutdown_confirmed);
    assert(!diagnostic_clock && !diagnostic_timer && alloc_calls == 1);
    assert(codec_adds == 2 && codec_removes == 1 && status.codec_i2c_hz == 10000);
    assert(fake_us >= 15000000);
    stop_channels();
}
static void test_missing_codec_stays_quiet_and_does_not_reset(void)
{
    reset_fixture();
    unavailable_attempts = 100;
    stop_after_waits = 5;
    assert(Audio_Start() == ESP_OK);
    if (!setjmp(exit_wait)) audio_initialize_until_ready();
    assert(retry_waits == 5 && status.init_failures == 5 && !status.configured);
    assert(status.retry_pending && audio_task_alive && !status.valid);
    assert(reset_writes == 0 && alloc_calls == 0 && amp_enables == 0);
    assert(status.failed_register == 0 && status.last_failure_register == 0);
    assert(status.shutdown_error == ESP_ERR_INVALID_STATE);
    assert(status.cleanup_error == ESP_OK && !diagnostic_clock && !diagnostic_timer);
    assert(!strcmp(status.init_stage, "READ_10KHZ"));
    assert(!strcmp(status.state, "INIT_ERROR"));
    assert(codec_adds == 2 && codec_removes == 1 && status.codec_i2c_hz == 10000);
}
static void test_i2s_partial_setup_releases_before_retry(void)
{
    for (unsigned failure = 1; failure <= 3; ++failure) {
        reset_fixture();
        fail_i2s_stage = failure;
        prepare();
        assert(status.init_attempts == 2 && retry_waits == 1 && status.configured);
        assert(alloc_calls == 2 && deleted_channels == 2 && live_channels == 2);
        assert(!strcmp(status.last_failure_stage, "I2S_START"));
        assert(disabled_channels == (failure == 3 ? 1U : 0U));
        assert(amp_enables == 0);
        stop_channels();
    }
}
static void test_cleanup_failure_cannot_overwrite_live_handle(void)
{
    reset_fixture();
    fail_i2s_stage = 3; /* TX running, RX enable fails. */
    disable_failures = 3;
    prepare();
    assert(status.configured && status.init_attempts == 3 && retry_waits == 2);
    assert(alloc_calls == 2 && deleted_channels == 2 && live_channels == 2);
    assert(!strcmp(status.last_failure_stage, "I2S_CLEANUP"));
    assert(amp_enables == 0);
    stop_channels();
}
static void test_start_idempotence_and_allocation_failure(void)
{
    reset_fixture();
    fail_task_create = 1;
    assert(Audio_Start() == ESP_ERR_NO_MEM);
    assert(!started && !audio_task_alive);
    assert(Audio_Start() == ESP_OK);
    assert(Audio_Start() == ESP_OK);
    assert(created_tasks == 2 && started && audio_task_alive);
    audio_initialize_until_ready();
    assert(Audio_Start() == ESP_OK && created_tasks == 2);
    assert(amp_enables == 0);
    stop_channels();
}
static void test_mclk_partial_setup_stops_timer_before_retry(void)
{
    reset_fixture();
    unavailable_attempts = 1;
    fail_mclk_channel = 1;
    prepare();
    assert(status.configured && retry_waits == 1 && timer_pauses == 1);
    assert(!diagnostic_clock && !diagnostic_timer && reset_writes == 1);
    assert(!strcmp(status.last_failure_stage, "MCLK_TEST"));
    assert(status.last_failure_register == 0xff && status.last_failure_error == ESP_FAIL);
    assert(amp_enables == 0);
    stop_channels();
}
static void test_50khz_failure_falls_back_once_and_keeps_10khz(void)
{
    reset_fixture();
    reject_50khz = true;
    prepare();
    assert(status.configured && status.init_attempts == 1 && retry_waits == 0);
    assert(codec_adds == 2 && codec_removes == 1 && codec_live);
    assert(status.codec_i2c_hz == 10000 && mock_codec_hz == 10000);
    assert(!strcmp(status.init_stage, "COMPLETE_10KHZ"));
    assert(reset_writes == 1 && !status.mclk_test_performed && amp_enables == 0);
    /* Rechecking the configured codec must reuse its low-speed handle. */
    assert(codec_check_access() == ESP_OK && codec_adds == 2 && codec_removes == 1);
    stop_channels();
}
static void test_fallback_handle_errors_do_not_leak_or_reuse_removed_handle(void)
{
    for (unsigned failure = 0; failure < 2; ++failure) {
        reset_fixture();
        reject_50khz = true;
        codec_remove_failures = failure == 0 ? 1 : 0;
        codec_add_failures = failure == 1 ? 1 : 0;
        prepare();
        assert(status.configured && status.init_attempts == 2 && retry_waits == 1);
        assert(codec_live && mock_codec_hz == 10000 && status.codec_i2c_hz == 10000);
        assert(codec_adds == (failure == 0 ? 2U : 3U));
        assert(codec_removes == (failure == 0 ? 2U : 1U));
        assert(!strcmp(status.last_failure_stage, failure == 0 ? "DEVICE_REMOVE" : "DEVICE_HANDLE"));
        assert(status.last_failure_register == 0xff && reset_writes == 1);
        assert(amp_enables == 0);
        stop_channels();
    }
}

int64_t esp_timer_get_time(void) { return (int64_t)fake_us; }
void vTaskDelay(uint32_t ticks)
{
    if (ticks == AUDIO_INIT_RETRY_MS) {
        assert(status.retry_pending && !status.configured && audio_task_alive);
        assert(status.next_retry_ms == (uint32_t)(fake_us / 1000) + AUDIO_INIT_RETRY_MS);
        assert(Audio_PlayToneWithId(1000, 100, 5, "0123456789abcdef") == ESP_ERR_INVALID_STATE);
        assert(!diagnostic_clock && !diagnostic_timer);
        ++retry_waits;
    }
    fake_us += (uint64_t)ticks * 1000;
    if (stop_after_waits && retry_waits >= stop_after_waits) longjmp(exit_wait, 1);
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *argument, unsigned priority, void *handle)
{
    (void)task; (void)name; (void)stack; (void)argument; (void)priority; (void)handle;
    ++created_tasks;
    if (fail_task_create) { --fail_task_create; return 0; }
    return pdPASS;
}
esp_err_t BoardBus_AddDeviceAtSpeed(uint8_t address, uint32_t speed, i2c_master_dev_handle_t *device)
{
    assert(address == 0x10 && (speed == 50000 || speed == 10000) && !*device && !codec_live);
    ++codec_adds;
    if (speed == 10000 && codec_add_failures) { --codec_add_failures; return ESP_ERR_NO_MEM; }
    codec_live = true; mock_codec_hz = speed;
    *device = registers;
    return ESP_OK;
}
esp_err_t BoardBus_RemoveDevice(i2c_master_dev_handle_t device)
{
    assert(device == registers && codec_live);
    ++codec_removes;
    if (codec_remove_failures) { --codec_remove_failures; return ESP_FAIL; }
    codec_live = false; mock_codec_hz = 0;
    return ESP_OK;
}
esp_err_t BoardBus_Probe(uint8_t address) { (void)address; return ESP_ERR_NOT_FOUND; }
esp_err_t BoardBus_Read(i2c_master_dev_handle_t device, const uint8_t *reg, size_t reg_length,
                        uint8_t *data, size_t length)
{
    assert(device == registers && codec_live && reg_length == 1 && length == 1);
    if (status.init_attempts <= unavailable_attempts || (reject_50khz && mock_codec_hz == 50000))
        return ESP_ERR_INVALID_STATE;
    *data = registers[*reg];
    return ESP_OK;
}
esp_err_t BoardBus_Write(i2c_master_dev_handle_t device, const uint8_t *data, size_t length)
{
    assert(device == registers && codec_live && length == 2);
    if (status.init_attempts <= unavailable_attempts || (reject_50khz && mock_codec_hz == 50000)) {
        assert(data[0] == 0x19); /* No reset/config before successful access. */
        ++mute_failures;
        return ESP_ERR_INVALID_STATE;
    }
    if (data[0] == 0 && data[1] == 0x80) ++reset_writes;
    registers[data[0]] = data[1];
    return ESP_OK;
}
esp_err_t BoardBus_XlConfigureOutput(uint8_t mask, uint8_t value)
{
    assert(mask == BOARD_XL_SPEAKER_SD && value == BOARD_XL_SPEAKER_SD);
    return ESP_OK;
}
esp_err_t BoardBus_XlOutput(uint8_t mask, uint8_t value)
{
    assert(mask == BOARD_XL_SPEAKER_SD);
    if (!value) ++amp_enables;
    return ESP_OK;
}
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *out_tx, i2s_chan_handle_t *out_rx)
{
    (void)config;
    assert(live_channels == 0 && !*out_tx && !*out_rx);
    ++alloc_calls; live_channels = 2;
    channels[0] = (struct FakeI2sChannel){1, 0};
    channels[1] = (struct FakeI2sChannel){1, 1};
    *out_tx = &channels[0]; *out_rx = &channels[1];
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *config)
{
    assert(handle->state == 1 && config->gpio_cfg.mclk == 6 && config->gpio_cfg.din == 17);
    if (fail_i2s_stage == 1 && handle->kind == 1 && alloc_calls == 1) return ESP_FAIL;
    handle->state = 2;
    return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle)
{
    assert(handle->state == 2);
    if (alloc_calls == 1 && ((fail_i2s_stage == 2 && handle->kind == 0) ||
                            (fail_i2s_stage == 3 && handle->kind == 1))) return ESP_FAIL;
    handle->state = 3;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle)
{
    assert(handle->state == 3);
    if (disable_failures) { --disable_failures; return ESP_FAIL; }
    ++disabled_channels; handle->state = 2;
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t handle)
{
    assert(handle->state == 1 || handle->state == 2);
    ++deleted_channels; --live_channels; handle->state = 0;
    return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void *data, size_t length, size_t *read, unsigned timeout)
{
    (void)handle; (void)data; (void)length; (void)read; (void)timeout;
    assert(!"Sampling loop is outside this initialization test"); return ESP_FAIL;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *data, size_t length, size_t *written, unsigned timeout)
{
    (void)handle; (void)data; (void)length; (void)written; (void)timeout;
    assert(!"Playback loop is outside this initialization test"); return ESP_FAIL;
}
esp_err_t ledc_timer_config(const ledc_timer_config_t *config) { assert(config->freq_hz == 4096000); return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *config)
{
    assert(config->gpio_num == 6);
    if (fail_mclk_channel) { --fail_mclk_channel; return ESP_FAIL; }
    return ESP_OK;
}
uint32_t ledc_get_freq(int mode, int timer) { (void)mode; (void)timer; return 4096000; }
esp_err_t ledc_stop(int mode, int channel, int idle) { (void)mode; (void)channel; (void)idle; return ESP_OK; }
esp_err_t ledc_timer_pause(int mode, int timer) { (void)mode; (void)timer; ++timer_pauses; return ESP_OK; }

int main(void)
{
    test_codec_returns_after_old_three_attempt_limit();
    test_missing_codec_stays_quiet_and_does_not_reset();
    test_i2s_partial_setup_releases_before_retry();
    test_cleanup_failure_cannot_overwrite_live_handle();
    test_start_idempotence_and_allocation_failure();
    test_mclk_partial_setup_stops_timer_before_retry();
    test_50khz_failure_falls_back_once_and_keeps_10khz();
    test_fallback_handle_errors_do_not_leak_or_reuse_removed_handle();
    puts("Audio driver recovery: 8 groups passed (real initializer, fault-injected SDK/bus).");
    return 0;
}
