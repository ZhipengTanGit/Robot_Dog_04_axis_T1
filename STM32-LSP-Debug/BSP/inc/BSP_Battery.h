#ifndef BSP_BATTERY_H
#define BSP_BATTERY_H

#include "stm32f10x.h"

/* Call after SysTick_Init(); PB10/PB11 and optional ADC1 are configured here.
 * This bus is also wired to AHT20. Future users must share one main-loop
 * scheduler: no concurrent driver or ISR may access PB10/PB11. */
void BSP_Battery_Init(void);
/* PA15 HIGH means IP5310 is awake, not an active-low interrupt request. */
uint8_t BSP_Battery_IsAwake(void);
/* Main-loop, single-master calls. Reads retry one failed I/O attempt; writes
 * are never automatically replayed. Output is unchanged on a failed read. */
#define BSP_BATTERY_IO_OK           0
#define BSP_BATTERY_IO_ARGUMENT    -1
#define BSP_BATTERY_IO_ASLEEP      -2
#define BSP_BATTERY_IO_NACK        -3
#define BSP_BATTERY_IO_SCL_TIMEOUT -4
#define BSP_BATTERY_IO_SDA_STUCK   -5
#define BSP_BATTERY_IO_STOP        -6

typedef enum {
    BSP_BATTERY_STAGE_IDLE = 0,
    BSP_BATTERY_STAGE_START = 1,
    BSP_BATTERY_STAGE_ADDRESS_WRITE = 2,
    BSP_BATTERY_STAGE_REGISTER = 3,
    BSP_BATTERY_STAGE_RESTART = 4,
    BSP_BATTERY_STAGE_ADDRESS_READ = 5,
    BSP_BATTERY_STAGE_DATA_READ = 6,
    BSP_BATTERY_STAGE_DATA_WRITE = 7,
    BSP_BATTERY_STAGE_STOP = 8,
    BSP_BATTERY_STAGE_RECOVERY = 9
} BSP_Battery_IoStage;

/* Sampled physical levels: bit0 SCL, bit1 SDA, bit2 AWAKE; 1 means HIGH.
 * transactions counts public Read/Write calls; failures counts failed attempts
 * (including a first attempt later recovered). Init clears all diagnostics.
 * Cleanup cannot overwrite the triggering failure. Successful calls clear only
 * last_result, preserving first/last failure evidence for intermittent faults. */
typedef struct {
    uint32_t transactions, failures, retries, recovered;
    int last_result;
    int first_error, last_error;
    uint32_t first_error_ms, last_error_ms;
    uint8_t first_stage, first_reg, first_write, first_pins;
    uint8_t last_stage, last_reg, last_write, last_pins;
} BSP_Battery_IoDiagnostics;

void BSP_Battery_GetIoDiagnostics(BSP_Battery_IoDiagnostics *out);
int BSP_Battery_ReadReg(uint8_t reg, uint8_t *value);
int BSP_Battery_WriteReg(uint8_t reg, uint8_t value);
/* Main-loop only, serialized with Battery_Task. Address is fixed to AHT20
 * (0x38); one write (1..3 bytes) OR read (1..7 bytes), no automatic retry.
 * Pauses with ASLEEP when L3 is low. Does not alter battery I/O diagnostics. */
int BSP_Battery_Aht20Transfer(const uint8_t *tx, uint8_t tx_length,
                            uint8_t *rx, uint8_t rx_length);
/* Returns an average of 8 ADC counts; -1 when disabled or not ready. */
int BSP_Battery_ReadTemperatureAdc(uint16_t *raw);
/* Releases both lines without driving either line high. */
void BSP_Battery_ReleaseBus(void);

#endif
