#include "PeripheralMath.h"
#include <math.h>

void PcmMetrics_AddStereoLeft(PcmMetrics *m, const int16_t *data, size_t frames)
{
    if (!m || !data) return;
    for (size_t i = 0; i < frames; ++i) {
        int32_t sample = data[i * 2];
        uint32_t absolute = sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
        m->sum += sample;
        m->sum_squares += (uint64_t)((int64_t)sample * sample);
        if (absolute > m->peak) m->peak = absolute;
        if (absolute >= 32760) ++m->clipped;
        ++m->samples;
    }
}
double PcmMetrics_Rms(const PcmMetrics *m)
{
    return m && m->samples ? sqrt((double)m->sum_squares / m->samples) : 0;
}
double PcmMetrics_AcRms(const PcmMetrics *m)
{
    if (!m || !m->samples) return 0;
    double mean = (double)m->sum / m->samples;
    double variance = (double)m->sum_squares / m->samples - mean * mean;
    return variance > 0 ? sqrt(variance) : 0;
}
bool Cst9217_Decode(const uint8_t *packet, size_t length, uint16_t x[2], uint16_t y[2], uint8_t *count)
{
    if (!packet || !x || !y || !count || length < 7 || packet[6] != 0xab) return false;
    *count = 0;
    unsigned reported = packet[5] & 0x7f;
    if (reported > 2 || (reported == 2 && length < 12)) return false;
    for (unsigned i = 0; i < reported; ++i) {
        const uint8_t *p = packet + (i == 0 ? 0 : 7);
        unsigned event = p[0] & 0x0f;
        unsigned id = p[0] >> 4;
        if (event != 0x06 && event != 0x07) continue;
        if (id >= 2) return false;
        uint16_t px = ((uint16_t)p[1] << 4) | (p[3] >> 4);
        uint16_t py = ((uint16_t)p[2] << 4) | (p[3] & 0x0f);
        if (px >= 410 || py >= 502) return false;
        x[*count] = px; y[*count] = py; ++*count;
    }
    return true;
}
