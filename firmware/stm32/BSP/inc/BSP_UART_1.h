#ifndef __BSP_UART_1_H__
#define __BSP_UART_1_H__

#include "stm32f10x.h"

void UART_1_Config(void);
void UART_1_Send_Byte(uint8_t data);
void UART_1_Send_String(char *str);
uint8_t UART_1_Receipt_Byte(void);
uint8_t UART_1_Get_Rx_Flag(void);
void UART_1_Clear_Rx_Flag(void);
void UART_1_Clear_Rx_Buffer(void);
uint16_t UART_1_Get_Rx_Count(void);
uint8_t *UART_1_Get_Rx_Buf(void);

#endif
