#include "Peripherals.h"
#include "BSP_Peripherals.h"
#include "BSP_SysTick.h"
#include "BSP_UART_3.h"
#include "Battery.h"
#include "Battery_Config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static uint32_t now, dropped, errors;
static uint8_t raw_key, raw_touch, led[2], buzzer;
static uint16_t pulses[4];
static Battery_Status battery;
static char rx[4096];
static unsigned rx_read, rx_write, writes, flushes;
static unsigned current_requests;
static char ack_output[32768];
static unsigned ack_length;
int test_printf(const char *format, ...)
{
    va_list args;
    int count;
    va_start(args, format);
    count = vsnprintf(ack_output + ack_length, sizeof(ack_output) - ack_length, format, args);
    va_end(args);
    assert(count >= 0 && ack_length + (unsigned)count < sizeof(ack_output));
    ack_length += (unsigned)count;
    return count;
}
uint32_t BSP_Millis(void) { return now; }
void Battery_GetStatus(Battery_Status *out) { *out = battery; }
int Battery_RequestNominalCurrent(uint16_t target)
{
    uint8_t code;
    ++current_requests;
    if (IP5310_VbusCodeFromNominal(target, &code) != IP5310_OK) { return BATTERY_CURRENT_UNSUPPORTED; }
    if (target > BATTERY_BOARD_MAX_NOMINAL_MA) { return BATTERY_CURRENT_BOARD_LIMIT; }
    if (battery.link != BATTERY_LINK_READY) { return BATTERY_CURRENT_NOT_READY; }
    battery.requested_nominal_ma = target;
    battery.current_request_state = BATTERY_CURRENT_PENDING;
    return BATTERY_CURRENT_ACCEPTED;
}
void BSP_Peripherals_Init(void) { }
uint8_t BSP_Peripherals_Key(void) { return raw_key; }
uint8_t BSP_Peripherals_Touch(void) { return raw_touch; }
void BSP_Peripherals_Led(uint8_t i, uint8_t on) { assert(i < 2U); led[i] = on; ++writes; }
void BSP_Peripherals_Buzzer(uint8_t on) { buzzer = on; ++writes; }
void BSP_Peripherals_Servo(uint8_t ch, uint16_t us) { assert(ch < 4U); pulses[ch] = us; ++writes; }
uint32_t UART_3_Get_Rx_Dropped(void) { return dropped; }
uint32_t UART_3_Get_Rx_Errors(void) { return errors; }
void UART_3_FlushRx(void) { rx_read = rx_write; ++flushes; }
uint16_t UART_3_Read(uint8_t *data, uint16_t capacity)
{
    assert(capacity == 1U);
    if (rx_read == rx_write) { return 0U; }
    *data = (uint8_t)rx[rx_read++];
    return 1U;
}
int BSP_Battery_Aht20Transfer(const uint8_t *tx, uint8_t nt, uint8_t *r, uint8_t nr)
{ (void)tx; (void)nt; (void)r; (void)nr; return -3; }
static void tick(uint32_t t) { now = t; Peripherals_Task(); }
static void feed(const char *text)
{
    size_t n = strlen(text);
    assert(rx_write + n < sizeof(rx));
    memcpy(rx + rx_write, text, n);
    rx_write += (unsigned)n;
    while (rx_read != rx_write) { Peripherals_Task(); }
}
static Peripherals_Status state(void)
{
    Peripherals_Status s;
    Peripherals_GetStatus(&s);
    return s;
}
static void reset(void)
{
    now = dropped = errors = 0U;
    raw_key = raw_touch = buzzer = 0U;
    memset(pulses, 0, sizeof(pulses)); memset(led, 0, sizeof(led));
    memset(&battery, 0, sizeof(battery));
    battery.link = BATTERY_LINK_READY;
    battery.snapshot_valid = battery.awake = 1U;
    battery.motion_request = BATTERY_MOTION_NORMAL;
    battery.faults = BATTERY_FAULT_TEMP_UNCONNECTED;
    rx_read = rx_write = writes = flushes = 0U;
    current_requests = ack_length = 0U; ack_output[0] = 0;
    Peripherals_Init(); Peripherals_Task();
}
static void test_debounce(void)
{
    reset(); raw_key = 1U; tick(1U); raw_key = 0U; tick(10U); raw_key = 1U; tick(15U);
    tick(34U); assert(!state().key && !state().key_press);
    tick(35U); assert(state().key && state().key_press == 1U);
    tick(100U); assert(state().key_press == 1U);
    raw_key = 0U; tick(101U); tick(121U); assert(!state().key && state().key_release == 1U);
    raw_touch = 1U; tick(299U); assert(!state().touch);
    tick(300U); tick(319U); assert(!state().touch); tick(320U); assert(state().touch_press == 1U);
    raw_touch = 0U; tick(350U); tick(370U); assert(state().touch_release == 1U);
}
static void test_commands_and_safety(void)
{
    unsigned before;
    reset(); assert(!state().servo_mask && !buzzer);
    feed("PERIPH LED 0 1\r\nPERIPH LED 1 1\n"); assert(led[0] && led[1]);
    feed("PERIPH BEEP 80\n"); assert(buzzer); tick(79U); assert(buzzer); tick(80U); assert(!buzzer);
    feed("PERIPH SERVO 0 1500\nPERIPH SERVO 3 2000\n");
    assert(pulses[0] == 1500U && pulses[3] == 2000U && state().servo_mask == 9U);
    before = writes;
    feed("PERIPH SERVO 0 499\nPERIPH SERVO 0 2501\nPERIPH SERVO 4 1500\nPERIPH SERVO 0 -1\nPERIPH LED 2 0\nPERIPH LED 0 2\nPERIPH BEEP 1001\nPERIPH BEEP 999999999999\nPERIPH STOP extra\nPERIPH LED 0 0 extra\nUNKNOWN\n");
    assert(writes == before && state().cmd_err == 11U && pulses[0] == 1500U);
    battery.faults |= BATTERY_FAULT_BUS; tick(81U);
    assert(!state().servo_mask && !pulses[0] && !pulses[3]);
    feed("PERIPH SERVO 1 1500\n"); assert(!pulses[1]);
    battery.faults = BATTERY_FAULT_TEMP_UNCONNECTED; tick(82U);
    assert(!state().servo_mask); /* Recovery never restores a previous pulse. */
    feed("PERIPH SERVO 1 1000\n"); assert(pulses[1] == 1000U);
    battery.motion_request = BATTERY_MOTION_STOP_REQUEST; tick(83U); assert(!pulses[1]);
    battery.motion_request = BATTERY_MOTION_LIMIT_REQUEST; tick(84U);
    feed("PERIPH SERVO 2 1250\nPERIPH STOP\n"); assert(!pulses[2] && !state().servo_mask);
    feed("PERIPH STATUS\n");
}
static void test_servo_pulse_endpoints(void)
{
    unsigned i, before;
    char line[64];
    reset();
    for (i = 0U; i < 4U; ++i) {
        sprintf(line, "PERIPH SERVO %u 500\n", i);
        feed(line);
        assert(pulses[i] == 500U && state().servo_us[i] == 500U);
        sprintf(line, "PERIPH SERVO %u 2500\n", i);
        feed(line);
        assert(pulses[i] == 2500U && state().servo_us[i] == 2500U);
        before = writes;
        sprintf(line, "PERIPH SERVO %u 499\nPERIPH SERVO %u 2501\n", i, i);
        feed(line);
        assert(writes == before && pulses[i] == 2500U && state().servo_us[i] == 2500U);
    }
    assert(state().servo_mask == 15U && state().cmd_ok == 8U && state().cmd_err == 8U);
    feed("PERIPH STOP\n");
    assert(!state().servo_mask);
    for (i = 0U; i < 4U; ++i) { assert(!pulses[i] && !state().servo_us[i]); }
}
static void test_overflow_and_fragmentation(void)
{
    unsigned before;
    char long_line[160];
    reset();
    feed("PERIPH SERVO 0 "); assert(!pulses[0]); feed("1500\n"); assert(pulses[0] == 1500U);
    before = writes;
    memset(long_line, ' ', sizeof(long_line));
    memcpy(long_line, "PERIPH LED 0 1", 14U);
    long_line[sizeof(long_line)-2U] = '\n'; long_line[sizeof(long_line)-1U] = 0;
    feed(long_line); assert(writes == before && !led[0] && state().cmd_err == 1U);
    feed("PERIPH LED 0 1\n"); assert(led[0]);
    feed("PERIPH SERVO 1 1"); ++dropped;
    feed("500\nPERIPH LED 1 1\n"); assert(flushes == 1U && !pulses[1] && !led[1]);
    feed("500\nPERIPH LED 1 1\n"); assert(!pulses[1] && led[1]);
    feed("PERIPH LED 1 0"); ++errors; tick(1U);
    feed("\nPERIPH LED 1 0\n"); assert(!led[1]);
    before = writes;
    feed("PERIPH LED 0 \x01" "0\n"); assert(writes == before && led[0]);
}

