#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Public USB build only. A worker briefly leases/copies a real frame; only
 * the existing STM->PC UART writer emits bounded complete protocol lines. */
esp_err_t CameraSerial_Start(void);
bool CameraSerial_Available(void);
bool CameraSerial_IsBusy(void);
esp_err_t CameraSerial_Request(const char *id);
bool CameraSerial_WriteNext(void);
