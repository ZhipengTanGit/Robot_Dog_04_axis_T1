STM32F103RBT6 / V0.24 外设测试固件
版本：stm32f103rb-public-test-350ma-20261029-r2
仅通过主板 SWD 的 ST-LINK 烧录，起始地址 0x08000000；HEX 自带地址。
可使用官方 STM32CubeProgrammer 选择 HEX，烧录并读回校验，然后复位运行。
默认并最高名义充电档为350mA，保持轻载不自动关机；上电不自动动作或发声。
电池温度/实际电流/电压未接采样，不提供物理充电安全保证。
UART：USART3 PC10 TX / PC11 RX，115200 8N1，经ESP32桥调试。
仅 public 目录可发布；workspace 与 private 日志包含本地路径等信息，不要上传。
