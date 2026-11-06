#ifndef TEST_STM32F10X_H
#define TEST_STM32F10X_H
#include <stdint.h>
typedef struct { unsigned identity; } GPIO_TypeDef;
extern GPIO_TypeDef test_gpioa, test_gpiob;
#define GPIOA (&test_gpioa)
#define GPIOB (&test_gpiob)
#define GPIO_Pin_10 ((uint16_t)0x0400U)
#define GPIO_Pin_11 ((uint16_t)0x0800U)
#define GPIO_Pin_15 ((uint16_t)0x8000U)
#define GPIO_Mode_IN_FLOATING 1U
#define GPIO_Mode_Out_OD 2U
#define GPIO_Speed_2MHz 2U
#define RCC_APB2Periph_GPIOA 1U
#define RCC_APB2Periph_GPIOB 2U
#define RCC_APB2Periph_AFIO 4U
#define GPIO_Remap_SWJ_JTAGDisable 1U
#define ENABLE 1
typedef enum { Bit_RESET = 0, Bit_SET = 1 } BitAction;
typedef struct { uint16_t GPIO_Pin; unsigned GPIO_Mode, GPIO_Speed; } GPIO_InitTypeDef;
void GPIO_ResetBits(GPIO_TypeDef *port, uint16_t pins);
void GPIO_SetBits(GPIO_TypeDef *port, uint16_t pins);
uint8_t GPIO_ReadInputDataBit(GPIO_TypeDef *port, uint16_t pin);
void GPIO_StructInit(GPIO_InitTypeDef *gpio);
void GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *gpio);
void RCC_APB2PeriphClockCmd(uint32_t clock, int enable);
void GPIO_PinRemapConfig(uint32_t mapping, int enable);
#endif