static void test_live_lease_and_freshness(void)
{
    unsigned before;
    reset();
    feed("PERIPH LIVE 1 0 0 500\n");
    assert(pulses[0] == 500U && state().last_id == 1U && state().servo_mode == SERVO_MODE_LIVE);
    assert(strstr(ack_output, "cmd=LIVE id=1 ok=1 reason=OK"));
    tick(1000U);
    feed("PERIPH LIVE 2 1000 3 2500\n");
    assert(pulses[0] == 500U && pulses[3] == 2500U && state().servo_mask == 9U);
    before = writes;
    feed("PERIPH LIVE 2 1000 0 1500\nPERIPH LIVE 1 1000 0 1500\n");
    assert(writes == before && strstr(ack_output, "reason=OLD_ID"));
    tick(2400U);
    feed("PERIPH KEEP 3 2400\n");
    assert(state().last_id == 3U && state().live_left_ms == 1500U);
    tick(3899U); assert(pulses[0] && pulses[3] && state().live_left_ms == 1U);
    /* Replayed KEEP is not a heartbeat and cannot extend the lease. */
    feed("PERIPH KEEP 3 2400\n");
    tick(3900U); assert(!state().servo_mask && state().servo_mode == SERVO_MODE_IDLE);
    feed("PERIPH KEEP 4 3900\n");
    assert(!state().servo_mask && state().last_id == 4U && strstr(ack_output, "reason=NOT_LIVE"));
    feed("PERIPH LIVE 5 1399 0 1500\nPERIPH LIVE 5 3901 0 1500\n");
    assert(!state().servo_mask && state().last_id == 4U && strstr(ack_output, "reason=STALE"));
    /* Timestamp age and the output lease have separate limits. */
    feed("PERIPH LIVE 5 1400 0 1500\nPERIPH STOP 4\n");
    assert(!state().servo_mask && state().last_id == 5U);
    assert(strstr(ack_output, "cmd=STOP id=4 ok=1 reason=OK"));
    feed("PERIPH STOP 20\nPERIPH LIVE 6 3900 0 1500\n");
    assert(state().last_id == 20U && !state().servo_mask);
    feed("PERIPH SERVO 0 1500\n"); tick(5399U); assert(pulses[0]);
    tick(5400U); assert(!pulses[0]); /* Legacy manual commands also expire. */
}

