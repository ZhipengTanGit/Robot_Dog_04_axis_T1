#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define AUDIO_ADC_REGISTER_COUNT 9U
typedef struct {
    uint8_t address, value, expected, mask;
    esp_err_t read_error;
    bool valid, matches;
} Audio_RegisterReadback;
typedef struct {
    char state[24];
    esp_err_t error;
    bool configured, valid, speaker_active;
    uint32_t sample_rate, samples, blocks, read_errors, clipped, last_sample_ms;
    uint32_t tone_completed, tone_errors;
    char tone_request_id[17], tone_state[24];
    esp_err_t tone_error;
    uint32_t tone_requested, tone_started, tone_stopped;
    uint32_t tone_hz, tone_duration_ms, tone_volume_percent;
    /* True only after amplifier shutdown readback and codec mute readback.
     * speaker_active remains conservative if shutdown cannot be confirmed. */
    bool speaker_shutdown_confirmed;
    char init_stage[24];
    uint32_t init_attempts, last_init_ms, mclk_test_hz;
    uint32_t codec_i2c_hz;
    bool mclk_test_performed;
    /* Initialization retries do not accept tones. Failure evidence is captured
     * before shutdown/cleanup, which have separate error fields. */
    bool retry_pending;
    uint32_t next_retry_ms, init_failures, last_failure_ms;
    char last_failure_stage[24];
    uint8_t last_failure_register;
    esp_err_t last_failure_error, shutdown_error, cleanup_error;
    esp_err_t probe_before_mclk, probe_after_mclk, probe_alternate;
    uint8_t failed_register;
    /* Latest successful verification read, retained on a later bus error. */
    uint8_t verify_register, verify_value, verify_expected, verify_mask;
    bool verify_valid;
    uint8_t dac_control3_readback;
    bool dac_control3_readback_valid;
    Audio_RegisterReadback adc_registers[AUDIO_ADC_REGISTER_COUNT];
    float rms, ac_rms, dc;
    float right_rms, right_ac_rms, right_dc;
    uint32_t peak;
    uint32_t right_peak;
} Audio_Status;
esp_err_t Audio_Start(void);
void Audio_GetStatus(Audio_Status *out);
/* Explicit low-volume diagnostic playback only: 100..4000 Hz, 20..1000 ms,
 * 1..20 percent digital amplitude. Boot does not enqueue any sound. */
esp_err_t Audio_PlayTone(unsigned hz, unsigned duration_ms, unsigned volume_percent);
esp_err_t Audio_PlayToneWithId(unsigned hz, unsigned duration_ms, unsigned volume_percent,
                             const char *request_id);
/* Asynchronous: acceptance is not hardware shutdown confirmation. Check
 * tone_request_id/tone_state and speaker_shutdown_confirmed in telemetry. */
esp_err_t Audio_StopTone(void);
esp_err_t Audio_StopToneWithId(const char *request_id);

#define AUDIO_PCM_RATE 16000U
#define AUDIO_PCM_CAPACITY 16000U
#define AUDIO_PCM_PACKET_SAMPLES 3200U
/* Bounded mono ring. Pure operations require caller synchronization; the audio
 * task and monitor wrappers below serialize them with a short critical section.
 * No allocation, filesystem writes, networking, or speaker playback here. */
typedef struct {
    int16_t samples[AUDIO_PCM_CAPACITY];
    uint32_t head, count, newest_ms, expires_ms, dropped;
    bool enabled;
} Audio_PcmBuffer;
typedef struct {
    bool enabled;
    uint32_t buffered_samples, dropped_samples, newest_age_ms;
} Audio_PcmBufferStatus;
void Audio_PcmBufferArm(Audio_PcmBuffer *buffer, uint32_t expires_ms, bool reset);
void Audio_PcmBufferStop(Audio_PcmBuffer *buffer);
void Audio_PcmBufferDiscard(Audio_PcmBuffer *buffer);
void Audio_PcmBufferPushLeft(Audio_PcmBuffer *buffer, const int16_t *stereo, size_t frames, uint32_t now);
size_t Audio_PcmBufferReadLatest(Audio_PcmBuffer *buffer, uint8_t *out, size_t max_samples,
                                uint32_t now, uint32_t max_age_ms, uint32_t *oldest_age_ms);
/* Only the audio-monitor task/control owns these APIs. Arm expiry is an absolute
 * wrapping millisecond deadline. Expiry is also enforced by the PCM producer. */
void Audio_MonitorArm(uint32_t expires_ms, bool reset_buffer);
void Audio_MonitorStop(void);
void Audio_MonitorDiscard(void);
size_t Audio_MonitorReadLatest(uint8_t *out, size_t max_samples, uint32_t max_age_ms,
                               uint32_t *oldest_age_ms);
void Audio_MonitorGetBufferStatus(Audio_PcmBufferStatus *out);
