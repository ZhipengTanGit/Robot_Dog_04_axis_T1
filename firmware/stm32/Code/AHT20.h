#ifndef AHT20_H
#define AHT20_H
#include <stdint.h>

typedef enum {
    AHT20_STARTING, AHT20_STANDBY, AHT20_MEASURING, AHT20_READY,
    AHT20_BUS_ERROR, AHT20_CRC_ERROR, AHT20_BUSY_TIMEOUT,
    AHT20_UNCALIBRATED, AHT20_SENSOR_ERROR
} AHT20_State;
typedef struct {
    int32_t temperature_cc;
    uint32_t humidity_cpct;
    uint32_t age_ms, errors;
    uint8_t valid, has_sample;
    AHT20_State state;
} AHT20_Status;

void AHT20_Init(uint32_t now);
/* Nonblocking conversion: main-loop only; transactions take bounded I2C time.
 * bus_allowed is false during charger startup, standby or link faults. */
void AHT20_Task(uint32_t now, uint8_t bus_allowed);
void AHT20_GetStatus(uint32_t now, AHT20_Status *out);
const char *AHT20_StateName(AHT20_State state);
uint8_t AHT20_Crc8(const uint8_t *data, uint8_t length);
/* 0 success; 1 busy; -1 CRC; -2 uncalibrated; -3 bad argument. */
int AHT20_Decode(const uint8_t frame[7], int32_t *temperature_cc, uint32_t *humidity_cpct);
#endif
