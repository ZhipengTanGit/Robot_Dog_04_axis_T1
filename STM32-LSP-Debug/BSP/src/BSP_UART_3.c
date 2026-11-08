#include "BSP_UART_3.h"

#define UART_3_RX_SIZE       256U
#define UART_3_RX_MASK       (UART_3_RX_SIZE - 1U)
#define UART_3_TX_SIZE 4096U
#define UART_3_TX_MASK (UART_3_TX_SIZE - 1U)

/* Single ISR producer / main-loop consumer. Volatile accesses preserve the
 * store-data-before-head and load-data-before-tail publication order.
 */
static volatile uint8_t rx_buffer[UART_3_RX_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static volatile uint32_t rx_dropped;
static volatile uint32_t rx_errors;
static volatile uint8_t tx_buffer[UART_3_TX_SIZE];
static volatile uint16_t tx_head,tx_tail;
static volatile uint32_t tx_dropped;

void UART_3_Config(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;
    NVIC_InitTypeDef nvic;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    USART_Cmd(USART3, DISABLE);
    USART_ITConfig(USART3, USART_IT_RXNE, DISABLE);
    GPIO_PinRemapConfig(GPIO_PartialRemap_USART3, ENABLE);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_11;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOC, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate = 115200UL;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART3, &usart);
    rx_head = 0U;
    rx_tail = 0U;
    rx_dropped = 0UL;
    rx_errors = 0UL;
    tx_head=tx_tail=0U;tx_dropped=0U;
    /* SR then DR clears any stale RX/error state before enabling the IRQ. */
    (void)USART3->SR;
    (void)USART3->DR;
    NVIC_ClearPendingIRQ(USART3_IRQn);
    nvic.NVIC_IRQChannel = USART3_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2U;
    nvic.NVIC_IRQChannelSubPriority = 2U;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
    USART_ITConfig(USART3, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART3, ENABLE);
}

int UART_3_Send_Byte(uint8_t data)
{
    uint32_t saved_irq;
    uint16_t next;
    if ((USART3->CR1 & (USART_CR1_UE | USART_CR1_TE)) !=
        (USART_CR1_UE | USART_CR1_TE)) {
        return -1;
    }
    /* Main-loop producer / IRQ consumer. No serial wire-time wait in printf.
     * The short publication critical section also fences TXE disable/enable. */
    saved_irq=__get_PRIMASK();__disable_irq();
    next=(uint16_t)((tx_head+1U)&UART_3_TX_MASK);
    if(next==tx_tail){++tx_dropped;__set_PRIMASK(saved_irq);return -1;}
    tx_buffer[tx_head]=data;tx_head=next;USART3->CR1|=USART_CR1_TXEIE;
    __set_PRIMASK(saved_irq);
    return 0;
}
uint16_t UART_3_Tx_Free(void)
{return (uint16_t)(UART_3_TX_MASK-((tx_head-tx_tail)&UART_3_TX_MASK));}
uint32_t UART_3_Get_Tx_Dropped(void){return tx_dropped;}

uint16_t UART_3_Read(uint8_t *data, uint16_t capacity)
{
    uint16_t count = 0U;
    if (data == 0) {
        return 0U;
    }
    while (count < capacity && rx_tail != rx_head) {
        data[count++] = rx_buffer[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1U) & UART_3_RX_MASK);
    }
    return count;
}

uint32_t UART_3_Get_Rx_Dropped(void)
{
    return rx_dropped;
}

void UART_3_FlushRx(void)
{
    NVIC_DisableIRQ(USART3_IRQn);
    rx_tail = rx_head;
    NVIC_EnableIRQ(USART3_IRQn);
}

uint32_t UART_3_Get_Rx_Errors(void)
{
    return rx_errors;
}

void USART3_IRQHandler(void)
{
    uint32_t flags = USART3->SR;
    uint16_t next;
    uint8_t data;
    if ((flags & (USART_SR_RXNE | USART_SR_ORE | USART_SR_NE |
                  USART_SR_FE | USART_SR_PE)) != 0U) {
        data = (uint8_t)USART3->DR;
        if ((flags & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) != 0U) {++rx_errors;}
        else {
            next = (uint16_t)((rx_head + 1U) & UART_3_RX_MASK);
            if(next==rx_tail){++rx_dropped;}
            else{rx_buffer[rx_head]=data;rx_head=next;}
        }
    }
    if((flags&USART_SR_TXE)&&(USART3->CR1&USART_CR1_TXEIE)){
        if(tx_tail!=tx_head){USART3->DR=tx_buffer[tx_tail];tx_tail=(uint16_t)((tx_tail+1U)&UART_3_TX_MASK);}
        if(tx_tail==tx_head)USART3->CR1&=~USART_CR1_TXEIE;
    }
}
