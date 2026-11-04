#include "AHT20.h"
#include "BSP_Battery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t frame[7];
static unsigned writes, reads;
static int io_result;
int BSP_Battery_Aht20Transfer(const uint8_t *tx, uint8_t nt, uint8_t *rx, uint8_t nr)
{
    if (nt) {
        assert(nt == 3U && !nr && tx[0] == 0xacU && tx[1] == 0x33U && tx[2] == 0U);
        ++writes;
    } else { assert(nr == 7U); ++reads; if (!io_result) { memcpy(rx, frame, 7U); } }
    return io_result;
}
static void sample(uint8_t flags, uint32_t humidity, uint32_t temperature)
{
    frame[0] = flags;
    frame[1] = (uint8_t)(humidity >> 12);
    frame[2] = (uint8_t)(humidity >> 4);
    frame[3] = (uint8_t)((humidity << 4) | (temperature >> 16));
    frame[4] = (uint8_t)(temperature >> 8);
    frame[5] = (uint8_t)temperature;
    frame[6] = AHT20_Crc8(frame, 6U);
}
static AHT20_Status state(uint32_t at)
{
    AHT20_Status s;
    AHT20_GetStatus(at, &s);
    return s;
}
static void reset(uint32_t now)
{
    writes = reads = 0U;
    io_result = 0;
    sample(0x18U, 524288U, 393216U); /* 50% RH, 25 C */
    AHT20_Init(now);
}
static void test_crc_decode(void)
{
    int32_t temp = 888;
    uint32_t humidity = 999U;
    static const uint8_t check[] = "123456789";
    assert(AHT20_Crc8(check, 9U) == 0xf7U); /* CRC-8/NRSC-5 polynomial/init */
    reset(0U);
    assert(!AHT20_Decode(frame, &temp, &humidity) && temp == 2500 && humidity == 5000U);
    sample(0x18U, 0U, 0U);
    assert(!AHT20_Decode(frame, &temp, &humidity) && temp == -5000 && humidity == 0U);
    sample(0x18U, 1048575U, 1048575U);
    assert(!AHT20_Decode(frame, &temp, &humidity) && temp == 15000 && humidity == 10000U);
    frame[6] ^= 1U;
    assert(AHT20_Decode(frame, &temp, &humidity) == -1 && temp == 15000);
    sample(0x98U, 0U, 0U);
    assert(AHT20_Decode(frame, &temp, &humidity) == 1);
    sample(0x10U, 0U, 0U);
    assert(AHT20_Decode(frame, &temp, &humidity) == -2);
    assert(AHT20_Decode(NULL, &temp, &humidity) == -3);
}
static void test_timing_standby_retry_and_stale(void)
{
    reset(0U);
    AHT20_Task(0U, 0U);
    AHT20_Task(1000U, 0U);
    assert(!writes && !reads && state(1000U).state == AHT20_STANDBY);
    AHT20_Task(1000U, 1U);
    AHT20_Task(1099U, 1U);
    assert(!writes);
    AHT20_Task(1100U, 1U);
    AHT20_Task(1179U, 1U);
    assert(writes == 1U && !reads);
    AHT20_Task(1180U, 1U);
    assert(state(1180U).valid && state(1180U).temperature_cc == 2500);
    assert(!state(6200U).valid && state(6200U).has_sample);
    io_result = -3;
    AHT20_Task(3100U, 1U);
    assert(state(3100U).state == AHT20_BUS_ERROR && state(3100U).errors == 1U);
    assert(!state(3100U).valid && state(3100U).temperature_cc == 2500);
    AHT20_Task(5099U, 1U);
    assert(writes == 2U);
    io_result = 0;
    AHT20_Task(5100U, 1U);
    AHT20_Task(5150U, 0U); /* Sleep cancels an in-flight conversion. */
    AHT20_Task(5180U, 0U);
    assert(reads == 1U && state(5180U).state == AHT20_STANDBY);
}
static void test_busy_crc_calibration_wrap(void)
{
    uint32_t i;
    reset(0U);
    AHT20_Task(0U, 1U); AHT20_Task(100U, 1U);
    sample(0x98U, 524288U, 393216U);
    for (i = 180U; i <= 300U; i += 10U) { AHT20_Task(i, 1U); }
    assert(state(300U).state == AHT20_BUSY_TIMEOUT && state(300U).errors == 1U);
    AHT20_Task(2300U, 1U);
    sample(0x18U, 524288U, 393216U); frame[6] ^= 1U;
    AHT20_Task(2380U, 1U);
    assert(state(2380U).state == AHT20_CRC_ERROR);
    AHT20_Task(4380U, 1U); sample(0U, 0U, 0U); AHT20_Task(4460U, 1U);
    assert(state(4460U).state == AHT20_UNCALIBRATED);
    reset(0xffffffc0UL);
    AHT20_Task(0xffffffc0UL, 1U);
    AHT20_Task(35U, 1U); assert(!writes);
    AHT20_Task(36U, 1U); assert(writes == 1U);
    AHT20_Task(116U, 1U); assert(state(116U).valid);
}
int main(void)
{
    test_crc_decode();
    test_timing_standby_retry_and_stale();
    test_busy_crc_calibration_wrap();
    puts("AHT20 tests passed: CRC, decode, busy/timeout, calibration, standby, retry, stale and tick wrap.");
    return 0;
}
