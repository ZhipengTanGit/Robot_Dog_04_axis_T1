#include "BSP_Peripherals.h"
#include "Servo_Config.h"
#include "stm32f10x.h"

void BSP_Peripherals_Init(void)
{
    GPIO_InitTypeDef gpio;
    TIM_TimeBaseInitTypeDef timer;
    TIM_OCInitTypeDef oc;
    RCC_ClocksTypeDef clocks;
    uint32_t tim1_hz;
    uint32_t tim3_hz;
#if !SERVO_TTL_ENABLED
    uint32_t tim4_hz;
#endif
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_TIM1, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4 | RCC_APB1Periph_TIM3, ENABLE);
    /* Off levels are loaded before pin mode changes. LEDs are active LOW. */
    GPIO_SetBits(GPIOB, GPIO_Pin_0 | GPIO_Pin_1);
    GPIO_ResetBits(GPIOA, GPIO_Pin_8);
    GPIO_ResetBits(GPIOB, GPIO_Pin_6 | GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1;
    GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio); /* PA0 external pulldown; PA1 external pullup. */

    RCC_GetClocksFreq(&clocks);
    tim1_hz = clocks.PCLK2_Frequency * (clocks.PCLK2_Frequency == clocks.HCLK_Frequency ? 1UL : 2UL);
    tim3_hz = clocks.PCLK1_Frequency * (clocks.PCLK1_Frequency == clocks.HCLK_Frequency ? 1UL : 2UL);
#if !SERVO_TTL_ENABLED
    tim4_hz = clocks.PCLK1_Frequency * (clocks.PCLK1_Frequency == clocks.HCLK_Frequency ? 1UL : 2UL);
#endif
    TIM_TimeBaseStructInit(&timer);
    timer.TIM_Prescaler = (uint16_t)(tim1_hz / 1000000UL - 1UL);
    timer.TIM_Period = 249U; /* 4 kHz at 1 us tick. */
    TIM_TimeBaseInit(TIM1, &timer);
    TIM_OCStructInit(&oc);
    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_Pulse = 0U;
    oc.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC1Init(TIM1, &oc);
    TIM_CtrlPWMOutputs(TIM1, ENABLE);
    TIM_Cmd(TIM1, ENABLE);

#if !SERVO_TTL_ENABLED
    timer.TIM_Prescaler = (uint16_t)(tim4_hz / 1000000UL - 1UL);
    timer.TIM_Period = 19999U; /* 50 Hz, default mapping PB6..PB9. */
    TIM_TimeBaseInit(TIM4, &timer);
    TIM_OC1Init(TIM4, &oc);
    TIM_OC2Init(TIM4, &oc);
    TIM_OC3Init(TIM4, &oc);
    TIM_OC4Init(TIM4, &oc);
    TIM_OC1PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC4PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_Cmd(TIM4, ENABLE);
#endif
    /* Independent hardware carrier: never add an interrupt to TTL sampling. */
    timer.TIM_Prescaler = (uint16_t)(tim3_hz / 1000000UL - 1UL);
    timer.TIM_Period = 999U;
    TIM_TimeBaseInit(TIM3, &timer);
    oc.TIM_OCPolarity = TIM_OCPolarity_Low;
    TIM_OC3Init(TIM3, &oc);
    TIM_OC3PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_Cmd(TIM3, ENABLE);
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin = GPIO_Pin_0;
    GPIO_Init(GPIOB, &gpio);

    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin = GPIO_Pin_8;
    GPIO_Init(GPIOA, &gpio);
#if !SERVO_TTL_ENABLED
    gpio.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9;
    GPIO_Init(GPIOB, &gpio);
#endif
}

uint8_t BSP_Peripherals_Key(void) { return GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) == Bit_SET; }
uint8_t BSP_Peripherals_Touch(void) { return GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_1) == Bit_RESET; }
void BSP_Peripherals_Led(uint8_t index, uint8_t on)
{
    uint16_t pin;
    if (index > 1U) { return; }
    if (index == 0U) { BSP_Peripherals_ChargeLedLevel(on ? 1000U : 0U); return; }
    pin = index ? GPIO_Pin_1 : GPIO_Pin_0;
    if (on) { GPIO_ResetBits(GPIOB, pin); } else { GPIO_SetBits(GPIOB, pin); }
}
void BSP_Peripherals_ChargeLedLevel(uint16_t level)
{
    TIM_SetCompare3(TIM3, level > 1000U ? 1000U : level);
}
void BSP_Peripherals_Buzzer(uint8_t on) { TIM_SetCompare1(TIM1, on ? 125U : 0U); }
void BSP_Peripherals_Servo(uint8_t channel, uint16_t pulse_us)
{
#if SERVO_TTL_ENABLED
    (void)channel; (void)pulse_us; /* No PWM writes to the new TTL buses. */
#else
    if (pulse_us != 0U && (pulse_us < BSP_SERVO_MIN_PULSE_US || pulse_us > BSP_SERVO_MAX_PULSE_US)) { return; }
    /* New pulse widths take effect at the next 20 ms boundary; a stop bypasses
     * preload so a fault ends the pulse immediately instead of waiting a frame. */
    switch (channel) {
    case 0U:
        if (!pulse_us) { TIM_OC1PreloadConfig(TIM4, TIM_OCPreload_Disable); }
        TIM_SetCompare1(TIM4, pulse_us);
        TIM_OC1PreloadConfig(TIM4, TIM_OCPreload_Enable);
        if (!pulse_us) { TIM_SetCompare1(TIM4, 0U); } break;
    case 1U:
        if (!pulse_us) { TIM_OC2PreloadConfig(TIM4, TIM_OCPreload_Disable); }
        TIM_SetCompare2(TIM4, pulse_us);
        TIM_OC2PreloadConfig(TIM4, TIM_OCPreload_Enable);
        if (!pulse_us) { TIM_SetCompare2(TIM4, 0U); } break;
    case 2U:
        if (!pulse_us) { TIM_OC3PreloadConfig(TIM4, TIM_OCPreload_Disable); }
        TIM_SetCompare3(TIM4, pulse_us);
        TIM_OC3PreloadConfig(TIM4, TIM_OCPreload_Enable);
        if (!pulse_us) { TIM_SetCompare3(TIM4, 0U); } break;
    case 3U:
        if (!pulse_us) { TIM_OC4PreloadConfig(TIM4, TIM_OCPreload_Disable); }
        TIM_SetCompare4(TIM4, pulse_us);
        TIM_OC4PreloadConfig(TIM4, TIM_OCPreload_Enable);
        if (!pulse_us) { TIM_SetCompare4(TIM4, 0U); } break;
    default: break;
    }
#endif
}
