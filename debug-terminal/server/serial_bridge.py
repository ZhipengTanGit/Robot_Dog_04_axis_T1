"""Bounded serial receiver. Opening the bridge never toggles reset/boot pins."""
from collections import deque
import base64
import binascii
import copy
import json
import re
import secrets
import threading
import time

from robot_server import validate, unique_object, public_wifi_payload

MAX_LINE = 32768


class SerialReceiver:
    """Pure line merger, separate from pyserial so ageing and replay rules are testable."""

    def __init__(self, state, port):
        self.state, self.port, self.clock = state, port, state.monotonic
        self.generation = state.serial_generation
        self.buffer = bytearray()
        self.discarding = False
        self.oversize = self.invalid_status = 0
        self.connected()

    def connected(self):
        self.epoch = secrets.token_hex(8)
        self.sequence = 0
        self.snapshot = {}
        self.snapshot_at = None
        self.board_boot = None
        self.board_seq = -1
        self.retired_board_boots = set()
        self.battery = self.peripheral = None
        self.logs = deque(maxlen=20)
        self.log_seq = 0
        self.buffer.clear()
        self.discarding = False
        self.sent_charge = self.sent_scan = self.sent_camera_check = None
        self.prepared_servo = None
        self.prepared_motion = None
        self.sent_audio = None
        self.next_audio_renew = 0
        self.state.serial_connection(True,generation=self.generation)

    def disconnected(self, error):
        self.state.serial_connection(False, str(error)[:200],generation=self.generation)

    def feed(self, data):
        if self.generation != self.state.serial_generation:
            return
        for byte in data:
            if byte == 10:
                if not self.discarding:
                    self.line(self.buffer.decode("utf-8", errors="replace").strip("\r"))
                self.buffer.clear()
                self.discarding = False
            elif not self.discarding:
                if len(self.buffer) >= MAX_LINE:
                    self.buffer.clear()
                    self.discarding = True
                    self.oversize += 1
                else:
                    self.buffer.append(byte)

    def line(self, line):
        now = self.clock()
        # If firmware ever echoes a provisioning command, keep its credentials
        # out of both the recorder and browser log view.
        if line.startswith(("ESP WIFI_", "WIFI_CONNECT ")):
            return
        if line.startswith("ESP_AUDIO "):
            # Audio is private, ephemeral: it never reaches logs, snapshots or JSONL.
            try:
                packet = json.loads(line[10:], object_pairs_hook=unique_object)
                if (not isinstance(packet,dict) or set(packet) != {"session_id","seq","sample_rate","channels","format","data"}
                        or type(packet["sample_rate"]) is not int or packet["sample_rate"] != 16000
                        or type(packet["channels"]) is not int or packet["channels"] != 1
                        or not isinstance(packet["data"],str) or len(packet["data"]) > 8536):
                    raise ValueError("Invalid PCM header")
                pcm = base64.b64decode(packet["data"],validate=True)
                self.state.ingest_audio(self.state.device_id,self.state.boot_id,packet["session_id"],packet["seq"],pcm,"16000","1",packet["format"])
            except (ValueError,TypeError,KeyError,binascii.Error,RecursionError):
                pass
            return
        if line.startswith("ESP_STATUS "):
            try:
                payload = json.loads(line[11:], object_pairs_hook=unique_object)
                payload = public_wifi_payload(payload)
                validate(payload)
                boot, sequence = payload["boot_id"], payload["seq"]
                if boot in self.retired_board_boots or (boot == self.board_boot and sequence <= self.board_seq):
                    return
                if self.board_boot is not None and boot != self.board_boot:
                    if len(self.retired_board_boots) >= 4096:
                        raise ValueError("Boot session limit; restart receiver")
                    self.retired_board_boots.add(self.board_boot)
                    self.epoch = secrets.token_hex(8)
                    self.battery = self.peripheral = None
                    self.logs.clear()
                    self.sent_charge = self.sent_scan = self.sent_camera_check = None
                self.board_boot, self.board_seq = boot, sequence
                self.snapshot, self.snapshot_at = payload, now
                for field, age_field, attr in (("raw","uart_age_ms","battery"),("peripheral_raw","peripheral_age_ms","peripheral")):
                    raw, age = payload.get(field,""), payload.get(age_field)
                    expected = "BAT t=" if attr == "battery" else "PERIPH t="
                    if raw.startswith(expected) and type(age) is int:
                        sample_at = now-age/1000
                        previous = getattr(self,attr)
                        if previous is None or sample_at > previous[1]:
                            valid = payload.get("uart_valid") is True if attr == "battery" else True
                            setattr(self,attr,(raw,sample_at,valid))
                self.publish()
            except (ValueError, TypeError, RecursionError):
                self.invalid_status += 1
            return
        if not line:
            return
        if line.startswith("ESP_AUDIO"):
            return
        self.logs.append(line[:2048])
        self.log_seq += 1
        if line.startswith("BAT t="):
            self.battery = (line[:2048],now,True)
        elif line.startswith("PERIPH t="):
            self.peripheral = (line[:2048],now,True)
        elif line.startswith("PERIPH_ACK "):
            fields = dict(part.split("=",1) for part in line.split() if "=" in part)
            self.state.accept_servo_ack(self.epoch,fields)
            self.state.accept_motion_ack(fields)
            self.publish()
            return
        elif line.startswith("BAT_CURRENT_ACK "):
            fields = dict(part.split("=",1) for part in line.split() if "=" in part)
            if (re.fullmatch(r"[a-f0-9]{16}",fields.get("id","")) and fields.get("status") in ("APPLIED","FAILED","REJECTED")
                    and fields.get("target_mA","").isdigit()):
                ack = {"id":fields["id"],"ma":int(fields["target_mA"]),"status":fields["status"],"detail":fields.get("reason","")}
                if ack["status"] == "APPLIED" and fields.get("applied_mA") != fields["target_mA"]:
                    ack.update(status="FAILED",detail="STM32 应答的应用值与目标不一致")
                self.publish(ack)
            return
        else:
            return
        self.publish()
        if line.startswith("PERIPH t="):
            self.state.accept_servo_readback(self.epoch)

    def publish(self, ack=None):
        now = self.clock()
        payload = copy.deepcopy(self.snapshot)
        self.sequence += 1
        payload.update(device_id="serial-"+re.sub(r"[^A-Za-z0-9_-]","_",self.port)[-48:], boot_id=self.epoch,
                       board_device_id=self.snapshot.get("device_id"),board_boot_id=self.board_boot,
                       seq=self.sequence, uptime_ms=payload.get("uptime_ms",0), logs=list(self.logs),
                       log_seq=self.log_seq, serial_oversize=self.oversize, serial_invalid_status=self.invalid_status,
                       esp_status_age_ms=int((now-self.snapshot_at)*1000) if self.snapshot_at is not None else None)
        # Snapshot ACKs may recover a dropped direct ACK. State checks the unique
        # command ID, current boot, requested current and deadline before accepting.
        if ack:
            payload["command_ack"] = ack
        for sample, field, age_field in ((self.battery,"raw","uart_age_ms"),(self.peripheral,"peripheral_raw","peripheral_age_ms")):
            payload[field] = sample[0] if sample else ""
            payload[age_field] = max(0,int((now-sample[1])*1000)) if sample else None
        payload["uart_valid"] = bool(self.battery and self.battery[2] and payload["uart_age_ms"] <= 3000)
        # Keep the true camera state; transport availability is a separate fact.
        payload["preview_transport"] = "unavailable"
        self.state.ingest(payload,serial_generation=self.generation)

    def commands(self):
        if self.generation != self.state.serial_generation:
            return []
        if self.state.active_connection() == "wifi":
            return []
        basis = self.clock()
        current, scan, audio, camera_check = self.state.serial_commands()
        result = []
        sound = self.state.outgoing_sound("serial")
        if sound:
            wire = (sound["line"]+"\n").encode("ascii")
            self.prepared_sound = (wire,sound["id"])
            result.append(wire)
        motion = self.state.outgoing_motion("serial")
        if motion:
            wire = (motion["line"]+"\n").encode("ascii")
            self.prepared_motion = (wire,motion["id"])
            result.append(wire)
        servo = self.state.outgoing_servo(self.epoch)
        if servo:
            wire = (("PERIPH STOP"+(" "+str(servo["firmware_id"]) if "firmware_id" in servo else "")+"\n").encode("ascii")
                    if servo["action"] == "stop" else f"PERIPH SERVO {servo['channel']} {servo['pulse_us']}\n".encode("ascii"))
            self.prepared_servo = (wire,servo["id"])
            result.append(wire)
        snapshot_fresh = self.snapshot_at is not None and self.clock()-self.snapshot_at < 3
        boot_prefix = (self.board_boot or "")[:8]
        if snapshot_fresh and re.fullmatch(r"[a-f0-9]{8}",boot_prefix):
            wifi = self.state.take_wifi_command(self.board_boot)
            if wifi:
                # Absolute host deadline plus the original USB clock snapshot:
                # pausing here cannot renew the five-second device deadline.
                deadline = (self.snapshot["uptime_ms"]+int((wifi["deadline"]-self.snapshot_at)*1000)-300) & 0xffffffff
                wire = f"ESP WIFI_{wifi['action'].upper()} {wifi['id']} {deadline} {boot_prefix}"
                if wifi["credentials"] is not None:
                    wire += " "+" ".join(wifi["credentials"])
                if wifi.get("api_token") is not None:
                    wire += " "+wifi.pop("api_token")
                result.append((wire+"\n").encode("ascii"))
        if (current and current["id"] != self.sent_charge and current["expires_in_ms"] >= 1000
                and snapshot_fresh and re.fullmatch(r"[a-f0-9]{8}",boot_prefix)):
            # Deadline stays in the ESP32 clock domain. A paused host must never
            # resurrect an expired command by sending a fresh relative TTL.
            remaining_from_snapshot = (basis-self.snapshot_at)*1000+current["expires_in_ms"]
            deadline = (self.snapshot["uptime_ms"]+int(remaining_from_snapshot)-300) & 0xffffffff
            self.sent_charge = current["id"]
            result.append(f"ESP CHARGE_CURRENT {current['id']} {current['ma']} {deadline} {boot_prefix}\n".encode("ascii"))
        if scan and scan["id"] != self.sent_scan:
            self.sent_scan = scan["id"]
            result.append(f"ESP I2C_SCAN {scan['id']}\n".encode("ascii"))
        if camera_check and camera_check["id"] != self.sent_camera_check:
            self.sent_camera_check = camera_check["id"]
            result.append(f"ESP CAMERA_CHECK {camera_check['id']}\n".encode("ascii"))
        session = audio["session_id"] if audio["enabled"] else None
        if self.sent_audio is not None and session != self.sent_audio:
            result.append(f"ESP AUDIO_STOP {self.sent_audio}\n".encode("ascii"))
            self.sent_audio = None
        if session and (session != self.sent_audio or self.clock() >= self.next_audio_renew):
            result.append(f"ESP AUDIO_START {session} {audio['expires_in_ms']}\n".encode("ascii"))
            self.sent_audio = session
            self.next_audio_renew = self.clock()+5
        return result

    def should_send(self, command):
        """Recheck immediately before write; the ESP also enforces its deadline."""
        if self.generation != self.state.serial_generation:
            return False
        if self.state.active_connection() == "wifi":
            return False
        if command.startswith(b"PERIPH "):
            if self.prepared_motion and self.prepared_motion[0] == command:
                return self.state.send_motion(self.prepared_motion[1],"serial")
            return bool(self.prepared_servo and self.prepared_servo[0] == command
                        and self.state.send_servo(self.prepared_servo[1],self.epoch))
        if command.startswith(b"ESP SOUND "):
            prepared = getattr(self,"prepared_sound",None)
            return bool(prepared and prepared[0] == command and self.state.send_sound(prepared[1],"serial"))
        parts = command.decode("ascii").split()
        if parts[1].startswith("WIFI_"):
            return bool(len(parts) >= 5 and self.snapshot_at is not None and self.clock()-self.snapshot_at < 3
                        and (self.board_boot or "")[:8] == parts[4]
                        and self.state.should_send_wifi(parts[2],parts[1][5:].lower(),parts[4]))
        current, scan, audio, camera_check = self.state.serial_commands()
        if parts[1] == "CHARGE_CURRENT":
            return bool(current and current["id"] == parts[2] and str(current["ma"]) == parts[3]
                        and current["expires_in_ms"] >= 1000 and self.snapshot_at is not None
                        and self.clock()-self.snapshot_at < 3 and (self.board_boot or "")[:8] == parts[5]
                        and self.state.claim_control(current["id"],"serial"))
        if parts[1] == "I2C_SCAN":
            return bool(scan and scan["id"] == parts[2] and scan["expires_in_ms"] >= 1000 and self.state.claim_control(scan["id"],"serial"))
        if parts[1] == "CAMERA_CHECK":
            return bool(camera_check and camera_check["id"] == parts[2] and camera_check["expires_in_ms"] >= 1000 and self.state.claim_control(camera_check["id"],"serial"))
        if parts[1] == "AUDIO_START":
            return bool(audio["enabled"] and audio["session_id"] == parts[2] and audio["expires_in_ms"] >= 1000)
        return parts[1] == "AUDIO_STOP"


