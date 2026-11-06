# ESP32-S3 ESP-IDF 调试固件

V0.24 主板，ESP32-S3 N16R8，ESP-IDF 5.4.4。当前源码公开构建标识 `public-servo-speed-20261106-r17`，包含串口桥接、Wi-Fi 自动重连/发现、OV5640、ES8388、横屏表情与触摸、四路 TTL 控制指令转发。

在已激活 ESP-IDF 5.4.4 的终端执行：

```sh
idf.py -B build_public -D SDKCONFIG=sdkconfig.public -D IDF_TARGET=esp32s3 -D ROBOT_PUBLIC_TEST_BUILD=ON build
```

Windows 可用 `tools/build_public_usb.ps1 -EspressifRoot <SDK安装根目录>`。脚本只构建，不自动烧录。首次公开构建不带个人网络凭据，经 USB 配对保存后可自动连接已保存 Wi-Fi。

`Bsp/*.example.h` 是配置模板；对应 `.local.h` 由开发者在本机创建并已忽略提交。源代码、组件许可证和主机 C 测试均保留。组件使用仓库内源码；构建时生成本机依赖锁文件，避免提交旧机器的绝对路径。小智工程的注册表依赖锁则单独保留。

本地网页与启动器已移至 `../HTML-WorkBench/Debug-Terminal/`。其中网页测试在调试终端执行；本目录测试集中依赖 server 的 JavaScript 项应使用调试终端的正式副本。原 `Bsp/battery_dashboard.html` 是嵌入式旧仪表盘，不是当前 8081 网页主入口。

小智版位于 `../ESP32-IDF-Xiaozhi/`，是不同应用和分区布局。构建、烧录与实物硬件验证是不同步骤；保留设备自己的 NVS 与相机校色参数。
