# 四轴机器狗 T1 · 网页工作台

机器狗配色、装配指导、爆炸图、动作编舞、彩色 3MF 导出和 USB 硬件测试网站。

- 在线工作台：https://110.42.234.162/robot-studio/
- 硬件测试：https://110.42.234.162/robot-studio/hardware.html

本仓库同步现有公网工作台项目。公网入口沿用现有登录；推送 Gitee 不会自动更新公网服务器。

## 本地运行

安装 Node.js，在项目根目录运行：

```sh
npm start
```

打开 http://127.0.0.1:8093/ 。Windows 也可双击 `启动配色工作台.cmd`。所有网页依赖均已包含，无需执行 `npm install`；不要直接双击 HTML。

## 功能与目录

- `dist/`：网页、13 区域配色模型、25 步装配指导、爆炸图、四轴编舞和 3MF 导出。
- `dist/firmware/`：公共测试固件、SHA-256 清单和烧录说明。
- `tools/serve.mjs`：本地静态服务。
- `tools/verify-*`：网页、串口、烧录器、模型与固件校验。
- `tools/cad_*`：SolidWorks 模型导出与处理工具。
- `tools/deploy_public.py`：现有服务器发布工具，服务器配置需另行提供。
- `使用说明.txt`：详细操作说明。

编舞页面仅预览模型，不直接驱动实物。硬件测试和烧录需要桌面 Chrome/Edge、相应 USB 权限及匹配固件；云服务器不连接用户电脑的 COM 口。

## 固件版本

保留现有公网的 **2026-10-29 公共测试包**，适用于 V0.24 主板的 ESP32-S3 N16R8 和 STM32F103RBT6。STM32 此包仍为 PWM 舵机测试，默认及最高名义充电档 350mA。

本次同步不包含后续本地 8081 调试平台的 TTL 舵机、180° 回读和 1850mA 充电固件。版本、文件哈希和验证范围以 `dist/firmware/catalog.json` 为准。

## 校验

```sh
npm run check
npm run verify:hardware
npm run verify:assembly
npm run verify:choreography
npm run verify:regions
python tools/verify-firmware-catalog.py
```

服务器凭据、私有配置、运行日志、原始 CAD 工程和本地诊断数据不在本仓库中。保留原有 `LICENSE`；第三方许可见 `dist/vendor/`。
