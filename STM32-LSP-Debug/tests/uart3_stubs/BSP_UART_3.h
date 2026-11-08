#ifndef UART3_HOST_STUB_H
#define UART3_HOST_STUB_H
#include <stdint.h>
#include <string.h>
typedef struct {volatile uint32_t SR,DR,CR1;} USART_TypeDef;
typedef struct {uint32_t GPIO_Pin,GPIO_Mode,GPIO_Speed;} GPIO_InitTypeDef;
typedef struct {uint32_t USART_BaudRate,USART_Mode;} USART_InitTypeDef;
typedef struct {uint32_t NVIC_IRQChannel,NVIC_IRQChannelPreemptionPriority,NVIC_IRQChannelSubPriority,NVIC_IRQChannelCmd;} NVIC_InitTypeDef;
extern USART_TypeDef host_uart3;
extern uint32_t host_primask;
#define USART3 (&host_uart3)
#define GPIOC ((void*)1)
#define ENABLE 1U
#define DISABLE 0U
#define RCC_APB2Periph_GPIOC 1U
#define RCC_APB2Periph_AFIO 2U
#define RCC_APB1Periph_USART3 4U
#define GPIO_PartialRemap_USART3 1U
#define GPIO_Pin_10 1024U
#define GPIO_Pin_11 2048U
#define GPIO_Mode_AF_PP 1U
#define GPIO_Mode_IPU 2U
#define GPIO_Speed_50MHz 3U
#define USART_Mode_Tx 8U
#define USART_Mode_Rx 4U
#define USART_CR1_TE 8U
#define USART_CR1_UE 8192U
#define USART_CR1_TXEIE 128U
#define USART_IT_RXNE 32U
#define USART_SR_TXE 128U
#define USART_SR_RXNE 32U
#define USART_SR_ORE 8U
#define USART_SR_NE 4U
#define USART_SR_FE 2U
#define USART_SR_PE 1U
#define USART3_IRQn 39U
static inline void RCC_APB2PeriphClockCmd(uint32_t c,uint32_t v){(void)c;(void)v;}
static inline void RCC_APB1PeriphClockCmd(uint32_t c,uint32_t v){(void)c;(void)v;}
static inline void GPIO_PinRemapConfig(uint32_t c,uint32_t v){(void)c;(void)v;}
static inline void GPIO_StructInit(GPIO_InitTypeDef *p){memset(p,0,sizeof(*p));}
static inline void GPIO_Init(void *p,GPIO_InitTypeDef *c){(void)p;(void)c;}
static inline void USART_StructInit(USART_InitTypeDef *p){memset(p,0,sizeof(*p));}
static inline void USART_Init(USART_TypeDef *p,USART_InitTypeDef *c){p->CR1=c->USART_Mode;}
static inline void USART_Cmd(USART_TypeDef *p,uint32_t v){if(v)p->CR1|=USART_CR1_UE;else p->CR1&=~USART_CR1_UE;}
static inline void USART_ITConfig(USART_TypeDef *p,uint32_t i,uint32_t v){if(v)p->CR1|=i;else p->CR1&=~i;}
static inline void NVIC_ClearPendingIRQ(uint32_t i){(void)i;}
static inline void NVIC_Init(NVIC_InitTypeDef *p){(void)p;}
static inline void NVIC_DisableIRQ(uint32_t i){(void)i;}
static inline void NVIC_EnableIRQ(uint32_t i){(void)i;}
static inline uint32_t __get_PRIMASK(void){return host_primask;}
static inline void __disable_irq(void){host_primask=1;}
static inline void __set_PRIMASK(uint32_t p){host_primask=p;}
void UART_3_Config(void);
int UART_3_Send_Byte(uint8_t data);
uint16_t UART_3_Tx_Free(void);
uint32_t UART_3_Get_Tx_Dropped(void);
uint16_t UART_3_Read(uint8_t *data,uint16_t size);
uint32_t UART_3_Get_Rx_Dropped(void);
uint32_t UART_3_Get_Rx_Errors(void);
void UART_3_FlushRx(void);
void USART3_IRQHandler(void);
#endif
