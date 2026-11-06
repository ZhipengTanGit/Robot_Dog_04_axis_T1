#include "Bsp_LED.h"

// LED 初始化
esp_err_t LED_Config(void)
{
    gpio_config_t gpio_led_config = {0};

    // 配置引脚
    gpio_led_config.pin_bit_mask = (1ULL << LED_GPIO_PIN);

    // 同时启用输入，供 Toggle 读取引脚电平
    gpio_led_config.mode = GPIO_MODE_INPUT_OUTPUT;

    // 关闭内部上下拉和中断
    gpio_led_config.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_led_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_led_config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&gpio_led_config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // 默认关闭 LED，同时向应用传播电平设置的错误
    return gpio_set_level(LED_GPIO_PIN, LED_OFF_LEVEL);
}

// LED 点亮
void LED_ON(void)
{
    gpio_set_level(LED_GPIO_PIN, LED_ON_LEVEL);
}

// LED 熄灭
void LED_OFF(void)
{
    gpio_set_level(LED_GPIO_PIN, LED_OFF_LEVEL);
}

// LED 状态翻转
void LED_Toggle(void)
{
    gpio_set_level(LED_GPIO_PIN, !gpio_get_level(LED_GPIO_PIN));
}


