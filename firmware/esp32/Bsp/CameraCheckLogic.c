#include "CameraCheckLogic.h"
#include <string.h>
#include <stdio.h>

bool CameraCheck_IdValid(const char *id)
{
    if (!id) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return id[16] == 0;
}
esp_err_t CameraCheck_Begin(Camera_CheckStatus *check, const char *id, uint32_t now, bool *start)
{
    if (start) *start = false;
    if (!check || !start || !CameraCheck_IdValid(id)) return ESP_ERR_INVALID_ARG;
    if (!strcmp(check->id, id)) return ESP_OK;
    if (check->running) return ESP_ERR_INVALID_STATE;
    uint32_t sequence = check->sequence + 1;
    memset(check, 0, sizeof(*check));
    memcpy(check->id, id, sizeof(check->id));
    strcpy(check->state, "RUNNING"); strcpy(check->stage, "WAIT_FRAME");
    check->sequence = sequence; check->started_ms = now; check->running = true;
    check->expander_error = check->sccb_error = check->id_error =
        check->init_error = check->frame_error = ESP_ERR_INVALID_STATE;
    *start = true;
    return ESP_OK;
}
void CameraCheck_Finish(Camera_CheckStatus *check, const char *stage, esp_err_t error, uint32_t now)
{
    if (!check) return;
    check->running = false; check->completed = true; check->completed_ms = now;
    check->usable = error == ESP_OK && check->expander_ok && check->sccb_ack &&
        check->id_ok && check->init_ok && check->frame_ok;
    check->error = error == ESP_OK && !check->usable ? ESP_ERR_INVALID_STATE : error;
    strcpy(check->state, check->usable ? "PASSED" : "FAILED");
    snprintf(check->stage, sizeof(check->stage), "%s", stage ? stage : "UNKNOWN");
}
bool CameraCheck_JpegValid(const uint8_t *data, size_t length, uint16_t width, uint16_t height)
{
    if (!data || length < 32 || length > CAMERA_CHECK_MAX_JPEG_BYTES || !width || !height ||
        data[0] != 0xff || data[1] != 0xd8 || data[length-2] != 0xff || data[length-1] != 0xd9) return false;
    bool dimensions = false;
    size_t position = 2;
    while (position < length - 2) {
        if (data[position++] != 0xff) return false;
        while (position < length - 2 && data[position] == 0xff) ++position;
        if (position >= length - 2) return false;
        uint8_t marker = data[position++];
        if (marker == 0 || marker == 0xd8 || marker == 0xd9 || marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) return false;
        if (position + 2 > length - 2) return false;
        size_t segment = (size_t)data[position] * 256 + data[position+1];
        if (segment < 2 || segment > length - 2 - position) return false;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (segment < 11 || data[position+2] != 8) return false;
            unsigned components = data[position+7];
            if ((components != 1 && components != 3) || segment != 8 + 3 * components) return false;
            uint16_t h = ((uint16_t)data[position+3] << 8) | data[position+4];
            uint16_t w = ((uint16_t)data[position+5] << 8) | data[position+6];
            if (w != width || h != height) return false;
            dimensions = true;
        }
        if (marker == 0xda) {
            if (!dimensions || segment < 6) return false;
            unsigned components = data[position+2];
            if (!components || components > 3 || segment != 6 + 2 * components) return false;
            /* Require actual entropy bytes between SOS and EOI. Hardware frame
             * origin and age are checked separately by the camera owner. */
            return position + segment < length - 2;
        }
        position += segment;
    }
    return false;
}
