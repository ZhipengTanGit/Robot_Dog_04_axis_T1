#ifndef TTL_STM32_STUB_H
#define TTL_STM32_STUB_H
#include <stdint.h>
extern uint32_t SystemCoreClock;
typedef struct { volatile uint32_t CRL, CRH, IDR, ODR, BSRR; } GPIO_TypeDef;
typedef struct { volatile uint32_t CR1, DIER, PSC, ARR, CNT, EGR, SR, CCR2, CCMR1; } TIM_TypeDef;
typedef struct { volatile uint32_t CCR, CPAR, CMAR, CNDTR; } DMA_Channel_TypeDef;
typedef struct { volatile uint32_t ISR, IFCR; } DMA_TypeDef;
typedef struct { uint32_t PCLK1_Frequency, HCLK_Frequency; } RCC_ClocksTypeDef;
typedef struct { unsigned NVIC_IRQChannel, NVIC_IRQChannelPreemptionPriority, NVIC_IRQChannelSubPriority, NVIC_IRQChannelCmd; } NVIC_InitTypeDef;
extern GPIO_TypeDef test_gpio;
extern TIM_TypeDef test_tim;
extern DMA_Channel_TypeDef test_channel, test_channel7;
extern DMA_TypeDef test_dma;
#define GPIOB (&test_gpio)
#define TIM2 (&test_tim)
#define DMA1_Channel2 (&test_channel)
#define DMA1_Channel7 (&test_channel7)
#define DMA1_FLAG_GL7 0x0F000000U
#define TIM_DIER_CC2DE 1024U
#define DMA1 (&test_dma)
#define DMA1_Channel2_IRQn 12U
#define ENABLE 1
#define RCC_AHBPeriph_DMA1 1
#define RCC_APB2Periph_GPIOB 2
#define RCC_APB1Periph_TIM2 4
#define TIM_EGR_UG 1U
#define TIM_DIER_UDE 256U
#define TIM_CR1_CEN 1U
#define DMA_CCR1_EN 1U
#define DMA_CCR1_TCIE 2U
#define DMA_CCR1_TEIE 8U
#define DMA_CCR1_DIR 16U
#define DMA_CCR1_MINC 128U
#define DMA_CCR1_PSIZE_0 256U
#define DMA_CCR1_PSIZE_1 512U
#define DMA_CCR1_MSIZE_0 1024U
#define DMA_CCR1_MSIZE_1 2048U
#define DMA_CCR1_PL 12288U
#define DMA1_FLAG_GL2 16U
#define DMA1_FLAG_TC2 32U
#define DMA1_FLAG_TE2 128U
void RCC_AHBPeriphClockCmd(unsigned clock, int enable);
void RCC_APB2PeriphClockCmd(unsigned clock, int enable);
void RCC_APB1PeriphClockCmd(unsigned clock, int enable);
void RCC_GetClocksFreq(RCC_ClocksTypeDef *out);
void NVIC_Init(NVIC_InitTypeDef *irq);
uint32_t __get_PRIMASK(void);
void __set_PRIMASK(uint32_t irq);
void __disable_irq(void);
#endif
