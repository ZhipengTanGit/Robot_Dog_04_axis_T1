#pragma once
#include "esp_err.h"
#include "cJSON.h"
/* Starts independent display/touch and audio tasks. Never plays on boot. */
esp_err_t Peripherals_Start(void);
/* Owned JSON snapshot, caller calls cJSON_Delete. No hardware I/O here. */
cJSON *Peripherals_CreateJson(void);
/* Nonblocking bounded copies, permitted before Start and without Wi-Fi. */
void Peripherals_UpdateRobot(const char *battery_line, const char *peripheral_line);
esp_err_t Peripherals_PlayTone(unsigned hz, unsigned duration_ms, unsigned volume_percent);
