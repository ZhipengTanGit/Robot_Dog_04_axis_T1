四足机器狗 V0.24 — 公共单项驱动测试固件 / 2026-10-29

入口：https://110.42.234.162/robot-studio/hardware.html
账号与密码均为 admin。固件下载沿用此登录。

一、适用硬件与验证范围
ESP32-S3 N16R8：16MB Flash / 8MB Octal PSRAM，ESP-IDF 5.4.4。
STM32F103RBT6：128KB Flash / 20KB RAM。
仅适用于本项目 V0.24 接线。这不是小智语音固件，烧录会替换当前程序。
已完成编译和模拟串口、烧录器、页面测试；本次公共版本尚待实板联测。
每个文件的大小与 SHA-256 在 catalog.json 中；网页写入前会校验文件，写入后校验 Flash。

二、网页烧录
使用桌面版 Chrome 或 Edge，通过 HTTPS 打开入口。
先关闭旧测试网站、串口助手和其他占用串口/调试器的程序。
ESP32：接主板 CH340 USB 数据口，断开网页测试连接，选择 ESP32 固件并点击烧录。
浏览器弹出选择框后选择自己的串口；未自动进入下载模式时按主板 BOOT/RESET 的正常下载步骤操作。
网页只写 bootloader@0x0、分区表@0x8000、app@0x10000，不执行整片擦除。
STM32：接 ST-LINK 的 SWDIO、SWCLK、GND 和目标参考电源，主板正常供电后选择 STM32 固件。
Windows 的 ST-LINK USB 驱动若不支持 WebUSB，网页会提示改用 STM32CubeProgrammer；网页不会自动更改系统驱动。
不要把 ESP32 的 CH340 串口当成 STM32 的烧录口。

三、离线烧录
ESP32 完整 ZIP 内含分段 BIN、合并 BIN 和说明。优先使用分段地址，避免合并文件的填充区覆盖原有 NVS 配置。
STM32 可下载 BIN 或 HEX：BIN 起始地址为 0x08000000；HEX 自带地址。
用 STM32CubeProgrammer 通过 ST-LINK/SWD 执行写入与校验，再复位运行。

四、连接并测试
两边烧好后，接 CH340 数据口，点击“连接主板”，串口为 921600 / 8N1。
ESP32 转发 STM32 数据；公网页面直接访问当前电脑授权的 USB，不从云服务器访问你的串口。
支持 I²C 扫描、OV5640 检测与 USB 单帧抓拍、板载麦克风试听、扬声器、蜂鸣器、四路舵机、LED、温湿度/按键与屏幕/TE 状态。
显示器写入成功不能证明屏幕实际亮起；I²C 地址应答不能证明器件所有功能正常。
麦克风试听播放的是板上麦克风数据，不申请电脑麦克风权限。
舵机上电不输出；请从 1500us 中位及较小行程开始手动测试。10秒动作自动停止；切后台、断开或失去通信时停止输出。
初版摄像头提供单帧抓拍，不提供连续网络视频。公共 ESP32 不含私有 Wi-Fi/服务器配置，也不自动联网。

五、电池配置
公共 STM32 固件默认且最高为名义 350mA 档，保留硬件充电许可，禁用轻载自动关闭5V。
该数值为配置读回，不是实测充电电流；实际电压/电流尚无可用采样时显示不可用。
上电不启动蜂鸣器或舵机。公共测试版不接受高于350mA的充电设置。

网页烧录器来源：
Espressif esptool-js 0.7.0（Apache-2.0）：https://github.com/espressif/esptool-js
WebSTLink（MIT，固定版本见 catalog.json）：https://github.com/devanlai/webstlink
对应许可与来源信息随网页 vendor 目录发布。
