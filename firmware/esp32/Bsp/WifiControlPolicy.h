#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Only the motion protocol may cross the HTTP -> STM32 bridge. Local ESP
 * commands are parsed separately by the existing USB command parser. */
bool WifiControl_PeripheralLine(const char *line);
bool WifiControl_Deadline(uint32_t now, uint32_t deadline);
