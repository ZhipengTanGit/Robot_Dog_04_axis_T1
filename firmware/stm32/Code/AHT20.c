#include "AHT20.h"
#include "BSP_Battery.h"
#include <string.h>

#define AHT20_PERIOD_MS 2000UL
#define AHT20_STALE_MS 5000UL
#define AHT20_TIMEOUT_MS 200UL
static AHT20_Status status;
static uint32_t next_at, conversion_at, good_at;
static uint8_t waiting, bus_was_allowed;

uint8_t AHT20_Crc8(const uint8_t *data, uint8_t length)
{
    uint8_t crc = 0xffU, i, bit;
    for (i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (uint8_t)((crc & 0x80U) ? ((uint32_t)crc << 1) ^ 0x31U : (uint32_t)crc << 1);
        }
    }
    return crc;
}

int AHT20_Decode(const uint8_t frame[7], int32_t *temperature_cc, uint32_t *humidity_cpct)
{
    uint32_t raw_t, raw_rh;
    if (!frame || !temperature_cc || !humidity_cpct) { return -3; }
    if (AHT20_Crc8(frame, 6U) != frame[6]) { return -1; }
    if (frame[0] & 0x80U) { return 1; }
    if (!(frame[0] & 0x08U)) { return -2; }
    raw_rh = ((uint32_t)frame[1] << 12) | ((uint32_t)frame[2] << 4) | (frame[3] >> 4);
    raw_t = ((uint32_t)(frame[3] & 15U) << 16) | ((uint32_t)frame[4] << 8) | frame[5];
    *humidity_cpct = (uint32_t)(((uint64_t)raw_rh * 10000UL + 524288UL) >> 20);
    *temperature_cc = (int32_t)(((uint64_t)raw_t * 20000UL + 524288UL) >> 20) - 5000;
    return 0;
}

void AHT20_Init(uint32_t now)
{
    memset(&status, 0, sizeof(status));
    status.state = AHT20_STARTING;
    next_at = now + 100UL;
    waiting = bus_was_allowed = 0U;
    good_at = conversion_at = now;
}

static void fail(AHT20_State state, uint32_t now)
{
    status.state = state;
    status.valid = 0U;
    ++status.errors;
    waiting = 0U;
    next_at = now + AHT20_PERIOD_MS;
}

void AHT20_Task(uint32_t now, uint8_t bus_allowed)
{
    static const uint8_t measure[3] = { 0xacU, 0x33U, 0U };
    uint8_t frame[7];
    int result;
    int32_t temperature;
    uint32_t humidity;
    if (!bus_allowed) {
        status.state = AHT20_STANDBY;
        status.valid = 0U;
        waiting = bus_was_allowed = 0U;
        return;
    }
    if (!bus_was_allowed) {
        bus_was_allowed = 1U;
        status.state = AHT20_STARTING;
        next_at = now + 100UL;
    }
    if ((int32_t)(now - next_at) < 0) { return; }
    if (!waiting) {
        result = BSP_Battery_Aht20Transfer(measure, 3U, 0, 0U);
        if (result != 0) { fail(AHT20_BUS_ERROR, now); return; }
        waiting = 1U;
        conversion_at = now;
        next_at = now + 80UL;
        status.state = AHT20_MEASURING;
        return;
    }
    result = BSP_Battery_Aht20Transfer(0, 0U, frame, 7U);
    if (result != 0) { fail(AHT20_BUS_ERROR, now); return; }
    result = AHT20_Decode(frame, &temperature, &humidity);
    if (result == 1) {
        if ((uint32_t)(now - conversion_at) >= AHT20_TIMEOUT_MS) { fail(AHT20_BUSY_TIMEOUT, now); }
        else { next_at = now + 10UL; }
        return;
    }
    if (result != 0) {
        fail(result == -1 ? AHT20_CRC_ERROR : AHT20_UNCALIBRATED, now);
        return;
    }
    status.temperature_cc = temperature;
    status.humidity_cpct = humidity;
    status.valid = status.has_sample = 1U;
    status.state = AHT20_READY;
    good_at = now;
    waiting = 0U;
    next_at = conversion_at + AHT20_PERIOD_MS;
}

void AHT20_GetStatus(uint32_t now, AHT20_Status *out)
{
    if (!out) { return; }
    *out = status;
    out->age_ms = status.has_sample ? (uint32_t)(now - good_at) : 0xffffffffUL;
    if (out->age_ms > AHT20_STALE_MS) { out->valid = 0U; }
}

const char *AHT20_StateName(AHT20_State state)
{
    switch (state) {
    case AHT20_STARTING: return "STARTING";
    case AHT20_STANDBY: return "STANDBY";
    case AHT20_MEASURING: return "MEASURING";
    case AHT20_READY: return "READY";
    case AHT20_BUS_ERROR: return "BUS_ERROR";
    case AHT20_CRC_ERROR: return "CRC_ERROR";
    case AHT20_BUSY_TIMEOUT: return "BUSY_TIMEOUT";
    case AHT20_UNCALIBRATED: return "UNCALIBRATED";
    default: return "SENSOR_ERROR";
    }
}
