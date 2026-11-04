#include "Bsp_AudioMonitor.h"
#include <string.h>
#include <stdio.h>

#define AUDIO_PACKET_PERIOD_MS 200U

static bool session_valid(const char *id)
{
    if (!id) return false;
    for (unsigned i = 0; i < 16; ++i) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return id[16] == 0;
}
uint32_t AudioMonitor_LeaseRemaining(const AudioMonitor_Lease *lease, uint32_t now)
{
    if (!lease || !lease->enabled || (int32_t)(now - lease->expires_ms) >= 0) return 0;
    return lease->expires_ms - now;
}
bool AudioMonitor_LeaseExpire(AudioMonitor_Lease *lease, uint32_t now)
{
    if (!lease || !lease->enabled || AudioMonitor_LeaseRemaining(lease, now)) return false;
    lease->enabled = false;
    lease->transport = AUDIO_MONITOR_OFF;
    ++lease->generation;
    return true;
}
void AudioMonitor_LeaseStop(AudioMonitor_Lease *lease)
{
    if (!lease) return;
    lease->enabled = false;
    lease->transport = AUDIO_MONITOR_OFF;
    lease->session_id[0] = 0;
    lease->expires_ms = lease->next_seq = 0;
    ++lease->generation;
}
esp_err_t AudioMonitor_LeaseSet(AudioMonitor_Lease *lease, const char *session_id, uint32_t ttl_ms, uint32_t now)
{
    if (!lease || !session_valid(session_id) || !ttl_ms || ttl_ms > AUDIO_MONITOR_MAX_TTL_MS) return ESP_ERR_INVALID_ARG;
    bool changed = strcmp(lease->session_id, session_id) != 0;
    bool expired = !AudioMonitor_LeaseRemaining(lease, now);
    if (changed) { memcpy(lease->session_id, session_id, 17); lease->next_seq = 0; }
    if (changed || expired) ++lease->generation;
    lease->expires_ms = now + ttl_ms;
    lease->enabled = true;
    return ESP_OK;
}

esp_err_t AudioMonitor_ControlSet(AudioMonitor_Lease *lease, AudioMonitor_Transport transport,
                                  const char *session_id, uint32_t ttl_ms, uint32_t now)
{
    if (!lease || !session_valid(session_id) || !ttl_ms || ttl_ms > AUDIO_MONITOR_MAX_TTL_MS ||
        (transport != AUDIO_MONITOR_HTTP && transport != AUDIO_MONITOR_SERIAL &&
         transport != AUDIO_MONITOR_PULL)) return ESP_ERR_INVALID_ARG;
    if (transport == AUDIO_MONITOR_HTTP &&
        (lease->transport == AUDIO_MONITOR_SERIAL || lease->transport == AUDIO_MONITOR_PULL) &&
        AudioMonitor_LeaseRemaining(lease, now)) return ESP_OK;
    bool switch_mode = lease->transport != AUDIO_MONITOR_OFF && lease->transport != transport;
    if (switch_mode) AudioMonitor_LeaseStop(lease);
    esp_err_t error = AudioMonitor_LeaseSet(lease, session_id, ttl_ms, now);
    if (error == ESP_OK) lease->transport = transport;
    return error;
}
bool AudioMonitor_ControlStop(AudioMonitor_Lease *lease, AudioMonitor_Transport transport,
                              const char *session_id, uint32_t now)
{
    if (!lease) return false;
    if (transport == AUDIO_MONITOR_HTTP) {
        if ((lease->transport == AUDIO_MONITOR_SERIAL || lease->transport == AUDIO_MONITOR_PULL) &&
            AudioMonitor_LeaseRemaining(lease, now)) return false;
    } else if (transport == AUDIO_MONITOR_SERIAL || transport == AUDIO_MONITOR_PULL) {
        if (lease->transport != transport || !session_valid(session_id) ||
            strcmp(lease->session_id, session_id)) return false;
    } else return false;
    AudioMonitor_LeaseStop(lease);
    return true;
}
bool AudioMonitor_CanTakePacket(const AudioMonitor_Lease *lease, AudioMonitor_Transport transport,
                                const char *session_id, uint32_t now, uint32_t last_attempt)
{
    if (!lease || (transport != AUDIO_MONITOR_SERIAL && transport != AUDIO_MONITOR_PULL) ||
        lease->transport != transport || !AudioMonitor_LeaseRemaining(lease, now) ||
        (uint32_t)(now - last_attempt) < AUDIO_PACKET_PERIOD_MS) return false;
    return transport == AUDIO_MONITOR_SERIAL ||
        (session_valid(session_id) && !strcmp(lease->session_id, session_id));
}
size_t AudioMonitor_FormatSerialJson(char *out, size_t capacity, const char *session_id,
                                     uint32_t sequence, const uint8_t *pcm, size_t bytes)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!out || !capacity || !session_valid(session_id) || !pcm || !bytes || bytes > 6400 || (bytes & 1)) return 0;
    int prefix = snprintf(out, capacity,
        "{\"session_id\":\"%s\",\"seq\":%lu,\"sample_rate\":16000,\"channels\":1,\"format\":\"s16le\",\"data\":\"",
        session_id, (unsigned long)sequence);
    size_t encoded = ((bytes + 2) / 3) * 4;
    if (prefix < 0 || (size_t)prefix >= capacity || encoded + 3 > capacity - (size_t)prefix) return 0;
    size_t written = (size_t)prefix;
    for (size_t i = 0; i < bytes; i += 3) {
        uint32_t value = (uint32_t)pcm[i] << 16;
        if (i + 1 < bytes) value |= (uint32_t)pcm[i + 1] << 8;
        if (i + 2 < bytes) value |= pcm[i + 2];
        out[written++] = alphabet[(value >> 18) & 63];
        out[written++] = alphabet[(value >> 12) & 63];
        out[written++] = i + 1 < bytes ? alphabet[(value >> 6) & 63] : '=';
        out[written++] = i + 2 < bytes ? alphabet[value & 63] : '=';
    }
    out[written++] = '"'; out[written++] = '}'; out[written] = 0;
    return written;
}

