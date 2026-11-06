#include "Bsp_Audio.h"
#include "Bsp_AudioMonitor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static Audio_PcmBuffer buffer;
static int16_t stereo[(AUDIO_PCM_CAPACITY + 1) * 2];
static uint8_t output[AUDIO_PCM_CAPACITY * 2];
static int16_t decoded(size_t index)
{
    return (int16_t)((uint16_t)output[index * 2] | (uint16_t)output[index * 2 + 1] << 8);
}
static void fill_ramp(size_t frames, int base)
{
    for (size_t i = 0; i < frames; ++i) {
        stereo[i * 2] = (int16_t)(base + (int)i);
        stereo[i * 2 + 1] = -12345;
    }
}
static void test_mono_and_off(void)
{
    memset(&buffer, 0, sizeof(buffer));
    const int16_t samples[] = {0x1234, 99, -1, 88, -32768, 77, 32767, 66};
    uint32_t age;
    Audio_PcmBufferPushLeft(&buffer, samples, 4, 100);
    assert(buffer.count == 0);
    Audio_PcmBufferArm(&buffer, 1000, true);
    Audio_PcmBufferPushLeft(&buffer, samples, 4, 100);
    assert(Audio_PcmBufferReadLatest(&buffer, output, 4, 110, 500, &age) == 8);
    assert(age == 10 && output[0] == 0x34 && output[1] == 0x12);
    assert(decoded(0) == 0x1234 && decoded(1) == -1 && decoded(2) == -32768 && decoded(3) == 32767);
    assert(Audio_PcmBufferReadLatest(&buffer, output, 4, 110, 500, &age) == 0 && age == UINT32_MAX);
}
static void test_overflow_latest(void)
{
    memset(&buffer, 0, sizeof(buffer));
    Audio_PcmBufferArm(&buffer, 10000, true);
    fill_ramp(AUDIO_PCM_CAPACITY, 0);
    Audio_PcmBufferPushLeft(&buffer, stereo, AUDIO_PCM_CAPACITY, 1000);
    fill_ramp(100, AUDIO_PCM_CAPACITY);
    Audio_PcmBufferPushLeft(&buffer, stereo, 100, 1006);
    assert(buffer.count == AUDIO_PCM_CAPACITY && buffer.dropped == 100);
    uint32_t age;
    assert(Audio_PcmBufferReadLatest(&buffer, output, 3200, 1006, 500, &age) == 6400);
    assert(decoded(0) == 12900 && decoded(3199) == 16099);
    assert(buffer.dropped == 12900 && age == 199 && buffer.count == 0);
    memset(&buffer, 0, sizeof(buffer));
    Audio_PcmBufferArm(&buffer, 10000, true);
    fill_ramp(AUDIO_PCM_CAPACITY + 1, 0);
    Audio_PcmBufferPushLeft(&buffer, stereo, AUDIO_PCM_CAPACITY + 1, 2000);
    assert(buffer.count == AUDIO_PCM_CAPACITY && buffer.dropped == 1);
    assert(Audio_PcmBufferReadLatest(&buffer, output, AUDIO_PCM_CAPACITY, 2000, 1200, &age) == 32000);
    assert(decoded(0) == 1 && decoded(AUDIO_PCM_CAPACITY - 1) == AUDIO_PCM_CAPACITY);
}
static void test_age_clear_and_expiry(void)
{
    memset(&buffer, 0, sizeof(buffer));
    Audio_PcmBufferArm(&buffer, 5000, true);
    fill_ramp(3200, 1);
    Audio_PcmBufferPushLeft(&buffer, stereo, 3200, 1000);
    assert(Audio_PcmBufferReadLatest(&buffer, output, 3200, 1302, 500, NULL) == 0);
    assert(buffer.count == 0 && buffer.dropped == 3200);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, 2000);
    Audio_PcmBufferArm(&buffer, 6000, false);
    assert(buffer.count == 4); /* Renewal does not erase fresh audio. */
    Audio_PcmBufferArm(&buffer, 6000, true);
    assert(buffer.count == 0);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, 2001);
    Audio_PcmBufferStop(&buffer);
    assert(!buffer.enabled && buffer.count == 0);
    Audio_PcmBufferArm(&buffer, 3000, false);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, 2999);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, 3000);
    assert(!buffer.enabled && buffer.count == 0); /* Producer itself enforces TTL. */
    Audio_PcmBufferArm(&buffer, 20, true);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, UINT32_MAX - 10);
    uint32_t age;
    assert(Audio_PcmBufferReadLatest(&buffer, output, 4, 5, 500, &age) == 8 && age == 16);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, 20);
    assert(!buffer.enabled && buffer.count == 0);
}
static void test_lease_validation_and_sequence(void)
{
    AudioMonitor_Lease lease = {0};
    const char *first = "0123456789abcdef", *second = "abcdef0123456789";
    assert(AudioMonitor_LeaseSet(&lease, first, 15000, 100) == ESP_OK);
    assert(lease.enabled && lease.generation == 1 && lease.next_seq == 0);
    assert(AudioMonitor_LeaseRemaining(&lease, 101) == 14999);
    lease.next_seq = 7;
    assert(AudioMonitor_LeaseSet(&lease, first, 15000, 200) == ESP_OK);
    assert(lease.generation == 1 && lease.next_seq == 7);
    AudioMonitor_Lease snapshot = lease;
    const char *invalid[] = {NULL, "", "0123", "0123456789abcdeg", "0123456789abcdef0", "0123456789abcde\n"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        assert(AudioMonitor_LeaseSet(&lease, invalid[i], 10, 300) == ESP_ERR_INVALID_ARG);
        assert(memcmp(&lease, &snapshot, sizeof(lease)) == 0);
    }
    assert(AudioMonitor_LeaseSet(&lease, first, 0, 300) == ESP_ERR_INVALID_ARG);
    assert(AudioMonitor_LeaseSet(&lease, first, 15001, 300) == ESP_ERR_INVALID_ARG);
    assert(memcmp(&lease, &snapshot, sizeof(lease)) == 0);
    assert(!AudioMonitor_LeaseExpire(&lease, 15199));
    assert(AudioMonitor_LeaseExpire(&lease, 15200) && !lease.enabled && lease.generation == 2);
    assert(!AudioMonitor_LeaseExpire(&lease, 15201));
    assert(AudioMonitor_LeaseSet(&lease, first, 1000, 15300) == ESP_OK);
    assert(lease.next_seq == 7 && lease.generation == 3); /* Late same-session renewal cannot replay seq 0. */
    assert(AudioMonitor_LeaseSet(&lease, second, 1000, 15400) == ESP_OK);
    assert(lease.next_seq == 0 && lease.generation == 4);
    AudioMonitor_LeaseStop(&lease);
    assert(!lease.enabled && !lease.session_id[0] && lease.next_seq == 0);
    assert(AudioMonitor_LeaseSet(&lease, first, 40, UINT32_MAX - 19) == ESP_OK);
    assert(AudioMonitor_LeaseRemaining(&lease, UINT32_MAX - 9) == 30);
    assert(AudioMonitor_LeaseRemaining(&lease, 10) == 10);
    assert(AudioMonitor_LeaseExpire(&lease, 20));
}
static void test_session_switch_and_disconnect(void)
{
    AudioMonitor_Lease lease = {0};
    memset(&buffer, 0, sizeof(buffer));
    assert(AudioMonitor_LeaseSet(&lease, "1111111111111111", 1000, 100) == ESP_OK);
    Audio_PcmBufferArm(&buffer, lease.expires_ms, true);
    fill_ramp(4, 1);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, 100);
    uint32_t old_generation = lease.generation;
    assert(AudioMonitor_LeaseSet(&lease, "2222222222222222", 1000, 101) == ESP_OK);
    Audio_PcmBufferArm(&buffer, lease.expires_ms, old_generation != lease.generation);
    assert(Audio_PcmBufferReadLatest(&buffer, output, 4, 101, 500, NULL) == 0);
    fill_ramp(4, 500);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, 102);
    Audio_PcmBufferDiscard(&buffer); /* Disconnect/upload failure discards backlog. */
    assert(Audio_PcmBufferReadLatest(&buffer, output, 4, 103, 500, NULL) == 0);
    fill_ramp(4, 900);
    Audio_PcmBufferPushLeft(&buffer, stereo, 4, 104);
    assert(Audio_PcmBufferReadLatest(&buffer, output, 4, 104, 500, NULL) == 8);
    assert(decoded(0) == 900);
}
static void test_transport_isolation(void)
{
    AudioMonitor_Lease lease = {0};
    const char *http = "1111111111111111", *serial = "2222222222222222", *new_serial = "3333333333333333";
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_HTTP, http, 15000, 10) == ESP_OK);
    assert(lease.transport == AUDIO_MONITOR_HTTP);
    lease.next_seq = 5;
    uint32_t generation = lease.generation;
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_SERIAL, serial, 15000, 20) == ESP_OK);
    assert(lease.transport == AUDIO_MONITOR_SERIAL && lease.next_seq == 0 && lease.generation != generation);
    lease.next_seq = 7;
    AudioMonitor_Lease before = lease;
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_HTTP, http, 15000, 30) == ESP_OK);
    assert(!memcmp(&before, &lease, sizeof(lease)));
    assert(!AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_HTTP, NULL, 31));
    assert(!AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_SERIAL, http, 31));
    assert(!memcmp(&before, &lease, sizeof(lease)));
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_SERIAL, serial, 15000, 40) == ESP_OK);
    assert(lease.next_seq == 7 && lease.generation == before.generation);
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_SERIAL, new_serial, 1000, 50) == ESP_OK);
    assert(lease.next_seq == 0 && lease.generation != before.generation);
    assert(!AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_SERIAL, serial, 51));
    assert(AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_SERIAL, new_serial, 52));
    assert(!lease.enabled && lease.transport == AUDIO_MONITOR_OFF);
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_SERIAL, serial, 200, 100) == ESP_OK);
    assert(AudioMonitor_LeaseExpire(&lease, 300) && lease.transport == AUDIO_MONITOR_OFF);
    assert(!lease.enabled);
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_HTTP, http, 1000, 301) == ESP_OK);
    assert(lease.transport == AUDIO_MONITOR_HTTP);
}
static void test_serial_json(void)
{
    static char json[AUDIO_MONITOR_SERIAL_JSON_CAPACITY];
    static uint8_t pcm[6400];
    const char *id = "0123456789abcdef";
    const uint8_t known[] = {0x00, 0x01, 0xfe, 0xff, 0x12, 0x34};
    size_t length = AudioMonitor_FormatSerialJson(json, sizeof(json), id, 123, known, sizeof(known));
    assert(length && length == strlen(json));
    assert(!strcmp(json, "{\"session_id\":\"0123456789abcdef\",\"seq\":123,\"sample_rate\":16000,\"channels\":1,\"format\":\"s16le\",\"data\":\"AAH+/xI0\"}"));
    assert(AudioMonitor_FormatSerialJson(json, sizeof(json), id, 1, known, 2));
    assert(strstr(json, "\"data\":\"AAE=\"}"));
    assert(AudioMonitor_FormatSerialJson(json, sizeof(json), id, 1, known, 4));
    assert(strstr(json, "\"data\":\"AAH+/w==\"}"));
    for (unsigned i = 0; i < sizeof(pcm); ++i) pcm[i] = (uint8_t)i;
    length = AudioMonitor_FormatSerialJson(json, sizeof(json), id, UINT32_MAX, pcm, sizeof(pcm));
    assert(length > 8536 && length < sizeof(json) && strstr(json, "\"seq\":4294967295"));
    assert(AudioMonitor_FormatSerialJson(json, length + 1, id, UINT32_MAX, pcm, sizeof(pcm)) == length);
    assert(!AudioMonitor_FormatSerialJson(json, length, id, UINT32_MAX, pcm, sizeof(pcm)));
    assert(!AudioMonitor_FormatSerialJson(json, sizeof(json), id, 1, known, 1));
    assert(!AudioMonitor_FormatSerialJson(json, sizeof(json), id, 1, known, 0));
    assert(!AudioMonitor_FormatSerialJson(json, sizeof(json), "invalid", 1, known, 2));
    assert(!AudioMonitor_FormatSerialJson(json, sizeof(json), id, 1, pcm, 6402));
}
static void test_pull_transport_and_packets(void)
{
    AudioMonitor_Lease lease = {0};
    const char *serial = "1111111111111111", *pull = "2222222222222222", *other = "3333333333333333";
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_SERIAL, serial, 1000, 0) == ESP_OK);
    lease.next_seq = 9;
    uint32_t generation = lease.generation;
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_PULL, pull, 1000, 10) == ESP_OK);
    assert(lease.transport == AUDIO_MONITOR_PULL && lease.next_seq == 0 && lease.generation != generation);
    AudioMonitor_Lease before = lease;
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_HTTP, other, 15000, 11) == ESP_OK);
    assert(!AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_HTTP, NULL, 11));
    assert(!AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_SERIAL, serial, 11));
    assert(!AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_PULL, other, 11));
    assert(!memcmp(&lease, &before, sizeof(lease)));
    /* A current UART writer and a stale/wrong HTTP request cannot steal PCM. */
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_SERIAL, NULL, 210, 10));
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_HTTP, pull, 210, 10));
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, NULL, 210, 10));
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, other, 210, 10));
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 209, 10));
    assert(AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 210, 10));
    lease.next_seq = 5;
    generation = lease.generation;
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_PULL, pull, 1000, 300) == ESP_OK);
    assert(lease.next_seq == 5 && lease.generation == generation);
    assert(AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 1299, 1000));
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 1300, 1000));
    assert(AudioMonitor_LeaseExpire(&lease, 1300) && lease.transport == AUDIO_MONITOR_OFF);
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_PULL, pull, 1000, 1400) == ESP_OK);
    assert(AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_PULL, pull, 1401));
    assert(!lease.enabled && !AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 1600, 1400));
    /* An explicit USB selection can replace WiFi and fences its late fetch. */
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_PULL, pull, 1000, 1700) == ESP_OK);
    generation = lease.generation;
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_SERIAL, serial, 1000, 1701) == ESP_OK);
    assert(lease.generation != generation && lease.transport == AUDIO_MONITOR_SERIAL);
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 1901, 1701));
    assert(AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_SERIAL, NULL, 1901, 1701));
    assert(!AudioMonitor_ControlStop(&lease, AUDIO_MONITOR_PULL, pull, 1901));
    /* Limit and lease arithmetic also work over the millisecond wrap. */
    assert(AudioMonitor_ControlSet(&lease, AUDIO_MONITOR_PULL, pull, 1000, UINT32_MAX - 99) == ESP_OK);
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 99, UINT32_MAX - 99));
    assert(AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 100, UINT32_MAX - 99));
    assert(!AudioMonitor_CanTakePacket(&lease, AUDIO_MONITOR_PULL, pull, 900, 100));
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--encoding-fixtures")) {
        static uint8_t pcm[6400];
        static char json[AUDIO_MONITOR_SERIAL_JSON_CAPACITY];
        for (unsigned bytes = 2; bytes <= sizeof(pcm); bytes += 2) {
            for (unsigned i = 0; i < bytes; ++i) pcm[i] = (uint8_t)(i * 37 + bytes);
            assert(AudioMonitor_FormatSerialJson(json, sizeof(json), "0123456789abcdef", bytes, pcm, bytes));
            puts(json);
        }
        return 0;
    }
    test_mono_and_off();
    test_overflow_latest();
    test_age_clear_and_expiry();
    test_lease_validation_and_sequence();
    test_session_switch_and_disconnect();
    test_transport_isolation();
    test_serial_json();
    test_pull_transport_and_packets();
    puts("audio_monitor: 8 test groups passed (PCM, lease/session, transport isolation, serial JSON/base64, WiFi pull session/rate/expiry)");
    return 0;
}
