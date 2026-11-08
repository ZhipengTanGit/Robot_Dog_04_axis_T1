# V0.24 机器狗小智语音与外设

基于 78/xiaozhi-esp32 v2.1.0（`213117ded24f4a41ae6b90cf146f92a4f014d2c0`），使用 ESP-IDF 5.4.4。2026-11-08 外设整合 r2 已烧录，用户确认语音对话、板载温湿度、屏幕表情和语音身体动作正常。配套 STM32 源码为本仓库的 [R23 标准库工程](../STM32-LSP-Debug/README.md)。

## 功能

- “你好小智”唤醒并与官方服务对话，ES8388 麦克风/扬声器，首次音量 30%，后续恢复用户保存值。
- 读取本机 AHT20 温度与相对湿度；传感器由 STM32 采集，经 UART1 上报。失效或过期时明确返回不可用，不以网络天气代替。
- CO5300 屏幕横屏动态眼睛，CST9217 触摸切换、头部铜箔舒服表情、对话情绪同步；主动指定表情避免被回复结束的 neutral 立即覆盖。
- 语音控制前进、后退、左转、右转、回中、撒娇和停止，复用 STM32 轨迹与电源/反馈联锁。方向动作默认 3 秒，可设 1–10 秒；撒娇固定 6 秒；回中保持指定时间后释放。握手尚未部署。

例句：“读取当前室内的温度和湿度”“显示开心的表情”“向前走一秒”“向左转一秒”“回中”“做一个撒娇动作”“停止动作”。首次测身体动作请托起机器狗。

当前小智版未集成摄像头和旧网页调试协议。ESP32 调试版与小智版是两种独立应用；STM32 仍负责舵机、供电和头部触摸。

## 构建与首次使用

硬件：ESP32-S3，16 MiB Flash，8 MiB Octal PSRAM。安装 ESP-IDF 5.4.4 并进入其开发终端，在本目录执行：

```sh
idf.py -B build_robot -D IDF_TARGET=esp32s3 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.robot" build
idf.py -B build_robot -p YOUR_PORT flash
```

Windows 可使用 `tools/build_voice.ps1 -IdfPath <ESP-IDF-5.4.4目录>` 构建。`dependencies.lock` 固定注册表依赖。首次从调试版切换小智版需要使用完整构建输出，因为分区表不同；先备份当前有效分区和个人配置。后续同分区升级按构建输出处理，不清除 NVS。

公开源码没有预置任何人的 Wi-Fi、账号或激活信息。首次启动按官方小智热点配网流程联网，在 [xiaozhi.me](https://xiaozhi.me) 绑定自己的设备，再说“你好小智”。固件可脱离电脑，通过普通电源线供电并使用已保存 Wi-Fi。

## 代码入口与接口

`main/boards/robot-dog-v024/`：板卡、ES8388 音频、屏幕适配、STM32 通信及 MCP 工具。

`components/robot_peripherals/`：共享 I²C、CO5300 QSPI/DMA/TE、CST9217 与原动态表情逻辑。音频和触摸共用同一总线锁，XL9555 使用原子读改写保留其他输出。

| MCP 工具 | 用途 |
| --- | --- |
| `self.environment.get_temperature_humidity` | 本地传感器读数与有效性 |
| `self.robot.get_motion_status` | STM32 在线、联锁、动作状态 |
| `self.robot.perform_action` | `action`: forward/backward/left/right/pet/center/stop；`duration_seconds`: 1–10 |
| `self.screen.set_expression` | neutral/curious/happy/sleepy/comfort |

动作使用单调命令编号、STM32 时钟时间戳、应答和 400 ms 续租；STM32 1500 ms 租约失联后停止，ESP32 也限制持续时间。接受命令与动作完成分开报告，拒绝原因直接返回。

串口是 **115200 / 8N1**，支持 `status`、`chat`、`volume 0..100`、`wifi-config`、`environment`、`robot-status`、`display-status`、`expression happy`、`action stop`。旧调试版 921600 波特率和网页控制服务不适用。

## 验证

主机有 C/C++17 编译器并安装 ESP-IDF 后：

```sh
python tests/run_host_tests.py --idf "$IDF_PATH"
```

也可传 `--cc` 和 `--cxx` 指定编译器。覆盖串口数据/读数有效性、动作应答/拒绝/超时/续租/重启，以及屏幕导航、渲染、旋转、TE/DMA 和恢复策略。

[本轮验证说明](../docs/validation-20261108.md) 与 [结果摘要](../docs/validation-20261108.json) 包含来源、实板测试与限制。个人 Wi-Fi 头文件、Flash/NVS 备份、对话日志、构建镜像和缓存不公开上传；需要恢复时使用自己的备份。
