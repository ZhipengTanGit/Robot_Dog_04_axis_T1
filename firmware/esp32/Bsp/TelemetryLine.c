#include "TelemetryLine.h"

void TelemetryLine_Reset(TelemetryLine *line, uint8_t discard_until_lf)
{
    line->length = 0U;
    line->discarding = discard_until_lf;
}

void TelemetryLine_Feed(TelemetryLine *line, const uint8_t *data, size_t length,
                        TelemetryLine_Emit emit, void *context)
{
    for (size_t i = 0U; i < length; ++i) {
        uint8_t value = data[i];
        if (line->discarding) {
            if (value == '\n') { line->discarding = 0U; }
            continue;
        }
        if (value == '\n') {
            while (line->length != 0U && line->data[line->length - 1U] == '\r') {
                --line->length;
            }
            if (line->length > TELEMETRY_LINE_MAX - 2U) {
                ++line->oversized_lines;
            } else if (line->length != 0U) {
                line->data[line->length++] = '\r';
                line->data[line->length++] = '\n';
                emit(context, line->data, line->length);
            }
            line->length = 0U;
        } else if (line->length == TELEMETRY_LINE_MAX) {
            ++line->oversized_lines;
            line->length = 0U;
            line->discarding = 1U;
        } else {
            line->data[line->length++] = value;
        }
    }
}
