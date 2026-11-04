#!/usr/bin/env python3
"""LAN robot monitor with bounded, acknowledged charge-current commands."""
import argparse
from collections import deque
import copy
import hmac
import ipaddress
import json
import logging
import logging.handlers
import math
import os
from pathlib import Path
import re
import secrets
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from http.cookies import SimpleCookie, CookieError
from urllib.parse import parse_qs, urlsplit

MAX_TELEMETRY = 64 * 1024
MAX_FRAME = 512 * 1024
DEVICE_TIMEOUT_MS = 5000
SAMPLE_TIMEOUT_MS = 3000
FRAME_TIMEOUT_MS = 5000
IDENTIFIER = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
ALLOWED_MA = tuple(50 + 100*(code & 1) + 200*((code >> 1) & 1) + 500*((code >> 2) & 1)
                   + 1200*((code >> 3) & 1) + 2300*((code >> 4) & 1) for code in range(32))
COMMAND_TTL_MS = 15000
CAMERA_CHECK_TTL_MS = 30000
WIFI_CONNECT_TTL_MS = 45000
WIFI_SEND_TTL_MS = 5000
WIFI_FRESH_MS = 3000
WIFI_STATUS_FIELDS = frozenset(("state","ssid","ip","rssi","request_id","request_status","error","save_error",
                                "saved","api_port","api_ready","stream_port","stream_ready","disconnect_reason"))
BOARD_LIMIT_MA = 3000
SESSION_TTL_S = 3600
AUDIO_LEASE_MS = 15000
AUDIO_PACKET_MAX = 6400
SERVO_TTL_MS = 5000
SERVO_MIN_US = 500
SERVO_MAX_US = 2500
SERVO_PERIOD_US = 20000
PROFILE_RANGES = {"cell_capacity_mah":(1,100000), "parallel_cells":(1,16),
                  "cell_max_charge_ma":(1,20000), "nominal_voltage_v":(2,4.5),
                  "usable_percent":(0,100), "efficiency_percent":(0,100), "output_voltage_v":(0.1,60)}


def validate_profile(profile):
    if not isinstance(profile, dict) or set(profile) != set(PROFILE_RANGES):
        raise Rejected("电芯配置字段不完整")
    for name, (low, high) in PROFILE_RANGES.items():
        value = profile[name]
        if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
            raise Rejected(f"电芯配置 {name} 必须在 {low}–{high} 范围内")
    if type(profile["parallel_cells"]) is not int:
        raise Rejected("并联数量必须为整数")
    return profile["parallel_cells"] * profile["cell_max_charge_ma"]


class TelemetryFileHandler(logging.handlers.RotatingFileHandler):
    """Report recorder failures to the monitor instead of silently swallowing them."""
    last_error = None

    def handleError(self, record):
        error = sys.exc_info()[1]
        self.last_error = str(error) if error else "Unknown recorder error"

    def emit(self, record):
        self.last_error = None
        super().emit(record)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


class Rejected(ValueError):
    def __init__(self, message, status=400):
        self.status = status
        super().__init__(message)


def finite_tree(value, depth=0):
    if depth > 12:
        raise Rejected("JSON nesting is too deep")
    if isinstance(value, float) and not math.isfinite(value):
        raise Rejected("Non-finite number")
    if isinstance(value, dict):
        for item in value.values():
            finite_tree(item, depth + 1)
    elif isinstance(value, list):
        for item in value:
            finite_tree(item, depth + 1)


def integer(value, name, optional=False):
    if value is None and optional:
        return
    if type(value) is not int or not 0 <= value <= 2**53 - 1:
        raise Rejected(name + " must be a nonnegative integer")


def public_wifi_payload(payload):
    """Ignore future WiFi fields without persisting credentials or unknown data."""
    if isinstance(payload,dict) and isinstance(payload.get("wifi"),dict):
        payload = {**payload,"wifi":{key:value for key,value in payload["wifi"].items() if key in WIFI_STATUS_FIELDS}}
    return payload


def validate(payload):
    if not isinstance(payload, dict):
        raise Rejected("Expected a JSON object")
    finite_tree(payload)
    for name in ("device_id", "boot_id"):
        if not isinstance(payload.get(name), str) or not IDENTIFIER.fullmatch(payload[name]):
            raise Rejected("Invalid " + name)
    for name in ("seq", "uptime_ms"):
        integer(payload.get(name), name)
    for name in ("uart_age_ms", "peripheral_age_ms", "log_seq", "uart_lines"):
        integer(payload.get(name), name, optional=True)
    for name in ("raw", "peripheral_raw"):
        if not isinstance(payload.get(name, ""), str) or len(payload.get(name, "")) > 2048:
            raise Rejected("Invalid " + name)
    if "uart_valid" in payload and type(payload["uart_valid"]) is not bool:
        raise Rejected("Invalid uart_valid")
    if "wifi_pairing_proto" in payload:
        integer(payload["wifi_pairing_proto"], "wifi_pairing_proto")
    logs = payload.get("logs", [])
    if not isinstance(logs, list) or len(logs) > 20 or any(not isinstance(line, str) or len(line) > 2048 for line in logs):
        raise Rejected("Invalid logs")
    for name in ("camera", "esp_peripherals", "audio_stream", "i2c_scan", "camera_check", "wifi", "sound_test"):
        if not isinstance(payload.get(name, {}), dict):
            raise Rejected("Invalid " + name)
    if not isinstance(payload.get("esp_peripherals", {}).get("audio", {}), dict):
        raise Rejected("Invalid audio status")
    sound = payload.get("sound_test", {})
    if "proto" in sound:
        integer(sound["proto"], "sound_test.proto")
    for target in ("speaker", "buzzer"):
        item = sound.get(target, {})
        if not isinstance(item, dict):
            raise Rejected("Invalid sound_test."+target)
        identifier = item.get("id", "")
        if not isinstance(identifier, str) or (identifier and not re.fullmatch(r"[a-f0-9]{16}", identifier)):
            raise Rejected("Invalid sound request id")
        state = item.get("status", "")
        if not isinstance(state, str) or not re.fullmatch(r"[A-Z_]{0,24}", state):
            raise Rejected("Invalid sound status")
        error = item.get("error", 0)
        if type(error) is not int or not -(2**31) <= error < 2**31:
            raise Rejected("Invalid sound error")
    if "ready" in payload.get("camera", {}) and type(payload["camera"]["ready"]) is not bool:
        raise Rejected("Invalid camera.ready")
    scan = payload.get("i2c_scan",{})
    if scan:
        if not isinstance(scan.get("id",""),str) or (scan.get("id") and not re.fullmatch(r"[a-f0-9]{16}",scan["id"])):
            raise Rejected("Invalid i2c_scan.id")
        for name in ("running","completed"):
            if name in scan and type(scan[name]) is not bool:
                raise Rejected("Invalid i2c_scan."+name)
        addresses = scan.get("addresses",[])
        if not isinstance(addresses,list) or len(addresses)>112 or any(type(value) is not int or not 8<=value<=119 for value in addresses):
            raise Rejected("Invalid I2C addresses")
        if "count" in scan and (type(scan["count"]) is not int or scan["count"] != len(addresses)):
            raise Rejected("I2C address count mismatch")
    check = payload.get("camera_check",{})
    if check:
        if not isinstance(check.get("id",""),str) or (check.get("id") and not re.fullmatch(r"[a-f0-9]{16}",check["id"])):
            raise Rejected("Invalid camera_check.id")
        for name in ("running","completed","usable","expander_ok","sccb_ack","id_ok","init_ok","frame_ok"):
            if name in check and type(check[name]) is not bool:
                raise Rejected("Invalid camera_check."+name)
        for name in ("frame_bytes","frame_timestamp_ms","sensor_pid","width","height"):
            if name in check:
                integer(check[name],"camera_check."+name)
    wifi = payload.get("wifi",{})
    if wifi:
        for key,limit in (("state",32),("ssid",32),("ip",45),("request_id",16),("request_status",32)):
            value = wifi.get(key,"")
            if not isinstance(value,str):
                raise Rejected("Invalid WiFi status")
            try:
                if len(value.encode("utf-8")) > limit or "\0" in value:
                    raise ValueError()
            except (ValueError,UnicodeEncodeError):
                raise Rejected("Invalid WiFi status")
        for key in ("saved","api_ready","stream_ready"):
            if key in wifi and type(wifi[key]) is not bool:
                raise Rejected("Invalid WiFi status")
        for key in ("rssi","error","save_error","api_port","stream_port","disconnect_reason"):
            if key in wifi and (type(wifi[key]) is not int or not -2**31 <= wifi[key] <= 2**31-1):
                raise Rejected("Invalid WiFi status")


def validate_wifi_request(payload):
    if not isinstance(payload,dict) or payload.get("action") not in ("connect","reconnect","disconnect"):
        raise Rejected("Expected WiFi connect, reconnect or disconnect")
    action = payload["action"]
    if set(payload) != ({"action","ssid","password"} if action == "connect" else {"action"}):
        raise Rejected("Invalid WiFi request fields")
    if action != "connect":
        return action, None
    try:
        if not isinstance(payload["ssid"],str) or not isinstance(payload["password"],str):
            raise ValueError()
        ssid, password = payload["ssid"].encode("utf-8"), payload["password"].encode("utf-8")
        valid_password = not password or 8 <= len(password) <= 63 or (len(password) == 64 and re.fullmatch(rb"[A-Fa-f0-9]{64}",password))
        if not 1 <= len(ssid) <= 32 or not valid_password or b"\0" in ssid or b"\0" in password:
            raise ValueError()
    except (ValueError,UnicodeEncodeError):
        raise Rejected("SSID 必须为 1–32 字节；密码为空、8–63 字节或 64 位十六进制 PSK")
    return action, (ssid.hex(),password.hex() if password else "-")


def private_camera_ip(value):
    try:
        ip = ipaddress.IPv4Address(value)
        return str(ip) if any(ip in ipaddress.IPv4Network(net) for net in ("10.0.0.0/8","172.16.0.0/12","192.168.0.0/16")) else None
    except (ipaddress.AddressValueError,TypeError):
        return None


from sound_control import SoundControlMixin


