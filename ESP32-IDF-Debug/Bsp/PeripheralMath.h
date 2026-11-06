#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t samples, peak, clipped; int64_t sum; uint64_t sum_squares; } PcmMetrics;
void PcmMetrics_AddStereoLeft(PcmMetrics *m, const int16_t *data, size_t frames);
double PcmMetrics_Rms(const PcmMetrics *m);
double PcmMetrics_AcRms(const PcmMetrics *m);
/* CST9217 D000 packet; false means malformed, true with count=0 is release. */
bool Cst9217_Decode(const uint8_t *packet, size_t length, uint16_t x[2], uint16_t y[2], uint8_t *count);
