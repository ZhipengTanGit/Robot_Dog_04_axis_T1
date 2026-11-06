#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAMERA_SERIAL_MAX_BYTES (512U * 1024U)
#define CAMERA_SERIAL_CHUNK_BYTES 1024U
#define CAMERA_SERIAL_TIMEOUT_MS 15000U
#define CAMERA_SERIAL_LINE_BYTES 1800U

typedef struct {
    uint32_t bytes, width, height, sequence, timestamp_ms, crc32;
} CameraSerial_Metadata;

bool CameraSerial_ValidId(const char *id);
uint32_t CameraSerial_Crc32(const uint8_t *data, size_t length);
size_t CameraSerial_FormatBegin(char *out, size_t capacity, const char *id,
                               const CameraSerial_Metadata *meta);
size_t CameraSerial_FormatData(char *out, size_t capacity, const char *id,
                              uint32_t index, const uint8_t *data, size_t length);
size_t CameraSerial_FormatEnd(char *out, size_t capacity, const char *id,
                             const CameraSerial_Metadata *meta);
size_t CameraSerial_FormatError(char *out, size_t capacity, const char *id,
                               int error, const char *detail);
