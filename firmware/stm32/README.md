# STM32F103RB 源码

此目录采用已经编译并写入实板的 `20261103-r13-180deg` 源码快照，不使用根工作目录中较早的启动配置。

- 四路独立 TTL 总线为 PB6、PB7、PB8、PB9；当前实板舵机为 115200bps、各总线 ID 1。
- 舵机位置 0–1023 映射到 0–180°，支持目标角度与扭矩寄存器回读；上电不自动运动。
- 通信租约到期或停止时请求关闭扭矩，并等待回读确认；机械零位仍需校准。
- IP5310 名义充电设置支持保存到 Flash。无有效记录时默认 350mA，配置上限 1850mA；保存区为最后两页 `0x0801F800..0x0801FFFF`。

1850mA 是当前板卡试验配置，并非实测充电电流；支路电流、温度和电芯工作条件仍应以实物验证。历史开发说明位于 `docs/`，实际构建选项以 `Code/Battery_Config.h` 和 `Code/Servo_Config.h` 为准。

## 构建

使用 Keil uVision / ARM Compiler 5.06 打开 `Project/main.uvprojx`，选择 `Target 1` 构建；也可执行：

```powershell
./tools/build.ps1 -KeilPath '你的 Keil 安装目录/UV4/UV4.exe'
```

产物为 `Output/Objects/main.hex`。该脚本不烧录。通过 ST-LINK/SWD 写入应用时需保留末尾设置区，避免整片擦除；BIN 应用起始地址为 `0x08000000`。

## 主机测试

```powershell
./tests/run_tests.ps1 -Gcc gcc
./tests/run_charge_persist.ps1 -Gcc gcc
./tests/run_ttl_tests.ps1 -Gcc gcc
```

CMSIS、标准外设库、启动文件和测试桩均已包含。测试和编译输出不进入版本库；没有包含任何 Flash 全片备份或现场诊断日志。
