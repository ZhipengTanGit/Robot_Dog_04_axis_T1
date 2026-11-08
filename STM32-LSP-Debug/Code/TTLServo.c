#include "TTLServo.h"
#include "TTLServo_Motion.h"
#include "Servo_Config.h"
#include <stdio.h>
#include <string.h>
static TTLServo_Status status[4];
static const uint32_t bauds[] = {1000000UL,115200UL,500000UL,250000UL,57600UL,9600UL,400000UL,200000UL,19200UL,117647UL,57143UL,9615UL,19231UL};
static uint16_t scan[4];
static uint8_t baud_index[4], failures[4], online[4], read_next[4], channel, active, reading;
static uint32_t started_ms, next_ms;
static int motion_op;
static uint8_t stream_read_due[4];
static uint32_t goal_last[4],goal_sum[4],goal_count[4],goal_max[4];
void TTLServo_ResetTiming(void)
{memset(goal_last,0,sizeof(goal_last));memset(goal_sum,0,sizeof(goal_sum));memset(goal_count,0,sizeof(goal_count));memset(goal_max,0,sizeof(goal_max));}

void TTLServo_Init(void)
{
    memset(status, 0, sizeof(status)); memset(scan, 0, sizeof(scan));
    memset(baud_index, 0, sizeof(baud_index)); memset(failures, 0, sizeof(failures));
    memset(online, 0, sizeof(online)); memset(read_next, 0, sizeof(read_next));
    channel = active = 0; started_ms = next_ms = 0;
    TTLServo_MotionInit(); motion_op=0;
    memset(stream_read_due,0,sizeof(stream_read_due));
    TTLServo_ResetTiming();
    BSP_TTLServo_Init();
}

static void complete(uint32_t now, int result)
{
    TTLServo_Status *s = &status[channel];
    s->result = result;
    if((result||s->status)&&(TTLServo_ActiveMask()&(1U<<channel)))TTLServo_StopAll();
    if (!result) {
        failures[channel] = 0; online[channel] = 1;
        if (reading) { s->valid = 1; s->sampled_ms = now; read_next[channel] = 1; stream_read_due[channel]=0; }
        else { read_next[channel] = 1; s->result = s->valid ? 0 : -7; }
    } else {
        s->valid = 0; ++s->errors; read_next[channel] = 0;
        if (reading) {
            if (result == -1 && online[channel]) { s->result = -7; }
        } else if (++failures[channel] >= 3U || !online[channel]) {
            online[channel] = failures[channel] = 0;
            if (!(TTLServo_ActiveMask() & (1U<<channel)) && ++baud_index[channel] >= sizeof(bauds)/sizeof(bauds[0])) {
                baud_index[channel] = 0;
                if (++scan[channel] >= 254U) { scan[channel] = 0; }
            }
        }
    }
    /* A successful position reply already proves presence. Keep reading the
     * known servo; failures return to Ping and retain the discovery backoff. */
    next_ms = now + (online[channel] ? 1U : 5U);
    active = 0; channel = (uint8_t)((channel + 1U) & 3U);
}

