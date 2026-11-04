#include "TTLServo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t request[8], selected;
static int fail;
static unsigned starts;
void BSP_TTLServo_Init(void) { }
void BSP_TTLServo_Abort(void) { }
int BSP_TTLServo_Start(uint8_t channel, uint32_t baud, const uint8_t *packet, size_t size)
{
    assert(channel < 4 && baud > 0); selected = channel;
    ++starts;
    if(fail==9) return -9;
    memset(request,0,sizeof(request)); memcpy(request, packet, size); return 0;
}
void BSP_TTLServo_Diagnostics(uint16_t *tx_errors,uint16_t *edges) { *tx_errors=0;*edges=20; }
static void reply(uint8_t *p, uint8_t id, unsigned pos)
{
    unsigned i, sum = 0;
    uint8_t value[14] = {255,255,0,10,0,0,0,23,0,11,0,50,29,0};
    memcpy(p, value, 14); p[2] = id; p[5] = (uint8_t)pos; p[6] = (uint8_t)(pos >> 8);
    for (i = 2; i < 13; ++i) { sum += p[i]; }
    p[13] = (uint8_t)(255U - (sum & 255U));
}
int BSP_TTLServo_Poll(uint8_t *bytes, size_t capacity, size_t *size)
{
    assert(capacity >= 14 && (request[4] == 1 || (request[4] == 2 && request[5] == 0x24 && request[6] == 8)));
    if (fail) { *size = 0; return 0; }
    if(request[4] == 1) { bytes[0]=bytes[1]=255;bytes[2]=request[2];bytes[3]=2;bytes[4]=0;bytes[5]=(uint8_t)(255U-(uint8_t)(request[2]+2U));*size=6; }
    else { reply(bytes, request[2], 100U + selected * 100U); *size = 14; }
    return 0;
}
static size_t samples_from(const uint8_t *bytes, size_t size, uint16_t *samples, unsigned phase)
{
    size_t i, offset = 16U + phase;
    unsigned bit, n;
    for (i = 0; i < 1024U; ++i) { samples[i] = 0xFFFFU; }
    for (i = 0; i < size; ++i) {
        for (bit = 0; bit < 10; ++bit) {
            unsigned high = bit == 9 || (bit && (bytes[i] & (1U << (bit - 1U))));
            for (n = 0; n < 4; ++n) { samples[offset++] = high ? 0xFFFFU : 0xFFBFU; }
        }
    }
    return offset + 8U;
}
int main(void)
{
    uint8_t p[14], decoded[96], buffer[40];
    uint16_t samples[1024];
    size_t count;
    unsigned id, phase, now;
    TTLServo_Status s;
    for (id = 0; id < 254; ++id) {
        TTLServo_ReadPacket((uint8_t)id, request);
        assert(request[2] != 254 && request[4] == 2);
        reply(p, (uint8_t)id, 512);
        for (phase = 0; phase < 4; ++phase) {
            count = samples_from(p, 14, samples, phase);
            count = TTLServo_DecodeSamples(samples, count, 64, decoded, 96);
            assert(count == 14 && memcmp(p, decoded, 14) == 0);
            assert(TTLServo_ParseReply(decoded, count, (uint8_t)id, &s) == 0 && s.position == 512);
        }
    }
    reply(p, 1, 512); p[13] ^= 1;
    assert(TTLServo_ParseReply(p, 14, 1, &s) < 0);
    reply(p, 1, 1024); assert(TTLServo_ParseReply(p, 14, 1, &s) == -3);
    reply(p, 1, 512); assert(TTLServo_ParseReply(p, 14, 2, &s) < 0);
    assert(TTLServo_ParseReply(p, 13, 1, &s) < 0);
    TTLServo_ReadPacket(1, buffer); memcpy(buffer + 8, p, 14);
    assert(TTLServo_ParseReply(buffer, 22, 1, &s) == 0);
    assert(TTLServo_ParseReply(buffer, 8, 1, &s) < 0);
    TTLServo_Init();
    for (now = 1; now < 100; ++now) { TTLServo_Task(now); }
    for (id = 0; id < 4; ++id) {
        TTLServo_GetStatus((uint8_t)id, now, &s);
        assert(s.valid && s.id == 1 && s.baud == 1000000 && s.position == 100U + id * 100U);
        TTLServo_GetStatus((uint8_t)id, now + 2000U, &s); assert(!s.valid);
    }
    fail = 1;
    for (; now < 150; ++now) { TTLServo_Task(now); }
    TTLServo_GetStatus(0, now, &s); assert(!s.valid && s.errors && s.result < 0);
    fail=9;starts=0;TTLServo_Init();
    for(now=1;now<100;++now) TTLServo_Task(now);
    assert(starts==4);
    for(id=0;id<4;++id) { TTLServo_GetStatus((uint8_t)id,now,&s);assert(s.result==-9 && s.errors==1 && !s.valid); }
    puts("TTL read-only tests passed: four ports, IDs, sample decoding, framing/stale rejection and pin-conflict latch without repeated drive.");
    return 0;
}