static void test_autonomous_motion_and_stop(void)
{
    unsigned before;
    reset();
    feed("PERIPH LIVE 1 0 1 1500\nPERIPH TEST 2 0 5 1300 1700 5000\n");
    assert(!state().servo_mask && state().test_state == SERVO_TEST_WAIT && state().test_id == 2U);
    assert(state().test_left_ms == 10000U && state().test_delay_ms == 5000U);
    before = writes;
    feed("PERIPH LIVE 3 0 0 1800\nPERIPH KEEP 4 0\nPERIPH SERVO 0 1500\nPERIPH TEST 5 0 1 500 2500 0\n");
    assert(writes == before && state().test_id == 2U && state().last_id == 5U);
    tick(4999U); assert(!state().servo_mask && state().test_delay_ms == 1U);
    tick(5000U); assert(state().test_state == SERVO_TEST_RUN && state().servo_mask == 5U);
    assert(pulses[0] == 1300U && pulses[2] == 1300U && !pulses[1] && !pulses[3]);
    tick(5500U); assert(pulses[0] == 1585U && state().test_left_ms == 9500U);
    tick(6000U); assert(pulses[0] == 1700U);
    tick(6500U); assert(pulses[0] == 1415U);
    tick(7000U); assert(pulses[0] == 1300U);
    /* No RX/KEEP for the whole test; local timing continues independently. */
    tick(14999U); assert(state().servo_mask == 5U && state().test_left_ms == 1U && pulses[0] == 1300U);
    tick(15000U); assert(!state().servo_mask && state().test_state == SERVO_TEST_DONE);
    assert(state().test_result == SERVO_RESULT_OK && state().test_id == 2U && !state().test_left_ms);
    feed("PERIPH STATUS\n");
    assert(strstr(ack_output, "servo_proto=2 servo_mode=IDLE last_id=5") &&
        strstr(ack_output, "test_id=2 test_state=DONE test_result=OK"));
    assert(strstr(ack_output, "test_ramp_ms=700 test_hold_ms=300"));
    feed("PERIPH TEST 6 15000 15 500 2500 0\n");
    assert(state().servo_mask == 15U && pulses[3] == 500U);
    tick(16000U); assert(pulses[3] == 2500U);
    feed("PERIPH STOP 7\n");
    assert(!state().servo_mask && state().test_state == SERVO_TEST_STOPPED &&
        state().test_result == SERVO_RESULT_STOP && state().test_id == 6U);
    tick(25000U); assert(!state().servo_mask && state().test_state == SERVO_TEST_STOPPED);
    feed("PERIPH TEST 8 25000 1 1300 1700 5000\nPERIPH STOP\n");
    assert(!state().servo_mask && state().test_state == SERVO_TEST_STOPPED);
    tick(40000U); assert(!state().servo_mask);
}