#ifndef AUDIO_MONITOR_HOST_TEST
#include "Bsp_Audio.h"
#include "BuildConfig.h"
#include <stdio.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_MAX_AGE_MS 500U
static portMUX_TYPE monitor_lock = portMUX_INITIALIZER_UNLOCKED;
static AudioMonitor_Lease lease;
static AudioMonitor_Status status = {.state = "OFF", .newest_age_ms = UINT32_MAX, .last_packet_age_ms = UINT32_MAX};
static char device_identity[49], boot_identity[49];
static uint8_t packet[AUDIO_PCM_PACKET_SAMPLES * 2];
static uint32_t packet_last_attempt;
static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void state_locked(const char *name) { snprintf(status.state, sizeof(status.state), "%s", name); }
static void expire_locked(uint32_t now)
{
    AudioMonitor_Transport previous = lease.transport;
    if (AudioMonitor_LeaseExpire(&lease, now)) {
        Audio_MonitorStop();
        state_locked(previous == AUDIO_MONITOR_SERIAL || previous == AUDIO_MONITOR_PULL ? "OFF" : "LEASE_EXPIRED");
    }
}
static bool codec_ready(void)
{
    Audio_Status audio;
    Audio_GetStatus(&audio);
    return audio.configured && audio.valid && (uint32_t)(millis() - audio.last_sample_ms) <= AUDIO_MAX_AGE_MS;
}
static bool network_ready(void)
{
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK && netif &&
        esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}
