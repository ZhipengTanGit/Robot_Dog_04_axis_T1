#ifndef TEST_BSP_BATTERY_H
#define TEST_BSP_BATTERY_H
#include <stdint.h>
#define BSP_BATTERY_IO_OK           0
#define BSP_BATTERY_IO_ARGUMENT    -1
#define BSP_BATTERY_IO_ASLEEP      -2
#define BSP_BATTERY_IO_NACK        -3
#define BSP_BATTERY_IO_SCL_TIMEOUT -4
#define BSP_BATTERY_IO_SDA_STUCK   -5
#define BSP_BATTERY_IO_STOP        -6
typedef struct {
    uint32_t transactions, failures, retries, recovered;
    int last_result;
    int first_error, last_error;
    uint32_t first_error_ms, last_error_ms;
    uint8_t first_stage, first_reg, first_write, first_pins;
    uint8_t last_stage, last_reg, last_write, last_pins;
} BSP_Battery_IoDiagnostics;
void BSP_Battery_GetIoDiagnostics(BSP_Battery_IoDiagnostics *out);
void BSP_Battery_Init(void);
uint8_t BSP_Battery_IsAwake(void);
int BSP_Battery_ReadReg(uint8_t reg, uint8_t *value);
int BSP_Battery_WriteReg(uint8_t reg, uint8_t value);
int BSP_Battery_ReadTemperatureAdc(uint16_t *raw);
void BSP_Battery_ReleaseBus(void);
#endif