static void test_motion_endpoint_holds(void)
{
    unsigned channel, before;
    const char *line, *end;
    reset(); tick(123U);
    feed("PERIPH TEST 1 123 15 500 2500 0\n");
    /* Deliberately avoid exact 700/1700 ms boundaries. Every selected PWM
     * reaches both configured endpoints and holds them across many frames. */
    tick(456U);
    for (channel = 0; channel < 4; ++channel) { assert(pulses[channel] == 1451U); }
    tick(859U); /* elapsed 736 ms, upper hold */
    for (channel = 0; channel < 4; ++channel) { assert(pulses[channel] == 2500U); }
    before = writes;
    tick(1007U); tick(1118U);
    assert(writes == before); /* Retain 2500 us, without new target writes. */
    tick(1456U); /* elapsed 1333 ms, return ramp */
    for (channel = 0; channel < 4; ++channel) { assert(pulses[channel] == 1549U); }
    tick(1857U); /* elapsed 1734 ms, lower hold */
    for (channel = 0; channel < 4; ++channel) { assert(pulses[channel] == 500U); }
    before = writes;
    tick(2011U); tick(2119U);
    assert(writes == before);
    tick(2859U); assert(pulses[0] == 2500U); /* Next complete round trip. */
    tick(8859U); assert(pulses[3] == 2500U); /* Last upper hold. */
    tick(9456U); assert(pulses[3] == 1549U); /* Final return. */
    tick(9857U); assert(pulses[3] == 500U && state().test_left_ms == 266U);
    before = writes;
    tick(10031U); tick(10122U);
    assert(writes == before && pulses[3] == 500U && state().test_left_ms == 1U);
    tick(10123U);
    assert(!state().servo_mask && state().test_state == SERVO_TEST_DONE);
    for (channel = 0; channel < 4; ++channel) { assert(!pulses[channel]); }
    feed("PERIPH STATUS\n");
    line = strstr(ack_output, "PERIPH t=");
    assert(line && (end = strstr(line, "\r\n")) != NULL && (size_t)(end + 2 - line) <= 1024U);
}