class SerialBridge(threading.Thread):
    def __init__(self, state, port, baud=921600):
        super().__init__(name="robot-serial",daemon=True)
        self.state, self.port, self.baud = state, port, baud
        self.stopping = threading.Event()
        self.wake = threading.Event()
        self.io_lock = threading.Lock()

    def select_port(self, port):
        # Exclude the bounded read/write block. An old-port command cannot race
        # with clearing state or be carried over to the newly selected device.
        with self.io_lock:
            self.port = port
            result = self.state.select_serial_port(port)
            self.wake.set()
            return result

    def stop(self):
        self.stopping.set()
        self.wake.set()
        self.join(timeout=3)

    def run(self):
        while not self.stopping.is_set():
            connection = None
            generation = self.state.serial_generation
            try:
                import serial
                with self.io_lock:
                    generation = self.state.serial_generation
                    self.wake.clear()
                    if not self.port:
                        self.state.serial_connection(False,"请选择串口",generation=generation)
                    else:
                        connection = serial.Serial(port=None, baudrate=self.baud, timeout=0.1, write_timeout=1)
                        connection.dtr = False
                        connection.rts = False
                        connection.port = self.port
                        connection.open()
                        receiver = SerialReceiver(self.state,self.port)
                if connection is None:
                    self.wake.wait(2)
                    continue
                next_status = 0
                while not self.stopping.is_set():
                    with self.io_lock:
                        if generation != self.state.serial_generation:
                            break
                        for command in receiver.commands():
                            if receiver.should_send(command):
                                connection.write(command)
                        now = time.monotonic()
                        if now >= next_status:
                            connection.write(b"ESP STATUS\n")
                            status = self.state.status()
                            next_status = now+(1 if status["i2c_scan_control"]["pending"] or status["charge_control"]["pending"] or status["camera_check_control"]["pending"] or status["wifi_control"]["pending"] or status["servo_control"]["pending"] else 2)
                        data = connection.read(min(max(connection.in_waiting,1),4096))
                        if data:
                            receiver.feed(data)
            except Exception as error:
                self.state.serial_connection(False, f"{type(error).__name__}: {error}"[:200],generation=generation)
            finally:
                if connection is not None:
                    connection.close()
            if generation == self.state.serial_generation:
                self.wake.wait(2)
        self.state.serial_connection(False, "串口服务已停止")
