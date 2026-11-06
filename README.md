# 四轴机器狗 T1

ESP32-S3 + STM32F103RB 四轴桌面机器狗的固件、网页工作台和硬件资料。2026-11-06 按以下七个模块整理。

| 目录 | 内容 | 当前状态 |
| --- | --- | --- |
| [ESP32-IDF-Debug](ESP32-IDF-Debug/README.md) | ESP-IDF 调试固件：串口桥接、Wi-Fi、音频、屏幕、摄像头 | 公开构建标识 public-servo-speed-20261106-r17 |
| [ESP32-IDF-Xiaozhi](ESP32-IDF-Xiaozhi/README-ROBOT.md) | 小智语音固件及 V0.24 板卡适配 | 上游 v2.1.0，ESP-IDF 5.4.4 |
| [STM32-LSP-Debug](STM32-LSP-Debug/README.md) | 标准外设库调试固件（SPL，目录按项目要求命名 LSP） | 实板 r18 源码，TTL 调速、示教、快速回读、充电指示 |
| [STM32-HAL-Debug](STM32-HAL-Debug/README.md) | STM32 HAL 调试工程预留位置 | 尚未找到该主板的有效 HAL 工程 |
| [HTML-WorkBench](HTML-WorkBench/README.md) | 本地调试终端和用户网页 | 包含回中、动作示教、四腿模型、速度设置 |
| [Datasheet](Datasheet/README.md) | 芯片、显示、摄像头和舵机协议资料 | 保留原文件及第三方声明 |
| [原理图](原理图/README.md) | 原生 EDA 工程及 V0.24 导出、BOM、网表 | 现有设计归档，未修改电路 |

## 启动本地调试工作台

Python 3.10+：

```sh
cd HTML-WorkBench/Debug-Terminal
python -m pip install -r requirements.txt
```

首次启动按该目录 README 配置本地配对密钥，然后在 Windows 执行：

```sh
python tools/launch_windows.py --mode serial
```

浏览器地址为 http://127.0.0.1:8081/ 。首次 USB 配网后，主板可使用已保存的 Wi-Fi 配置自动连接。源码不包含设备的 Wi-Fi 密码、配对密钥或 NVS 备份。

用户网页独立运行：

```sh
cd HTML-WorkBench/User-Web
npm start
```

打开 http://127.0.0.1:8093/ 。现有公网入口为 https://110.42.234.162/robot-studio/ ；推送 Gitee 不会自动部署公网服务。

## 固件和版本边界

- ESP32 调试版与小智语音版是两个独立应用。小智版保留自己的分区表和依赖锁定；切换前备份自己设备的有效分区，按对应工程完整构建输出烧录。
- STM32 使用已经写入实板的 r18 源码。四路 PB6/PB7/PB8/PB9 分别对应右后腿、左后腿、右前腿、左前腿，角度范围 0–180°。
- STM32 无保存记录时默认名义充电电流 350 mA，板端上限 1850 mA；设备已经保存的个人设定不属于源码。保留 Flash 最后 2 KiB 设置区 0x0801F800–0x0801FFFF，应用烧录避免整片擦除。
- `HTML-WorkBench/User-Web/dist/firmware/` 是历史 2026-10-29 公共下载包，STM32 仍为 PWM 测试版，**不作为当前 TTL 舵机固件使用**。本次整理不替换历史下载包。
- 本地调试页包括最新四腿模型方向映射、快速角度回读、四腿回中和动作帧编辑；回中确认到位后恢复释放扭矩的手动示教状态。

目录迁移说明见 [docs/layout.md](docs/layout.md)，文件清单及 SHA-256 见 [docs/source-manifest.json](docs/source-manifest.json)。项目原许可及第三方许可保持原样。构建输出、运行日志、本地配置和 Flash/NVS 备份不进入仓库。
