# 调试终端

本机 8081 调试平台，包含 USB/Wi-Fi 连接、七个功能页面、摄像头、音频、电源、总线和 TTL 舵机测试。该目录与公网用户工作台独立，网页由本机 Python 服务提供。

## 启动

安装 Python 3.10 或更新版本，在本目录执行：

```sh
python -m pip install -r requirements.txt
python server/robot_server.py --host 127.0.0.1 --transport serial --serial-port COM15 --port 8081
```

将 `COM15` 改成实际 CH340 串口，默认波特率 921600。打开 http://127.0.0.1:8081/ 。先关闭其他占用同一串口的软件。停止服务可按 Ctrl+C。

Windows 可使用 `python tools/launch_windows.py --mode serial` 自动发现串口并打开页面；添加 `--stop` 停止该启动器创建的服务。运行状态在 `server/data-windows/`，不进入版本库。

Wi-Fi 配网经 USB 完成；板卡和电脑需在同一局域网。网络凭据由用户现场输入，本仓库不带 Wi-Fi 密码、设备会话令牌或历史遥测。

## 校验

在本目录执行：

```sh
python -m unittest discover -s server -p "test_*.py"
node server/test_dashboard.js
node server/test_workspace.js
node server/test_camera.js
node server/test_sound_ui.js
```

`server/README.md` 保留早期部署说明；当前入口、命令和布局以本文件为准。本次整理没有启动串口、下发动作或重新烧录板卡。
