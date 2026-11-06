#ifndef TELEMETRY_LINE_H
#define TELEMETRY_LINE_H

#include <stddef.h>
#include <stdint.h>

#define TELEMETRY_LINE_MAX 1024U

typedef struct {
    uint8_t data[TELEMETRY_LINE_MAX];
    size_t length;
    uint32_t oversized_lines;
    uint8_t discarding;
} TelemetryLine;

typedef void (*TelemetryLine_Emit)(void *context, const uint8_t *data, size_t length);

/* Discard through LF after a connection boundary: never send half a line. */
void TelemetryLine_Reset(TelemetryLine *line, uint8_t discard_until_lf);
/* One callback per nonempty complete line; trailing CR/CRCR becomes CRLF. */
void TelemetryLine_Feed(TelemetryLine *line, const uint8_t *data, size_t length,
                        TelemetryLine_Emit emit, void *context);

#endif