static void test_motion_interlock_and_arguments(void)
{
    unsigned before;
    reset();
    feed("PERIPH TEST 1 0 1 1300 1700 5000\n");
    battery.faults |= BATTERY_FAULT_BUS;
    tick(1U);
    assert(state().test_state == SERVO_TEST_FAILED && state().test_result == SERVO_RESULT_BATTERY);
    battery.faults = BATTERY_FAULT_TEMP_UNCONNECTED;
    tick(6000U); assert(!state().servo_mask && state().test_state == SERVO_TEST_FAILED);
    feed("PERIPH TEST 2 6000 1 1300 1700 0\n"); assert(pulses[0] == 1300U);
    battery.motion_request = BATTERY_MOTION_STOP_REQUEST;
    tick(6001U); assert(!state().servo_mask && state().test_state == SERVO_TEST_FAILED);
    feed("PERIPH LIVE 3 6001 0 1500\n"); assert(!state().servo_mask && state().last_id == 3U);
    battery.motion_request = BATTERY_MOTION_NORMAL;
    tick(6002U);
    feed("PERIPH LIVE 3 6001 0 1500\n"); assert(!state().servo_mask);
    before = writes;
    feed("PERIPH LIVE 4 6002 0 499\nPERIPH LIVE 4 6002 0 2501\nPERIPH LIVE 4 6002 4 1500\n"
        "PERIPH TEST 4 6002 0 1300 1700 0\nPERIPH TEST 4 6002 16 1300 1700 0\n"
        "PERIPH TEST 4 6002 1 499 1700 0\nPERIPH TEST 4 6002 1 1300 2501 0\n"
        "PERIPH TEST 4 6002 1 1700 1300 0\nPERIPH TEST 4 6002 1 1500 1500 0\n"
        "PERIPH TEST 4 6002 1 1300 1700 10001\nPERIPH TEST 4 6002 1 1300 1700\n"
        "PERIPH TEST 4 6002 1 1300 1700 0 extra\nPERIPH KEEP 4 6002 extra\n"
        "PERIPH LIVE 4294967296 6002 0 1500\nPERIPH LIVE 4 4294967296 0 1500\n");
    assert(writes == before && state().last_id == 3U && !state().servo_mask);
}

