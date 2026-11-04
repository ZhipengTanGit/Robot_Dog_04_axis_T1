#include "stm32f10x.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
GPIO_TypeDef test_gpio;
TIM_TypeDef test_tim;
DMA_Channel_TypeDef test_channel, test_channel7;
DMA_TypeDef test_dma;
void RCC_AHBPeriphClockCmd(unsigned clock, int enable) { (void)clock; assert(enable); }
void RCC_APB2PeriphClockCmd(unsigned clock, int enable) { (void)clock; assert(enable); }
void RCC_APB1PeriphClockCmd(unsigned clock, int enable) { (void)clock; assert(enable); }
void RCC_GetClocksFreq(RCC_ClocksTypeDef *out) { out->HCLK_Frequency=72000000; out->PCLK1_Frequency=36000000; }
void NVIC_Init(NVIC_InitTypeDef *irq) { assert(irq->NVIC_IRQChannel == DMA1_Channel2_IRQn); }
uint32_t __get_PRIMASK(void) { return 0; }
void __set_PRIMASK(uint32_t irq) { (void)irq; }
void __disable_irq(void) { }
uint32_t SystemCoreClock=72000000UL;
static uint32_t simulated_cycles(void);
#define CYCLE_COUNT simulated_cycles()

#include "../BSP/src/BSP_TTLServo.c"
static uint32_t tick, reply_start, reply_cycles;
static uint8_t reply_bytes[14], force_high;
static unsigned reply_length;
static uint32_t simulated_cycles(void)
{
    unsigned bit, byte, within;
    ++tick;
    test_gpio.IDR |= test_gpio.BSRR & 65535U;
    test_gpio.IDR &= ~(test_gpio.BSRR >> 16);
    test_gpio.BSRR=0;
    if(reply_start && tick >= reply_start) {
        bit=(tick-reply_start)/reply_cycles;
        byte=bit/10U; within=bit%10U;
        if(byte < reply_length && (!within || (within<9U && !(reply_bytes[byte] & (1U<<(within-1U)))))) test_gpio.IDR &= ~pin_mask;
        else test_gpio.IDR |= pin_mask;
    }
    if(force_high) test_gpio.IDR |= pin_mask;
    if(phase==2 && test_channel.CNDTR && tick%(test_tim.ARR+1U)==0) {
        samples[4096U-test_channel.CNDTR]= (uint16_t)test_gpio.IDR;
        if(!--test_channel.CNDTR) { test_dma.ISR=DMA1_FLAG_TC2;DMA1_Channel2_IRQHandler(); }
    }
    return tick;
}
int main(void)
{
    unsigned ch, i, sum;
    uint8_t packet[8], bytes[96];
    size_t size;
    TTLServo_Status s;
    BSP_TTLServo_Init();
    TTLServo_ReadPacket(1,packet);
    for(ch=0;ch<4;++ch) {
        reply_start=0; test_gpio.IDR=0x3C0U;
        assert(BSP_TTLServo_Start((uint8_t)ch,1000000,packet,8)==0);
        assert(phase==2 && rx_offset>300U && rx_offset<400U);
        memset(reply_bytes,0,sizeof(reply_bytes));
        reply_bytes[0]=reply_bytes[1]=255;reply_bytes[2]=1;reply_bytes[3]=10;reply_bytes[6]=2;
        sum=0;for(i=2;i<13;++i)sum+=reply_bytes[i];reply_bytes[13]=(uint8_t)(255U-(sum&255U));
        reply_length=14; reply_cycles=72;reply_start=tick+36000U;
        while(phase==2) simulated_cycles();
        assert(BSP_TTLServo_Poll(bytes,96,&size)==0 && !tx_mismatches && rx_edges>0);
        assert(TTLServo_ParseReply(bytes,size,1,&s)==0 && s.position==512);
    }
    reply_start=0;force_high=1;test_gpio.IDR=0x3c0;
    assert(BSP_TTLServo_Start(0,1000000,packet,8)==-9);
    assert(phase==0 && test_tim.CR1==0 && test_channel.CCR==0);
    force_high=0; test_gpio.IDR=0;
    assert(BSP_TTLServo_Start(0,1000000,packet,8)==-6);
    puts("TTL GPIO/DMA integration passed: four ports, physical TX decoding, reply isolation, position readback, stuck-HIGH/LOW cleanup.");
    return 0;
}
