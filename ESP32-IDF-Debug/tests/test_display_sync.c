#include "DisplaySyncLogic.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint32_t deadline = 0;
    uint32_t budget = DisplaySync_TransferBudgetUs(410, 24, 40000000, 16);
    assert(budget == 1142); /* 984 us payload + 8 us headers + 150 us software. */
    uint32_t slow_budget = DisplaySync_TransferBudgetUs(410, 24, 20000000, 16);
    assert(slow_budget == 2134);
    assert(DisplaySync_TransferBudgetUs(410, 16, 40000000, 16) == 810);
    assert(DisplaySync_TransferBudgetUs(410, 17, 40000000, 16) == 855);
    assert(DisplaySync_TransferBudgetUs(0, 24, 40000000, 16) == UINT32_MAX);
    assert(DisplaySync_TransferBudgetUs(410, 24, 40000000, 0) == UINT32_MAX);
    /* The first visible row must not be starved by an impossible window.
     * At the observed panel period, 20 MHz cannot finish even with zero lag. */
    assert(DisplaySync_Check(1000000, 1000000, 16733, 3, 54, 502, slow_budget, &deadline) == DISPLAY_SYNC_TOO_LATE);
    assert(deadline == 1799);
    assert(DisplaySync_Check(1000556, 1000000, 16733, 3, 54, 502, budget, NULL) == DISPLAY_SYNC_READY);
    assert(DisplaySync_Check(1000557, 1000000, 16733, 3, 54, 502, budget, NULL) == DISPLAY_SYNC_TOO_LATE);
    assert(DisplaySync_Check(1000100, 1000000, 16667, 3, 54, 502, budget, &deadline) == DISPLAY_SYNC_READY);
    assert(deadline == 1792);
    assert(DisplaySync_Check(1000600, 1000000, 16667, 3, 54, 502, budget, NULL) == DISPLAY_SYNC_TOO_LATE);
    assert(DisplaySync_Check(1000100, 1000000, 16667, 2, 54, 502, budget, NULL) == DISPLAY_SYNC_PERIOD_INVALID);
    assert(DisplaySync_Check(1000100, 1000000, 100, 3, 54, 502, budget, NULL) == DISPLAY_SYNC_PERIOD_INVALID);
    assert(DisplaySync_Check(1000100, 1000000, 60000, 3, 54, 502, budget, NULL) == DISPLAY_SYNC_PERIOD_INVALID);
    assert(DisplaySync_Check(1100001, 1000000, 16667, 3, 54, 502, budget, NULL) == DISPLAY_SYNC_PERIOD_INVALID);
    assert(DisplaySync_Check(1000000, 1000000, 16667, 3, 0, 502, budget, NULL) == DISPLAY_SYNC_TOO_LATE);
    assert(DisplaySync_Check(1000000, 1000000, 16667, 3, 502, 502, budget, NULL) == DISPLAY_SYNC_PERIOD_INVALID);
    assert(DisplaySync_Check(49, UINT32_MAX - 50, 16667, 3, 54, 502, budget, NULL) == DISPLAY_SYNC_READY);
    assert(!DisplaySync_SignalFresh(3, 16667, 100001));
    assert(!DisplaySync_SignalFresh(0, 16667, 0));
    assert(!DisplaySync_SignalFresh(3, 7999, 1));
    assert(!DisplaySync_SignalFresh(3, 50001, 1));
    bool dirty[] = {true, true, false, true};
    unsigned cursor = 0;
    assert(DisplaySync_NextDirty(dirty, 4, &cursor) == 0);
    /* Row 0 failed: leave it dirty. Row 1 must still get the next attempt. */
    assert(DisplaySync_NextDirty(dirty, 4, &cursor) == 1);
    dirty[1] = false;
    assert(DisplaySync_NextDirty(dirty, 4, &cursor) == 3);
    dirty[3] = false;
    assert(DisplaySync_NextDirty(dirty, 4, &cursor) == 0);
    dirty[0] = false;
    assert(DisplaySync_NextDirty(dirty, 4, &cursor) == 4);
    assert(DisplaySync_NextDirty(NULL, 4, &cursor) == 4);
    puts("display sync: timing/window/wrap checks and failed-strip retry without starvation PASS");
    return 0;
}
