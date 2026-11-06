# 本机机器狗监测服务

当前主入口是 Windows USB：主板 CH340 数据口 → 本机串口服务 → 浏览器 `http://127.0.0.1:8081/`。基础诊断不需要 Ubuntu、WiFi 配置或额外 USB 转串口工具。ESP32 负责采集和转发，本机负责网页、状态老化和音频播放缓冲。摄像头预览通过网页配网后走 Wi-Fi，电脑主动拉图，不需要 Windows 入站防火墙规则。

## 启动

优先使用项目提供的 USB 桌面入口；启动器负责选串口、依赖、环境及打开网页。底层要求 Python 3.10+，USB 模式额外需要 `pyserial`，网页无 npm 运行依赖。手动运行：

```powershell
python robot_server.py --transport serial --serial-port COM18 --serial-baud 921600 --host 127.0.0.1 --port 8081 --data-dir data-usb
```

`ROBOT_INGEST_TOKEN` 环境变量仍由启动器从本地忽略提交的配置读取，不能放进网页或日志。它用于电脑向 ESP32 数据接口认证，以及保留的网络设备写入接口；USB 模式拒绝设备网络上传。不要同时用串口助手占用 COM18。无设备或串口占用时网页仍启动，服务每两秒重试打开；打开前设置 DTR/RTS 为 false，不主动复位主板。主机端为 **921600 / 8N1**，ESP32 与 STM32 之间仍为 **115200**。

可选 WiFi 模式：`python robot_server.py --transport wifi --host 0.0.0.0 --port 8080 --data-dir data-wifi`，需要同时启用并配置 ESP32 网络上传。两种服务的端口、内存状态和数据目录独立。`robot-monitor.service` 仅供以后 Ubuntu 部署复用。

网页顶部的“USB 串口连接”可以刷新本机端口并选择 COM 口。切换时关闭旧连接，清除旧板遥测、未完成命令和音频会话，再等待新端口的实际数据；端口打开不等于主板在线。当前选择保存到数据目录的 `serial-port.json`。Windows 启动器优先使用显式指定的 COM 口，其次是仍存在的已保存选择，然后自动选择唯一 CH340；无法唯一选择时先启动网页等待手动选择。

## 网页功能

- 电池：显示芯片的电量区间、充电标志及寄存器名义电流。当前板不能实测电池电压和实际充电电流；电池温度线未接，AHT20 的环境温度不能代替电池温度。
- 外设：AHT20、机械按键/触摸、LED/蜂鸣器、四路舵机状态、XL9555、屏幕/触摸、ES8388 音频诊断。舵机显示输出配置，没有角度反馈；发送显示命令不代表实屏正常。
- I²C：页面顶部快捷入口跳到扫描卡片，手动开始扫描 ESP32 GPIO 8/19 总线。只读探测地址应答，不扫描 STM32 总线上的 IP5310 / AHT20。地址映射是预期用途，不是芯片型号验证；音频焊接恢复后可能仍需断电重启初始化。
- 音频：点击“开始试听”后，将板载麦克风 PCM 传给电脑播放，不调用电脑麦克风权限，不依赖板上扬声器。使用单声道 16 kHz PCM16LE；音量控制的是电脑播放增益。芯片 NACK/未初始化时只显示等待，不产生假声音。隐藏或离开页面即停止，不自动恢复。
- 相机：点击“检测摄像头”，依次验证 XL9555 复位/待机控制读回、0x3C 应答、芯片 ID、驱动初始化、真实 JPEG 帧。只有全部完成并验证真实帧才报告通过；地址 ACK 不等于能够成像。无应答时检查 FPC 方向、接触与供电。USB 模式用于取帧检测和配网，预览图像走下述 Wi-Fi 通道。

## 扬声器 / 蜂鸣器测试

