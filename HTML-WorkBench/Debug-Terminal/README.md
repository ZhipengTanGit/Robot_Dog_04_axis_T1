# 本地机器狗调试终端

Python 3.10+；Windows USB 需要 pyserial。包含设备连接、摄像头、音频、TTL 舵机、动作示教、电源、总线和日志页面。

```sh
python -m pip install -r requirements.txt
```

## 本地配对密钥

复制 `config/TelemetryConfig.example.h` 为 `config/TelemetryConfig.local.h`。使用 `python -c "import secrets; print(secrets.token_hex(32))"` 生成本机随机密钥，将模板中 ROBOT_INGEST_TOKEN 的值替换为 64 位小写十六进制字符串。该 `.local.h` 文件已忽略提交。

也可使用环境变量 `ROBOT_INGEST_TOKEN`，或复用仓库 `ESP32-IDF-Debug/Bsp/TelemetryConfig.local.h`。优先级为环境变量、本终端配置、ESP32 私有配置。已有设备需使用原配对密钥，或通过 USB 重新配对 Wi-Fi；不要上传私有配置。

Windows：

```sh
python tools/launch_windows.py --mode serial
```

自动识别或复用串口，打开 http://127.0.0.1:8081/ 。`--serial-port COM18` 可显式指定；`--stop` 只停止该启动器的服务。运行数据保存在 `server/data-windows/`。

跨平台手动入口：

```sh
python server/robot_server.py --host 127.0.0.1 --transport serial --serial-port COM18 --port 8081
```

手动入口需事先设置 `ROBOT_INGEST_TOKEN`。将 COM18 改成实际串口；电脑侧 921600，主板 STM32 侧 115200。Wi-Fi 与 USB 均可控制已配对的同一设备，首次配网通过 USB 完成。

## 当前行为

四腿角度回读和模型显示；很慢/慢速/标准/快速/更快五档手动运动速度；到位后保持。动作页支持释放扭矩示教、记录/编辑/导入/导出动作帧，以及四腿回中（90°）。回中确认到位后自动释放扭矩，继续手动示教；不会写入额外动作帧。模型显示方向校正与原始实物角度、保存动作数据分开处理。

## 软件检查

```sh
python -m unittest discover -s server -p "test_*.py"
python -m unittest discover -s tools -p "test_*.py"
node tests/test_robot_pose.cjs
node tests/test_servo_hold.cjs
node tests/test_servo_hold_worker.cjs
node server/test_dashboard.js
node server/test_workspace.js
```

软件验证不等于新的硬件联测。源码不带运行日志、个人动作文件、Wi-Fi 密码、设备令牌和 Flash 备份。