static void test_motion_millis_wrap(void)
{
    reset(); tick(0xFFFFFF00UL);
    feed("PERIPH LIVE 1 4294967040 0 1500\n");
    tick(0x000004DBUL); assert(pulses[0] && state().live_left_ms == 1U);
    tick(0x000004DCUL); assert(!pulses[0]);
    reset(); tick(0xFFFFFF00UL);
    feed("PERIPH TEST 1 4294967040 1 1300 1700 0\n");
    tick(0x000002E8UL); assert(pulses[0] == 1700U);
    tick(0x00002610UL); assert(!pulses[0] && state().test_state == SERVO_TEST_DONE);
    reset(); feed("PERIPH STOP 4294967295\nPERIPH LIVE 0 0 0 1500\n");
    assert(state().last_id == 0xFFFFFFFFUL && !state().servo_mask);
    reset(); tick(0xFFFFFFF0UL);
    feed("PERIPH LIVE 1 10 0 1500\n"); /* Future timestamp across wrap. */
    assert(!state().servo_mask && state().last_id == 0U);
    tick(0x10UL);
    feed("PERIPH LIVE 1 4294967280 0 1500\n"); /* 32 ms old across wrap. */
    assert(pulses[0] == 1500U && state().live_left_ms == 1500U);
}
static void test_current_command_protocol(void)
{
    unsigned before;
    char oversized[128];
    reset();
    battery.applied_nominal_ma = 350U;
    battery.control_verified = battery.charge_config_valid = 1U;
    feed("BAT CURRENT 250 web_1-a\n");
    assert(current_requests == 1U);
    assert(strstr(ack_output, "id=web_1-a target_mA=250 status=PENDING reason=QUEUED") != NULL);
    assert(strstr(ack_output, "status=APPLIED") == NULL);
    feed("BAT CURRENT 250 web_1-a\nBAT CURRENT 350 web_1-a\nBAT CURRENT 250 busy_2\n");
    assert(current_requests == 1U && strstr(ack_output, "reason=ID_CONFLICT") && strstr(ack_output, "reason=BUSY"));
    battery.current_request_state = BATTERY_CURRENT_APPLIED;
    battery.applied_nominal_ma = 250U;
    tick(250U);
    assert(strstr(ack_output, "id=web_1-a target_mA=250 status=APPLIED reason=OK applied_mA=250"));
    feed("BAT CURRENT 250 web_1-a\n"); assert(current_requests == 1U);
    feed("BAT CURRENT 1000 bad_tier\nBAT CURRENT 1000 bad_tier\n");
    assert(current_requests == 2U && strstr(ack_output, "status=REJECTED reason=UNSUPPORTED"));
    feed("BAT CURRENT 350 failure_3\n"); assert(current_requests == 3U);
    battery.current_request_state = BATTERY_CURRENT_FAILED;
    battery.current_request_result = IP5310_ERROR_VERIFY;
    battery.control_verified = 0U;
    tick(500U);
    assert(strstr(ack_output, "id=failure_3 target_mA=350 status=FAILED reason=VERIFY applied_mA=0"));
    feed("BAT CURRENT 350 failure_3\n"); assert(current_requests == 3U);
    feed("BAT CURRENT 3050 too_high\nBAT CURRENT 3050 too_high\n");
    assert(current_requests == 4U && strstr(ack_output, "id=too_high target_mA=3050 status=REJECTED reason=BOARD_LIMIT"));
    before = current_requests;
    feed("BAT CURRENT 250 a!b\nBAT CURRENT 250 abcdefghijklmnopqrstuvwxyz1234567\nBAT CURRENT -250 signed\nBAT CURRENT 250 too many\n");
    assert(current_requests == before);
    memset(oversized, 'x', sizeof(oversized));
    memcpy(oversized, "BAT CURRENT 250 ", 16U);
    oversized[126] = '\n'; oversized[127] = 0;
    feed(oversized); assert(current_requests == before);
    feed("BAT CURRENT 250 partial"); ++dropped; tick(501U);
    feed("\n"); assert(current_requests == before);
}

int main(void)
{
    test_debounce(); test_commands_and_safety(); test_servo_pulse_endpoints(); test_overflow_and_fragmentation();
    test_live_lease_and_freshness(); test_autonomous_motion_and_stop();
    test_motion_endpoint_holds();
    test_motion_interlock_and_arguments(); test_motion_millis_wrap();
    test_current_command_protocol();
    puts("Peripheral tests passed: pulse endpoints, live lease/fresh IDs, 700 ms ramp/300 ms endpoint holds, local 10-second stop, STOP/interlock, millis wrap, RX safety and battery current protocol.");
    return 0;
}
