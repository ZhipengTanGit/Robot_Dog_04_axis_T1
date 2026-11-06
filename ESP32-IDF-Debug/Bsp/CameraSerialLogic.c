#include "CameraSerialLogic.h"
#include <stdio.h>
#include <string.h>

bool CameraSerial_ValidId(const char *id)
{
    if (!id || strlen(id) != 16) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}

uint32_t CameraSerial_Crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0U - (crc & 1U)) & UINT32_C(0xedb88320));
    }
    return ~crc;
}

static bool metadata_valid(const CameraSerial_Metadata *m)
{
    return m && m->bytes >= 4 && m->bytes <= CAMERA_SERIAL_MAX_BYTES &&
        m->width && m->width <= 65535 && m->height && m->height <= 65535;
}
static size_t complete(char *out, size_t capacity, int written)
{
    if (written <= 0 || (size_t)written >= capacity) {
        if (out && capacity) out[0] = 0;
        return 0;
    }
    return (size_t)written;
}
size_t CameraSerial_FormatBegin(char *out, size_t capacity, const char *id,
                               const CameraSerial_Metadata *m)
{
    if (!out || !capacity || !CameraSerial_ValidId(id) || !metadata_valid(m)) return 0;
    return complete(out, capacity, snprintf(out, capacity,
        "ESP_CAMERA_FRAME {\"id\":\"%s\",\"phase\":\"BEGIN\",\"bytes\":%lu,\"width\":%lu,\"height\":%lu,"
        "\"chunks\":%lu,\"chunk_bytes\":%u,\"crc32\":%lu,\"seq\":%lu,\"timestamp_ms\":%lu}\r\n",
        id, (unsigned long)m->bytes, (unsigned long)m->width, (unsigned long)m->height,
        (unsigned long)((m->bytes + CAMERA_SERIAL_CHUNK_BYTES - 1) / CAMERA_SERIAL_CHUNK_BYTES),
        CAMERA_SERIAL_CHUNK_BYTES, (unsigned long)m->crc32, (unsigned long)m->sequence,
        (unsigned long)m->timestamp_ms));
}
size_t CameraSerial_FormatData(char *out, size_t capacity, const char *id,
                              uint32_t index, const uint8_t *data, size_t length)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!out || !capacity || !CameraSerial_ValidId(id) || !data || !length ||
        length > CAMERA_SERIAL_CHUNK_BYTES || index >= CAMERA_SERIAL_MAX_BYTES / CAMERA_SERIAL_CHUNK_BYTES) return 0;
    int prefix = snprintf(out, capacity,
        "ESP_CAMERA_FRAME {\"id\":\"%s\",\"phase\":\"DATA\",\"index\":%lu,\"data\":\"",
        id, (unsigned long)index);
    size_t encoded = ((length + 2) / 3) * 4;
    if (prefix <= 0 || (size_t)prefix + encoded + 5 >= capacity) { out[0] = 0; return 0; }
    size_t cursor = (size_t)prefix;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t value = (uint32_t)data[i] << 16;
        if (i + 1 < length) value |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < length) value |= data[i + 2];
        out[cursor++] = alphabet[(value >> 18) & 63];
        out[cursor++] = alphabet[(value >> 12) & 63];
        out[cursor++] = i + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        out[cursor++] = i + 2 < length ? alphabet[value & 63] : '=';
    }
    memcpy(out + cursor, "\"}\r\n", 5);
    return cursor + 4;
}
size_t CameraSerial_FormatEnd(char *out, size_t capacity, const char *id,
                             const CameraSerial_Metadata *m)
{
    if (!out || !capacity || !CameraSerial_ValidId(id) || !metadata_valid(m)) return 0;
    return complete(out, capacity, snprintf(out, capacity,
        "ESP_CAMERA_FRAME {\"id\":\"%s\",\"phase\":\"END\",\"bytes\":%lu,\"chunks\":%lu,\"crc32\":%lu,\"seq\":%lu}\r\n",
        id, (unsigned long)m->bytes,
        (unsigned long)((m->bytes + CAMERA_SERIAL_CHUNK_BYTES - 1) / CAMERA_SERIAL_CHUNK_BYTES),
        (unsigned long)m->crc32, (unsigned long)m->sequence));
}
size_t CameraSerial_FormatError(char *out, size_t capacity, const char *id,
                               int error, const char *detail)
{
    if (!out || !capacity || !CameraSerial_ValidId(id) || !detail || !*detail || strlen(detail) > 40) return 0;
    for (const char *p = detail; *p; ++p) if (!((*p >= 'A' && *p <= 'Z') || *p == '_')) return 0;
    return complete(out, capacity, snprintf(out, capacity,
        "ESP_CAMERA_FRAME {\"id\":\"%s\",\"phase\":\"ERROR\",\"error\":%d,\"detail\":\"%s\"}\r\n",
        id, error, detail));
}
