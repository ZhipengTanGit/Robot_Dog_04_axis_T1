#include "TTLServo.h"

int TTLServo_ControlReady(const TTLServo_Status *s)
{
    return s && s->valid && !s->status &&
        (s->baud == 115200UL || s->baud == 117647UL);
}

void TTLServo_PingPacket(uint8_t id, uint8_t p[6])
{
    p[0] = p[1] = 255U; p[2] = id; p[3] = 2U; p[4] = 1U;
    p[5] = (uint8_t)(255U - (uint8_t)(id + 3U));
}

int TTLServo_ParsePing(const uint8_t *p, size_t size, uint8_t id, TTLServo_Status *out)
{
    size_t i;
    for (i = 0; i + 6U <= size; ++i) {
        if (p[i] == 255U && p[i+1U] == 255U && p[i+2U] == id && p[i+3U] == 2U &&
            ((unsigned)p[i+2U]+p[i+3U]+p[i+4U]+p[i+5U]) % 256U == 255U) {
            out->status = p[i+4U]; return 0;
        }
    }
    return size ? -2 : -1;
}

void TTLServo_ReadPacket(uint8_t id, uint8_t p[8])
{
    p[0] = p[1] = 0xFFU; p[2] = id; p[3] = 4U;
    p[4] = 2U; p[5] = 0x24U; p[6] = 8U;
    p[7] = (uint8_t)(255U - (uint8_t)(id + 4U + 2U + 0x24U + 8U));
}

int TTLServo_ParseReply(const uint8_t *data, size_t size, uint8_t id, TTLServo_Status *out)
{
    size_t offset, j;
    unsigned sum;
    const uint8_t *p;
    for (offset = 0; offset + 6U <= size; ++offset) {
        p = data + offset;
        if (p[0] != 0xFFU || p[1] != 0xFFU || p[2] != id || p[3] < 2U || p[3] > 10U ||
            offset + p[3] + 4U > size) { continue; }
        sum = 0;
        for (j = 2; j < (size_t)p[3] + 4U; ++j) { sum += p[j]; }
        if ((sum & 255U) != 255U) { continue; }
        if (p[3] == 2U && p[4]) { out->status = p[4]; return -4; }
        /* Length 10: status plus eight bytes, then checksum. This excludes
         * the master's echoed READ packet and all short/foreign replies. */
        if (p[3] != 10U) { continue; }
        if (((unsigned)p[5] | ((unsigned)p[6] << 8)) > 1023U) { return -3; }
        out->position = (uint16_t)((unsigned)p[5] | ((unsigned)p[6] << 8));
        out->speed = (uint16_t)((unsigned)p[7] | ((unsigned)p[8] << 8));
        out->load = (uint16_t)((unsigned)p[9] | ((unsigned)p[10] << 8));
        out->voltage = p[11]; out->temperature = p[12]; out->status = p[4];
        return 0;
    }
    return size ? -2 : -1;
}

size_t TTLServo_DecodeSamples(const uint16_t *s, size_t count, uint16_t mask, uint8_t *bytes, size_t capacity)
{
    size_t i = 1U, used = 0;
    unsigned bit;
    uint8_t value;
    /* Four samples/bit. Validate start and stop, then resynchronize on the
     * next falling edge. Sampling DMA is independent of CPU/IRQ latency. */
    while (i + 39U < count && used < capacity) {
        if (!(s[i - 1U] & mask) || (s[i] & mask) || (s[i + 2U] & mask)) { ++i; continue; }
        value = 0;
        for (bit = 0; bit < 8U; ++bit) {
            if (s[i + 6U + bit * 4U] & mask) { value |= (uint8_t)(1U << bit); }
        }
        if (!(s[i + 38U] & mask)) { ++i; continue; }
        bytes[used++] = value;
        i += 39U;
    }
    return used;
}
