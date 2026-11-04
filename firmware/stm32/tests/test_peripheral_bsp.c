#include "BSP_Peripherals.h"
#include "Servo_Config.h"
#include "stm32f10x.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
GPIO_TypeDef test_porta, test_portb;
TIM_TypeDef test_timer1, test_timer4;
void RCC_APB2PeriphClockCmd(uint32_t mask, unsigned on) { (void)mask; (void)on; }
void RCC_APB1PeriphClockCmd(uint32_t mask, unsigned on) { (void)mask; (void)on; }
void RCC_GetClocksFreq(RCC_ClocksTypeDef *c) { c->HCLK_Frequency = c->PCLK2_Frequency = 72000000UL; c->PCLK1_Frequency = 36000000UL; }
void GPIO_SetBits(GPIO_TypeDef *p, uint16_t pins) { p->levels |= pins; }
void GPIO_ResetBits(GPIO_TypeDef *p, uint16_t pins) { p->levels &= (uint16_t)~pins; }
void GPIO_StructInit(GPIO_InitTypeDef *g) { memset(g, 0, sizeof(*g)); }
void GPIO_Init(GPIO_TypeDef *p, GPIO_InitTypeDef *g)
{
    unsigned i;
    for (i = 0U; i < 16U; ++i) { if (g->GPIO_Pin & (1U << i)) { p->mode[i] = g->GPIO_Mode; } }
}
unsigned GPIO_ReadInputDataBit(GPIO_TypeDef *p, uint16_t pin) { return (p->levels & pin) != 0U; }
void TIM_TimeBaseStructInit(TIM_TimeBaseInitTypeDef *c) { memset(c, 0, sizeof(*c)); }
void TIM_TimeBaseInit(TIM_TypeDef *t, TIM_TimeBaseInitTypeDef *c) { t->prescaler = c->TIM_Prescaler; t->period = c->TIM_Period; }
void TIM_OCStructInit(TIM_OCInitTypeDef *c) { memset(c, 0, sizeof(*c)); }
static void oc_init(TIM_TypeDef *t, TIM_OCInitTypeDef *c, unsigned i)
{
    assert(c->TIM_OCMode == TIM_OCMode_PWM1 && c->TIM_OutputState == TIM_OutputState_Enable);
    t->active[i] = t->pending[i] = (uint16_t)c->TIM_Pulse;
}
void TIM_OC1Init(TIM_TypeDef *t, TIM_OCInitTypeDef *c) { oc_init(t, c, 0U); }
void TIM_OC2Init(TIM_TypeDef *t, TIM_OCInitTypeDef *c) { oc_init(t, c, 1U); }
void TIM_OC3Init(TIM_TypeDef *t, TIM_OCInitTypeDef *c) { oc_init(t, c, 2U); }
void TIM_OC4Init(TIM_TypeDef *t, TIM_OCInitTypeDef *c) { oc_init(t, c, 3U); }
void TIM_CtrlPWMOutputs(TIM_TypeDef *t, unsigned on) { assert(t == TIM1 && on); }
void TIM_Cmd(TIM_TypeDef *t, unsigned on) { t->enabled = (uint8_t)on; }
void TIM_OC1PreloadConfig(TIM_TypeDef *t, unsigned on) { t->preload[0] = (uint8_t)on; }
void TIM_OC2PreloadConfig(TIM_TypeDef *t, unsigned on) { t->preload[1] = (uint8_t)on; }
void TIM_OC3PreloadConfig(TIM_TypeDef *t, unsigned on) { t->preload[2] = (uint8_t)on; }
void TIM_OC4PreloadConfig(TIM_TypeDef *t, unsigned on) { t->preload[3] = (uint8_t)on; }
static void compare(TIM_TypeDef *t, unsigned i, uint16_t v)
{
    /* Keep active and buffered registers independent to catch stops which
     * clear only one copy and can reappear at the next update event. */
    if (t->preload[i]) { t->pending[i] = v; } else { t->active[i] = v; }
}
void TIM_SetCompare1(TIM_TypeDef *t, uint16_t v) { compare(t, 0U, v); }
void TIM_SetCompare2(TIM_TypeDef *t, uint16_t v) { compare(t, 1U, v); }
void TIM_SetCompare3(TIM_TypeDef *t, uint16_t v) { compare(t, 2U, v); }
void TIM_SetCompare4(TIM_TypeDef *t, uint16_t v) { compare(t, 3U, v); }
static void update(TIM_TypeDef *t)
{
    unsigned i;
    for (i = 0U; i < 4U; ++i) { if (t->preload[i]) { t->active[i] = t->pending[i]; } }
}
int main(void)
{
    unsigned i;
    BSP_Peripherals_Init();
    assert(TIM1->prescaler == 71U && TIM1->period == 249U && !TIM1->active[0]);
#if SERVO_TTL_ENABLED
    assert(!TIM4->enabled && !TIM4->period);
#else
    assert(TIM4->prescaler == 71U && TIM4->period == 19999U);
#endif
    assert((GPIOB->levels & 3U) == 3U && GPIOA->mode[0] == GPIO_Mode_IN_FLOATING && GPIOA->mode[1] == GPIO_Mode_IN_FLOATING);
    assert(!GPIOB->mode[10] && !GPIOB->mode[11]); /* Shared I2C untouched. */
    BSP_Peripherals_Led(0U, 1U); assert(!(GPIOB->levels & 1U));
    BSP_Peripherals_Led(0U, 0U); assert(GPIOB->levels & 1U);
    GPIOA->levels = GPIO_Pin_0; assert(BSP_Peripherals_Key() && BSP_Peripherals_Touch());
    GPIOA->levels = GPIO_Pin_1; assert(!BSP_Peripherals_Key() && !BSP_Peripherals_Touch());
    BSP_Peripherals_Buzzer(1U); assert(TIM1->active[0] == 125U);
    BSP_Peripherals_Buzzer(0U); assert(!TIM1->active[0]);
    for (i = 0U; i < 4U; ++i) {
#if SERVO_TTL_ENABLED
        BSP_Peripherals_Servo((uint8_t)i, 1500U); update(TIM4);
        assert(!TIM4->active[i] && !TIM4->pending[i] && !GPIOB->mode[6U+i]);
#else
        assert(!TIM4->active[i] && !TIM4->pending[i] && GPIOB->mode[6U+i] == GPIO_Mode_AF_PP);
        BSP_Peripherals_Servo((uint8_t)i, 1500U);
        assert(!TIM4->active[i]); update(TIM4); assert(TIM4->active[i] == 1500U);
        BSP_Peripherals_Servo((uint8_t)i, 500U);
        assert(TIM4->active[i] == 1500U); update(TIM4); assert(TIM4->active[i] == 500U);
        BSP_Peripherals_Servo((uint8_t)i, 2500U);
        assert(TIM4->active[i] == 500U); update(TIM4); assert(TIM4->active[i] == 2500U);
        BSP_Peripherals_Servo((uint8_t)i, 499U); update(TIM4); assert(TIM4->active[i] == 2500U);
        BSP_Peripherals_Servo((uint8_t)i, 2501U); update(TIM4); assert(TIM4->active[i] == 2500U);
        BSP_Peripherals_Servo((uint8_t)i, 0U);
        assert(!TIM4->active[i]); update(TIM4); assert(!TIM4->active[i]);
#endif
    }
    puts(SERVO_TTL_ENABLED ? "TTL peripheral BSP passed: PWM never enabled; PWM calls cannot drive servo pins." :
        "Peripheral BSP tests passed: APB timer clocks, pin polarity, boot silence, frame-boundary PWM and persistent immediate stop.");
    return 0;
}
