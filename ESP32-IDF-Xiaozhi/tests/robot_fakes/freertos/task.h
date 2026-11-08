#pragma once
#include <cstdint>
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
int xTaskCreate(void (*task)(void*),const char*,unsigned,void*,unsigned,void*);
void vTaskDelay(uint32_t);
