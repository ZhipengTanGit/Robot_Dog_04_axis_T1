#ifndef BSP_UDP_TELEMETRY_H
#define BSP_UDP_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "SoundCommandLogic.h"
bool UdpTelemetry_QueueSound(const SoundCommand *command);

/* Starts telemetry identities and the USB-configured Wi-Fi manager. Authenticated
 * data/JPEG APIs are on port 81; legacy outbound uploads remain opt-in. */
esp_err_t UdpTelemetry_Start(void);
/* Single producer: call only from the STM32 -> PC bridge task. Never waits
 * for Wi-Fi or a socket. Updates a bounded latest-value/log cache; upload
 * failures never block the UART producer and do not queue stale telemetry. */
void UdpTelemetry_Mirror(const uint8_t *data, size_t length);
/* Same STM32 UART producer only. After an actual RX loss, discard the partial
 * frame and mark cached sensor values invalid until new complete lines arrive. */
void UdpTelemetry_InvalidateStream(void);
/* Thread-safe status snapshot; no password is included. Caller owns UART TX
 * and must print this only at a line boundary. Returns bytes, excluding NUL. */
size_t UdpTelemetry_FormatStatus(char *buffer, size_t size);
/* PC requests are coalesced and rate limited. Only the STM -> PC writer takes
 * snapshots at a line boundary. Free returned JSON with cJSON_free(). */
void UdpTelemetry_RequestSerialStatus(void);
char *UdpTelemetry_TakeSerialStatus(void);
/* Deadline uses ESP_STATUS uptime_ms, not a renewed relative TTL. */
bool UdpTelemetry_QueueSerialCurrent(const char *id, unsigned ma, uint32_t deadline_ms,
                                     const char *boot_prefix);
/* Nonblocking USB command admission; deadline is absolute ESP uptime. */
bool UdpTelemetry_QueueWifi(const char *id, const char *action, const char *ssid,
                            const char *password, uint32_t deadline_ms, const char *boot_prefix);
bool UdpTelemetry_QueueWifiPaired(const char *id, const char *action, const char *ssid,
                                  const char *password, uint32_t deadline_ms,
                                  const char *boot_prefix, const char *api_token);

#endif
