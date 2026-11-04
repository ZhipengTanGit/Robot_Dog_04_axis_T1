#include "TelemetryLine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned emitted;
static uint8_t saved[8][TELEMETRY_LINE_MAX];
static size_t sizes[8];

static void capture(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    assert(emitted < 8U && length <= TELEMETRY_LINE_MAX);
    memcpy(saved[emitted], data, length);
    sizes[emitted++] = length;
}

static void test_arbitrary_splits(void)
{
    static const char input[] = "BAT a=1\r\r\nBAT b=2\n\r\nBAT c=3\r\n";
    for (size_t split = 0U; split < sizeof(input); ++split) {
        TelemetryLine line = { { 0 }, 0U, 0U, 0U };
        emitted = 0U;
        TelemetryLine_Feed(&line, (const uint8_t *)input, split, capture, NULL);
        TelemetryLine_Feed(&line, (const uint8_t *)input + split,
                           sizeof(input) - 1U - split, capture, NULL);
        assert(emitted == 3U);
        assert(sizes[0] == 9U && memcmp(saved[0], "BAT a=1\r\n", 9U) == 0);
        assert(sizes[1] == 9U && memcmp(saved[1], "BAT b=2\r\n", 9U) == 0);
        assert(sizes[2] == 9U && memcmp(saved[2], "BAT c=3\r\n", 9U) == 0);
    }
}

static void test_reconnect_discards_partial(void)
{
    TelemetryLine line = { { 0 }, 0U, 0U, 0U };
    emitted = 0U;
    TelemetryLine_Feed(&line, (const uint8_t *)"old", 3, capture, NULL);
    TelemetryLine_Reset(&line, 1U);
    TelemetryLine_Feed(&line, (const uint8_t *)"tail\r\nnew\r\n", 11, capture, NULL);
    assert(emitted == 1U && sizes[0] == 5U && memcmp(saved[0], "new\r\n", 5) == 0);
}

static void test_long_lines_and_recovery(void)
{
    TelemetryLine line = { { 0 }, 0U, 0U, 0U };
    uint8_t data[TELEMETRY_LINE_MAX + 10U];
    memset(data, 'x', sizeof(data));
    emitted = 0U;
    TelemetryLine_Feed(&line, data, TELEMETRY_LINE_MAX - 2U, capture, NULL);
    TelemetryLine_Feed(&line, (const uint8_t *)"\r\r\n", 3, capture, NULL);
    assert(emitted == 1U && sizes[0] == TELEMETRY_LINE_MAX);
    assert(saved[0][TELEMETRY_LINE_MAX - 2U] == '\r');
    assert(saved[0][TELEMETRY_LINE_MAX - 1U] == '\n');
    TelemetryLine_Feed(&line, data, TELEMETRY_LINE_MAX - 1U, capture, NULL);
    TelemetryLine_Feed(&line, (const uint8_t *)"\n", 1, capture, NULL);
    assert(emitted == 1U && line.oversized_lines == 1U);
    TelemetryLine_Feed(&line, data, sizeof(data), capture, NULL);
    TelemetryLine_Feed(&line, (const uint8_t *)"tail\nOK\n", 8, capture, NULL);
    assert(emitted == 2U && line.oversized_lines == 2U);
    assert(sizes[1] == 4U && memcmp(saved[1], "OK\r\n", 4) == 0);
}

static void test_embedded_bytes(void)
{
    TelemetryLine line = { { 0 }, 0U, 0U, 0U };
    static const uint8_t input[] = { 'a', 0U, 'b', '\r', 'c', '\n' };
    static const uint8_t expected[] = { 'a', 0U, 'b', '\r', 'c', '\r', '\n' };
    emitted = 0U;
    for (size_t i = 0; i < sizeof(input); ++i) {
        TelemetryLine_Feed(&line, &input[i], 1U, capture, NULL);
    }
    assert(emitted == 1U && sizes[0] == sizeof(expected));
    assert(memcmp(saved[0], expected, sizeof(expected)) == 0);
}

int main(void)
{
    test_arbitrary_splits();
    test_reconnect_discards_partial();
    test_long_lines_and_recovery();
    test_embedded_bytes();
    puts("Telemetry line tests passed");
    return 0;
}
