#include "DisplayRecoveryLogic.h"

bool DisplayRecovery_CanRetry(unsigned attempts, bool resources_idle)
{
    return attempts > 0 && attempts < DISPLAY_INIT_MAX_ATTEMPTS && resources_idle;
}

bool DisplayRecovery_ShouldRetry(uint32_t now_ms, uint32_t last_attempt_ms,
    unsigned attempts, bool resources_idle, bool bus_healthy)
{
    return DisplayRecovery_CanRetry(attempts, resources_idle) && bus_healthy &&
        now_ms - last_attempt_ms >= DISPLAY_INIT_RETRY_DELAY_MS;
}
