#ifndef TTL_SERVO_H
#define TTL_SERVO_H
#include <stdint.h>
#include <stddef.h>
#define TTL_SERVO_SAMPLE_COUNT 4096U
#define TTL_SERVO_REPLY_MAX 96U
typedef struct {
    uint32_t baud, sampled_ms, errors;
    uint16_t position, speed, load, tx_mismatches, rx_edges;
    uint8_t id, status, voltage, temperature, valid;
    int result;
} TTLServo_Status;
/* Both discovery timings correspond to the supported nominal 115200 bus. */
int TTLServo_ControlReady(const TTLServo_Status *feedback);
/* Only PING and READ DATA are emitted. No register/torque/ID writes. */
void TTLServo_PingPacket(uint8_t id, uint8_t packet[6]);
int TTLServo_ParsePing(const uint8_t *data, size_t size, uint8_t id, TTLServo_Status *out);
void TTLServo_ReadPacket(uint8_t id, uint8_t packet[8]);
int TTLServo_ParseReply(const uint8_t *data, size_t size, uint8_t id, TTLServo_Status *out);
size_t TTLServo_DecodeSamples(const uint16_t *samples, size_t count, uint16_t mask, uint8_t *bytes, size_t capacity);
void TTLServo_Init(void);
void TTLServo_Task(uint32_t now);
void TTLServo_PrintStatus(uint32_t now);
void TTLServo_GetStatus(uint8_t channel, uint32_t now, TTLServo_Status *out);
void TTLServo_ResetTiming(void);
/* Bounded cycle-timed TX and asynchronous TIM2/DMA capture on PB6..PB9. */
void BSP_TTLServo_Init(void);
int BSP_TTLServo_Start(uint8_t channel, uint32_t baud, const uint8_t *packet, size_t size);
int BSP_TTLServo_Poll(uint8_t *bytes, size_t capacity, size_t *size);
void BSP_TTLServo_Diagnostics(uint16_t *tx_mismatches, uint16_t *rx_edges);
void BSP_TTLServo_Abort(void);
#endif
