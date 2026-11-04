# 四轴机器狗 T1

四轴机器狗的用户网页、本地调试终端、ESP32/STM32 固件源码和硬件设计资料。

- 在线工作台：https://110.42.234.162/robot-studio/
- 硬件测试：https://110.42.234.162/robot-studio/hardware.html

公网入口沿用现有登录；推送 Gitee 不会自动更新公网服务器。2026-11-04 按五个模块整理本地已找到的资料。

## 本地运行

用户网页：安装 Node.js，在仓库根目录运行：

```sh
cd user-web
npm start
```

打开 http://127.0.0.1:8093/ 。Windows 也可双击 `user-web/启动配色工作台.cmd`。所有网页依赖均已包含，无需执行 `npm install`；不要直接双击 HTML。

本地调试终端：安装 Python 3.10+，按 [调试终端说明](debug-terminal/README.md) 安装 pyserial 并启动，默认网页地址为 http://127.0.0.1:8081/ 。调试服务不会随用户网页自动启动。

## 功能与目录

| 模块 | 目录 | 内容 |
| --- | --- | --- |
| 用户网页 | [user-web](user-web/使用说明.txt) | 公网工作台、配色模型、装配指导、编舞与 3MF 导出 |
| 调试终端 | [debug-terminal](debug-terminal/README.md) | Python 服务、七个调试页面、USB/Wi-Fi 与板载测试 |
| ESP32 代码 | [firmware/esp32](firmware/esp32/README.md) | ESP-IDF 5.4.4、驱动、桥接、组件与测试 |
| STM32 代码 | [firmware/stm32](firmware/stm32/README.md) | Keil 工程、TTL 舵机 180° 控制/回读、充电设置持久化 |
| 硬件设计 | [hardware](hardware/README.md) | V0.24 原理图、BOM、网表、EDA 工程和芯片手册 |

[来源清单](docs/source-manifest.json) 记录复制文件的相对来源、大小与 SHA-256。

编舞页面仅预览模型，不直接驱动实物。硬件测试和烧录需要桌面 Chrome/Edge、相应 USB 权限及匹配固件；云服务器不连接用户电脑的 COM 口。

## 固件版本

`user-web/dist/firmware/` 保留现有公网的 **2026-10-29 公共测试包**，适用于 V0.24 主板的 ESP32-S3 N16R8 和 STM32F103RBT6。STM32 此包仍为 PWM 舵机测试，默认及最高名义充电档 350mA。

`firmware/` 为后续开发源码：ESP32 公开构建标识为 `public-usb-jpeg-small-20261102-r1`；STM32 使用实板 r13-180deg 源码快照，支持 TTL 控制/回读和最高 1850mA 名义设置持久化，无保存记录时默认 350mA。上传源码不会替换用户网页中的旧固件下载。

## 校验

```sh
cd user-web
npm run check
npm run verify:hardware
npm run verify:assembly
npm run verify:choreography
npm run verify:regions
python tools/verify-firmware-catalog.py
```

调试终端及固件测试命令见各模块 README。此次整理不连接硬件、不执行烧录，也不把源码编译或文件校验等同于新的实物联测。

服务器凭据、私有 Wi-Fi 配置、运行日志、原始机械 CAD、大型例程和 Flash 全片备份未上传。保留原有 `LICENSE`；第三方组件和器件手册保留各自的许可与声明。
