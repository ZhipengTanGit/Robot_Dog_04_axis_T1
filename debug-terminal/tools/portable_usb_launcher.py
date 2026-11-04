#!/usr/bin/env python3
"""Portable USB monitor: no project installation, network or saved credentials required."""
import argparse
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import socket
import sys
import threading
import time
import webbrowser

MANAGEMENT_PATH = "/__portable/identity"
STOP_PATH = "/__portable/stop"
PORTABLE_ID = "robot-portable-usb"


def package_id(root):
    return hashlib.sha256(os.path.normcase(str(Path(root).resolve())).encode("utf-8")).hexdigest()


class PackageLock:
    """The lock is held for the whole server lifetime, including device waiting."""
    def __init__(self, path):
        self.path, self.file = Path(path), None

    def acquire(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        handle = self.path.open("a+b")
        if handle.tell() == 0:
            handle.write(b"0")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            handle.close()
            return False
        self.file = handle
        return True

    def release(self):
        if self.file is not None:
            self.file.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.file.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.file, fcntl.LOCK_UN)
            self.file.close()
            self.file = None


def read_metadata(path, expected_package):
    try:
        path = Path(path)
        if path.stat().st_size > 16384:
            return None
        data = json.loads(path.read_text(encoding="utf-8"))
        if (not isinstance(data, dict) or data.get("package_id") != expected_package
                or type(data.get("port")) is not int or not 1 <= data["port"] <= 65535
                or type(data.get("pid")) is not int or data["pid"] <= 0
                or not isinstance(data.get("instance_id"), str)
                or not re.fullmatch(r"[a-f0-9]{32}", data["instance_id"])
                or not isinstance(data.get("stop_token"), str)
                or not re.fullmatch(r"[a-f0-9]{64}", data["stop_token"])):
            return None
        return data
    except (OSError, ValueError, TypeError):
        return None


def write_metadata(path, data):
    path = Path(path)
    temporary = path.with_name(path.name + "." + secrets.token_hex(4) + ".tmp")
    try:
        temporary.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def management_request(metadata, stop=False):
    connection = http.client.HTTPConnection("127.0.0.1", metadata["port"], timeout=1)
    try:
        connection.request("POST" if stop else "GET", STOP_PATH if stop else MANAGEMENT_PATH,
                           body=b"" if stop else None, headers={"X-Portable-Token": metadata["stop_token"]})
        response = connection.getresponse()
        if response.status != 200:
            return None
        body = response.read(8193)
        if len(body) > 8192:
            return None
        data = json.loads(body)
        if (not isinstance(data, dict) or data.get("server_id") != PORTABLE_ID
                or any(data.get(key) != metadata[key] for key in ("package_id", "instance_id", "pid"))):
            return None
        return data
    except (OSError, ValueError, http.client.HTTPException):
        return None
    finally:
        connection.close()


def open_page(port):
    webbrowser.open(f"http://127.0.0.1:{port}/", new=2)


def describe_port(port):
    return f"{port.device}  {getattr(port, 'description', '') or '未提供设备名称'}"


def is_ch340(port):
    description = (getattr(port, "description", "") or "").upper()
    return getattr(port, "vid", None) == 0x1A86 or "CH340" in description or "CH341" in description


def enumerate_ports():
    from serial.tools import list_ports
    return sorted(list_ports.comports(), key=lambda item: item.device)


def list_ports_command(ports):
    if not ports:
        print("未发现串口。请插入主板 CH340 USB 数据口。", flush=True)
    for port in ports:
        print(("[CH340/WCH] " if is_ch340(port) else "            ") + describe_port(port), flush=True)


