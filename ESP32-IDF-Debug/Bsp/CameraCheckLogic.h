#pragma once
#include "Bsp_Camera.h"
#define CAMERA_CHECK_MAX_JPEG_BYTES (96U * 1024U)

bool CameraCheck_IdValid(const char *id);
esp_err_t CameraCheck_Begin(Camera_CheckStatus *check, const char *id, uint32_t now, bool *start);
void CameraCheck_Finish(Camera_CheckStatus *check, const char *stage, esp_err_t error, uint32_t now);
/* Structural JPEG validation, not a pixel decode or electrical measurement. */
bool CameraCheck_JpegValid(const uint8_t *data, size_t length, uint16_t width, uint16_t height);
