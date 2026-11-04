#ifndef TEST_PERIPHERAL_STM32_H
#define TEST_PERIPHERAL_STM32_H
#include <stdint.h>
typedef struct { uint16_t levels; unsigned mode[16]; } GPIO_TypeDef;
typedef struct { uint16_t active[4], pending[4], prescaler, period; uint8_t preload[4], enabled; } TIM_TypeDef;
extern GPIO_TypeDef test_porta, test_portb;
extern TIM_TypeDef test_timer1, test_timer4;
#define GPIOA (&test_porta)
#define GPIOB (&test_portb)
#define TIM1 (&test_timer1)
#define TIM4 (&test_timer4)
#define GPIO_Pin_0 1U
#define GPIO_Pin_1 2U
#define GPIO_Pin_6 64U
#define GPIO_Pin_7 128U
#define GPIO_Pin_8 256U
#define GPIO_Pin_9 512U
#define GPIO_Speed_2MHz 2U
#define GPIO_Mode_Out_PP 1U
#define GPIO_Mode_AF_PP 2U
#define GPIO_Mode_IN_FLOATING 3U
#define RCC_APB2Periph_GPIOA 1U
#define RCC_APB2Periph_GPIOB 2U
#define RCC_APB2Periph_TIM1 4U
#define RCC_APB1Periph_TIM4 8U
#define TIM_OCMode_PWM1 1U
#define TIM_OutputState_Enable 1U
#define TIM_OCPolarity_High 1U
#define TIM_OCPreload_Enable 1U
#define TIM_OCPreload_Disable 0U
#define ENABLE 1U
#define Bit_SET 1U
#define Bit_RESET 0U
typedef struct { uint16_t GPIO_Pin; unsigned GPIO_Speed, GPIO_Mode; } GPIO_InitTypeDef;
typedef struct { uint32_t HCLK_Frequency, PCLK1_Frequency, PCLK2_Frequency; } RCC_ClocksTypeDef;
typedef struct { uint16_t TIM_Prescaler, TIM_Period; } TIM_TimeBaseInitTypeDef;
typedef struct { unsigned TIM_OCMode, TIM_OutputState, TIM_Pulse, TIM_OCPolarity; } TIM_OCInitTypeDef;
void RCC_APB2PeriphClockCmd(uint32_t mask, unsigned on);
void RCC_APB1PeriphClockCmd(uint32_t mask, unsigned on);
void RCC_GetClocksFreq(RCC_ClocksTypeDef *clocks);
void GPIO_SetBits(GPIO_TypeDef *port, uint16_t pins);
void GPIO_ResetBits(GPIO_TypeDef *port, uint16_t pins);
void GPIO_StructInit(GPIO_InitTypeDef *gpio);
void GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *gpio);
unsigned GPIO_ReadInputDataBit(GPIO_TypeDef *port, uint16_t pin);
void TIM_TimeBaseStructInit(TIM_TimeBaseInitTypeDef *timer);
void TIM_TimeBaseInit(TIM_TypeDef *timer, TIM_TimeBaseInitTypeDef *config);
void TIM_OCStructInit(TIM_OCInitTypeDef *oc);
void TIM_OC1Init(TIM_TypeDef *timer, TIM_OCInitTypeDef *oc);
void TIM_OC2Init(TIM_TypeDef *timer, TIM_OCInitTypeDef *oc);
void TIM_OC3Init(TIM_TypeDef *timer, TIM_OCInitTypeDef *oc);
void TIM_OC4Init(TIM_TypeDef *timer, TIM_OCInitTypeDef *oc);
void TIM_CtrlPWMOutputs(TIM_TypeDef *timer, unsigned on);
void TIM_Cmd(TIM_TypeDef *timer, unsigned on);
void TIM_OC1PreloadConfig(TIM_TypeDef *timer, unsigned on);
void TIM_OC2PreloadConfig(TIM_TypeDef *timer, unsigned on);
void TIM_OC3PreloadConfig(TIM_TypeDef *timer, unsigned on);
void TIM_OC4PreloadConfig(TIM_TypeDef *timer, unsigned on);
void TIM_SetCompare1(TIM_TypeDef *timer, uint16_t compare);
void TIM_SetCompare2(TIM_TypeDef *timer, uint16_t compare);
void TIM_SetCompare3(TIM_TypeDef *timer, uint16_t compare);
void TIM_SetCompare4(TIM_TypeDef *timer, uint16_t compare);
#endif
