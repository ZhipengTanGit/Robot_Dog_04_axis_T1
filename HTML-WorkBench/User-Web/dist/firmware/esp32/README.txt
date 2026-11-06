# ESP32-S3 USB 单项驱动测试固件

版本：public-usb-20261029；ESP-IDF v5.4.4。用于机器狗 V0.24 的 ESP32-S3-WROOM-1-N16R8（16MB Flash、8MB Octal PSRAM）。需要实装 PSRAM；不适用于普通 ESP32、ESP32-C3 或缺少 PSRAM 的 S3 板。

此固件提供统一的 USB 测试通道，网页可分别测试 I²C、摄像头、麦克风、扬声器、屏幕/触摸状态，以及经 STM32 串口桥接的电池、环境、按键、LED、蜂鸣器和舵机。它不是小智语音对话固件；STM32 项目需要配套测试固件。

默认不连接 Wi-Fi、不读取旧 NVS 凭据自动重连、不向任何服务器上传数据。未编入私有 Wi-Fi 名称、密码、服务器地址或令牌。USB 显式配网可用于连接诊断，但此版没有网络数据接口认证令牌，摄像头图像使用手动 USB 单张抓图。

## 烧录文件

优先使用三个分段文件，Flash 模式 DIO、频率 80MHz、大小 16MB：

| 地址 | 文件 |
|---|---|
| 0x0000 | bootloader.bin |
| 0x8000 | partition-table.bin |
| 0x10000 | esp32-robot-dog-public-usb.bin |

flash-manifest.json 可用于支持 ESP32-S3 的网页烧录器。分段烧录不主动擦除 0x9000–0xEFFF 的 NVS；不要把应用文件单独写入地址 0。

命令行示例（先选择正确串口，关闭其他占用该口的程序）：

```text
python -m esptool --chip esp32s3 --port COMxx --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 esp32-robot-dog-public-usb.bin
```

esp32s3-public-usb-merged.bin 从地址 0 写入，包含分段之间的空白区，会覆盖该区内原有 NVS。保留原配置时使用上述分段文件。所有文件的 SHA256 见 checksums.json。

## 网页测试

运行期连接主板 CH340 USB 数据口，串口参数 921600 / 8N1，不使用 ESP32 原生 USB/JTAG 通道。网页需要单独获取本机串口权限；烧录和测试不能同时占用同一个串口。连接并不会自动转动舵机、播放测试音或发送麦克风/相机图像。

摄像头“检测”只验证初始化和真实 JPEG，点击“抓取一张”才传输图像。单张最多 512KiB、15秒期限，传输期间可穿插完整遥测行；网页必须验证请求 ID、块序号、总长度和 CRC32 后显示。此版不是 USB 视频流，不能以单张抓图速度当作摄像头帧率。

麦克风试听输出板载 ES8388 的真实单声道 16kHz PCM16LE，按用户操作申请/续租，停止后不再传输，不保存录音。扬声器、蜂鸣器、舵机测试均需手动启动，具体命令及回包见 USB-PROTOCOL.md。

屏幕写入/TE、I²C ACK、输出配置等软件状态不能证明实际画面、实测电压/电流、舵机机械角度或实际发声。当前版本完成 SDK 构建和主机协议测试，未烧录到现场硬件验证；release.json 中保留该状态。

## 重建

在 ESP32_robot_dog 源码项目中运行 tools/build_public_usb.ps1。它使用已安装的 ESP-IDF 5.4.4，输出 build_public，并单独使用 sdkconfig.public 和 build_public/dependencies.lock；不修改私有 local.h，也不执行烧录或擦除。公开开关为 ROBOT_PUBLIC_TEST_BUILD=ON，默认私有工程构建仍保持原行为。tools/package_public_usb.py 校验 SDK、构建标识、私有字面值扫描后生成本包。