void TTLServo_Task(uint32_t now)
{
    uint8_t packet[16], bytes[TTL_SERVO_REPLY_MAX];
    size_t size = 0;
    int result;
    TTLServo_Status *s = &status[channel];
    if (active) {
        result = BSP_TTLServo_Poll(bytes, sizeof(bytes), &size);
        if (motion_op && result <= 0) {
            TTLServo_MotionComplete(channel,motion_op,bytes,size,result,now);
            if(TTLServo_IsStreaming())stream_read_due[channel]=1;
            motion_op=0;active=0;channel=(uint8_t)((channel+1U)&3U);next_ms=now+(TTLServo_IsStreaming()?1U:5U);return;
        }
        if (result == 0) {
            BSP_TTLServo_Diagnostics(&s->tx_mismatches, &s->rx_edges);
            complete(now, reading ? TTLServo_ParseReply(bytes,size,s->id,s) : TTLServo_ParsePing(bytes,size,s->id,s));
        }
        else if (result < 0 || (uint32_t)(now - started_ms) > (TTL_SERVO_SAMPLE_COUNT*250UL/s->baud + 15UL)) {
            BSP_TTLServo_Diagnostics(&s->tx_mismatches, &s->rx_edges);
            BSP_TTLServo_Abort(); complete(now, result < 0 ? result : -5);
        }
        return;
    }
    if ((int32_t)(now - next_ms) < 0) { return; }
    /* A commanded LOW that remains HIGH needs physical inspection. Do not
     * repeatedly drive a potentially contended line; reset after inspection. */
    if (s->result == -9) { channel=(uint8_t)((channel+1U)&3U); next_ms=now+5U; return; }
    /* Try 1,2,3,4,0,5..253. Each original physical port is an independent bus;
     * the same factory ID on different ports does not collide. */
    s->id = (uint8_t)(scan[channel] < 4U ? scan[channel] + 1U : scan[channel] == 4U ? 0U : scan[channel]);
    s->baud = bauds[baud_index[channel]];
    /* Interleave fresh position reads; continuous targets may never starve
     * the independent feedback/interlock path. */
    motion_op=TTLServo_IsStreaming()&&stream_read_due[channel]?0:TTLServo_MotionNext(channel,s->id,packet,&size,now);
    if(motion_op) {
        if(motion_op==1&&TTLServo_IsStreaming()){
            if(goal_last[channel]){uint32_t gap=(uint32_t)(now-goal_last[channel]);goal_sum[channel]+=gap;++goal_count[channel];if(gap>goal_max[channel])goal_max[channel]=gap;}
            goal_last[channel]=now;
        }
        result=BSP_TTLServo_Start(channel,s->baud,packet,size);
        if(result){TTLServo_MotionComplete(channel,motion_op,bytes,0,result,now);motion_op=0;channel=(uint8_t)((channel+1U)&3U);next_ms=now+5U;}
        else {active=1;started_ms=now;}return;
    }
    reading = read_next[channel];
    if (reading) { TTLServo_ReadPacket(s->id, packet); } else { TTLServo_PingPacket(s->id,packet); }
    result = BSP_TTLServo_Start(channel, s->baud, packet, reading ? 8U : 6U);
    if (result) { complete(now, result); }
    else { active = 1; started_ms = now; }
}

void TTLServo_GetStatus(uint8_t index, uint32_t now, TTLServo_Status *out)
{
    if (index >= 4U || !out) { return; }
    *out = status[index];
    if ((uint32_t)(now - out->sampled_ms) >= 1500U) { out->valid = 0; }
}

void TTLServo_PrintStatus(uint32_t now)
{
    uint8_t index;
    TTLServo_Status s;
    printf(" servo_bus=TTL_CONTROL ttl_span=%u ttl_speed_proto=1",(unsigned)SERVO_TTL_RANGE_DEG);
    for (index = 0; index < 4U; ++index) {
        TTLServo_GetStatus(index, now, &s);
        printf(" ttl%u=%u,%lu,", (unsigned)index, (unsigned)s.id, (unsigned long)s.baud);
        if (s.valid) { printf("%u", (unsigned)s.position); } else { printf("NA"); }
        printf(",%u,%lu,%lu,%d", (unsigned)s.status,
            (unsigned long)(s.sampled_ms ? (uint32_t)(now - s.sampled_ms) : 0xFFFFFFFFUL),
            (unsigned long)s.errors, s.result);
        printf(" tx%u=%u rx%u=%u",(unsigned)index,(unsigned)s.tx_mismatches,(unsigned)index,(unsigned)s.rx_edges);
        printf(" tq%u=%u",(unsigned)index,(unsigned)TTLServo_TorqueState(index));
    }
    printf(" wdt=%lu,%lu,%lu,%lu wmx=%lu,%lu,%lu,%lu",
        (unsigned long)(goal_count[0]?goal_sum[0]/goal_count[0]:0),(unsigned long)(goal_count[1]?goal_sum[1]/goal_count[1]:0),
        (unsigned long)(goal_count[2]?goal_sum[2]/goal_count[2]:0),(unsigned long)(goal_count[3]?goal_sum[3]/goal_count[3]:0),
        (unsigned long)goal_max[0],(unsigned long)goal_max[1],(unsigned long)goal_max[2],(unsigned long)goal_max[3]);
}
