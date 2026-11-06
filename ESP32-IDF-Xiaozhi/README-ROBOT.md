# V0.24 机器狗小智语音工程

上游：78/xiaozhi-esp32 v2.1.0，基线提交 `213117ded24f4a41ae6b90cf146f92a4f014d2c0`；使用 ESP-IDF 5.4.4。`main/boards/robot-dog-v024/` 是该主板的 ES8388、功放与串口适配，`dependencies.lock` 固定已解析依赖。

```sh
idf.py -B build_robot -D IDF_TARGET=esp32s3 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.robot" build
```

Windows 也可执行 `tools/build_voice.ps1 -IdfPath <ESP-IDF 5.4.4目录>`。

本工程用于官方小智服务的语音对话；不启动原摄像头、屏幕调试或机器狗运动控制。串口为 115200，支持 status/chat/volume/wifi-config，和调试版 921600 协议不同。

未带入 `wifi_seed.local.h`、设备激活信息、用户网络配置、Flash 备份和构建缓存。首次联网使用小智热点配网，在 xiaozhi.me 绑定自己的设备。上游 README、LICENSE 和硬件适配说明 PORTING.txt 保留。

语音版与调试版分区表不同，切换需使用对应完整构建输出。历史开发机回退镜像未公开；如需回退，应自行备份当前设备的有效分区及个人配置。本仓库不提供带他人配置的镜像。
