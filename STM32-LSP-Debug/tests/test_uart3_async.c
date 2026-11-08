#include "BSP_UART_3.h"
#include <assert.h>
#include <stdio.h>
USART_TypeDef host_uart3;
uint32_t host_primask;
int main(void)
{
    unsigned i;uint8_t byte;
    assert(UART_3_Send_Byte(1)<0);
    UART_3_Config();host_uart3.SR=0;
    /* Enqueue succeeds even when the wire is busy; it must never wait TXE. */
    assert(UART_3_Send_Byte(42)==0);assert(host_uart3.DR==0);assert(UART_3_Tx_Free()==4094);
    host_uart3.SR=USART_SR_TXE;USART3_IRQHandler();assert(host_uart3.DR==42);
    assert(!(host_uart3.CR1&USART_CR1_TXEIE));assert(UART_3_Get_Rx_Errors()==0);
    for(i=0;i<4095;++i)assert(UART_3_Send_Byte((uint8_t)i)==0);
    assert(UART_3_Tx_Free()==0);assert(UART_3_Send_Byte(9)<0);assert(UART_3_Get_Tx_Dropped()==1);
    for(i=0;i<4095;++i){host_uart3.SR=USART_SR_TXE;USART3_IRQHandler();assert(host_uart3.DR==(uint8_t)i);}
    assert(UART_3_Tx_Free()==4095);assert(!(host_uart3.CR1&USART_CR1_TXEIE));
    assert(UART_3_Send_Byte(99)==0);host_uart3.DR=77;host_uart3.SR=USART_SR_TXE|USART_SR_RXNE;
    USART3_IRQHandler();assert(host_uart3.DR==99);assert(UART_3_Read(&byte,1)==1&&byte==77);
    assert(UART_3_Send_Byte(100)==0);host_uart3.SR=USART_SR_TXE|USART_SR_ORE;USART3_IRQHandler();
    assert(host_uart3.DR==100&&UART_3_Get_Rx_Errors()==1);
    /* RX overflow must not interrupt concurrent TX progress. */
    for(i=0;i<256;++i){host_uart3.DR=i;host_uart3.SR=USART_SR_RXNE;USART3_IRQHandler();}
    assert(UART_3_Get_Rx_Dropped()==1);UART_3_FlushRx();assert(UART_3_Read(&byte,1)==0);
    host_primask=1;assert(UART_3_Send_Byte(12)==0);assert(host_primask==1);
    puts("UART3 async: bounded enqueue, FIFO wrap/full, TX IRQ drain, RX coexistence/errors, IRQ state preserved.");return 0;
}