class PortSupervisor(threading.Thread):
    """Wait for a device without preventing the local page or stop command."""
    def __init__(self, server, bridge_factory, requested=None, ports=enumerate_ports, prompt=input, interactive=None):
        super().__init__(name="portable-usb-discovery", daemon=True)
        self.server, self.bridge_factory, self.requested = server, bridge_factory, requested
        self.ports, self.prompt = ports, prompt
        self.interactive = bool(sys.stdin and sys.stdin.isatty()) if interactive is None else interactive
        self.stopping = threading.Event()
        self.bridge = None
        self.bridge_lock = threading.Lock()
        self.last_notice = None
        self.prompted = None

    def notice(self, message):
        self.server.state.serial_connection(False, message)
        if message != self.last_notice:
            print(message, flush=True)
            self.last_notice = message

    def step(self):
        if self.bridge is not None or self.stopping.is_set():
            return
        selected = self.requested
        if not selected:
            matches = [port for port in self.ports() if is_ch340(port)]
            if not matches:
                self.notice("等待 CH340 USB 设备；插入后会自动连接。")
                self.prompted = None
                return
            if len(matches) == 1:
                selected = matches[0].device
            else:
                self.notice("发现多个 CH340/WCH 设备，请在此窗口选择；也可使用 --serial-port COM号。")
                signature = tuple(port.device for port in matches)
                if self.prompted != signature:
                    self.prompted = signature
                    for index, port in enumerate(matches, 1):
                        print(f"  {index}. {describe_port(port)}", flush=True)
                    if self.interactive:
                        try:
                            choice = self.prompt("输入编号或上述串口名（回车继续等待）：").strip()
                        except (EOFError, OSError):
                            choice = ""
                        if choice.isdecimal() and 1 <= int(choice) <= len(matches):
                            selected = matches[int(choice)-1].device
                        else:
                            selected = next((port.device for port in matches if port.device.casefold() == choice.casefold()), None)
                        if choice and selected is None:
                            print("选择无效，请重新输入列表中的编号或串口名。", flush=True)
                        if selected is None:
                            self.prompted = None
                if not selected:
                    return
        if self.stopping.is_set():
            return
        with self.server.state.lock:
            self.server.state.serial_port = selected
        print(f"连接 {selected}，921600 baud；请勿同时用串口助手占用此端口。", flush=True)
        with self.bridge_lock:
            if self.stopping.is_set():
                return
            self.bridge = self.bridge_factory(self.server.state, selected, 921600)
            self.bridge.start()

    def run(self):
        while not self.stopping.is_set() and self.bridge is None:
            try:
                self.step()
            except Exception as error:
                self.notice(f"串口发现暂时失败：{type(error).__name__}；将自动重试。")
            self.stopping.wait(1)

    def stop(self):
        self.stopping.set()
        with self.bridge_lock:
            if self.bridge is not None:
                self.bridge.stop()
        if self.is_alive():
            self.join(timeout=1.5)


def create_service(monitor, root, preferred_port=8081):
    """Bind exclusively to localhost; never reuse or terminate a foreign listener."""
    identity = {"server_id":PORTABLE_ID,"package_id":package_id(root),"instance_id":secrets.token_hex(16),"pid":os.getpid()}
    stop_token = secrets.token_hex(32)

    class PortableHandler(monitor.RobotHandler):
        def management(self, stopping=False):
            self.close_connection = True
            supplied = self.headers.get_all("X-Portable-Token", [])
            if (len(supplied) != 1 or not supplied[0].isascii()
                    or not hmac.compare_digest(supplied[0], self.server.portable_token)):
                self.send_json(403, {"error":"Invalid portable instance token"})
                return
            result = dict(self.server.portable_identity, port=self.server.server_address[1], serial_port=self.server.state.serial_port)
            self.send_json(200, result)
            if stopping:
                threading.Thread(target=self.server.shutdown, name="portable-stop", daemon=True).start()

        def do_GET(self):
            if self.path == MANAGEMENT_PATH:
                self.management()
            else:
                super().do_GET()

        def do_POST(self):
            if self.path == STOP_PATH:
                self.management(stopping=True)
            else:
                super().do_POST()

    class PortableServer(monitor.RobotServer):
        allow_reuse_address = False

        def server_bind(self):
            if os.name == "nt" and hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            super().server_bind()

    state = monitor.TelemetryState(data_dir=Path(root)/"data", transport="serial", serial_baud=921600)
    # Token is runtime-only; serial mode does not accept device HTTP uploads.
    ingest_token = secrets.token_urlsafe(48)
    candidates = [0] if preferred_port == 0 else list(range(preferred_port, min(65536, preferred_port+16)))+[0]
    server, last_error = None, None
    for port in candidates:
        try:
            server = PortableServer(("127.0.0.1", port), ingest_token, state)
            break
        except OSError as error:
            last_error = error
            if error.errno not in (13, 48, 98, 10013, 10048) and getattr(error, "winerror", None) not in (10013,10048):
                close_recorders(state)
                raise
    if server is None:
        close_recorders(state)
        raise last_error
    server.RequestHandlerClass = PortableHandler
    server.portable_identity, server.portable_token = identity, stop_token
    metadata = dict(identity, port=server.server_address[1], stop_token=stop_token)
    return server, metadata


