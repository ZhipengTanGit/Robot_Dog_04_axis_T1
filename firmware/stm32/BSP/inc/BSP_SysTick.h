#ifndef __BSP_SYSTICK_H__
#define __BSP_SYSTICK_H__

#include "stm32f10x.h"

// 初始化
void SysTick_Init(void);

/* Unsigned subtraction of two readings remains valid across wraparound. */
uint32_t BSP_Millis(void);
/* Called only from SysTick_Handler. */
void BSP_SysTick_IncTick(void);

// 微秒延时
void Delay_us(uint32_t us);

// 毫秒延时
void Delay_ms(uint32_t ms);

#endif