class TelemetryState(SoundControlMixin):
    def _using_wifi(self):
        return bool(getattr(self,"wifi_reachable",False) and self.wifi_payload is not None
                    and self.wifi_at is not None and (self.monotonic()-self.wifi_at)*1000 < WIFI_FRESH_MS)

    @property
    def payload(self):
        return self.wifi_payload if self._using_wifi() else self._payload

    @payload.setter
    def payload(self, value):
        self._payload = value

    @property
    def received_at(self):
        return self.wifi_at if self._using_wifi() else self._received_at

    @received_at.setter
    def received_at(self, value):
        self._received_at = value

    @property
    def received_wall(self):
        return self.wifi_wall if self._using_wifi() else self._received_wall

    @received_wall.setter
    def received_wall(self, value):
        self._received_wall = value

    def active_connection(self):
        if self._using_wifi():
            return "wifi"
        if self.serial_connected and self.serial_data_ready:
            return "serial"
        return None

    def __init__(self, data_dir=None, monotonic=time.monotonic, wall=time.time, transport="wifi", serial_port=None, serial_baud=921600):
        if transport not in ("wifi", "serial"):
            raise ValueError("Unknown transport")
        self.transport, self.serial_port = transport, serial_port
        self.serial_baud = serial_baud
        self.serial_data_ready = False
        self.serial_connected, self.serial_error = False, None
        self.serial_generation = 0
        self.serial_switching, self.serial_save_error = False, None
        self.data_dir = Path(data_dir) if data_dir else None
        self.lock = threading.RLock()
        self.frame_changed = threading.Condition(self.lock)
        self.monotonic, self.wall = monotonic, wall
        self.payload, self.received_at, self.received_wall = None, None, None
        self.frame, self.frame_at, self.frame_wall = None, None, None
        self.device_id, self.boot_id, self.seq = None, None, -1
        self.retired_boots = set()
        self.record_error = None
        self.pending_command, self.last_command = None, None
        self.pending_scan, self.last_scan = None, None
        self.pending_camera_check, self.last_camera_check = None, None
        self.pending_wifi, self.last_wifi = None, None
        # Host-only trust key, injected by RobotServer. Never part of a status,
        # recorder payload, pending request, or credentials stored on the board.
        self._wifi_pairing_token = None
        self.pending_servo, self.last_servo = None, None
        self.servo_epoch = secrets.token_hex(8)
        self.live_servo = None
        self.live_targets = {}
        self.live_next_send = 0.0
        self.live_next_keep = 0.0
        self.motion_sequence = 0
        self.motion_pending = None
        self.motion_last = None
        self.motion_boot_epoch = secrets.token_hex(8)
        self.motion_observation = None
        self.test_waiting_id = None
        self._init_sound()
        self.control_claims = deque(maxlen=256)
        self.stm_tick, self.stm_sample_at = None, None
        self.wifi_target = None
        self.wifi_generation = 0
        self.wifi_payload, self.wifi_at, self.wifi_wall = None, None, None
        self.wifi_error, self.wifi_reachable = None, False
        self.wifi_command_diagnostic = None
        self.wifi_control_heartbeat = None
        self.wifi_frame, self.wifi_frame_at, self.wifi_frame_wall = None, None, None
        self.wifi_seq, self.wifi_frame_seq = -1, -1
        self.wifi_stream_epoch = 0
        self.wifi_remote_generation = None
        self.wifi_fps_samples = deque(maxlen=120)
        if self.data_dir:
            try:
                pairing = json.loads((self.data_dir / "wifi-pairing.json").read_text(encoding="utf-8"))
                if (set(pairing) == {"ip","device","boot"} and private_camera_ip(pairing["ip"])
                        and all(isinstance(pairing[k],str) and IDENTIFIER.fullmatch(pairing[k]) for k in ("device","boot"))):
                    self.wifi_target = {**pairing,"generation":self.wifi_generation}
            except (OSError,ValueError,TypeError):
                pass
        self.audio_lease = None
        self.audio_packets = deque(maxlen=5)
        self.audio_last_seq = -1
        self.audio_received = 0
        self.logger = None
        if data_dir:
            directory = Path(data_dir)
            directory.mkdir(parents=True, exist_ok=True)
            self.logger = logging.Logger("robot-telemetry", logging.INFO)
            handler = TelemetryFileHandler(directory / "telemetry.jsonl", maxBytes=8*1024*1024, backupCount=3, encoding="utf-8")
            handler.setFormatter(logging.Formatter("%(message)s"))
            self.logger.addHandler(handler)

    def _clear_audio(self):
        self.audio_lease = None
        self.audio_packets.clear()
        self.audio_last_seq = -1

    def _audio_status(self, now):
        if self.audio_lease is not None and now >= self.audio_lease["deadline"]:
            self._clear_audio()
        if self.audio_lease is None:
            return {"enabled":False,"session_id":None,"expires_in_ms":0}
        return {"enabled":True,"session_id":self.audio_lease["session_id"],
                "expires_in_ms":max(0,int((self.audio_lease["deadline"]-now)*1000))}

    def request_audio(self, owner, enabled):
        if type(enabled) is not bool:
            raise Rejected("enabled must be boolean")
        with self.lock:
            now = self.monotonic()
            self._audio_status(now)
            if self.audio_lease is not None and self.audio_lease["owner"] != owner:
                raise Rejected("另一浏览器正在试听，请等待其停止",409)
            if not enabled:
                self._clear_audio()
            else:
                if not self._control_online(now):
                    raise Rejected("设备离线或WiFi控制固件需要升级",409)
                if self.audio_lease is None:
                    self.audio_packets.clear()
                    self.audio_last_seq = -1
                    self.audio_lease = {"owner":owner,"session_id":secrets.token_hex(8)}
                self.audio_lease["deadline"] = now+AUDIO_LEASE_MS/1000
            return {"ok":True,"audio_monitor":self._audio_status(now)}

    def ingest_audio(self, device, boot, session, sequence, pcm, sample_rate, channels, sample_format):
        if not 2 <= len(pcm) <= AUDIO_PACKET_MAX or len(pcm)%2:
            raise Rejected("Expected at most 6400 bytes of complete PCM16 samples")
        if sample_rate != "16000" or channels != "1" or sample_format != "s16le":
            raise Rejected("Expected mono 16000 Hz PCM16LE")
        if type(sequence) is not int or sequence < 0 or sequence > 2**32-1:
            raise Rejected("Invalid audio sequence")
        with self.lock:
            now = self.monotonic()
            if not self._audio_status(now)["enabled"]:
                raise Rejected("Audio lease expired or stopped",410)
            if (device,boot) != (self.device_id,self.boot_id) or session != self.audio_lease["session_id"]:
                raise Rejected("Audio session mismatch",409)
            if sequence <= self.audio_last_seq:
                raise Rejected("Duplicate or out-of-order audio packet",409)
            if (now-self.received_at)*1000 >= DEVICE_TIMEOUT_MS or (self.payload or {}).get("esp_peripherals",{}).get("audio",{}).get("configured") is not True:
                raise Rejected("Audio codec is not ready",409)
            while self.audio_packets and now-self.audio_packets[0][1] >= 1:
                self.audio_packets.popleft()
            self.audio_packets.append((sequence,now,pcm))
            self.audio_last_seq = sequence
            self.audio_received += 1
            return {"ok":True,"seq":sequence}

    def read_audio(self, owner, session, after):
        with self.lock:
            now = self.monotonic()
            if not self._audio_status(now)["enabled"]:
                raise Rejected("Audio lease expired or stopped",410)
            if self.audio_lease["owner"] != owner:
                raise Rejected("Audio belongs to another browser session",403)
            if self.audio_lease["session_id"] != session:
                raise Rejected("Audio session mismatch",409)
            while self.audio_packets and now-self.audio_packets[0][1] >= 1:
                self.audio_packets.popleft()
            newer = [packet for packet in self.audio_packets if packet[0] > after]
            if not newer:
                return None
            # Catch up instead of accumulating delayed playback after browser stalls.
            return newer[-2] if len(newer)>2 else newer[0]

    def _battery_fields(self):
        raw = (self.payload or {}).get("raw", "")
        if not raw.startswith("BAT t="):
            return {}
        return dict(part.split("=", 1) for part in raw.split() if "=" in part)

    def _can_apply(self, now):
        if not self._online(now):
            return False, "设备离线，不能修改"
        if self._using_wifi() and self.wifi_payload.get("control_proto") != 1:
            return False,"ESP32 需要升级固件才能通过 WiFi 控制"
        age = self.payload.get("uart_age_ms")
        if self.payload.get("uart_valid") is not True or age is None or age+(now-self.received_at)*1000 > SAMPLE_TIMEOUT_MS:
            return False, "电池数据已过期，等待新读数"
        fields = self._battery_fields()
        if fields.get("valid") != "1" or fields.get("link") != "READY" or fields.get("awake") != "1":
            return False, "电池未就绪，不能修改"
        if fields.get("mode") != "VBUS_LIMIT":
            return False, "固件不在固定充电限流模式"
        return True, ""

    def _online(self, now):
        return (self.payload is not None and (now-self.received_at)*1000 < DEVICE_TIMEOUT_MS
                and (self._using_wifi() or self.transport != "serial" or (self.serial_connected and self.serial_data_ready)))

    def _control_online(self, now):
        return self._online(now) and (not self._using_wifi() or self.wifi_payload.get("control_proto") == 1)

    def serial_connection(self, connected, error=None, generation=None):
        with self.lock:
            if generation is not None and generation != self.serial_generation:
                return
            self.serial_connected, self.serial_error = bool(connected), error
            self.serial_switching = False
            self.serial_data_ready = False
            if not connected and not self._using_wifi():
                self._finish_sound("CANCELLED", "串口已断开，声音命令不重放")
                self._finish_command("CANCELLED_TRANSPORT", "串口已断开，旧命令取消且不会重放")
                self._finish_scan("CANCELLED_TRANSPORT", "串口已断开")
                self._finish_camera_check("CANCELLED_TRANSPORT", "串口已断开，摄像头检测取消")
                self._finish_wifi("CANCELLED_TRANSPORT", "串口已断开，配网命令不会重放")
                self._cancel_servo("CANCELLED_TRANSPORT", "串口已断开，命令取消；已有 PWM 输出不保证自动停止")
                self._clear_audio()

    def _serial_control(self):
        return {"available":self.transport == "serial", "selected":self.serial_port,
                "connected":self.serial_connected, "error":self.serial_error,
                "switching":self.serial_switching, "save_error":self.serial_save_error}

    def select_serial_port(self, port):
        """Called by the bridge while its I/O lock excludes old-port writes/reads."""
        with self.lock:
            self._finish_command("CANCELLED_TRANSPORT", "切换串口，旧命令取消")
            self._finish_scan("CANCELLED_TRANSPORT", "切换串口，旧扫描取消")
            self._finish_camera_check("CANCELLED_TRANSPORT", "切换串口，旧检测取消")
            self._finish_wifi("CANCELLED_TRANSPORT", "切换串口，旧配网取消")
            self._cancel_servo("CANCELLED_TRANSPORT", "切换串口，上一主板的舵机状态不再代表当前主板",retire=True)
            self._clear_audio()
            self._reset_wifi_target(None)
            self.serial_port, self.serial_connected, self.serial_data_ready = port, False, False
            self.serial_error, self.serial_switching = None, True
            self.serial_generation += 1
            self.payload = self.received_at = self.received_wall = None
            self.frame = self.frame_at = self.frame_wall = None
            self.device_id = self.boot_id = None
            self.seq = -1
            self.retired_boots.clear()
            self.serial_save_error = None
            if self.data_dir:
                try:
                    temporary = self.data_dir / "serial-port.json.tmp"
                    temporary.write_text(json.dumps({"port":port})+"\n", encoding="utf-8")
                    temporary.replace(self.data_dir / "serial-port.json")
                except OSError as error:
                    self.serial_save_error = str(error)[:200]
            return {"ok":True,"serial_control":self._serial_control()}

    def _servo_fields(self, now):
        payload = self.payload or {}
        age, raw = payload.get("peripheral_age_ms"), payload.get("peripheral_raw", "")
        if (not self._online(now) or type(age) is not int or age+(now-self.received_at)*1000 >= SAMPLE_TIMEOUT_MS
                or not raw.startswith("PERIPH t=")):
            return None
        fields = dict(part.split("=",1) for part in raw.split() if "=" in part)
        try:
            mask = int(fields["servo_mask"],0)
            allowed = fields["servo_allowed"]
            pulses = [int(fields[f"servo{i}_us"]) for i in range(4)]
            if not 0 <= mask <= 15 or allowed not in ("0","1") or any(p != 0 and not SERVO_MIN_US <= p <= SERVO_MAX_US for p in pulses):
                return None
        except (KeyError,ValueError):
            return None
        return {"allowed":allowed == "1","mask":mask,"pulses":pulses}

    def _servo_ready(self, now, stop=False):
        if self._using_wifi() and self.wifi_payload.get("control_proto") != 1:
            return False,"ESP32 需要升级固件才能通过 WiFi 控制"
        if not self._using_wifi() and not self._usb_fresh(now):
            return False,"等待新鲜的 WiFi 或 USB 主板状态"
        fields = self._servo_fields(now)
        if fields is not None and "servo_bus=TTL_READONLY" in self.payload.get("peripheral_raw", "").split():
            return False,"当前为 TTL 位置回读固件，PWM 控制已停用；不发送运动或扭矩指令"
        if not stop and fields is None:
            return False,"舵机状态已过期，等待 STM32 新数据"
        if not stop and not fields["allowed"]:
            battery = self._battery_fields()
            age = self.payload.get("uart_age_ms")
            fresh = (self.payload.get("uart_valid") is True and age is not None and
                     age + (now-self.received_at)*1000 <= SAMPLE_TIMEOUT_MS)
            if fresh and battery.get("link") == "BUS_ERROR":
                if battery.get("io_last", "").startswith("NACK:ADDR"):
                    return False,"IP5310 电源芯片 0x75 无应答，电池联锁禁止舵机输出；请检查电池、电源芯片供电及 I²C 接线"
                return False,"IP5310 电源通信失败，电池联锁禁止舵机输出；请先检查电源与电池状态"
            if fresh and (battery.get("awake") == "0" or battery.get("link") == "ASLEEP"):
                return False,"电源芯片尚未唤醒，电池联锁禁止舵机输出；请检查主板电源开关"
            if fresh and battery.get("valid") == "0":
                return False,"电池数据无效，电池联锁禁止舵机输出；请先检查电源与电池状态"
            if fresh and battery.get("valid") == "1" and battery.get("soc") in ("[0,3]%", "(0,3]%"):
                return False,"电池电量处于 0–3% 档，低电量联锁禁止舵机输出；请先充电"
            return False,"电池联锁禁止舵机输出"
        return True,""

    def _finish_servo(self, status, detail):
        if self.pending_servo is not None:
            self.last_servo = {key:value for key,value in self.pending_servo.items()
                               if key not in ("deadline","epoch","sent_at","acked")}
            self.last_servo.update(status=status,detail=detail,completed_unix_ms=int(self.wall()*1000))
            self.pending_servo = None

    def _cancel_servo(self, status, detail, retire=False):
        if status == "CANCELLED_BOOT" or retire:
            self._sync_motion_result()
            history = (self._motion_public(self.motion_pending) if self.motion_pending
                       and self.motion_pending["kind"] == "TEST" else copy.deepcopy(self.motion_last))
            if history is not None and not history.get("previous_boot"):
                observed = self.motion_observation
                if (observed and history.get("kind") == "TEST" and history.get("boot_epoch") == observed["boot_epoch"]
                        and history["id"] == observed["id"]):
                    history.update(test_state=observed["state"],test_result=observed["result"])
                complete = history.get("test_state") == "DONE" and history.get("test_result") == "OK"
                history.update(previous_boot=True,status="COMPLETED" if complete else "CANCELLED_BOOT",
                               detail="上一启动的测试已完成；当前启动未继承此测试" if complete else detail+"；上一动作不能视为当前动作",
                               retired_unix_ms=int(self.wall()*1000))
                self.motion_last = history
            self.motion_boot_epoch = secrets.token_hex(8)
            self.motion_observation = None
        self.servo_epoch = secrets.token_hex(8)
        self.live_servo = None
        self.live_targets.clear()
        self.motion_pending = None
        self.test_waiting_id = None
        self._finish_servo(status,detail)

    def _expire_servo(self, now):
        if self.pending_servo and now >= self.pending_servo["deadline"]:
            self._finish_servo("TIMED_OUT","未在时限内收到应答和输出配置回读；不自动重发")

    def _servo_feedback(self, now):
        payload = self.payload or {}
        raw = payload.get("peripheral_raw", "")
        fields = dict(part.split("=", 1) for part in raw.split() if "=" in part)
        if not raw.startswith("PERIPH t=") or fields.get("servo_bus") not in ("TTL_READONLY","TTL_CONTROL"):
            return None
        fresh = self._servo_fields(now) is not None
        age_value = payload.get("peripheral_age_ms")
        transport_age = (age_value if type(age_value) is int else SAMPLE_TIMEOUT_MS) + (now-self.received_at)*1000
        channels = []
        for index in range(4):
            item = {"channel":index,"valid":False,"state":"INVALID","position":None,"angle_deg":None}
            try:
                values = fields[f"ttl{index}"].split(",")
                if len(values) != 7: raise ValueError()
                identifier, baud, position, flags, age, errors, result = values
                identifier, baud, flags, age, errors, result = map(int, (identifier, baud, flags, age, errors, result))
                position = None if position == "NA" else int(position)
                if not (0 <= identifier <= 253 and baud in (1000000,115200,500000,250000,57600,9600,400000,200000,19200,117647,57143,9615,19231)
                        and 0 <= flags <= 255 and 0 <= age <= 0xffffffff and 0 <= errors <= 0xffffffff
                        and -9 <= result <= 0 and (position is None or 0 <= position <= 1023)):
                    raise ValueError()
                valid = fresh and position is not None and result == 0 and age + transport_age < 1500
                item.update(id=identifier,baud=baud,status=flags,age_ms=round(age+transport_age),errors=errors,
                            valid=valid,position=position if valid else None,
                            angle_deg=round(position*180/1023,1) if valid else None,
                            state="STALE" if not fresh or position is not None and age+transport_age >= 1500 else
                                  "FAULT" if flags and result in (0,-4,-7) else "OK" if valid else
                                  {-1:"NO_RESPONSE",-2:"BAD_FRAME",-3:"RANGE",-4:"FAULT",-5:"IO_ERROR",-6:"BUS_LOW",-7:"NO_POSITION",-8:"TX_ERROR",-9:"PIN_CONFLICT"}.get(result,"SCANNING"))
                for source, target, maximum in ((f"tx{index}","tx_mismatches",80),(f"rx{index}","rx_edges",4096),(f"tq{index}","torque_state",4)):
                    value = fields.get(source, "")
                    if value.isascii() and value.isdigit() and int(value) <= maximum:
                        item[target] = int(value)
            except (KeyError,ValueError,TypeError):
                pass
            channels.append(item)
        return {"mode":fields["servo_bus"],"angle_max_deg":180,"channels":channels}

    def _servo_control(self, now):
        self._expire_servo(now)
        self._expire_live(now)
        fields = self._servo_fields(now)
        ready, reason = self._servo_ready(now)
        stop_ready, _ = self._servo_ready(now,stop=True)
        pending = None if self.pending_servo is None else {
            key:value for key,value in self.pending_servo.items() if key not in ("deadline","epoch","sent_at","acked")}
        if pending is not None:
            pending["expires_in_ms"] = max(0,int((self.pending_servo["deadline"]-now)*1000))
        motion = self._motion_fields(now)
        capable = motion is not None
        if self.test_waiting_id is not None and int((motion or {}).get("test_id",0)) == self.test_waiting_id:
            self.test_waiting_id = None
        busy = ((motion or {}).get("test_state") in ("WAIT","RUN") or self.test_waiting_id is not None
                or bool(self.motion_pending and self.motion_pending["kind"] in ("TEST","STOP")))
        test = {"id":int((motion or {}).get("test_id",0)),"state":(motion or {}).get("test_state","IDLE"),
                "result":(motion or {}).get("test_result","NONE"),"remaining_ms":int((motion or {}).get("test_left_ms",0)),
                "delay_ms":int((motion or {}).get("test_delay_ms",0)),"mask":int((motion or {}).get("test_mask",0)),
                "min_us":int((motion or {}).get("test_lo",1300)),"max_us":int((motion or {}).get("test_hi",1700)),
                "ramp_ms":int((motion or {}).get("test_ramp_ms",1000)),"hold_ms":int((motion or {}).get("test_hold_ms",0)),
                "authoritative":capable}
        return {"can_apply":ready and pending is None,"can_stop":stop_ready,"epoch":self.servo_epoch,
                "feedback":self._servo_feedback(now),
                "can_live":ready and capable and not busy and pending is None,
                "can_test":ready and capable and not busy and pending is None and "servo_bus=TTL_CONTROL" not in self.payload.get("peripheral_raw", "").split(),
                "capability":{"live":capable,"test":capable},"test":test,
                "live":{"active":self.live_servo is not None,"expires_in_ms":max(0,int((self.live_servo["deadline"]-now)*1000)) if self.live_servo else 0},
                "motion_pending":self._motion_public(self.motion_pending),"motion_last":copy.deepcopy(self.motion_last),
                "reason":"正在等待上一条舵机命令" if pending else reason,
                "pending":pending,"last":copy.deepcopy(self.last_servo),"allowed":fields["allowed"] if fields else None,
                "channels":[] if fields is None else [{"channel":i,"pulse_us":fields["pulses"][i],"enabled":bool(fields["mask"] & (1<<i))} for i in range(4)],
                "min_us":SERVO_MIN_US,"max_us":SERVO_MAX_US,"period_us":SERVO_PERIOD_US}

    def request_servo(self, payload):
        if not isinstance(payload,dict):
            raise Rejected("Expected servo object")
        if set(payload) == {"action"} and payload["action"] == "stop":
            action, channel, pulse = "stop",None,None
        elif (set(payload) == {"channel","pulse_us","expected_epoch"} and type(payload["channel"]) is int and 0 <= payload["channel"] <= 3
              and type(payload["pulse_us"]) is int and SERVO_MIN_US <= payload["pulse_us"] <= SERVO_MAX_US
              and isinstance(payload["expected_epoch"],str) and re.fullmatch(r"[a-f0-9]{16}",payload["expected_epoch"])):
            action, channel, pulse = "set",payload["channel"],payload["pulse_us"]
        else:
            raise Rejected(f"需 channel=0..3、pulse_us={SERVO_MIN_US}..{SERVO_MAX_US}、expected_epoch，或 action=stop")
        with self.lock:
            now = self.monotonic()
            self._expire_servo(now)
            ready, reason = self._servo_ready(now,stop=action == "stop")
            if not ready:
                raise Rejected(reason,409)
            if action == "set" and self._using_wifi():
                raise Rejected("WiFi 舵机控制请使用实时滑动接口",409)
            if action == "set" and payload["expected_epoch"] != self.servo_epoch:
                raise Rejected("全停或连接状态已改变，请等待最新状态后重新操作",409)
            if self.pending_servo:
                if action != "stop":
                    raise Rejected("已有舵机命令等待完成",409)
                if self.pending_servo["action"] == "stop":
                    return {"ok":True,"servo_control":self._servo_control(now)}
                self._finish_servo("CANCELLED_STOP","被全部停止指令取代")
            if action == "stop":
                self.servo_epoch = secrets.token_hex(8)
                self.live_servo = None
                self.live_targets.clear()
                self.motion_pending = None
                self.test_waiting_id = None
            self.pending_servo = {"id":secrets.token_hex(8),"action":action,"channel":channel,"pulse_us":pulse,
                                  "status":"QUEUED","detail":"等待串口下发","deadline":now+SERVO_TTL_MS/1000,
                                  "epoch":self.boot_id,"acked":False,"created_unix_ms":int(self.wall()*1000)}
            if action == "stop" and self._motion_fields(now):
                self.pending_servo["firmware_id"] = self._new_motion("STOP")["id"]
            return {"ok":True,"servo_control":self._servo_control(now)}

    def outgoing_servo(self, epoch):
        with self.lock:
            now = self.monotonic()
            self._expire_servo(now)
            command = self.pending_servo
            if command and command["action"] == "set" and self._using_wifi():
                self._finish_servo("CANCELLED_TRANSPORT","已切换 WiFi，请重新拖动滑杆；旧手动命令不重放")
                return None
            if (command is None or command["status"] != "QUEUED" or command["epoch"] != epoch
                    or not self._servo_ready(now,stop=command["action"] == "stop")[0]):
                return None
            return copy.deepcopy(command)

    def send_servo(self, identifier, epoch):
        with self.lock:
            now = self.monotonic()
            self._expire_servo(now)
            command = self.pending_servo
            if (command is None or command["id"] != identifier or command["epoch"] != epoch or command["status"] != "QUEUED"
                    or not self._servo_ready(now,stop=command["action"] == "stop")[0]):
                return False
            command.update(status="SENT",detail="已下发，等待 STM32 应答",sent_at=now)
            return True

    def accept_servo_ack(self, epoch, fields):
        """Only direct UART ACK lines call this; cached ESP logs are never ACKs."""
        with self.lock:
            self._expire_servo(self.monotonic())
            command = self.pending_servo
            if (command is None or command["epoch"] != epoch or command["status"] != "SENT"
                    or fields.get("cmd") != ("STOP" if command["action"] == "stop" else "SERVO")
                    or ("firmware_id" in command and str(command["firmware_id"]) != str(fields.get("id")))):
                return
            if fields.get("ok") == "0":
                self._finish_servo("FAILED","STM32 拒绝："+fields.get("reason","UNKNOWN")[:80])
            elif fields.get("ok") == "1":
                command.update(status="AWAITING_READBACK",acked=True,detail="STM32 已应答，等待下一条输出配置回读")

    def accept_servo_readback(self, epoch):
        """Called after a newly received direct PERIPH line, after its ingestion."""
        with self.lock:
            now = self.monotonic()
            self._expire_servo(now)
            command, fields = self.pending_servo, self._servo_fields(now)
            if command is None or command["epoch"] != epoch or not command["acked"] or fields is None:
                return
            matches = (fields["mask"] == 0 and fields["pulses"] == [0]*4) if command["action"] == "stop" else (
                fields["allowed"] and bool(fields["mask"] & (1<<command["channel"])) and fields["pulses"][command["channel"]] == command["pulse_us"])
            if matches:
                self._finish_servo("CONFIRMED","STM32 应答与 PWM 输出配置回读一致；没有舵机位置反馈")

    def _usb_fresh(self, now):
        payload = self._payload or {}
        age = payload.get("esp_status_age_ms")
        return bool(self.transport == "serial" and self.serial_connected and self.serial_data_ready and type(age) is int
                    and self._received_at is not None and age+(now-self._received_at)*1000 < WIFI_FRESH_MS
                    and re.fullmatch(r"[a-f0-9]{8}",str(payload.get("board_boot_id", ""))[:8])
                    and type(payload.get("uptime_ms")) is int)

    def _motion_fields(self, now):
        if self._servo_fields(now) is None:
            return None
        fields = dict(part.split("=",1) for part in self.payload.get("peripheral_raw", "").split() if "=" in part)
        if fields.get("servo_proto") != "2":
            return None
        for key in ("t","last_id","test_id","test_left_ms","test_delay_ms","test_mask","test_lo","test_hi","test_ramp_ms","test_hold_ms"):
            if not fields.get(key, "0").isascii() or not fields.get(key,"0").isdigit():
                return None
        return fields

    def _observe_stm(self, payload, now):
        age, raw = payload.get("peripheral_age_ms"),payload.get("peripheral_raw","")
        if type(age) is not int or age >= SAMPLE_TIMEOUT_MS or not raw.startswith("PERIPH t="):
            return
        fields = dict(part.split("=",1) for part in raw.split() if "=" in part)
        try:
            tick = int(fields["t"])
        except (KeyError,ValueError):
            return
        sample_at = now-age/1000
        if self.stm_sample_at is not None and sample_at <= self.stm_sample_at:
            return
        if self.stm_tick is not None and ((tick-self.stm_tick)&0xffffffff) > 0x7fffffff:
            self._cancel_servo("CANCELLED_BOOT","STM32 已重启，旧动作取消且不会重放")
            self.motion_sequence = 0
        self.stm_tick,self.stm_sample_at = tick,sample_at
        if fields.get("servo_proto") == "2" and fields.get("test_id","").isdigit():
            self.motion_observation = {"id":int(fields["test_id"]),"boot_epoch":self.motion_boot_epoch,
                                       "state":fields.get("test_state"),"result":fields.get("test_result")}
            self._sync_motion_result()

    def _sync_motion_result(self):
        history,observed = self.motion_last,self.motion_observation
        if (not history or not observed or history.get("previous_boot") or history.get("kind") != "TEST"
                or history.get("boot_epoch") != observed["boot_epoch"] or history["id"] != observed["id"]):
            return
        history.update(test_state=observed["state"],test_result=observed["result"])
        terminal = {("DONE","OK"):("COMPLETED","STM32 已完成 10 秒测试并停止输出；没有舵机位置反馈"),
                    ("STOPPED","STOP"):("STOPPED","STM32 已停止本次测试"),
                    ("FAILED","BATTERY"):("FAILED","电池联锁中断测试")}.get((observed["state"],observed["result"]))
        if terminal:
            history.update(status=terminal[0],detail=terminal[1])

    def claim_control(self, identifier, route):
        with self.lock:
            if self.active_connection() != route or identifier in self.control_claims:
                return False
            self.control_claims.append(identifier)
            return True

    @staticmethod
    def _motion_public(command):
        return None if command is None else {key:command[key] for key in
            ("id","kind","status","boot_id","boot_epoch","servo_epoch","previous_boot") if key in command}

    def _new_motion(self, kind, args=()):
        fields = self._motion_fields(self.monotonic()) or {}
        self.motion_sequence = max(self.motion_sequence,int(fields.get("last_id",0)))+1
        if self.motion_sequence > 0xffffffff:
            raise Rejected("舵机命令序号已用完，请重启主板",409)
        return {"id":self.motion_sequence,"kind":kind,"args":args,"status":"QUEUED",
                "request_id":secrets.token_hex(8),"deadline":self.monotonic()+1.2,
                "boot_id":(self.payload or {}).get("board_boot_id") or (self.payload or {}).get("boot_id"),
                "boot_epoch":self.motion_boot_epoch,"servo_epoch":self.servo_epoch,"previous_boot":False,
                "epoch":self.servo_epoch,"tick":int(fields.get("t",0))}

    def _expire_live(self, now):
        if self.live_servo and (now >= self.live_servo["deadline"] or not self._servo_ready(now)[0]):
            self.live_servo = None
            self.live_targets.clear()
            self.servo_epoch = secrets.token_hex(8)
            if self._servo_ready(now,stop=True)[0]:
                self.motion_pending = self._new_motion("STOP")
        if self.motion_pending and now >= self.motion_pending["deadline"]:
            self.motion_last = {**self._motion_public(self.motion_pending),"status":"UNKNOWN","detail":"未收到板端确认；不会重发"}
            self.motion_pending = None

    def request_live(self, owner, payload):
        if not isinstance(payload,dict) or payload.get("action") not in ("set","renew","release"):
            raise Rejected("Expected live set, renew or release")
        action = payload["action"]
        keys = {"action","expected_epoch"} | ({"channel","pulse_us"} if action == "set" else set())
        if set(payload) != keys:
            raise Rejected("Invalid live control fields")
        if action == "set" and (type(payload["channel"]) is not int or not 0 <= payload["channel"] < 4
                                or type(payload["pulse_us"]) is not int or not SERVO_MIN_US <= payload["pulse_us"] <= SERVO_MAX_US):
            raise Rejected("Invalid servo channel or pulse")
        with self.lock:
            now = self.monotonic()
            control = self._servo_control(now)
            if payload["expected_epoch"] != self.servo_epoch:
                raise Rejected("状态已改变，请重新拖动",409)
            if self.live_servo and self.live_servo["owner"] != owner:
                raise Rejected("另一页面正在控制舵机",409)
            if action == "release":
                self.servo_epoch = secrets.token_hex(8)
                if self.live_servo:
                    self.live_servo = None
                    self.live_targets.clear()
                    self.motion_pending = self._new_motion("STOP")
            else:
                if not control["can_live"]:
                    raise Rejected(control["reason"] or "舵机测试忙或固件不支持实时控制",409)
                if action == "renew" and self.live_servo is None:
                    raise Rejected("实时控制已经结束，请重新拖动",409)
                if self.live_servo is None:
                    self.live_servo = {"owner":owner}
                self.live_servo["deadline"] = now+1.2
                if action == "set":
                    self.live_targets[payload["channel"]] = payload["pulse_us"]
            return {"ok":True,"servo_control":self._servo_control(now)}

    def request_servo_test(self, payload):
        if (not isinstance(payload,dict) or set(payload) != {"mask","min_us","max_us","expected_epoch"}
                or any(type(payload[key]) is not int for key in ("mask","min_us","max_us"))
                or not 1 <= payload["mask"] <= 15 or not SERVO_MIN_US <= payload["min_us"] < payload["max_us"] <= SERVO_MAX_US):
            raise Rejected("测试需要通道mask=1..15和500..2500us范围")
        with self.lock:
            control = self._servo_control(self.monotonic())
            if payload["expected_epoch"] != self.servo_epoch or not control["can_test"]:
                raise Rejected("状态已改变、测试忙或固件不支持，请等待新数据",409)
            self.live_servo = None
            self.live_targets.clear()
            self.servo_epoch = secrets.token_hex(8)
            self.motion_pending = self._new_motion("TEST",(payload["mask"],payload["min_us"],payload["max_us"],0))
            self.test_waiting_id = self.motion_pending["id"]
            return {"ok":True,"servo_control":self._servo_control(self.monotonic())}

    def outgoing_motion(self, route):
        with self.lock:
            now = self.monotonic()
            self._expire_live(now)
            if self.active_connection() != route or not self._servo_ready(now,stop=True)[0] or self.pending_servo:
                return None
            command = self.motion_pending
            if command is None and self.live_servo and now >= self.live_next_send and (self.live_targets or now >= self.live_next_keep):
                if self.live_targets:
                    channel = next(iter(self.live_targets))
                    command = self._new_motion("LIVE",(channel,self.live_targets.pop(channel)))
                else:
                    command = self._new_motion("KEEP")
                self.motion_pending = command
            if command is None or command["status"] != "QUEUED":
                return None
            args = (command["id"],) if command["kind"] == "STOP" else (command["id"],command["tick"],*command["args"])
            return {**command,"line":"PERIPH "+command["kind"]+" "+" ".join(map(str,args))}

    def send_motion(self, identifier, route):
        with self.lock:
            now = self.monotonic()
            self._expire_live(now)
            command = self.motion_pending
            if (command is None or command["id"] != identifier or command["status"] != "QUEUED"
                    or command["epoch"] != self.servo_epoch or self.active_connection() != route):
                return False
            command["status"] = "SENT"
            self.live_next_send,self.live_next_keep = now+0.1,now+0.4
            if command["kind"] in ("LIVE","KEEP"):
                self.motion_last = self._motion_public(command)
                self.motion_pending = None
            return True

    def accept_motion_ack(self, fields):
        with self.lock:
            command = self.motion_pending
            if command and str(command["id"]) == str(fields.get("id")) and command["status"] == "SENT" and command["kind"] == fields.get("cmd"):
                self.motion_last = {**self._motion_public(command),"status":"ACKNOWLEDGED" if str(fields.get("ok")) == "1" else "FAILED",
                                    "detail":str(fields.get("reason",""))[:80]}
                if command["kind"] == "TEST" and str(fields.get("ok")) != "1":
                    self.test_waiting_id = None
                self.motion_pending = None
                self._sync_motion_result()
            elif (self.motion_last and not self.motion_last.get("previous_boot")
                    and self.motion_last.get("boot_epoch") == self.motion_boot_epoch
                    and self.motion_last["status"] in ("SENT","ACKNOWLEDGED","UNKNOWN")
                    and str(self.motion_last["id"]) == str(fields.get("id")) and self.motion_last["kind"] == fields.get("cmd")):
                self.motion_last.update(status="ACKNOWLEDGED" if str(fields.get("ok")) == "1" else "FAILED",detail=str(fields.get("reason",""))[:80])
                self._sync_motion_result()
                if str(fields.get("ok")) != "1" and self.motion_last["kind"] in ("LIVE","KEEP"):
                    self._cancel_servo("FAILED","STM32 拒绝实时控制")

    def _finish_wifi(self, status, detail):
        if self.pending_wifi is not None:
            self.last_wifi = {"id":self.pending_wifi["id"],"action":self.pending_wifi["action"],
                              "status":status,"detail":detail,"completed_unix_ms":int(self.wall()*1000)}
            self.pending_wifi = None  # Credentials never enter the public result.

    def _expire_wifi(self, now):
        if self.pending_wifi is not None:
            if now >= self.pending_wifi["deadline"]:
                self._finish_wifi("TIMED_OUT","WiFi 操作超时，请查看当前状态后再试")
            elif not self.pending_wifi["claimed"] and now >= self.pending_wifi["send_deadline"]:
                self._finish_wifi("TIMED_OUT","配网命令已过发送期限，未重发")

    def _wifi_control(self, now):
        self._expire_wifi(now)
        pending = self.pending_wifi
        public = None if pending is None else {"id":pending["id"],"action":pending["action"],
                 "status":"SENT" if pending["claimed"] else "QUEUED",
                 "expires_in_ms":max(0,int((pending["deadline"]-now)*1000))}
        return {"can_control":self._usb_fresh(now) and pending is None,"pending":public,"last":copy.deepcopy(self.last_wifi)}

    def request_wifi(self, payload):
        action, credentials = validate_wifi_request(payload)
        with self.lock:
            now = self.monotonic()
            self._expire_wifi(now)
            if not self._usb_fresh(now):
                raise Rejected("请连接 USB，并等待新鲜的 ESP32 状态后再配网",409)
            if self.pending_wifi is not None:
                raise Rejected("已有 WiFi 操作正在执行",409)
            pairing = action != "disconnect" and self._payload.get("wifi_pairing_proto") == 1
            if action != "disconnect" and self._payload.get("public_test_build") is True and not pairing:
                raise Rejected("当前公开固件不支持安全 Wi-Fi 配对，请升级 ESP32 测试固件后再连接",409)
            if pairing and not re.fullmatch(r"[a-f0-9]{64}", self._wifi_pairing_token or ""):
                raise Rejected("本机 Wi-Fi 配对认证未就绪，请使用配套调试服务",409)
            self.pending_wifi = {"id":secrets.token_hex(8),"action":action,"credentials":credentials,
                                 "claimed":False,"board_boot":self._payload["board_boot_id"],"pairing":pairing,
                                 "send_deadline":now+WIFI_SEND_TTL_MS/1000,"deadline":now+WIFI_CONNECT_TTL_MS/1000}
            self._reset_wifi_target(None)
            return {"ok":True,"wifi_control":self._wifi_control(now)}

    def take_wifi_command(self, board_boot):
        """One atomic claim; credentials are deleted before the serial write."""
        with self.lock:
            now = self.monotonic()
            self._expire_wifi(now)
            pending = self.pending_wifi
            if not pending or pending["claimed"] or not self._usb_fresh(now) or board_boot != pending["board_boot"]:
                return None
            pending["claimed"] = True
            credentials = pending.pop("credentials")
            return {"id":pending["id"],"action":pending["action"],"credentials":credentials,
                    "api_token":self._wifi_pairing_token if pending["pairing"] else None,
                    "deadline":pending["send_deadline"]}

    def should_send_wifi(self, identifier, action, boot_prefix):
        with self.lock:
            now = self.monotonic()
            self._expire_wifi(now)
            pending = self.pending_wifi
            return bool(pending and pending["claimed"] and pending["id"] == identifier and pending["action"] == action
                        and now < pending["send_deadline"] and self._usb_fresh(now)
                        and self._payload["board_boot_id"][:8] == boot_prefix)

    def _reset_wifi_target(self, endpoint):
        self.wifi_generation += 1
        self.wifi_target = None if endpoint is None else {**endpoint,"generation":self.wifi_generation}
        self.wifi_payload = self.wifi_at = None
        self.wifi_reachable, self.wifi_error = False, None
        self.wifi_frame = self.wifi_frame_at = self.wifi_frame_wall = None
        self.wifi_seq = self.wifi_frame_seq = -1
        self.wifi_stream_epoch = 0
        self.wifi_remote_generation = None
        self.wifi_fps_samples.clear()
        if self.data_dir:
            try:
                path = self.data_dir / "wifi-pairing.json"
                if endpoint is None:
                    path.unlink(missing_ok=True)
                else:
                    temporary = self.data_dir / "wifi-pairing.json.tmp"
                    temporary.write_text(json.dumps(endpoint)+"\n",encoding="utf-8")
                    temporary.replace(path)
            except OSError:
                pass
        self.frame_changed.notify_all()

    def _interrupt_wifi_stream(self):
        self.wifi_stream_epoch += 1
        self.wifi_frame = self.wifi_frame_at = self.wifi_frame_wall = None
        self.wifi_fps_samples.clear()
        self.frame_changed.notify_all()

    def _accept_usb_wifi(self, payload, now):
        if not self._usb_fresh(now):
            return
        wifi = payload.get("wifi",{})
        self._expire_wifi(now)
        pending = self.pending_wifi
        if pending is not None:
            if wifi.get("request_id") != pending["id"]:
                return  # An older READY snapshot must not revive an old endpoint.
            status = wifi.get("request_status")
            if status == "SUCCEEDED":
                expected = "OFF" if pending["action"] == "disconnect" else "READY"
                if wifi.get("state") == expected:
                    self._finish_wifi("COMPLETED","WiFi 已断开" if expected == "OFF" else "WiFi 已连接")
            elif status in ("FAILED","EXPIRED"):
                self._finish_wifi("FAILED" if status == "FAILED" else "TIMED_OUT","ESP32 未完成 WiFi 操作，错误码 "+str(wifi.get("error",0)))
        ip = private_camera_ip(wifi.get("ip"))
        device, boot = payload.get("board_device_id"),payload.get("board_boot_id")
        if (wifi.get("state") != "READY" or wifi.get("api_ready") is not True or wifi.get("api_port") != 81
                or not ip or not isinstance(device,str) or not IDENTIFIER.fullmatch(device)
                or not isinstance(boot,str) or not IDENTIFIER.fullmatch(boot)):
            if self.wifi_target is not None:
                self._reset_wifi_target(None)
            return
        endpoint = {"ip":ip,"device":device,"boot":boot}
        if self.wifi_target is None or any(self.wifi_target[key] != value for key,value in endpoint.items()):
            self._reset_wifi_target(endpoint)

    def wifi_camera_target(self):
        with self.lock:
            return copy.deepcopy(self.wifi_target)

    def wifi_capture_context(self):
        with self.lock:
            if not self._wifi_status(self.monotonic())["reachable"] or self.wifi_payload.get("camera",{}).get("ready") is not True:
                return None
            wifi = self.wifi_payload.get("wifi",{})
            declared = "stream_port" in wifi or "stream_ready" in wifi
            return {"target":copy.deepcopy(self.wifi_target),"status_seq":self.wifi_seq,
                    "stream_declared":declared,"stream_ready":wifi.get("stream_port") == 82 and wifi.get("stream_ready") is True}

    def accept_wifi_stream_identity(self, target, headers):
        if (headers.get("x-device-id"),headers.get("x-boot-id")) != (target["device"],target["boot"]):
            raise Rejected("Camera stream identity changed",409)
        generation = headers.get("x-wifi-generation","")
        if not re.fullmatch(r"[0-9]{1,10}",generation) or int(generation)>0xffffffff:
            raise Rejected("Invalid camera WiFi generation")
        with self.lock:
            if target != self.wifi_target or not self._wifi_status(self.monotonic())["reachable"]:
                raise Rejected("Camera pairing changed",409)
            if self.wifi_remote_generation is not None and self.wifi_remote_generation != generation:
                self._interrupt_wifi_stream()
            self.wifi_remote_generation = generation

    def _wifi_status(self, now):
        age = max(0,int((now-self.wifi_at)*1000)) if self.wifi_at is not None else None
        frame_age = max(0,int((now-self.wifi_frame_at)*1000)) if self.wifi_frame_at is not None else None
        reachable = bool(self.wifi_target and self.wifi_reachable and age is not None and age < WIFI_FRESH_MS)
        while self.wifi_fps_samples and now-self.wifi_fps_samples[0] > 2:
            self.wifi_fps_samples.popleft()
        samples = self.wifi_fps_samples
        fps = ((len(samples)-1)/(now-samples[0])) if len(samples)>1 and now>samples[0] else 0.0
        camera = (self.wifi_payload or {}).get("camera",{})
        wifi = (self.wifi_payload or {}).get("wifi",{})
        streaming = (not ("stream_port" in wifi or "stream_ready" in wifi)
                     or (wifi.get("stream_port") == 82 and wifi.get("stream_ready") is True))
        available = bool(reachable and self.wifi_frame is not None and frame_age < WIFI_FRESH_MS
                         and camera.get("ready") is True and wifi.get("state") == "READY" and streaming)
        return {"reachable":reachable,"error":self.wifi_error,"age_ms":age,"frame_age_ms":frame_age,
                "url":"/#cameraPanel" if self.wifi_target else None,
                "stream_available":available,"stream_url":"/stream.mjpg" if self.wifi_target else None,
                "stream_id":f"{self.wifi_generation}:{self.wifi_stream_epoch}" if self.wifi_target else None,
                "frame_seq":self.wifi_frame_seq if self.wifi_frame is not None else None,
                "fps":round(fps,2) if reachable else 0.0,"target_fps":30,
                "width":camera.get("width",0),"height":camera.get("height",0)}

    def ingest_wifi_status(self, target, headers, payload):
        payload = public_wifi_payload(payload)
        validate(payload)
        if (payload["device_id"] != target["device"] or headers.get("x-device-id") != target["device"]
                or headers.get("x-boot-id") != payload["boot_id"]):
            raise Rejected("Camera identity changed",409)
        with self.lock:
            if target != self.wifi_target:
                raise Rejected("Stale camera status",409)
            if payload["boot_id"] != target["boot"]:
                if payload["boot_id"] in self.retired_boots:
                    raise Rejected("Retired WiFi boot",409)
                self.retired_boots.add(target["boot"])
                self._cancel_servo("CANCELLED_BOOT","ESP32 已重启，旧动作取消且不会重放")
                self._finish_command("CANCELLED_BOOT","ESP32 已重启")
                self._finish_scan("CANCELLED_BOOT","ESP32 已重启")
                self._finish_camera_check("CANCELLED_BOOT","ESP32 已重启")
                self._clear_audio()
                self._reset_wifi_target({"ip":target["ip"],"device":target["device"],"boot":payload["boot_id"]})
            if payload["seq"] <= self.wifi_seq:
                raise Rejected("Stale camera status",409)
            self.wifi_seq = payload["seq"]
            self.wifi_payload, self.wifi_at, self.wifi_wall = copy.deepcopy(payload),self.monotonic(),self.wall()
            self.wifi_reachable, self.wifi_error = True,None
            if self.logger:
                self.logger.info(json.dumps({"received_unix_ms":int(self.wifi_wall*1000),"source":"wifi","telemetry":payload},
                                            ensure_ascii=False,allow_nan=False,separators=(",",":")))
                self.record_error = self.logger.handlers[0].last_error
            self._observe_stm(payload,self.wifi_at)
            self._accept_ack(payload,self.wifi_at)
            self._accept_scan(payload,self.wifi_at)
            self._accept_camera_check(payload,self.wifi_at)
            ack = payload.get("periph_ack")
            if isinstance(ack,str) and ack.startswith("PERIPH_ACK "):
                ack = dict(part.split("=",1) for part in ack.split() if "=" in part)
            ack_age = payload.get("periph_ack_age_ms")
            if isinstance(ack,dict) and type(ack_age) is int and 0 <= ack_age < SAMPLE_TIMEOUT_MS:
                self.accept_motion_ack(ack)
                self.accept_servo_ack(self.boot_id,ack)
                peripheral_age = payload.get("peripheral_age_ms")
                if type(peripheral_age) is int and peripheral_age <= ack_age:
                    self.accept_servo_readback(self.boot_id)
            wifi = payload.get("wifi",{})
            ready = (payload.get("camera",{}).get("ready") is True and wifi.get("state") == "READY"
                     and (not ("stream_port" in wifi or "stream_ready" in wifi)
                          or (wifi.get("stream_port") == 82 and wifi.get("stream_ready") is True)))
            if not ready:
                if self.wifi_frame is not None:
                    self._interrupt_wifi_stream()
                self.wifi_error = "CAMERA_NOT_READY"
            self.frame_changed.notify_all()
            return ready

    def ingest_wifi_frame(self, target, headers, jpeg):
        if not 4 <= len(jpeg) <= MAX_FRAME or not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
            raise Rejected("Invalid camera frame")
        if (headers.get("x-device-id"),headers.get("x-boot-id")) != (target["device"],target["boot"]):
            raise Rejected("Camera frame identity changed",409)
        sequence = headers.get("x-frame-seq","")
        timestamp = headers.get("x-frame-timestamp-ms","")
        if not re.fullmatch(r"[0-9]{1,10}",sequence) or not re.fullmatch(r"[0-9]{1,10}",timestamp):
            raise Rejected("Invalid frame cursor")
        sequence,timestamp = int(sequence),int(timestamp)
        with self.lock:
            now = self.monotonic()
            wifi = (self.wifi_payload or {}).get("wifi",{})
            stream_disabled = (("stream_port" in wifi or "stream_ready" in wifi)
                               and (wifi.get("stream_port") != 82 or wifi.get("stream_ready") is not True))
            if (target != self.wifi_target or not self._wifi_status(now)["reachable"] or sequence <= self.wifi_frame_seq
                    or sequence > 0xffffffff or timestamp > 0xffffffff or self.wifi_payload.get("camera",{}).get("ready") is not True
                    or wifi.get("state") != "READY" or stream_disabled):
                raise Rejected("Stale camera frame",409)
            device_now = (self.wifi_payload["uptime_ms"]+int((now-self.wifi_at)*1000)) & 0xffffffff
            frame_age = (device_now-timestamp) & 0xffffffff
            # A small clock/transport skew is allowed; an old boot/frame is not.
            if frame_age >= WIFI_FRESH_MS and frame_age < 0xffffffff-500:
                raise Rejected("Camera frame timestamp is stale",409)
            self.wifi_frame_seq = sequence
            if self.wifi_frame_at is not None and (now-self.wifi_frame_at)*1000 >= WIFI_FRESH_MS:
                self._interrupt_wifi_stream()
            elapsed = frame_age/1000 if frame_age < WIFI_FRESH_MS else 0
            self.wifi_frame,self.wifi_frame_at,self.wifi_frame_wall = bytes(jpeg),now-elapsed,self.wall()-elapsed
            self.wifi_fps_samples.append(now)
            self.wifi_error = None
            self.frame_changed.notify_all()

    def wifi_camera_failed(self, target, error, unreachable=True):
        with self.lock:
            if target == self.wifi_target:
                self.wifi_error = error
                if unreachable:
                    self.wifi_reachable = False
                self._interrupt_wifi_stream()

    def next_stream_frame(self, cursor=None, timeout=1.0):
        """Return only the newest distinct frame, never a per-client frame queue.

        A cursor is (stream_id, frame_seq); pairing changes or failures terminate
        an old stream even if the UI missed the brief disconnected state.
        """
        deadline = time.monotonic()+max(0,min(timeout,1.0))
        with self.frame_changed:
            while True:
                status = self._wifi_status(self.monotonic())
                if self.transport != "serial" or not status["reachable"]:
                    raise Rejected("WiFi camera stream is unavailable",503)
                if cursor is not None and cursor[0] != status["stream_id"]:
                    raise Rejected("Camera stream identity changed",409)
                if status["stream_available"] and (cursor is None or self.wifi_frame_seq > cursor[1]):
                    return status["stream_id"],self.wifi_frame_seq,self.wifi_frame,status["frame_age_ms"]
                if self.wifi_frame is not None and not status["stream_available"]:
                    raise Rejected("Camera frame expired",503)
                remaining = deadline-time.monotonic()
                if remaining <= 0:
                    return None
                self.frame_changed.wait(remaining)

    def _finish_scan(self, status, detail, result=None):
        if self.pending_scan is not None:
            self.last_scan = {"id":self.pending_scan["id"], "status":status, "detail":detail,
                              "completed_unix_ms":int(self.wall()*1000), "result":copy.deepcopy(result)}
            self.pending_scan = None

    def _expire_scan(self, now):
        if self.pending_scan is not None and now >= self.pending_scan["deadline"]:
            self._finish_scan("TIMED_OUT", "扫描超时，未收到本次扫描的完成结果")

    def _outgoing_scan(self, now):
        self._expire_scan(now)
        if self.pending_scan is None or not self._control_online(now):
            return None
        return {"id":self.pending_scan["id"], "expires_in_ms":max(0,int((self.pending_scan["deadline"]-now)*1000))}

    def _scan_control(self, now):
        self._expire_scan(now)
        return {"can_scan":self._control_online(now) and self.pending_scan is None,
                "pending":self._outgoing_scan(now), "last":copy.deepcopy(self.last_scan)}

    def request_scan(self):
        with self.lock:
            now = self.monotonic()
            self._expire_scan(now)
            if not self._control_online(now):
                raise Rejected("设备离线或WiFi控制固件需要升级", 409)
            snapshot = (self.payload or {}).get("i2c_scan", {})
            if self.pending_scan is not None or (isinstance(snapshot,dict) and snapshot.get("running") is True):
                raise Rejected("已有扫描正在执行", 409)
            self.pending_scan = {"id":secrets.token_hex(8), "deadline":now+COMMAND_TTL_MS/1000}
            return {"ok":True, "i2c_scan_control":self._scan_control(now)}

    def _accept_scan(self, payload, now):
        self._expire_scan(now)
        scan = payload.get("i2c_scan")
        if not self.pending_scan or not isinstance(scan,dict) or scan.get("id") != self.pending_scan["id"]:
            return
        if scan.get("completed") is True and scan.get("running") is not True:
            self._finish_scan("COMPLETED" if scan.get("error",0) == 0 else "FAILED", "扫描已结束", scan)

    def serial_commands(self):
        with self.lock:
            now = self.monotonic()
            return self._outgoing_command(now), self._outgoing_scan(now), self._audio_status(now), self._outgoing_camera_check(now)

    def _finish_camera_check(self, status, detail, result=None):
        if self.pending_camera_check is not None:
            self.last_camera_check = {"id":self.pending_camera_check["id"],"status":status,"detail":detail,
                                      "completed_unix_ms":int(self.wall()*1000),"result":copy.deepcopy(result)}
            self.pending_camera_check = None

    def _expire_camera_check(self, now):
        if self.pending_camera_check is not None and now >= self.pending_camera_check["deadline"]:
            self._finish_camera_check("TIMED_OUT", "摄像头检测超时，未收到本次完整结果")

    def _outgoing_camera_check(self, now):
        self._expire_camera_check(now)
        if self.pending_camera_check is None or not self._control_online(now):
            return None
        return {"id":self.pending_camera_check["id"],"expires_in_ms":max(0,int((self.pending_camera_check["deadline"]-now)*1000))}

    def _camera_check_control(self, now):
        self._expire_camera_check(now)
        return {"can_check":self._control_online(now) and self.pending_camera_check is None,
                "pending":self._outgoing_camera_check(now),"last":copy.deepcopy(self.last_camera_check)}

    def request_camera_check(self):
        with self.lock:
            now = self.monotonic()
            self._expire_camera_check(now)
            if not self._control_online(now):
                raise Rejected("设备离线或WiFi控制固件需要升级",409)
            snapshot = (self.payload or {}).get("camera_check",{})
            if self.pending_camera_check is not None or snapshot.get("running") is True:
                raise Rejected("已有摄像头检测正在进行",409)
            self.pending_camera_check = {"id":secrets.token_hex(8),"deadline":now+CAMERA_CHECK_TTL_MS/1000}
            return {"ok":True,"camera_check_control":self._camera_check_control(now)}

    def _accept_camera_check(self, payload, now):
        self._expire_camera_check(now)
        check = payload.get("camera_check")
        if not self.pending_camera_check or not isinstance(check,dict) or check.get("id") != self.pending_camera_check["id"]:
            return
        if check.get("completed") is True and check.get("running") is not True:
            passed = (check.get("error") == 0 and all(check.get(name) is True for name in ("usable","expander_ok","sccb_ack","id_ok","init_ok","frame_ok"))
                      and all(check.get(name,0) > 0 for name in ("frame_bytes","width","height","sensor_pid")))
            self._finish_camera_check("PASSED" if passed else "FAILED",
                                      "已在板端取得真实 JPEG 帧" if passed else "摄像头检测未通过，请查看各阶段结果",check)

    def _finish_command(self, status, detail):
        if self.pending_command is None:
            return
        result = {key: value for key, value in self.pending_command.items() if key not in ("deadline", "acked")}
        result.update(status=status, detail=detail, completed_unix_ms=int(self.wall()*1000))
        self.last_command, self.pending_command = result, None

    def _expire_command(self, now):
        if self.pending_command is not None and now >= self.pending_command["deadline"]:
            self._finish_command("TIMED_OUT", "命令超时，未确认修改结果；请查看当前寄存器读回")

    def _command_public(self, now):
        command = self.pending_command
        if command is None:
            return None
        return {"id":command["id"], "type":"charge_current", "ma":command["ma"],
                "status":command["status"], "detail":command["detail"],
                "expires_in_ms":max(0, int((command["deadline"]-now)*1000))}

    def _board_charge_limit(self):
        # New STM32 firmware advertises its compiled cap independently of the
        # current setting. Older firmware retains the existing host ceiling.
        reported = self._battery_fields().get("limit_mA", "")
        if re.fullmatch(r"[0-9]{1,4}", reported) and int(reported) > 0:
            return min(BOARD_LIMIT_MA, int(reported))
        return BOARD_LIMIT_MA

    def _charge_control(self, now):
        self._expire_command(now)
        allowed, reason = self._can_apply(now)
        return {"allowed_ma":list(ALLOWED_MA), "board_limit_ma":self._board_charge_limit(), "can_apply":allowed and self.pending_command is None,
                "reason":"正在等待上一条命令完成" if self.pending_command else reason,
                "pending":self._command_public(now), "last":copy.deepcopy(self.last_command)}

    def request_current(self, ma, profile):
        if type(ma) is not int or ma not in ALLOWED_MA:
            raise Rejected("该名义电流不在 32 档寄存器映射内", 400)
        if ma > BOARD_LIMIT_MA:
            raise Rejected("超出 IP5310 标称 3 A 能力；更换电芯不能提高充电芯片能力", 400)
        profile_max = validate_profile(profile)
        if ma > profile_max:
            raise Rejected(f"所选 {ma} mA 超过当前电芯配置的理想总上限 {profile_max:g} mA", 400)
        with self.lock:
            now = self.monotonic()
            self._expire_command(now)
            if self.pending_command is not None:
                raise Rejected("已有充电电流命令等待完成", 409)
            allowed, reason = self._can_apply(now)
            if not allowed:
                raise Rejected(reason, 409)
            limit = self._board_charge_limit()
            if ma > limit:
                raise Rejected(f"所选 {ma} mA 超过 STM32 当前固件上限 {limit} mA", 400)
            self.pending_command = {"id":secrets.token_hex(8), "ma":ma, "status":"QUEUED",
                                    "detail":"等待 ESP32 领取命令", "deadline":now+COMMAND_TTL_MS/1000,
                                    "acked":False, "created_unix_ms":int(self.wall()*1000), "profile":copy.deepcopy(profile)}
            return {"ok":True, "charge_control":self._charge_control(now)}

    def _accept_ack(self, payload, now):
        self._expire_command(now)
        command, ack = self.pending_command, payload.get("command_ack")
        if command is None:
            return
        if isinstance(ack, dict) and ack.get("id") == command["id"] and type(ack.get("ma")) is int and ack["ma"] == command["ma"]:
            status = ack.get("status")
            detail = ack.get("detail", "")
            detail = detail[:160] if isinstance(detail, str) else ""
            if status in ("REJECTED", "FAILED"):
                self._finish_command(status, detail or "STM32 未能完成电流设置")
                return
            if status == "APPLIED":
                command.update(acked=True, status="AWAITING_READBACK", detail="STM32 已应答，等待 BAT 寄存器回读确认")
        if command["acked"]:
            allowed, _ = self._can_apply(now)
            fields = self._battery_fields()
            if allowed and fields.get("set_mA") == str(command["ma"]) and fields.get("verified") == "1":
                self._finish_command("CONFIRMED", "STM32 应答与 BAT 寄存器回读一致")

    def _outgoing_command(self, now):
        self._expire_command(now)
        command = self.pending_command
        allowed, _ = self._can_apply(now)
        if command is None or command["acked"] or not allowed:
            return None
        command.update(status="SENT", detail="已向 ESP32 下发，等待 STM32 应答与回读")
        return {"id":command["id"], "type":"charge_current", "ma":command["ma"],
                "expires_in_ms":max(0, int((command["deadline"]-now)*1000))}

    def ingest(self, payload, serial_generation=None):
        payload = public_wifi_payload(payload)
        validate(payload)
        with self.lock:
            if serial_generation is not None and serial_generation != self.serial_generation:
                raise Rejected("Discarded old serial connection",409)
            if serial_generation is not None and payload.get("board_boot_id") in self.retired_boots:
                raise Rejected("Discarded retired board USB data",409)
            device, boot, seq = payload["device_id"], payload["boot_id"], payload["seq"]
            if self.device_id is not None and device != self.device_id:
                raise Rejected("Receiver is assigned to another device", 409)
            if boot in self.retired_boots:
                raise Rejected("Retired boot session", 409)
            if boot == self.boot_id and seq <= self.seq:
                raise Rejected("Duplicate or out-of-order sequence", 409)
            if boot != self.boot_id:
                if len(self.retired_boots) >= 4096:
                    raise Rejected("Boot session limit reached; restart receiver", 503)
                if self.boot_id is not None:
                    self.retired_boots.add(self.boot_id)
                    same_wifi_board = self._using_wifi() and payload.get("board_boot_id") == self.wifi_target["boot"]
                    if same_wifi_board:
                        if self.pending_servo:
                            self.pending_servo["epoch"] = boot
                    else:
                        self._finish_command("CANCELLED_BOOT", "ESP32 已重启，旧命令取消且不会重放")
                        self._finish_scan("CANCELLED_BOOT", "ESP32 已重启，旧扫描取消")
                        self._finish_camera_check("CANCELLED_BOOT", "ESP32 已重启，旧摄像头检测取消")
                        self._finish_wifi("CANCELLED_BOOT", "ESP32 已重启，旧配网命令取消且不会重放")
                        self._cancel_servo("CANCELLED_BOOT", "主板已重启，旧舵机命令取消且不会重放")
                        self._clear_audio()
                self.frame = self.frame_at = self.frame_wall = None
            self.payload = copy.deepcopy(payload)
            if self.transport == "serial":
                self.serial_data_ready = True
            self.received_at, self.received_wall = self.monotonic(), self.wall()
            if not self._using_wifi():
                self._observe_stm(payload,self._received_at)
            self.device_id, self.boot_id, self.seq = device, boot, seq
            self._accept_ack(payload, self.received_at)
            self._accept_scan(payload, self.received_at)
            self._accept_camera_check(payload, self.received_at)
            if self.transport == "serial":
                if self.wifi_target and payload.get("board_boot_id") != self.wifi_target["boot"]:
                    self._reset_wifi_target(None)
                self._accept_usb_wifi(payload,self.received_at)
            if self.logger:
                try:
                    self.logger.info(json.dumps({"received_unix_ms": int(self.received_wall*1000), "telemetry": payload}, ensure_ascii=False, allow_nan=False, separators=(",", ":")))
                    self.record_error = self.logger.handlers[0].last_error
                except OSError as error:
                    self.record_error = str(error)
            now = self.monotonic()
            return {"ok": True, "seq": seq, "command":self._outgoing_command(now),"audio_monitor":self._audio_status(now),"i2c_scan":self._outgoing_scan(now),"camera_check":self._outgoing_camera_check(now)}

    def status(self):
        with self.lock:
            now = self.monotonic()
            receiver = {"server_id":"robot-monitor", "transport":self.transport, "serial_port":self.serial_port,
                        "serial_connected":self.serial_connected, "serial_error":self.serial_error, "serial_baud":self.serial_baud if self.transport == "serial" else None,
                        "preview_transport":"unavailable" if self.transport == "serial" else "http"}
            receiver["connection"] = {"active":self.active_connection(),"wifi_connected":self._using_wifi(),"usb_connected":self.serial_connected}
            receiver["connection"]["wifi_control_ready"] = self._using_wifi() and self.wifi_payload.get("control_proto") == 1
            receiver["connection"]["command_loop_age_ms"] = None if self.wifi_control_heartbeat is None else max(0,int((now-self.wifi_control_heartbeat)*1000))
            receiver["connection"]["last_command"] = copy.deepcopy(self.wifi_command_diagnostic)
            receiver["serial_control"] = self._serial_control()
            receiver["servo_control"] = self._servo_control(now)
            receiver["sound_control"] = self._sound_control(now)
            receiver["wifi_control"] = self._wifi_control(now)
            receiver["camera_wifi"] = self._wifi_status(now)
            if self.transport == "serial" and self.wifi_target is not None:
                receiver["preview_transport"] = "wifi_proxy"
            if self.payload is None:
                return {**receiver, "raw": "", "peripheral_raw": "", "logs": [], "device_online": False,
                        "device_age_ms": None, "uart_valid": False, "uart_age_ms": None,
                        "peripheral_valid": False, "peripheral_age_ms": None,
                        "camera": {"ready": False, "state": "WAITING_DEVICE"}, "esp_peripherals": {},
                        "wifi":{},
                        "received_unix_ms": None, "recording_error": self.record_error,
                        "charge_control":self._charge_control(now),"audio_monitor":self._audio_status(now), "i2c_scan_control":self._scan_control(now),"camera_check_control":self._camera_check_control(now)}
            result = copy.deepcopy(self.payload)
            result.setdefault("wifi",{})
            age = max(0, int((now - self.received_at) * 1000))
            online = self._online(now)
            result.update(receiver)
            result.update(device_online=online, device_age_ms=age,
                          received_unix_ms=int(self.received_wall*1000), recording_error=self.record_error)
            for name in ("uart_age_ms", "peripheral_age_ms"):
                result[name] = age + result[name] if result.get(name) is not None else None
            result["uart_valid"] = bool(online and result.get("uart_valid") is True and result.get("uart_age_ms") is not None and result["uart_age_ms"] <= SAMPLE_TIMEOUT_MS)
            result["peripheral_valid"] = bool(online and result.get("peripheral_raw") and result.get("peripheral_age_ms") is not None and result["peripheral_age_ms"] <= SAMPLE_TIMEOUT_MS)
            frame_age = max(0, int((now-self.frame_at)*1000)) if self.frame_at is not None else None
            result["frame_age_ms"] = frame_age
            result["frame_available"] = bool(online and self.frame is not None and frame_age < FRAME_TIMEOUT_MS and result.get("camera", {}).get("ready") is True)
            result["charge_control"] = self._charge_control(now)
            result["audio_monitor"] = self._audio_status(now)
            result["i2c_scan_control"] = self._scan_control(now)
            result["camera_check_control"] = self._camera_check_control(now)
            if self.transport == "serial" and not self._using_wifi():
                esp_age = result.get("esp_status_age_ms")
                result["esp_status_age_ms"] = esp_age+age if esp_age is not None else None
                for part in result.get("esp_peripherals",{}).values():
                    if isinstance(part,dict) and "valid" in part:
                        part["valid"] = bool(part["valid"] and result["esp_status_age_ms"] is not None and result["esp_status_age_ms"] < 5000)
            if self.transport == "serial":
                if result["camera_wifi"]["reachable"]:
                    result["camera"] = copy.deepcopy(self.wifi_payload.get("camera",{}))
                    result["wifi"] = copy.deepcopy(self.wifi_payload.get("wifi",{}))
                wifi_frame_age = result["camera_wifi"]["frame_age_ms"]
                result["frame_age_ms"] = wifi_frame_age
                result["frame_available"] = bool(result["camera_wifi"]["reachable"] and self.wifi_frame is not None
                                                  and wifi_frame_age is not None and wifi_frame_age < WIFI_FRESH_MS
                                                  and result.get("camera",{}).get("ready") is True)
            return result

    def logs(self):
        with self.lock:
            data = self.payload or {}
            return {name: copy.deepcopy(data.get(name, default)) for name, default in
                    (("logs", []), ("log_seq", 0), ("uart_lines", 0), ("device_id", None), ("boot_id", None))}

    def ingest_frame(self, device, boot, jpeg):
        if not 4 <= len(jpeg) <= MAX_FRAME or jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
            raise Rejected("Invalid or oversized JPEG")
        with self.lock:
            if self.payload is None or (device, boot) != (self.device_id, self.boot_id):
                raise Rejected("Frame session does not match telemetry", 409)
            if (self.monotonic()-self.received_at)*1000 >= DEVICE_TIMEOUT_MS or self.payload.get("camera", {}).get("ready") is not True:
                raise Rejected("Camera is not ready", 409)
            self.frame, self.frame_at, self.frame_wall = jpeg, self.monotonic(), self.wall()
            return {"ok": True}

    def capture(self):
        with self.lock:
            now = self.monotonic()
            if self.transport == "serial":
                status = self._wifi_status(now)
                if (not status["reachable"] or self.wifi_frame is None or status["frame_age_ms"] >= WIFI_FRESH_MS
                        or self.wifi_payload.get("camera",{}).get("ready") is not True):
                    raise Rejected("No fresh paired WiFi camera frame",503)
                return self.wifi_frame,int(self.wifi_frame_wall*1000),status["frame_age_ms"]
            if (self.frame is None or self.payload is None or (now-self.frame_at)*1000 >= FRAME_TIMEOUT_MS
                    or (now-self.received_at)*1000 >= DEVICE_TIMEOUT_MS or self.payload.get("camera", {}).get("ready") is not True):
                raise Rejected("No fresh camera frame", 503)
            return self.frame, int(self.frame_wall*1000), int((now-self.frame_at)*1000)


class RobotServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, token, state=None):
        if not isinstance(token, str) or len(token) < 32 or not token.isascii():
            raise ValueError("ROBOT_INGEST_TOKEN must contain at least 32 ASCII characters")
        self.token = token
        self.state = state if state is not None else TelemetryState()
        self.state._wifi_pairing_token = token if re.fullmatch(r"[a-f0-9]{64}", token) else None
        self.page = (Path(__file__).parent / "index.html").read_bytes()
        self.capacity = threading.BoundedSemaphore(24)
        self.stream_capacity = threading.BoundedSemaphore(2)
        self.stream_stopping = threading.Event()
        self.session_lock = threading.Lock()
        self.sessions = {}
        self.serial_bridge = None
        self.allowed_names = {"localhost", socket.gethostname().lower()}
        super().__init__(address, RobotHandler)

    def serial_ports(self):
        if self.state.transport != "serial":
            raise Rejected("当前服务不是 USB 串口模式",409)
        try:
            from serial.tools import list_ports
            ports = sorted(({"device":p.device,"description":p.description or "","hwid":p.hwid or ""}
                            for p in list_ports.comports()),key=lambda p:p["device"])
        except Exception as error:
            raise Rejected("无法枚举串口："+str(error)[:160],503)
        with self.state.lock:
            return {"ports":ports,**self.state._serial_control()}

    def select_serial_port(self, payload):
        if not isinstance(payload,dict) or set(payload) != {"port"} or not isinstance(payload["port"],str):
            raise Rejected("Expected port string only")
        available = self.serial_ports()
        port = payload["port"]
        if not port or port not in {item["device"] for item in available["ports"]}:
            raise Rejected("该串口不存在，请刷新端口列表",400)
        if self.serial_bridge is None:
            raise Rejected("串口工作线程尚未启动",503)
        return self.serial_bridge.select_port(port)

    def server_close(self):
        self.stream_stopping.set()
        with self.state.frame_changed:
            self.state.frame_changed.notify_all()
        super().server_close()

    def new_session(self):
        now = time.monotonic()
        with self.session_lock:
            self.sessions = {key:value for key, value in self.sessions.items() if value[1] > now}
            if len(self.sessions) >= 64:
                self.sessions.pop(next(iter(self.sessions)))
            session, csrf = secrets.token_hex(24), secrets.token_hex(32)
            self.sessions[session] = (csrf, now+SESSION_TTL_S)
            return session, csrf

    def valid_session(self, session, csrf):
        with self.session_lock:
            item = self.sessions.get(session)
            return bool(item and item[1] > time.monotonic() and hmac.compare_digest(item[0], csrf))

    def session_exists(self, session):
        with self.session_lock:
            item = self.sessions.get(session)
            return bool(item and item[1] > time.monotonic())

    def process_request(self, request, client_address):
        if not self.capacity.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.capacity.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.capacity.release()


class RobotHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "RobotMonitor/1"

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, format, *args):
        # Do not log request headers, payloads, bearer tokens, or GET polling noise.
        if len(args) > 1 and str(args[1]).isdigit() and int(args[1]) >= 400:
            super().log_message(format, *args)

    def send_body(self, status, body, content_type="application/json; charset=utf-8", headers=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self'; connect-src 'self'; frame-ancestors 'none'")
        for key, value in (headers or {}).items():
            self.send_header(key, str(value))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, status, data, headers=None):
        self.send_body(status, json.dumps(data, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8"), headers=headers)

    def browser_origin(self, required=False):
        hosts = self.headers.get_all("Host", [])
        if len(hosts) != 1 or any(char in hosts[0] for char in " /\\@\t\r\n"):
            raise Rejected("Invalid host", 403)
        try:
            expected = urlsplit("http://" + hosts[0])
            hostname, port = expected.hostname, expected.port or 80
            if not hostname or port != self.server.server_address[1]:
                raise ValueError()
            try:
                address = ipaddress.ip_address(hostname)
                host_allowed = address.is_private or address.is_loopback
            except ValueError:
                host_allowed = hostname.lower() in self.server.allowed_names
            if not host_allowed:
                raise ValueError()
        except ValueError:
            raise Rejected("Untrusted host", 403)
        origin = self.headers.get("Origin")
        if required and not origin:
            raise Rejected("Origin is required", 403)
        if origin:
            try:
                received = urlsplit(origin)
                same = (received.scheme == "http" and received.hostname == hostname
                        and (received.port or 80) == port and not received.path and not received.query
                        and not received.fragment and received.username is None)
            except ValueError:
                same = False
            if not same:
                raise Rejected("Cross-origin request rejected", 403)
        if self.headers.get("Sec-Fetch-Site") not in (None, "same-origin", "none"):
            raise Rejected("Cross-site request rejected", 403)

    def verify_csrf(self):
        self.browser_origin(required=True)
        session = self.browser_session_id()
        csrf = self.headers.get("X-CSRF-Token", "")
        if not csrf.isascii() or not self.server.valid_session(session, csrf):
            raise Rejected("页面会话已失效，请重新点击操作", 403)
        return session

    def browser_session_id(self):
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie", ""))
        except CookieError:
            raise Rejected("Invalid session", 403)
        session = cookie.get("robot_session")
        if not session or not self.server.session_exists(session.value):
            raise Rejected("页面会话已失效，请重新点击操作", 403)
        return session.value

    def read_json(self, limit):
        if self.headers.get_content_type() != "application/json":
            raise Rejected("Expected application/json", 415)
        body = self.read_body(limit)
        try:
            return json.loads(body, object_pairs_hook=unique_object)
        except (ValueError, UnicodeDecodeError, RecursionError):
            raise Rejected("Invalid JSON")

    def send_camera_stream(self):
        self.browser_origin()
        if not self.server.stream_capacity.acquire(blocking=False):
            raise Rejected("Too many camera viewers",429)
        try:
            frame = self.server.state.next_stream_frame(timeout=0.5)
            if frame is None:
                raise Rejected("Waiting for a fresh camera frame",503)
            self.send_response(200)
            self.send_header("Content-Type","multipart/x-mixed-replace; boundary=robotframe")
            self.send_header("Cache-Control","no-store, no-cache, must-revalidate")
            self.send_header("X-Content-Type-Options","nosniff")
            self.send_header("Cross-Origin-Resource-Policy","same-origin")
            self.send_header("Connection","close")
            self.end_headers()
            self.close_connection = True
            self.connection.settimeout(1)
            self.connection.setsockopt(socket.SOL_SOCKET,socket.SO_SNDBUF,64*1024)
            cursor = None
            try:
                while not self.server.stream_stopping.is_set():
                    if frame is not None:
                        identity,sequence,jpeg,age = frame
                        header = ("--robotframe\r\nContent-Type: image/jpeg\r\nContent-Length: "+str(len(jpeg))+
                                  "\r\nX-Frame-Seq: "+str(sequence)+"\r\nX-Frame-Age-Ms: "+str(age)+"\r\n\r\n").encode("ascii")
                        self.wfile.write(header+jpeg+b"\r\n")
                        self.wfile.flush()
                        cursor = (identity,sequence)
                        frame = None  # At most one frame retained by a slow viewer.
                    frame = self.server.state.next_stream_frame(cursor,timeout=1)
            except (Rejected,OSError):
                pass  # End the multipart response; never replay the last frame.
        finally:
            self.server.stream_capacity.release()

    def do_GET(self):
        path = urlsplit(self.path).path
        try:
            if path == "/":
                self.send_body(200, self.server.page, "text/html; charset=utf-8")
            elif path == "/api/status":
                self.send_json(200, self.server.state.status())
            elif path == "/api/serial-ports":
                self.browser_origin()
                self.send_json(200,self.server.serial_ports())
            elif path == "/api/logs":
                self.send_json(200, self.server.state.logs())
            elif path == "/api/session":
                self.browser_origin()
                session, csrf = self.server.new_session()
                self.send_json(200, {"csrf_token":csrf, "expires_in_s":SESSION_TTL_S},
                               {"Set-Cookie":f"robot_session={session}; Path=/; HttpOnly; SameSite=Strict; Max-Age={SESSION_TTL_S}"})
            elif path == "/api/audio":
                self.browser_origin()
                owner = self.browser_session_id()
                query = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
                if set(query) != {"session","after"} or any(len(value)!=1 for value in query.values()) or not re.fullmatch(r"[a-f0-9]{16}",query["session"][0]):
                    raise Rejected("Expected session and after")
                try:
                    after = int(query["after"][0])
                    if not -1 <= after <= 2**32-1:
                        raise ValueError()
                except ValueError:
                    raise Rejected("Invalid audio cursor")
                packet = self.server.state.read_audio(owner,query["session"][0],after)
                if packet is None:
                    self.send_body(204,b"","application/octet-stream")
                else:
                    sequence, created, pcm = packet
                    self.send_body(200,pcm,"application/octet-stream",{"X-Audio-Seq":sequence,"X-Audio-Session":query["session"][0],"X-Sample-Rate":16000,"X-Channels":1,"X-Sample-Format":"s16le","X-Audio-Age-Ms":max(0,int((self.server.state.monotonic()-created)*1000))})
            elif path == "/capture.jpg":
                jpeg, received, age = self.server.state.capture()
                self.send_body(200, jpeg, "image/jpeg", {"X-Frame-Received-Unix-Ms": received, "X-Frame-Age-Ms": age})
            elif path == "/stream.mjpg":
                self.send_camera_stream()
            else:
                self.send_json(404, {"error": "Not found"})
        except Rejected as error:
            self.send_json(error.status, {"error": str(error)})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True

    def read_body(self, limit):
        if self.headers.get("Transfer-Encoding"):
            raise Rejected("Chunked requests are not accepted", 411)
        try:
            lengths = self.headers.get_all("Content-Length", [])
            if len(lengths) != 1:
                raise ValueError()
            length = int(lengths[0])
        except ValueError:
            raise Rejected("A single Content-Length is required", 411)
        if length <= 0 or length > limit:
            raise Rejected("Body too large or empty", 413)
        data = self.rfile.read(length)
        if len(data) != length:
            raise Rejected("Incomplete body")
        return data

    def do_POST(self):
        try:
            path = urlsplit(self.path).path
            if path == "/api/serial-port":
                self.verify_csrf()
                payload = self.read_json(256)
                self.send_json(202,self.server.select_serial_port(payload))
                return
            if path == "/api/sound":
                self.verify_csrf()
                self.send_json(202,self.server.state.request_sound(self.read_json(512)))
                return
            if path == "/api/servo":
                self.verify_csrf()
                payload = self.read_json(512)
                self.send_json(202,self.server.state.request_servo(payload))
                return
            if path == "/api/servo-live":
                owner = self.verify_csrf()
                self.send_json(202,self.server.state.request_live(owner,self.read_json(512)))
                return
            if path == "/api/servo-test":
                self.verify_csrf()
                self.send_json(202,self.server.state.request_servo_test(self.read_json(512)))
                return
            if path == "/api/charge-current":
                self.verify_csrf()
                payload = self.read_json(2048)
                if not isinstance(payload, dict) or set(payload) != {"ma", "profile"}:
                    raise Rejected("Expected ma and profile fields")
                self.send_json(202, self.server.state.request_current(payload["ma"], payload["profile"]))
                return
            if path == "/api/audio-monitor":
                owner = self.verify_csrf()
                payload = self.read_json(256)
                if not isinstance(payload,dict) or set(payload) != {"enabled"}:
                    raise Rejected("Expected enabled only")
                self.send_json(200,self.server.state.request_audio(owner,payload["enabled"]))
                return
            if path == "/api/i2c-scan":
                self.verify_csrf()
                payload = self.read_json(256)
                if payload != {}:
                    raise Rejected("Expected an empty object")
                self.send_json(202,self.server.state.request_scan())
                return
            if path == "/api/camera-check":
                self.verify_csrf()
                payload = self.read_json(256)
                if payload != {}:
                    raise Rejected("Expected an empty object")
                self.send_json(202,self.server.state.request_camera_check())
                return
            if path == "/api/wifi":
                self.verify_csrf()
                payload = self.read_json(2048)
                self.send_json(202,self.server.state.request_wifi(payload))
                return
            if self.server.state.transport == "serial":
                raise Rejected("串口模式不接收网络设备上传", 409)
            authorization = self.headers.get("Authorization", "")
            if not hmac.compare_digest(authorization.encode("utf-8"), ("Bearer " + self.server.token).encode("ascii")):
                raise Rejected("Unauthorized", 401)
            if path == "/api/telemetry":
                payload = self.read_json(MAX_TELEMETRY)
                self.send_json(200, self.server.state.ingest(payload))
            elif path == "/api/frame":
                if self.headers.get_content_type() != "image/jpeg":
                    raise Rejected("Expected image/jpeg", 415)
                jpeg = self.read_body(MAX_FRAME)
                self.send_json(200, self.server.state.ingest_frame(self.headers.get("X-Device-Id"), self.headers.get("X-Boot-Id"), jpeg))
            elif path == "/api/audio":
                if self.headers.get_content_type() != "application/octet-stream":
                    raise Rejected("Expected application/octet-stream",415)
                body = self.read_body(AUDIO_PACKET_MAX)
                sequence = self.headers.get("X-Audio-Seq", "")
                if not sequence.isascii() or not sequence.isdigit():
                    raise Rejected("Invalid audio sequence")
                self.send_json(200,self.server.state.ingest_audio(self.headers.get("X-Device-Id"),self.headers.get("X-Boot-Id"),self.headers.get("X-Audio-Session"),int(sequence),body,self.headers.get("X-Sample-Rate"),self.headers.get("X-Channels"),self.headers.get("X-Sample-Format")))
            else:
                raise Rejected("Not found", 404)
        except Rejected as error:
            self.close_connection = True
            self.send_json(error.status, {"ok": False, "error": str(error)})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int)
    parser.add_argument("--transport", choices=("wifi","serial"), default="wifi")
    parser.add_argument("--serial-port", default="COM18")
    parser.add_argument("--serial-baud", type=int, default=921600)
    parser.add_argument("--data-dir", default=str(Path(__file__).parent / "data"))
    args = parser.parse_args()
    token = os.environ.get("ROBOT_INGEST_TOKEN", "")
    port = args.port or (8081 if args.transport == "serial" else 8080)
    state = TelemetryState(args.data_dir, transport=args.transport, serial_port=args.serial_port if args.transport == "serial" else None, serial_baud=args.serial_baud)
    server = RobotServer((args.host, port), token, state)
    bridge = None
    proxy = None
    controls = None
    if args.transport == "serial":
        from serial_bridge import SerialBridge
        bridge = SerialBridge(state, args.serial_port, args.serial_baud)
        server.serial_bridge = bridge
        bridge.start()
        from wifi_camera import WifiCameraProxy
        proxy = WifiCameraProxy(state,token)
        proxy.start()
        from wifi_control_bridge import WifiControlBridge
        controls = WifiControlBridge(state,token)
        controls.start()
    print(f"Robot monitor {args.transport} listening on {args.host}:{port}; waiting for real device telemetry", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if controls is not None:
            controls.stop()
        if proxy is not None:
            proxy.stop()
        if bridge is not None:
            bridge.stop()
        server.server_close()


if __name__ == "__main__":
    main()
