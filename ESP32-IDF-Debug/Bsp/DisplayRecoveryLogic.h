#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_INIT_MAX_ATTEMPTS 3U
#define DISPLAY_INIT_RETRY_DELAY_MS 2000U

bool DisplayRecovery_CanRetry(unsigned attempts, bool resources_idle);
bool DisplayRecovery_ShouldRetry(uint32_t now_ms, uint32_t last_attempt_ms,
    unsigned attempts, bool resources_idle, bool bus_healthy);
