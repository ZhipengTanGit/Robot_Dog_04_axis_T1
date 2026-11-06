#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define AUDIO_MONITOR_MAX_TTL_MS 15000U
#define AUDIO_MONITOR_SERIAL_JSON_CAPACITY 8800U
typedef enum { AUDIO_MONITOR_OFF, AUDIO_MONITOR_HTTP, AUDIO_MONITOR_SERIAL, AUDIO_MONITOR_PULL } AudioMonitor_Transport;
typedef struct {
    bool enabled;
    AudioMonitor_Transport transport;
    char session_id[17];
    uint32_t expires_ms, generation, next_seq;
} AudioMonitor_Lease;
/* Pure lease operations; the runtime serializes access. Same-session refreshes
 * retain sequence. New sessions reset sequence and increment generation. */
esp_err_t AudioMonitor_LeaseSet(AudioMonitor_Lease *lease, const char *session_id, uint32_t ttl_ms, uint32_t now);
void AudioMonitor_LeaseStop(AudioMonitor_Lease *lease);
bool AudioMonitor_LeaseExpire(AudioMonitor_Lease *lease, uint32_t now);
uint32_t AudioMonitor_LeaseRemaining(const AudioMonitor_Lease *lease, uint32_t now);
/* Explicit serial/WiFi pull sessions can replace each other. Legacy HTTP
 * cannot replace or stop either current manual session. Stops must match. */
esp_err_t AudioMonitor_ControlSet(AudioMonitor_Lease *lease, AudioMonitor_Transport transport,
                                  const char *session_id, uint32_t ttl_ms, uint32_t now);
bool AudioMonitor_ControlStop(AudioMonitor_Lease *lease, AudioMonitor_Transport transport,
                              const char *session_id, uint32_t now);
bool AudioMonitor_CanTakePacket(const AudioMonitor_Lease *lease, AudioMonitor_Transport transport,
                                const char *session_id, uint32_t now, uint32_t last_attempt);
size_t AudioMonitor_FormatSerialJson(char *out, size_t capacity, const char *session_id,
                                     uint32_t sequence, const uint8_t *pcm, size_t bytes);

typedef struct {
    bool started, enabled, codec_ready;
    char state[24], session_id[17], transport[8];
    uint32_t lease_remaining_ms, next_seq, packets, samples, failed_packets;
    uint32_t buffered_samples, dropped_samples, newest_age_ms;
    uint32_t last_upload_ms, last_packet_age_ms;
    int http_status;
    esp_err_t error;
} AudioMonitor_Status;
esp_err_t AudioMonitor_Start(const char *device_id, const char *boot_id);
esp_err_t AudioMonitor_SetLease(const char *session_id, uint32_t ttl_ms);
void AudioMonitor_Stop(void);
esp_err_t AudioMonitor_SetSerialLease(const char *session_id, uint32_t ttl_ms);
void AudioMonitor_StopSerial(const char *session_id);
esp_err_t AudioMonitor_SetPullLease(const char *session_id, uint32_t ttl_ms);
void AudioMonitor_StopPull(const char *session_id);
/* Single UART0 writer only. Real PCM, rate-limited to one packet per 200 ms.
 * Heap-owned JSON (no prefix/newline), free using cJSON_free(). No audio file. */
char *AudioMonitor_TakeSerialPacket(void);
/* Same JSON/ownership as serial; only the matching active WiFi session can
 * consume PCM. UART cannot consume packets while the pull session is active. */
char *AudioMonitor_TakePullPacket(const char *session_id);
void AudioMonitor_GetStatus(AudioMonitor_Status *out);
