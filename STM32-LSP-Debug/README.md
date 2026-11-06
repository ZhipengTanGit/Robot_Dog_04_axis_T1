# STM32F103RB 标准外设库调试工程

SPL 标准外设库项目，目录按仓库约定命名 `STM32-LSP-Debug`。采用已编译并烧录的 r18 快照，版本 `stm32f103rb-action-teach-20261106-r18`。

- PB6 右后腿、PB7 左后腿、PB8 右前腿、PB9 左前腿；独立 TTL 总线，115200，0–180°。
- 目标角度与速度一起写入并读回核对；到位后保持。TEACH/POSE 支持手动示教和四路姿态，通信失联或停止时释放。
- 在线舵机连续读位置，异常回到发现流程；完整外设日志每 200 ms，电池日志仍每秒输出。
- LED0/PB0 充电呼吸、充满常亮，硬件 PWM；IP5310 名义充电档位可持久保存。
- 无保存记录默认 350 mA，上限 1850 mA。设备自己保存的设定不在源码内。Flash 最后 2 KiB 为设置区 0x0801F800–0x0801FFFF，应用烧录保留该区域。

## 构建

Keil uVision / ARM Compiler 5.06 打开 `Project/main.uvprojx`，或执行：

```powershell
./tools/build.ps1 -KeilPath '你的Keil安装目录/UV4/UV4.exe'
```

输出 `Output/Objects/main.hex`，验证后的应用包在 `Output/firmware/`。脚本不烧录，不依赖旧开发机的 Output 快照目录。

## 主机测试

```powershell
./tests/run_tests.ps1 -Gcc gcc
./tests/run_charge_persist.ps1 -Gcc gcc
./tests/run_ttl_tests.ps1 -Gcc gcc
```

CMSIS、SPL、启动文件和测试桩随源码保留。新构建和主机测试不等于在新板上完成硬件联测；协议名义充电档位不代表实测电流。
