#include "WifiControlPolicy.h"
#include <string.h>
#include <stddef.h>

bool WifiControl_Deadline(uint32_t now, uint32_t deadline)
{
    int32_t left = (int32_t)(deadline - now);
    return left > 0 && left <= 15000;
}
static bool numbers(const char *p, uint32_t *values, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        uint32_t value = 0;
        if (*p < '0' || *p > '9') return false;
        do {
            unsigned digit = (unsigned)(*p++ - '0');
            if (value > (UINT32_MAX - digit) / 10U) return false;
            value = value * 10U + digit;
        } while (*p >= '0' && *p <= '9');
        values[i] = value;
        if (i + 1 < count && *p++ != ' ') return false;
    }
    return !*p;
}
bool WifiControl_PeripheralLine(const char *line)
{
    if (!line) return false;
    uint32_t v[6];
    if (!strcmp(line, "PERIPH STATUS") || !strcmp(line, "PERIPH STOP")) return true;
    if (!strncmp(line, "PERIPH STOP ", 12)) return numbers(line + 12, v, 1) && v[0];
    if (!strncmp(line, "PERIPH KEEP ", 12)) return numbers(line + 12, v, 2) && v[0];
    if (!strncmp(line, "PERIPH LIVE ", 12))
        return numbers(line + 12, v, 4) && v[0] && v[2] < 4 && v[3] >= 500 && v[3] <= 2500;
    if (!strncmp(line, "PERIPH TEST ", 12))
        return numbers(line + 12, v, 6) && v[0] && v[2] > 0 && v[2] <= 15 &&
            v[3] >= 500 && v[3] < v[4] && v[4] <= 2500 && v[5] <= 10000;
    return false;
}