打开本机网页的 [扬声器 / 蜂鸣器测试](http://127.0.0.1:8081/#soundTest)。沿用当前 USB 或 Wi-Fi 控制通道，点击对应播放按钮才会让主板发声；打开页面、修改参数和重新连接均不自动播放。

- 扬声器：ES8388 + MS8002A，频率 100–4000 Hz、时长 20–1000 ms、数字幅度 1–20%；默认 **1000 Hz、500 ms、5%**。
- 蜂鸣器：STM32 PA8 的 TIM1_CH1 输出固定 4 kHz、50% 占空比，时长 20–1000 ms；默认 **200 ms**。

两种短测试音均由板端计时停止，也有各自的停止按钮；停止蜂鸣器不会停止舵机，停止扬声器不会关闭麦克风试听。扬声器必须先有有效的音频初始化状态。命令确认只能证明软件处理结果，是否实际发声仍需现场听音；下方“测试电脑声音”只验证电脑播放，与板载扬声器测试不同。

2026-10-27：新的 STM32 蜂鸣器固件已经编译并通过主机测试，文件为 STM32 工程 `Output/2026-10-27-sound/STM32-sound-test.hex`。它会上报 `buzzer_proto=1`，支持带请求 ID 的开始、自动完成、停止和取消应答；目前因 SWD 无法读取核心 ID，**尚未烧录到主板**。ESP32 会根据新鲜 STM32 状态自动选择协议；旧版使用已有 `PERIPH BEEP <ms>`，网页只显示兼容模式已交给 ESP32 转发、结果未确认（`UNCONFIRMED`），不将排队或无 ID 的旧应答当成执行完成。旧固件仍具有短响计时和 `BEEP 0` 独立停止功能。

## PWM 舵机调试

使用 USB 连接，在“舵机调试”中选择一路目标脉宽后明确点击应用。滑杆、数字输入和微调按钮只修改目标，不立即发送；打开页面不会归中或启动输出。当前 STM32 固件使用 TIM4 的 50 Hz PWM，支持 500–2500 μs，页面的 1500 μs 是初始待应用目标。舵机 1–4 分别对应协议通道 0–3、PB6/PB7/PB8/PB9。V0.24 接头为 1 脚信号、2 脚 +5V、3 脚 GND；接线仍需对照实物线序。

“全部停止”发送 `PERIPH STOP`，关闭四路 PWM，也停止蜂鸣器。当前固件没有单路停止命令或通信超时停机机制：**关闭网页、退出服务或拔掉 USB 不会自动关闭已经启用的 PWM**，结束调试前使用全部停止。电池联锁失效会关闭输出，联锁恢复后不自动重启。

后端只在本次 USB 连接及新鲜遥测允许时发送应用命令，单次请求只发一次。等待发送后的 STM32 直接 ACK 和新的 `PERIPH` 输出配置回读后才显示成功；这不代表机械位置已到位。全停会更换控制代号，使此前延迟到达的应用请求失效；页面不自动重试运动请求。切口、断线和设备重启也取消旧请求。

用户提供的型号为 P-0090。2026-10-25 用户提供 Kpower M09YY 控制规格截图（500–2500 μs、中位1500 μs、50–333 Hz），并明确客服确认 P-0090 同样适用。范围据此从原1000–2000 μs扩展至500–2500 μs，保持50 Hz；页面、后端与STM32固件需配套更新。截图未给出角度对应关系，因此不把脉宽直接标为未经标定的0–180°。

## Wi-Fi 摄像头预览

1. 用 USB 连接主板，打开“Wi-Fi 连接”，填写与电脑能互访的 **2.4 GHz** 网络名称与密码。电脑可继续连接同一路由器的 5 GHz 网络。
2. 点击连接，等待取得 IP 并由电脑验证数据接口。成功后打开页面显示的 `http://127.0.0.1:8081/#cameraPanel`。网址指向这台电脑，后台服务需要保持运行。
3. 成功连接的配置保存在主板 NVS；可使用已保存配置重连，或主动断开 Wi-Fi。新密码失败不会覆盖旧配置。密码不进入状态、JSONL 或浏览器本地存储。

电脑只接受新鲜 USB 快照中的私有 IPv4 和设备身份，向固定 81 端口发起带 Bearer 认证的 `GET /api/status`，向 82 端口请求 `/stream.mjpg` 持续图像流，拒绝重定向、身份改变、旧序号和过期帧。浏览器只访问同源本机 `/stream.mjpg` 代理，不接触设备密钥。ESP32 不托管 HTML。

目标 30 帧/秒与实测接收 FPS 分别显示。2026-10-26 固件目标分辨率为 1920×1080，使用原生中心裁剪；相机双帧缓存、ESP32 流发送缓冲、电脑 multipart 解析与单帧接收上限均为每帧 512 KiB。超过上限或不完整的帧会被拒绝，不作为正常画面显示。状态通道每秒更新，图像通道持续接收，不再每 500 ms 请求一张图。电脑只保留最新帧，慢客户端跳过旧帧，避免延迟积累；暂停或切后台终止浏览器连接，画面保留并标灰。81 端口 `/capture.jpg` 留作单帧诊断与旧固件兼容；声明支持 82 端口的固件在流故障时会明确报错，不会悄悄退回低帧率。

已建立的无线画面可在 USB 断开后继续接收；USB 控制仍禁用。ESP32 重启或本机服务重启后，需要重新通过 USB 确认设备身份。Wi-Fi 断开时旧帧不再被当作实时画面。旧版便携 ZIP 未包含这次功能，不能替代更新后的项目服务。

所有数据持续老化：设备五秒未上报则离线，电池/STM 外设三秒过期。网页可保留上次值，但明确当前未确认。服务器在线不代表主板在线。串口断开立即撤销在线状态及未完成控制，重连后必须收到新数据才重新启用。

## 充电控制与容量计算

下拉列出旧版寄存器 0x26 低五位的全部 32 个名义编码：

`mA = 50 + 100*b0 + 200*b1 + 500*b2 + 1200*b3 + 2300*b4`

全部可选择查看。后端下发上限取电芯配置的 `并联数 × 单芯厂家最大充电电流` 与本板 IP5310 标称 **3000 mA** 的较小值，实际对应最高编码 2950 mA。默认 2×700 mAh、单芯 1000 mA，理想总上限 2000 mA，对应最高编码 1950 mA；网页不会自动修改现有 1850 mA 设定。没有 1000 或 1500 mA 编码。

选择后必须点击应用。单次命令最多等待 15 秒，只有匹配 ID 的 STM32 最终 ACK 与新的 `set_mA` / `verified=1` 回读一致，网页才显示确认成功；排队、失败、超时都不冒充已应用。设备离线、数据过期或充电芯片未 READY 时不能应用。重启、重连、过期不会重放旧命令。

电芯配置/容量计算只保存浏览器本地并附于这一次申请，不是硬件检测。固定 1S 并联：mAh 相加、Wh = mAh × 标称 V / 1000；再乘可用比例和转换效率，除输出电压得到输出侧等效 mAh。结果是估算，实际并联可能不均流。**仍按本板单串 4.2 V 锂电配置；标称电压输入不会改变终止电压。** 旧版编码尚未与实装定制批次完整核对，更大理论编码不保证实板可达。[IP5310 官方资料](https://www.injoinic.com/api/static/uploads/20250528/20250528180440_6836dfb8a7f7f.pdf)标称 3 A，散热与板级能力还需实测。

## USB 协议

串口按换行分帧，最大 32 KiB，超长行完整丢弃至下一换行。正常日志和 JSON 行可交错，但固件保证不在一行内部插入其他日志。

主机命令：

```text
ESP STATUS
ESP I2C_SCAN <16位小写hex id>
ESP CAMERA_CHECK <16位小写hex id>
ESP CHARGE_CURRENT <id> <ma> <deadline_ms uint32> <boot_prefix8>
ESP AUDIO_START <16位小写hex session_id> <ttl_ms 1..15000>
ESP AUDIO_STOP <session_id>
ESP WIFI_CONNECT <id> <deadline_ms uint32> <boot_prefix8> <SSID的UTF8小写hex> <密码的UTF8小写hex或->
ESP WIFI_RECONNECT <id> <deadline_ms uint32> <boot_prefix8>
ESP WIFI_DISCONNECT <id> <deadline_ms uint32> <boot_prefix8>
```

STATUS 启动立即请求、通常每三秒请求，待扫描/充电/摄像头检测期间每秒请求。充电截止时间使用最新 ESP32 `uptime_ms` 换算并扣除 300 ms 余量；只用三秒以内的当前 boot 快照，发送前再次校验 pending ID 和剩余时间。固件校验 boot 前缀及设备时钟截止时间，不允许主机暂停后把过期命令“续命”。每个命令 ID 在一次串口连接中最多写一次。

设备回传：

- `BAT t=...` / `PERIPH t=...`：立即记录真实接收时间。ESP_STATUS 不会把已老化的 BAT 重新标成刚收到。
- `BAT_CURRENT_ACK id=... target_mA=... status=APPLIED|FAILED|REJECTED reason=... applied_mA=...`：匹配在途命令，APPLIED 还要等待 BAT 回读。
- `ESP_STATUS {JSON}`：同 WiFi telemetry 字段，logs 为空。包含 device_id / boot_id / seq / uptime_ms、原始行及年龄、camera、esp_peripherals、audio_stream、i2c_scan、uart_bridge。服务器使用本机递增序号和串口会话，并保留 board_device_id / board_boot_id。
- `ESP_AUDIO {"session_id":"...","seq":0,"sample_rate":16000,"channels":1,"format":"s16le","data":"base64..."}`：只接受当前监听会话、递增序号和已初始化音频芯片，每包解码后偶数 2..6400 字节。**音频行绕过所有日志和 JSONL。**

音频使用 15 秒租约，浏览器每五秒续约，桥接端每五秒向 ESP32 更新剩余时间。停止时发送匹配 ID 的 STOP；连接中断清队列及租约。主机突然退出时设备租约自动到期。

## HTTP API 与数据保护

页面控制要求同源 Origin、JSON、HttpOnly / SameSite=Strict 会话 cookie 和 `X-CSRF-Token`。`GET /api/session` 生成随机页面会话，与设备 ingest token 无关。这是可信本机/局域网应用的跨站请求保护，不是多用户账户系统。USB 默认绑定 localhost；局域网模式不要直接暴露到公网。

- `GET /api/status`：状态、设备年龄、`server_id:robot-monitor`、transport、serial_port/serial_baud/serial_connected、充电/扫描/音频控制状态。
- `GET /api/serial-ports`：本机实际端口列表和当前选择、打开状态、切换状态及错误。`POST /api/serial-port` 接收 `{port:"COM18"}`，只允许当前枚举列表中的设备；202 表示接受切换，不表示已连上主板。
- `POST /api/servo`：全停为 `{action:"stop"}`，USB / Wi-Fi 均支持；`CONFIRMED` 表示匹配 ACK 和后续 PWM 配置回读一致。旧的 `{channel,pulse_us,expected_epoch}` 应用接口只兼容 USB；网页改用实时接口。
- `POST /api/servo-live`：`{action:"set",channel:0..3,pulse_us:500..2500,expected_epoch}`；`renew` / `release` 只携带 action 和 expected_epoch。按通道合并目标，最高 10 Hz，单浏览器会话持有，板端 1500 ms 未续约停止输出。
- `POST /api/servo-test`：`{mask:1..15,min_us,max_us,expected_epoch}`，板端执行固定 10 秒、每 2 秒往返，完成关闭 PWM；页面默认仅选通道 1、500–2500 µs，另有 1300–1700 µs 小范围预设。新固件每半周期 700 ms 移动、300 ms 端点停留，网页按 `test_ramp_ms/test_hold_ms` 显示，旧固件缺字段仍标 1000/0。进度/结果以 STM32 回报为准，不用电脑计时冒充执行完成；跨 boot 历史明确标记，不能复活旧 ACK。
- `GET /api/logs`：最近二十条日志快照。
- `POST /api/charge-current`：`{ma,profile}`，严格验证全部七项配置和上限。
- `POST /api/i2c-scan`：`{}`，在线时创建一个 15 秒扫描 ID。完成结果必须匹配 ID，超时不重发。
- `POST /api/camera-check`：`{}`，在线时创建一个 30 秒检测 ID，等待匹配 ID 的分阶段 `camera_check` 结果。重复点击拒绝、旧结果不确认、断线/换 boot 撤销。`completed` 只代表检测结束；`PASSED` 要求控制/应答/ID/初始化/真实帧全部通过且尺寸、字节有效。`preview_transport` 单独表示有无图传，不改写真实 `camera.ready` / `camera.state`。
- `POST /api/audio-monitor`：`{enabled:true|false}`，一次只允许一个浏览器会话监听。
- `POST /api/wifi`：`{action:"connect",ssid,password}`，或 `{action:"reconnect"|"disconnect"}`。配网只走新鲜 USB 会话，命令截止约 5 秒，连接过程单独等待最多 45 秒；返回排队不代表已连上，最终结果需匹配请求 ID。`wifi` 表示主板连接状态，`wifi_control` 表示控制结果，`camera_wifi` 表示电脑实际读取到的数据接口状态。
- `GET /api/audio?session=<id>&after=<seq>`：需对应浏览器 cookie；返回下一包 PCM 或 204，无重复播放。只缓存最多五包/约一秒，不保存录音；落后时丢旧数据追上实时。

WiFi 模式设备接口：Bearer 认证的 `POST /api/telemetry`（64 KiB）、`/api/frame`（JPEG 512 KiB）、`/api/audio`（PCM 6400 bytes）。JSON 拒绝非有限数、重复字段、过深结构、重复/乱序 seq、旧 boot 重放。遥测响应含 `command`、`i2c_scan` 和 `audio_monitor`。JPEG 与 PCM 头需要当前 `X-Device-Id` / `X-Boot-Id`；PCM 还需会话和格式头。USB 模式全部拒绝设备网络上传，防止两个数据源互相覆盖。

真实遥测快照按 JSONL 记录，8 MiB 滚动，当前加三个备份约 32 MiB。图片、PCM 和密钥不落盘。写入失败显示 `recording_error`，内存监测继续。不会从历史文件恢复假在线状态。

## 验证

### Wi-Fi 全量通讯（2026-10-25）

仍使用本机 `http://127.0.0.1:8081/`。USB 完成首次配网后，电脑主动轮询 ESP32 的认证接口；无需 Windows 入站防火墙规则。新鲜 Wi-Fi 数据自动优先，`connection.active` 显示实际数据源。电池、外设、日志、充电设置、I²C 扫描、摄像头检测、舵机控制和麦克风均可经 Wi-Fi；USB 是备用与初始配网通道。

配对 IP / device_id / boot 保存在数据目录 `wifi-pairing.json`，同设备重启可恢复，旧动作不重发。路由器更换 IP 或首次配对时接回 USB 更新地址。拔 USB 期间板子需由电池保持供电；停止 PWM 不等于切断舵机电源。要验证电池单独带载，也应断开 ST-LINK 等外部供电。

ESP32 81 端口新增 `POST /api/control`：`{id:<16hex>,boot_id,deadline_ms,line}`，仅允许白名单命令。HTTP accepted 只表示接收，实际执行还须 ACK / 状态回读；boot、截止时间和 STM32 递增 ID 阻止旧动作复活。`GET /api/audio?session=<16hex>` 返回当前会话真实 PCM JSON 或 204，音频不落盘。82 端口视频流保持不变。

Wi-Fi 专用 STOP 槽不受普通队列占用影响；USB 半行断线超过 250 ms 后先丢弃半行，再继续无线指令。实时控制失联后板端超时停止；手动启动的 10 秒测试由板端独立结束，可显式 STOP 提前取消。

```text
python -m unittest discover -p "test_*.py"
node test_dashboard.js
node test_camera.js
```

测试仅使用合成数据/localhost 临时端口，包括充电 ACK 与回读、全部 32 档及容量单位、浏览器 CSRF、串口分片/上限/老化/重连、boot/seq 去重、扫描 TTL、USB PCM 和不落盘、WebAudio 停止/隐藏/队列，以及相机/状态轮询不闪烁。实机验证由主任务统一启动并记录。

2026-10-27 实测记录：ESP32声音版已通过COM23烧录并校验；通过当前Wi-Fi链路完成扬声器播放及停止（错误0、关闭功放读回成功），旧版蜂鸣器有BEEP ok=1应答且停止后buzzer=0。用户确认扬声器和蜂鸣器都有声音。STM32新回执固件仍未烧录，蜂鸣器继续按旧版兼容显示未确认。完整记录见 `../review-output/2026-10-27-sound-test/result.json`。