static esp_err_t set_lease(AudioMonitor_Transport transport, const char *session_id, uint32_t ttl_ms)
{
    uint32_t now = millis();
    portENTER_CRITICAL(&monitor_lock);
    uint32_t generation = lease.generation;
    esp_err_t error = AudioMonitor_ControlSet(&lease, transport, session_id, ttl_ms, now);
    if (error == ESP_OK) {
        Audio_MonitorArm(lease.expires_ms, generation != lease.generation);
        if (generation != lease.generation) {
            packet_last_attempt = now;
            state_locked("WAIT_SAMPLES");
            status.error = ESP_OK;
            status.http_status = 0;
        }
    }
    portEXIT_CRITICAL(&monitor_lock);
    return error;
}
esp_err_t AudioMonitor_SetLease(const char *session_id, uint32_t ttl_ms)
{
    return set_lease(AUDIO_MONITOR_HTTP, session_id, ttl_ms);
}
esp_err_t AudioMonitor_SetSerialLease(const char *session_id, uint32_t ttl_ms)
{
    return set_lease(AUDIO_MONITOR_SERIAL, session_id, ttl_ms);
}
esp_err_t AudioMonitor_SetPullLease(const char *session_id, uint32_t ttl_ms)
{
    return set_lease(AUDIO_MONITOR_PULL, session_id, ttl_ms);
}
void AudioMonitor_Stop(void)
{
    portENTER_CRITICAL(&monitor_lock);
    if (AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_HTTP, NULL, millis())) {
        Audio_MonitorStop(); state_locked("OFF");
    }
    portEXIT_CRITICAL(&monitor_lock);
}
void AudioMonitor_StopSerial(const char *session_id)
{
    portENTER_CRITICAL(&monitor_lock);
    if (AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_SERIAL, session_id, millis())) {
        Audio_MonitorStop(); state_locked("OFF");
    }
    portEXIT_CRITICAL(&monitor_lock);
}
void AudioMonitor_StopPull(const char *session_id)
{
    portENTER_CRITICAL(&monitor_lock);
    if (AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_PULL, session_id, millis())) {
        Audio_MonitorStop(); state_locked("OFF");
    }
    portEXIT_CRITICAL(&monitor_lock);
}
void AudioMonitor_GetStatus(AudioMonitor_Status *out)
{
    if (!out) return;
    Audio_PcmBufferStatus buffer;
    uint32_t now = millis();
    portENTER_CRITICAL(&monitor_lock);
    expire_locked(now);
    *out = status;
    out->enabled = lease.enabled;
    memcpy(out->session_id, lease.session_id, sizeof(out->session_id));
    out->lease_remaining_ms = AudioMonitor_LeaseRemaining(&lease, now);
    out->next_seq = lease.next_seq;
    snprintf(out->transport, sizeof(out->transport), "%s",
        lease.transport == AUDIO_MONITOR_SERIAL ? "serial" : lease.transport == AUDIO_MONITOR_PULL ? "wifi" :
        lease.transport == AUDIO_MONITOR_HTTP ? "http" : "off");
    portEXIT_CRITICAL(&monitor_lock);
    Audio_MonitorGetBufferStatus(&buffer);
    out->buffered_samples = buffer.buffered_samples;
    out->dropped_samples = buffer.dropped_samples;
    out->newest_age_ms = buffer.newest_age_ms;
    out->codec_ready = codec_ready();
}
static char *take_packet(AudioMonitor_Transport transport, const char *session_id)
{
    uint32_t now = millis(), generation;
    bool ready = codec_ready();
    portENTER_CRITICAL(&monitor_lock);
    expire_locked(now);
    if (!AudioMonitor_CanTakePacket(&lease, transport, session_id, now, packet_last_attempt)) {
        portEXIT_CRITICAL(&monitor_lock); return NULL;
    }
    packet_last_attempt = now;
    if (!ready) {
        Audio_MonitorDiscard(); state_locked("CODEC_NOT_READY");
        portEXIT_CRITICAL(&monitor_lock); return NULL;
    }
    generation = lease.generation;
    portEXIT_CRITICAL(&monitor_lock);
    /* One bounded heap allocation: JSON in front, temporary PCM at the tail.
     * No large automatic array on the 6144-byte UART writer task stack. */
    char *json = cJSON_malloc(AUDIO_MONITOR_SERIAL_JSON_CAPACITY + AUDIO_PCM_PACKET_SAMPLES * 2);
    if (!json) {
        portENTER_CRITICAL(&monitor_lock);
        if (lease.generation == generation && lease.transport == transport) {
            ++status.failed_packets; status.error = ESP_ERR_NO_MEM;
            Audio_MonitorDiscard();
            state_locked(transport == AUDIO_MONITOR_PULL ? "WIFI_MEMORY_ERROR" : "SERIAL_MEMORY_ERROR");
        }
        portEXIT_CRITICAL(&monitor_lock);
        return NULL;
    }
    uint8_t *pcm = (uint8_t *)json + AUDIO_MONITOR_SERIAL_JSON_CAPACITY;
    uint32_t age = UINT32_MAX, sequence;
    char session[17];
    portENTER_CRITICAL(&monitor_lock);
    expire_locked(millis());
    if (lease.generation != generation || lease.transport != transport || !lease.enabled) {
        portEXIT_CRITICAL(&monitor_lock); cJSON_free(json); return NULL;
    }
    size_t bytes = Audio_MonitorReadLatest(pcm, AUDIO_PCM_PACKET_SAMPLES, AUDIO_MAX_AGE_MS, &age);
    if (!bytes) {
        state_locked("WAIT_SAMPLES"); portEXIT_CRITICAL(&monitor_lock); cJSON_free(json); return NULL;
    }
    sequence = lease.next_seq++;
    memcpy(session, lease.session_id, sizeof(session));
    portEXIT_CRITICAL(&monitor_lock);
    size_t length = AudioMonitor_FormatSerialJson(json, AUDIO_MONITOR_SERIAL_JSON_CAPACITY, session, sequence, pcm, bytes);
    portENTER_CRITICAL(&monitor_lock);
    expire_locked(millis());
    bool current = lease.generation == generation && lease.transport == transport && lease.enabled;
    if (current && length) {
        ++status.packets; status.samples += (uint32_t)(bytes / 2);
        status.last_upload_ms = millis(); status.last_packet_age_ms = age;
        status.error = ESP_OK; status.http_status = 0;
        state_locked(transport == AUDIO_MONITOR_PULL ? "WIFI_STREAMING" : "SERIAL_STREAMING");
    }
    portEXIT_CRITICAL(&monitor_lock);
    if (!current || !length) { cJSON_free(json); return NULL; }
    return json;
}
char *AudioMonitor_TakeSerialPacket(void)
{
    return take_packet(AUDIO_MONITOR_SERIAL, NULL);
}
char *AudioMonitor_TakePullPacket(const char *session_id)
{
    return take_packet(AUDIO_MONITOR_PULL, session_id);
}
static esp_http_client_handle_t client_create(void)
{
    char url[192];
    int length = snprintf(url, sizeof(url), "%s/api/audio", ROBOT_SERVER_URL);
    if (length < 0 || (size_t)length >= sizeof(url)) return NULL;
    const esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 600,
        .disable_auto_redirect = true, .keep_alive_enable = true,
        .buffer_size = 256, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    if (esp_http_client_set_header(client, "Authorization", "Bearer " ROBOT_INGEST_TOKEN) != ESP_OK ||
        esp_http_client_set_header(client, "X-Device-Id", device_identity) != ESP_OK ||
        esp_http_client_set_header(client, "X-Boot-Id", boot_identity) != ESP_OK ||
        esp_http_client_set_header(client, "X-Sample-Rate", "16000") != ESP_OK ||
        esp_http_client_set_header(client, "X-Channels", "1") != ESP_OK ||
        esp_http_client_set_header(client, "X-Sample-Format", "s16le") != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}
static void monitor_task(void *unused)
{
    (void)unused;
    esp_http_client_handle_t client = NULL;
    uint32_t last_attempt = millis();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint32_t now = millis();
        bool ready = codec_ready();
        portENTER_CRITICAL(&monitor_lock);
        expire_locked(now);
        bool enabled = lease.enabled;
        bool http = lease.transport == AUDIO_MONITOR_HTTP;
        if (enabled && !ready) {
            Audio_MonitorDiscard();
            state_locked("CODEC_NOT_READY");
        }
        portEXIT_CRITICAL(&monitor_lock);
        if (!enabled || !ready || !http) {
            if (client) { esp_http_client_cleanup(client); client = NULL; }
            continue;
        }
        if (!network_ready()) {
            portENTER_CRITICAL(&monitor_lock);
            if (lease.enabled && lease.transport == AUDIO_MONITOR_HTTP) {
                Audio_MonitorDiscard(); state_locked("WAIT_WIFI");
            }
            portEXIT_CRITICAL(&monitor_lock);
            if (client) { esp_http_client_cleanup(client); client = NULL; }
            continue;
        }
        if ((uint32_t)(now - last_attempt) < AUDIO_PACKET_PERIOD_MS) continue;
        last_attempt = now;
        uint32_t age = UINT32_MAX, sequence, generation;
        char session[17], seq_text[16];
        portENTER_CRITICAL(&monitor_lock);
        if (lease.transport != AUDIO_MONITOR_HTTP || !AudioMonitor_LeaseRemaining(&lease, now)) {
            portEXIT_CRITICAL(&monitor_lock); continue;
        }
        size_t bytes = Audio_MonitorReadLatest(packet, AUDIO_PCM_PACKET_SAMPLES, AUDIO_MAX_AGE_MS, &age);
        if (!bytes) { state_locked("WAIT_SAMPLES"); portEXIT_CRITICAL(&monitor_lock); continue; }
        generation = lease.generation;
        sequence = lease.next_seq++;
        memcpy(session, lease.session_id, sizeof(session));
        portEXIT_CRITICAL(&monitor_lock);
        if (!client) client = client_create();
        esp_err_t error = client ? ESP_OK : ESP_ERR_NO_MEM;
        int http_status = 0;
        snprintf(seq_text, sizeof(seq_text), "%lu", (unsigned long)sequence);
        if (error == ESP_OK) error = esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        if (error == ESP_OK) error = esp_http_client_set_header(client, "X-Audio-Session", session);
        if (error == ESP_OK) error = esp_http_client_set_header(client, "X-Audio-Seq", seq_text);
        if (error == ESP_OK) error = esp_http_client_set_post_field(client, (const char *)packet, (int)bytes);
        portENTER_CRITICAL(&monitor_lock);
        bool current = lease.transport == AUDIO_MONITOR_HTTP && lease.generation == generation &&
            AudioMonitor_LeaseRemaining(&lease, millis()) > 0;
        portEXIT_CRITICAL(&monitor_lock);
        if (error == ESP_OK && current) {
            error = esp_http_client_perform(client);
            if (error == ESP_OK) http_status = esp_http_client_get_status_code(client);
        } else if (!current) error = ESP_ERR_INVALID_STATE;
        if (client) esp_http_client_set_post_field(client, NULL, 0);
        bool success = error == ESP_OK && http_status >= 200 && http_status < 300;
        if (!success && client) { esp_http_client_cleanup(client); client = NULL; }
        portENTER_CRITICAL(&monitor_lock);
        if (lease.generation == generation) {
            status.error = error;
            status.http_status = http_status;
            status.last_packet_age_ms = age;
            if (success) {
                ++status.packets; status.samples += (uint32_t)(bytes / 2);
                status.last_upload_ms = millis(); state_locked("STREAMING");
            } else {
                ++status.failed_packets;
                Audio_MonitorDiscard(); /* Failed packets and captured backlog are never retried. */
                state_locked("UPLOAD_ERROR");
            }
            expire_locked(millis());
        }
        portEXIT_CRITICAL(&monitor_lock);
    }
}
esp_err_t AudioMonitor_Start(const char *device_id, const char *boot_id)
{
    if (!device_id || !boot_id || !device_id[0] || !boot_id[0] ||
        strlen(device_id) >= sizeof(device_identity) || strlen(boot_id) >= sizeof(boot_identity)) return ESP_ERR_INVALID_ARG;
    if (status.started) return !strcmp(device_identity, device_id) && !strcmp(boot_identity, boot_id) ? ESP_OK : ESP_ERR_INVALID_STATE;
    snprintf(device_identity, sizeof(device_identity), "%s", device_id);
    snprintf(boot_identity, sizeof(boot_identity), "%s", boot_id);
    if (xTaskCreate(monitor_task, "audio_monitor", 4096, NULL, 1, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&monitor_lock); status.started = true; portEXIT_CRITICAL(&monitor_lock);
    return ESP_OK;
}
#endif /* AUDIO_MONITOR_HOST_TEST */