def close_recorders(state):
    if state.logger:
        for handler in state.logger.handlers:
            handler.close()


def existing_instance(state_file, root, attempts=1):
    for attempt in range(attempts):
        metadata = read_metadata(state_file, package_id(root))
        if metadata and management_request(metadata):
            return metadata
        if attempt+1 < attempts:
            time.sleep(0.2)
    return None


def run(args, root=None):
    root = Path(root).resolve() if root is not None else Path(__file__).resolve().parent.parent
    state_dir = root/"run"
    state_file = state_dir/"instance.json"
    if args.list_ports:
        list_ports_command(enumerate_ports())
        return 0
    if args.stop:
        metadata = existing_instance(state_file, root)
        if metadata is None:
            print("本工具当前没有可验证的运行实例；未停止任何其他进程。", flush=True)
            return 0
        if management_request(metadata, stop=True):
            print("已请求停止本工具，串口将随服务退出释放。", flush=True)
            return 0
        print("未能验证停止请求；未操作其他进程。", flush=True)
        return 1
    lock = PackageLock(state_dir/"instance.lock")
    if not lock.acquire():
        metadata = existing_instance(state_file, root, attempts=15)
        if metadata:
            print(f"本工具已运行：http://127.0.0.1:{metadata['port']}/", flush=True)
            if not args.no_browser:
                open_page(metadata["port"])
        else:
            print("本工具已有启动窗口，正在准备或等待设备；未启动第二个串口服务。", flush=True)
        return 0
    server = supervisor = metadata = None
    try:
        previous = existing_instance(state_file, root)
        if previous:
            print(f"本工具已运行：http://127.0.0.1:{previous['port']}/", flush=True)
            if not args.no_browser:
                open_page(previous["port"])
            return 0
        import robot_server
        from serial_bridge import SerialBridge
        # Fail clearly for an incomplete package, before starting a device wait.
        import serial  # noqa: F401
        server, metadata = create_service(robot_server, root, args.port)
        write_metadata(state_file, metadata)
        supervisor = PortSupervisor(server, SerialBridge, requested=args.serial_port)
        supervisor.start()
        actual_port = server.server_address[1]
        if args.port and actual_port != args.port:
            print(f"端口 {args.port} 已被使用，本工具改用 {actual_port}。", flush=True)
        print(f"网页：http://127.0.0.1:{actual_port}/\n保持此窗口运行。Ctrl+C 或双击 stop.cmd 可停止并释放串口。", flush=True)
        if not args.no_browser:
            threading.Thread(target=open_page,args=(actual_port,),name="portable-browser",daemon=True).start()
        try:
            server.serve_forever(poll_interval=0.2)
        except KeyboardInterrupt:
            print("正在停止 USB 监测服务…", flush=True)
        return 0
    finally:
        if supervisor is not None:
            supervisor.stop()
        if server is not None:
            server.server_close()
            close_recorders(server.state)
        if metadata:
            current = read_metadata(state_file, package_id(root))
            if current and current["instance_id"] == metadata["instance_id"]:
                try:
                    state_file.unlink()
                except FileNotFoundError:
                    pass
        lock.release()


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="机器狗 USB 调试工具：本机网页、自动查找 CH340，关闭窗口前可按 Ctrl+C。")
    parser.add_argument("--serial-port", help="明确选择串口，如 COM7；未指定时自动查找唯一 CH340/WCH")
    parser.add_argument("--port", type=int, default=8081, help="优先网页端口，默认 8081；占用时改用空闲端口，0 为自动分配")
    parser.add_argument("--no-browser", action="store_true", help="启动后不自动打开浏览器")
    parser.add_argument("--list-ports", action="store_true", help="列出串口及 CH340/WCH 标记后退出，不打开串口")
    parser.add_argument("--stop", action="store_true", help="仅停止本解压目录启动的服务")
    args = parser.parse_args(argv)
    if not 0 <= args.port <= 65535:
        parser.error("--port 必须为 0..65535")
    if args.serial_port is not None and (not re.fullmatch(r"COM[1-9][0-9]{0,3}", args.serial_port, re.IGNORECASE)):
        parser.error("--serial-port 请使用 Windows 串口名，例如 COM7")
    try:
        return run(args)
    except ImportError as error:
        print(f"工具依赖不完整：{error.name}。请重新完整解压工具包，不要单独移动启动文件。", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"启动失败：{error}。请将工具解压到可写目录后重试。", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
