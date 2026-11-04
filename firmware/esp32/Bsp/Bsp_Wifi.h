#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char state[12], ssid[33], ip[16], request_id[17], request_status[16];
    uint32_t ip_address, generation, disconnects, got_ip_events;
    int error, rssi, save_error;
    unsigned disconnect_reason;
    bool saved, ready, api_ready;
} BoardWifi_Status;

/* The callback returns a cJSON-allocated, secret-free telemetry string.
 * api_ready covers both authenticated servers: status/capture on 81 and one
 * MJPEG stream client on 82. Camera readiness remains a separate condition. */
esp_err_t BoardWifi_Start(const char *device, const char *boot, char *(*snapshot)(void),
                          const char *legacy_ssid, const char *legacy_password);
bool BoardWifi_Queue(const char *id, const char *action, const char *ssid,
                     const char *password, uint32_t deadline_ms, const char *boot_prefix);
/* Called only by the physical UART command path. api_token stays in RAM. */
bool BoardWifi_QueuePaired(const char *id, const char *action, const char *ssid,
                           const char *password, uint32_t deadline_ms,
                           const char *boot_prefix, const char *api_token);
void BoardWifi_GetStatus(BoardWifi_Status *out);
