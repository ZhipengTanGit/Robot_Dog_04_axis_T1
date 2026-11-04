#ifndef BSP_UART_BRIDGE_H
#define BSP_UART_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

/* An explicit web command is injected only between complete CR/LF PC lines.
 * Deadline uses xTaskGetTickCount(); the TX queue must drain and leave whole
 * frame line time plus a scheduling margin, otherwise the command is dropped. */
bool UartBridge_QueueCommand(const char *line, size_t length, TickType_t deadline);
#include "esp_err.h"

/* V0.24 board: CH340 <-> UART0, STM32 PC11/PC10 <-> UART1. */
#define UART_BRIDGE_PC_BAUD    921600
#define UART_BRIDGE_STM_BAUD   115200
#define UART_BRIDGE_PC_TX_GPIO 43
#define UART_BRIDGE_PC_RX_GPIO 44
#define UART_BRIDGE_STM_TX_GPIO 4
#define UART_BRIDGE_STM_RX_GPIO 5

typedef struct {
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t read_errors;
    uint32_t write_errors;
    uint32_t unforwarded_bytes;
    uint32_t fifo_overflows;
    uint32_t buffer_full_events;
    uint32_t frame_errors;
    uint32_t parity_errors;
    uint32_t break_events;
    uint32_t resyncs;
    uint32_t command_expired;
    uint32_t command_tx_errors;
} UartBridge_DirectionStats;

/* Debugger counters, never injected into the forwarded byte stream.
 * Index 0: PC -> STM32; index 1: STM32 -> PC. One task owns each index.
 */
extern volatile UartBridge_DirectionStats g_uart_bridge_stats[2];

/* Owns UART0 and UART1. Call once from app_main; no console/stdin readers or
 * GPIO4 LED task may run concurrently. Normal runtime ESP logs are silenced.
 * Healthy UART payloads (including NUL and CR/LF) pass unchanged. Actual RX
 * loss invalidates the partial frame and discards through a fresh delimiter.
 * STM32 lines are also mirrored to telemetry; [NET] status is inserted on the PC side
 * at line boundaries at most every five seconds. Never sent toward STM32.
 */
esp_err_t UartBridge_Start(void);

#endif
