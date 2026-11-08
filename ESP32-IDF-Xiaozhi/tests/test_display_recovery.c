#include "DisplayRecoveryLogic.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* A still-held bus or outstanding DMA must never trigger a new init. */
    assert(!DisplayRecovery_ShouldRetry(10000, 0, 1, true, false));
    assert(!DisplayRecovery_ShouldRetry(10000, 0, 1, false, true));
    assert(!DisplayRecovery_ShouldRetry(1999, 0, 1, true, true));
    assert(DisplayRecovery_ShouldRetry(2000, 0, 1, true, true));
    assert(DisplayRecovery_ShouldRetry(4000, 2000, 2, true, true));
    assert(!DisplayRecovery_ShouldRetry(10000, 2000, 3, true, true));
    assert(!DisplayRecovery_ShouldRetry(10000, 0, 0, true, true));
    assert(!DisplayRecovery_CanRetry(1, false));
    assert(!DisplayRecovery_CanRetry(3, true));
    /* Millisecond wrap must preserve the backoff, not stall for 49 days. */
    assert(!DisplayRecovery_ShouldRetry(1000, UINT32_MAX - 500, 1, true, true));
    assert(DisplayRecovery_ShouldRetry(1600, UINT32_MAX - 500, 1, true, true));
    puts("display recovery: bus health, DMA ownership, retry cap/backoff and wrap PASS");
    return 0;
}
