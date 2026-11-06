#include "DisplaySyncLogic.h"
#include <limits.h>

bool DisplaySync_PeriodValid(uint32_t period_us)
{
    return period_us >= DISPLAY_TE_MIN_PERIOD_US && period_us <= DISPLAY_TE_MAX_PERIOD_US;
}
bool DisplaySync_SignalFresh(uint8_t valid_periods, uint32_t period_us, uint32_t age_us)
{
    return valid_periods >= DISPLAY_TE_REQUIRED_PERIODS && DisplaySync_PeriodValid(period_us) &&
        age_us <= DISPLAY_TE_MAX_AGE_US;
}
unsigned DisplaySync_NextDirty(const bool *dirty, unsigned count, unsigned *cursor)
{
    if (!dirty || !count || !cursor) return count;
    unsigned start = *cursor % count;
    for (unsigned offset = 0; offset < count; ++offset) {
        unsigned candidate = (start + offset) % count;
        if (dirty[candidate]) {
            *cursor = (candidate + 1) % count;
            return candidate;
        }
    }
    return count;
}
uint32_t DisplaySync_TransferBudgetUs(unsigned width, unsigned height, uint32_t qspi_hz,
    unsigned max_rows_per_transfer)
{
    if (!width || !height || !qspi_hz || !max_rows_per_transfer ||
        width > 4096 || height > 4096 || max_rows_per_transfer > 4096) return UINT32_MAX;
    unsigned segments = (height + max_rows_per_transfer - 1U) / max_rows_per_transfer;
    /* RGB565 uses four data lanes; each segment also sends three 32-bit
     * headers plus two four-byte address windows on one lane (160 clocks). */
    uint64_t clocks = (uint64_t)width * height * 4U + (uint64_t)160U * segments;
    uint64_t result = (clocks * 1000000U + qspi_hz - 1U) / qspi_hz +
        DISPLAY_SPI_COMMAND_BUDGET_US;
    return result <= UINT32_MAX ? (uint32_t)result : UINT32_MAX;
}
DisplaySync_Decision DisplaySync_Check(uint32_t now_us, uint32_t edge_us,
    uint32_t period_us, uint8_t valid_periods, unsigned y, unsigned panel_height,
    uint32_t transfer_budget_us, uint32_t *scan_start_us)
{
    if (scan_start_us) *scan_start_us = 0;
    uint32_t elapsed = now_us - edge_us;
    if (!DisplaySync_SignalFresh(valid_periods, period_us, elapsed) ||
        !panel_height || y >= panel_height) return DISPLAY_SYNC_PERIOD_INVALID;
    uint32_t deadline = (uint32_t)((uint64_t)y * period_us / panel_height);
    if (scan_start_us) *scan_start_us = deadline;
    return (uint64_t)elapsed + transfer_budget_us + DISPLAY_TE_GUARD_US < deadline
        ? DISPLAY_SYNC_READY : DISPLAY_SYNC_TOO_LATE;
}
