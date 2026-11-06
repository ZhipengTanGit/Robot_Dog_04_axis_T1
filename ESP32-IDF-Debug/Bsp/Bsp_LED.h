#ifndef BSP_LED_H
#define BSP_LED_H

// ESP-IDF 头文件
#include "driver/gpio.h"
#include "esp_err.h"

// 核心板 LED 引脚
#define LED_GPIO_PIN GPIO_NUM_4

// LED 电平：低电平点亮，高电平熄灭
#define LED_ON_LEVEL  0
#define LED_OFF_LEVEL 1

// LED 初始化，返回 GPIO 配置或默认电平设置的结果
esp_err_t LED_Config(void);

// 以下控制接口须在 LED_Config 成功后调用
void LED_ON(void);
void LED_OFF(void);
void LED_Toggle(void);

#endif
