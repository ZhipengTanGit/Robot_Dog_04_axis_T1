# 2026-11-06 目录整理

| 原目录 | 新目录 |
| --- | --- |
| firmware/esp32 | ESP32-IDF-Debug |
| 新找到的小智语音工程 | ESP32-IDF-Xiaozhi |
| firmware/stm32 | STM32-LSP-Debug |
| HAL 模块预留 | STM32-HAL-Debug |
| debug-terminal | HTML-WorkBench/Debug-Terminal |
| user-web | HTML-WorkBench/User-Web |
| hardware/datasheets | Datasheet |
| hardware/schematics | 原理图/Exports |
| hardware/eda-source | 原理图/EDA |

调试终端同步最新 2026-11-06 版本，含四腿命名/模型方向、调速、快速反馈和示教回中。STM32 使用 r18 完整源码，ESP32 调试版使用 r17，小智为独立应用。

HAL 未发现有效工程，保留明确说明。未加入个人网络、配对密钥、设备运行数据、录音/相机记录、Flash/NVS 回退镜像、构建缓存或 IDE 工作空间。历史公共固件下载保留在用户网页目录，不能当作当前 TTL 固件。

`source-manifest.json` 记录整理后的文件及哈希；`layout-provenance.json` 记录本次同步源码的相对来源。此整理不部署公网网站，也不执行硬件烧录。
