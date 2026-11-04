# ESP32-S3 源码

V0.24 主板 / ESP32-S3-WROOM-1-N16R8，使用 ESP-IDF 5.4.4。包含 USB 桥接、Wi-Fi 会话控制、OV5640 摄像头、ES8388 音频、显示和共用 I²C 总线驱动。

源码取自当前 `ESP32_robot_dog` 工作目录，2026-11-04 整理。当前公开构建标识为 `public-usb-jpeg-small-20261102-r1`，与用户网页携带的 2026-10-29 旧固件包不同。

## 构建

在已激活 ESP-IDF 5.4.4 的终端中执行：

```sh
idf.py -B build_public -D SDKCONFIG=sdkconfig.public -D IDF_TARGET=esp32s3 -D ROBOT_PUBLIC_TEST_BUILD=ON build
```

Windows 也可运行 `./tools/build_public_usb.ps1`，使用 `-EspressifRoot` 指定 SDK 安装根目录。构建脚本只编译，不自动烧录或擦除。组件源文件和原许可保留在 `components/`。

`ROBOT_PUBLIC_TEST_BUILD=ON` 构建不包含本地私有配置、不自动联网。需要开发用网络构建时，将 `Bsp/NetworkConfig.example.h`、`Bsp/TelemetryConfig.example.h` 分别复制为对应 `.local.h` 并填入自己的配置；这些文件已忽略，不应提交。

## 主机验证

```powershell
./tests/run_audio_recovery.ps1 -Gcc gcc
```

`tests/` 还保留相机、串口、Wi-Fi 策略与音频协议测试源码。原本依赖同级 `server/` 的网页测试现在应在仓库 `debug-terminal/` 中运行其正式测试集。

ESP32 通过 USB 提供 STM32 桥接，完整板载测试还需要匹配的 STM32 固件。屏幕接入后 ES8388 通信异常仍是现场排查项，不因源码上传而视为已修复。
