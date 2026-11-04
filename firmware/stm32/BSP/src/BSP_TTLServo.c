#include "TTLServo.h"
#include "stm32f10x.h"
#include "stm32f10x_dma.h"
#include "stm32f10x_tim.h"
#include <string.h>
#include <stdio.h>
#if defined(__CC_ARM)
#pragma O2
#endif
#ifndef CYCLE_COUNT
#define CYCLE_COUNT (*((volatile uint32_t *)0xE0001004UL))
#endif
static uint16_t samples[TTL_SERVO_SAMPLE_COUNT];
static uint8_t request[16];
static size_t request_size;
static uint32_t timer_hz;
static uint16_t pin_mask, rx_offset, tx_mismatches, rx_edges;
static uint8_t pin_number;
static volatile uint8_t phase;
static uint16_t start_idr;
static uint32_t start_crl, start_crh, start_odr;
static uint8_t reported[4];

static void pin_mode(uint32_t mode)
{
    volatile uint32_t *reg = pin_number < 8U ? &GPIOB->CRL : &GPIOB->CRH;
    uint32_t shift = (pin_number & 7U) * 4U;
    *reg = (*reg & ~(15UL << shift)) | (mode << shift);
}
static void wait_until(uint32_t deadline)
{
    while ((int32_t)(CYCLE_COUNT - deadline) < 0) { }
}
void BSP_TTLServo_Init(void)
{
    RCC_ClocksTypeDef clocks;
    NVIC_InitTypeDef irq;
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    RCC_GetClocksFreq(&clocks);
    timer_hz = clocks.PCLK1_Frequency * (clocks.PCLK1_Frequency == clocks.HCLK_Frequency ? 1UL : 2UL);
    GPIOB->BSRR = 0x03C0UL;
    GPIOB->CRL = (GPIOB->CRL & ~0xFF000000UL) | 0x88000000UL;
    GPIOB->CRH = (GPIOB->CRH & ~0xFFUL) | 0x88UL;
    phase = 0;
    irq.NVIC_IRQChannel = DMA1_Channel2_IRQn;
    irq.NVIC_IRQChannelPreemptionPriority = 1U;
    irq.NVIC_IRQChannelSubPriority = 0U;
    irq.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&irq);
}
int BSP_TTLServo_Start(uint8_t channel, uint32_t baud, const uint8_t *packet, size_t size)
{
    unsigned byte, bit;
    uint32_t cycles, deadline, saved_irq;
    if (phase || channel >= 4U || !baud || size < 6U || size > 11U) { return -5; }
    pin_number = (uint8_t)(6U + channel); pin_mask = (uint16_t)(1U << pin_number);
    if (!(GPIOB->IDR & pin_mask)) { return -6; }
    memcpy(request,packet,size); request_size=size;
    DMA1_Channel2->CCR = 0;
    TIM2->CR1 = 0; TIM2->DIER = 0;
    TIM2->PSC = 0;
    TIM2->ARR = (timer_hz + baud * 2UL) / (baud * 4UL) - 1UL;
    TIM2->CNT = 0; TIM2->EGR = TIM_EGR_UG; TIM2->SR = 0;
    DMA1_Channel2->CPAR = (uint32_t)&GPIOB->IDR;
    DMA1_Channel2->CMAR = (uint32_t)samples;
    DMA1_Channel2->CNDTR = TTL_SERVO_SAMPLE_COUNT;
    DMA1_Channel2->CCR = DMA_CCR1_MINC | DMA_CCR1_PSIZE_0 | DMA_CCR1_MSIZE_0 |
        DMA_CCR1_PL | DMA_CCR1_TCIE | DMA_CCR1_TEIE;
    DMA1->IFCR = DMA1_FLAG_GL2;
    phase = 2U;
    DMA1_Channel2->CCR |= DMA_CCR1_EN;
    TIM2->DIER = TIM_DIER_UDE; TIM2->CR1 = TIM_CR1_CEN;
    cycles = (SystemCoreClock + baud / 2UL) / baud;
    GPIOB->BSRR = pin_mask; pin_mode(3U);
    saved_irq = __get_PRIMASK();
    /* Fast requests last at most 695 us. Slow baud rates tolerate the short
     * SysTick/UART ISRs and keep interrupts enabled; capture remains hardware-timed. */
    if (baud >= 115200UL) { __disable_irq(); }
    deadline = CYCLE_COUNT + cycles * 2UL;
    wait_until(deadline);
    for (byte=0;byte<size;++byte) {
        GPIOB->BSRR = (uint32_t)pin_mask << 16;
        if (!byte) {
            wait_until(deadline + cycles / 2UL);
            start_idr = (uint16_t)GPIOB->IDR; start_crl=GPIOB->CRL; start_crh=GPIOB->CRH; start_odr=GPIOB->ODR;
            if (start_idr & pin_mask) {
                BSP_TTLServo_Abort(); __set_PRIMASK(saved_irq);
                printf("TTL_PIN_FAULT ch=%u idr=%04X odr=%08lX crl=%08lX crh=%08lX\r\n",
                    (unsigned)channel,(unsigned)start_idr,(unsigned long)start_odr,(unsigned long)start_crl,(unsigned long)start_crh);
                return -9;
            }
        }
        deadline += cycles; wait_until(deadline);
        for (bit=0;bit<8U;++bit) {
            GPIOB->BSRR = packet[byte] & (1U << bit) ? pin_mask : (uint32_t)pin_mask << 16;
            deadline += cycles; wait_until(deadline);
        }
        GPIOB->BSRR = pin_mask;
        deadline += cycles; wait_until(deadline);
    }
    /* Save the boundary before releasing the master. Capture includes the
     * physical request for verification; parsing the reply starts after it. */
    rx_offset = (uint16_t)(TTL_SERVO_SAMPLE_COUNT - DMA1_Channel2->CNDTR);
    pin_mode(8U);
    __set_PRIMASK(saved_irq);
    return 0;
}
void DMA1_Channel2_IRQHandler(void)
{
    uint32_t flags = DMA1->ISR;
    TIM2->CR1 = 0; TIM2->DIER = 0;
    DMA1_Channel2->CCR &= ~DMA_CCR1_EN;
    DMA1->IFCR = DMA1_FLAG_GL2;
    phase = flags & DMA1_FLAG_TE2 ? 4U : 3U;
}
int BSP_TTLServo_Poll(uint8_t *bytes, size_t capacity, size_t *size)
{
    uint8_t transmitted[16];
    size_t used, i;
    if (phase == 4U) { phase = 0; return -5; }
    if (phase != 3U) { return 1; }
    used = TTLServo_DecodeSamples(samples,rx_offset,pin_mask,transmitted,sizeof(transmitted));
    if (!reported[pin_number-6U]) {
        printf("TTL_DIAG ch=%u cpu_start_low=%u tx_bytes=%u offset=%u sample0=%04X samplemid=%04X crl=%08lX crh=%08lX odr=%08lX\r\n",
            (unsigned)(pin_number-6U),(unsigned)((start_idr&pin_mask)==0),(unsigned)used,(unsigned)rx_offset,
            (unsigned)samples[0],(unsigned)samples[rx_offset/2U],(unsigned long)start_crl,(unsigned long)start_crh,(unsigned long)start_odr);
        reported[pin_number-6U]=1;
    }
    tx_mismatches = used == request_size ? 0U : 1U;
    if (!tx_mismatches) {
        for (i=0;i<used;++i) { if (transmitted[i] != request[i]) { ++tx_mismatches; } }
    }
    rx_edges=0;
    for (i=rx_offset+1U;i<TTL_SERVO_SAMPLE_COUNT;++i) {
        if (!!(samples[i] & pin_mask) != !!(samples[i-1U] & pin_mask)) { ++rx_edges; }
    }
    *size = TTLServo_DecodeSamples(samples+rx_offset,TTL_SERVO_SAMPLE_COUNT-rx_offset,pin_mask,bytes,capacity);
    phase=0;
    return tx_mismatches ? -8 : 0;
}
void BSP_TTLServo_Diagnostics(uint16_t *tx_errors,uint16_t *edges)
{
    *tx_errors=tx_mismatches; *edges=rx_edges;
}
void BSP_TTLServo_Abort(void)
{
    uint32_t irq=__get_PRIMASK();
    __disable_irq();
    TIM2->CR1=0;TIM2->DIER=0;DMA1_Channel2->CCR=0;DMA1->IFCR=DMA1_FLAG_GL2;
    GPIOB->BSRR=pin_mask;
    if (pin_mask) { pin_mode(8U); }
    phase=0;
    __set_PRIMASK(irq);
}
