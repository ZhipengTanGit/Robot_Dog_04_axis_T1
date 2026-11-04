#include "BSP_UART_1.h"
#include "BSP_UART_3.h"
#include "Battery_Config.h"
#include "stdio.h"

/*
PA9   TX
PA10  RX
*/

// UART1 接收缓冲区大小
#define UART_1_RX_BUF_SIZE      256


// UART1 接收缓冲区
// 用于存放串口接收到的数据
static uint8_t  UART_1_Rx_Buf[UART_1_RX_BUF_SIZE];


// UART1 当前接收数据长度
// 每接收到一个字节自动 +1
static uint16_t UART_1_Rx_Count = 0;


// UART1 接收完成标志位
// 0：未接收完成
// 1：接收完成（触发空闲中断）
static uint8_t  UART_1_Rx_Flag  = 0;


// GPIOA 时钟启动函数
#define UART_1_GPIO_RCC_CMD     RCC_APB2PeriphClockCmd

// USART1 时钟启动函数
#define UART_1_UART_RCC_CMD     RCC_APB2PeriphClockCmd

// USART1 中断通道
#define UART_1_IRQ              USART1_IRQn


// GPIOA 端口时钟
#define UART_1_GPIO_RCC         RCC_APB2Periph_GPIOA

// USART1 外设时钟
#define UART_1_UART_RCC         RCC_APB2Periph_USART1


// USART1 使用 GPIOA
#define UART_1_GPIO_PORT        GPIOA

// USART1 RX 接收引脚：PA10
#define UART_1_RX_PIN           GPIO_Pin_10

// USART1 TX 发送引脚：PA9
#define UART_1_TX_PIN           GPIO_Pin_9

// USART1 外设编号
#define UART_1_NUM              USART1

// USART1 通信波特率
#define UART_1_BAUD             115200


void UART_1_Config(void)
{
    // 1、启动时钟
    UART_1_GPIO_RCC_CMD(UART_1_GPIO_RCC, ENABLE);
    UART_1_UART_RCC_CMD(UART_1_UART_RCC, ENABLE);

    // 2、配置 GPIO
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;

    // 2.1 配置 TX 引脚：PA9，复用推挽输出
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStruct.GPIO_Pin  = UART_1_TX_PIN;
    GPIO_Init(UART_1_GPIO_PORT, &GPIO_InitStruct);

    // 2.2 配置 RX 引脚：PA10，浮空输入
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStruct.GPIO_Pin  = UART_1_RX_PIN;
    GPIO_Init(UART_1_GPIO_PORT, &GPIO_InitStruct);

    // 3、配置 USART
    USART_InitTypeDef USART_InitStruct = {0};

    USART_InitStruct.USART_BaudRate            = UART_1_BAUD;
    USART_InitStruct.USART_WordLength          = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits            = USART_StopBits_1;
    USART_InitStruct.USART_Parity              = USART_Parity_No;
    USART_InitStruct.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;

    USART_Init(UART_1_NUM, &USART_InitStruct);

    // 4、配置 USART1 中断
    NVIC_InitTypeDef NVIC_InitStruct = {0};

    NVIC_InitStruct.NVIC_IRQChannel = UART_1_IRQ;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;

    NVIC_Init(&NVIC_InitStruct);

    // 5、开启接收中断
    USART_ITConfig(UART_1_NUM, USART_IT_RXNE, ENABLE);

    // 6、开启空闲中断
    USART_ITConfig(UART_1_NUM, USART_IT_IDLE, ENABLE);

    // 7、启动 USART
    USART_Cmd(UART_1_NUM, ENABLE);
}


// 发送一个字节
void UART_1_Send_Byte(uint8_t data)
{
    USART_SendData(UART_1_NUM, data);

    while(USART_GetFlagStatus(UART_1_NUM, USART_FLAG_TXE) == RESET);
}


// 发送字符串
void UART_1_Send_String(char *str)
{
    while(*str != '\0')
    {
        UART_1_Send_Byte(*str);
        str++;
    }
}


// 接收一个字节
uint8_t UART_1_Receipt_Byte(void)
{
    while(USART_GetFlagStatus(UART_1_NUM, USART_FLAG_RXNE) == RESET);

    return USART_ReceiveData(UART_1_NUM);
}


// 获取 UART1 接收完成标志位
uint8_t UART_1_Get_Rx_Flag(void)
{
    return UART_1_Rx_Flag;
}


// 清除 UART1 接收完成标志位
void UART_1_Clear_Rx_Flag(void)
{
    UART_1_Rx_Flag = 0;
}


void UART_1_Clear_Rx_Buffer(void)
{
    uint16_t i;

    UART_1_Rx_Flag = 0;
    UART_1_Rx_Count = 0;

    for(i = 0; i < UART_1_RX_BUF_SIZE; i++)
    {
        UART_1_Rx_Buf[i] = 0;
    }
}


// 获取 UART1 当前接收数据长度
uint16_t UART_1_Get_Rx_Count(void)
{
    return UART_1_Rx_Count;
}


// 获取 UART1 接收缓冲区地址
uint8_t *UART_1_Get_Rx_Buf(void)
{
    return UART_1_Rx_Buf;
}


// USART1 中断服务函数
void USART1_IRQHandler(void)
{
    uint8_t data = 0;

    // 接收中断
    if(USART_GetITStatus(UART_1_NUM, USART_IT_RXNE) != RESET)
    {
        data = USART_ReceiveData(UART_1_NUM);

        if(UART_1_Rx_Count < UART_1_RX_BUF_SIZE)
        {
            UART_1_Rx_Buf[UART_1_Rx_Count++] = data;
        }

        USART_ClearITPendingBit(UART_1_NUM, USART_IT_RXNE);
    }

    // 空闲中断
    if(USART_GetITStatus(UART_1_NUM, USART_IT_IDLE) != RESET)
    {
        // 清除空闲中断标志位
        USART_GetFlagStatus(UART_1_NUM, USART_FLAG_IDLE);
        USART_ReceiveData(UART_1_NUM);

        // 表示一帧数据接收完成
        UART_1_Rx_Flag = 1;
    }
}


int fputc(int ch, FILE *f)
{
    (void)f;
#if BATTERY_UART_DIAGNOSTICS
#if BATTERY_DIAGNOSTIC_UART == 3
    if (UART_3_Send_Byte((uint8_t)ch) != 0) {
        return EOF;
    }
#else
    // 等待发送缓冲区为空（TXE = 1），确保上一个字符已写入发送寄存器
    while ((USART1->SR & USART_SR_TXE) == 0);

    // 写入数据寄存器，自动开始发送
    USART1->DR = (uint8_t)ch;
#endif
#endif
    return ch;
}



