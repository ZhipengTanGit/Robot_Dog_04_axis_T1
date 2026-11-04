/* Reuse the integration fixture and run its existing regression suite too. */
#define main existing_peripheral_test_main
#include "test_peripherals.c"
#undef main

static unsigned occurrences(const char *text, const char *needle)
{
    unsigned count = 0U;
    while ((text = strstr(text, needle)) != NULL) { ++count; text += strlen(needle); }
    return count;
}

static void test_identified_completion(void)
{
    reset();
    feed("PERIPH BEEP 800 0123456789abcdef\n");
    assert(buzzer && state().cmd_ok == 1U);
    assert(strstr(ack_output, "PERIPH_ACK cmd=BEEP id=0123456789abcdef ok=1 reason=STARTED\r\n"));
    tick(799U); assert(buzzer && !strstr(ack_output, "reason=DONE"));
    tick(800U); assert(!buzzer && state().cmd_ok == 1U);
    assert(strstr(ack_output, "PERIPH_ACK cmd=BEEP id=0123456789abcdef ok=1 reason=DONE\r\n"));
    tick(801U); assert(occurrences(ack_output, "reason=DONE") == 1U);
    Peripherals_PrintStatus(); assert(strstr(ack_output, " buzzer_proto=1 "));
}

static void test_cancel_and_buzzer_only_stop(void)
{
    reset(); feed("PERIPH SERVO 0 1500\nPERIPH BEEP 1000 0123456789abcdef\n");
    tick(10U); feed("PERIPH BEEP 300 fedcba9876543210\n");
    assert(strstr(ack_output, "id=0123456789abcdef ok=0 reason=CANCELLED"));
    assert(buzzer && pulses[0] == 1500U);
    feed("PERIPH BEEP 0 1111111111111111\n");
    assert(!buzzer && pulses[0] == 1500U && state().servo_mask == 1U);
    assert(strstr(ack_output, "id=fedcba9876543210 ok=0 reason=CANCELLED"));
    assert(strstr(ack_output, "id=1111111111111111 ok=1 reason=STOPPED"));
    tick(1000U); assert(!strstr(ack_output, "reason=DONE"));
    feed("PERIPH BEEP 500 2222222222222222\nPERIPH STOP\n");
    assert(!buzzer && !pulses[0]);
    assert(strstr(ack_output, "id=2222222222222222 ok=0 reason=CANCELLED"));
    tick(1501U); assert(!strstr(ack_output, "reason=DONE"));
}

static void test_legacy_and_rejection(void)
{
    unsigned before;
    reset();
    feed("PERIPH BEEP 80 0123456789abcdef\nPERIPH BEEP 50\n");
    assert(strstr(ack_output, "id=0123456789abcdef ok=0 reason=CANCELLED"));
    assert(strstr(ack_output, "PERIPH_ACK cmd=BEEP ok=1 reason=OK\r\n"));
    tick(50U); assert(!buzzer && !strstr(ack_output, "reason=DONE"));
    before = writes;
    feed("PERIPH BEEP 1001 0123456789abcdef\nPERIPH BEEP -1 0123456789abcdef\n"
         "PERIPH BEEP 50 ABCDEF0123456789\nPERIPH BEEP 50 0123456789abcde\n"
         "PERIPH BEEP 50 0123456789abcdef0\nPERIPH BEEP\n"
         "PERIPH BEEP 50 0123456789abcdef extra\n");
    assert(writes == before && state().cmd_err == 7U);
    assert(occurrences(ack_output, "id=0123456789abcdef ok=0 reason=SYNTAX") == 2U);
    assert(!strstr(ack_output, "ABCDEF0123456789"));
}

static void test_wrap_and_interlock_cancel(void)
{
    reset(); tick(UINT32_MAX - 40U);
    feed("PERIPH BEEP 80 0123456789abcdef\n");
    tick(38U); assert(buzzer);
    tick(39U); assert(!buzzer && strstr(ack_output, "reason=DONE"));
    reset(); feed("PERIPH SERVO 0 1500\nPERIPH BEEP 900 0123456789abcdef\n");
    battery.faults |= BATTERY_FAULT_BUS; tick(1U);
    assert(!buzzer && !pulses[0]);
    assert(strstr(ack_output, "id=0123456789abcdef ok=0 reason=CANCELLED"));
    tick(1000U); assert(!strstr(ack_output, "reason=DONE"));
}

int main(void)
{
    assert(existing_peripheral_test_main() == 0);
    test_identified_completion(); test_cancel_and_buzzer_only_stop();
    test_legacy_and_rejection(); test_wrap_and_interlock_cancel();
    puts("Buzzer command tests passed: matched STARTED/DONE/STOPPED IDs, cancellation, no servo change on beep-stop, malformed input, legacy compatibility and millis wrap.");
    return 0;
}
