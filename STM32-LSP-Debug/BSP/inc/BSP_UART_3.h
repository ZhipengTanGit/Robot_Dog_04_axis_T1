#ifndef BSP_UART_3_H
#define BSP_UART_3_H

#include "stm32f10x.h"

/* V0.24 link: PC10 TX -> ESP GPIO5 RX; PC11 RX <- ESP GPIO4 TX.
 * USART3 partial remap is mandatory: PB10/PB11 belong to battery I2C.
 * 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control.
 */
void UART_3_Config(void);
/* Bounded wait; returns 0 after enqueueing one byte, -1 if unavailable. */
int UART_3_Send_Byte(uint8_t data);
uint16_t UART_3_Tx_Free(void);
uint32_t UART_3_Get_Tx_Dropped(void);
/* Main-loop consumer, nonblocking. The RX ring holds up to 255 bytes.
 * Receiving bytes does not execute a command or produce an echo.
 */
uint16_t UART_3_Read(uint8_t *data, uint16_t capacity);
uint32_t UART_3_Get_Rx_Dropped(void);
uint32_t UART_3_Get_Rx_Errors(void);
/* Main-loop recovery: atomically discard queued bytes after RX data loss. */
void UART_3_FlushRx(void);

#endif
