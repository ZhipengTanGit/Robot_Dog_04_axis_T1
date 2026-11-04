#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_TE_MIN_PERIOD_US 8000U
#define DISPLAY_TE_MAX_PERIOD_US 50000U
#define DISPLAY_TE_REQUIRED_PERIODS 3U
#define DISPLAY_TE_MAX_AGE_US 100000U
#define DISPLAY_SPI_COMMAND_BUDGET_US 150U
#define DISPLAY_TE_GUARD_US 100U

typedef enum {
    DISPLAY_SYNC_READY,
    DISPLAY_SYNC_PERIOD_INVALID,
    DISPLAY_SYNC_TOO_LATE,
} DisplaySync_Decision;

bool DisplaySync_PeriodValid(uint32_t period_us);
bool DisplaySync_SignalFresh(uint8_t valid_periods, uint32_t period_us, uint32_t age_us);
/* Includes single-line window/command headers for each DMA segment. */
uint32_t DisplaySync_TransferBudgetUs(unsigned width, unsigned height, uint32_t qspi_hz,
    unsigned max_rows_per_transfer);
/* Returns count when no strip is dirty. Advances before the attempt, so a
 * failed/late strip can stay dirty without starving another strip. */
unsigned DisplaySync_NextDirty(const bool *dirty, unsigned count, unsigned *cursor);
/* TE is the start of Vblank. Ignore blanking duration when estimating scan
 * arrival at y, making the permitted write window smaller, not larger.
 * Unsigned elapsed subtraction supports the 32-bit microsecond wrap. */
DisplaySync_Decision DisplaySync_Check(uint32_t now_us, uint32_t edge_us,
    uint32_t period_us, uint8_t valid_periods, unsigned y, unsigned panel_height,
    uint32_t transfer_budget_us, uint32_t *scan_start_us);
