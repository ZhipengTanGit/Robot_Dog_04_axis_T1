"""Bounded, explicit board sound tests; no browser/host-generated substitute sound."""
import copy
import re
import secrets

TERMINAL = {"COMPLETED", "STOPPED", "FAILED", "TIMED_OUT", "CANCELLED", "UNCONFIRMED"}


class SoundControlMixin:
    def _init_sound(self):
        self.sound_pending = self.sound_last = None

    def _sound_identity(self):
        p = self.payload or {}
        return (p.get("board_boot_id") or p.get("boot_id"),
                p.get("board_device_id") or p.get("device_id"), self.serial_generation,
                self.motion_boot_epoch)

    def _sound_fresh(self, now):
        return self._control_online(now) and (self._using_wifi() or self._usb_fresh(now))

    def _sound_ready(self, target, now, stop=False):
        if not self._sound_fresh(now):
            return False, "等待新鲜的 USB 或 Wi-Fi 主板状态"
        p = self.payload or {}
        if p.get("sound_test", {}).get("proto") != 1:
            return False, "请先升级 ESP32 声音测试固件"
        if target == "speaker":
            audio = p.get("esp_peripherals", {}).get("audio", {})
            if not stop and audio.get("configured") is not True:
                return False, "ES8388 尚未初始化，请检查 I²C 和音频供电"
            if not stop and (audio.get("speaker_active") is True or audio.get("tone_state") in
                             ("QUEUED", "STARTING", "PLAYING", "DRAINING", "STOPPING")):
                return False, "扬声器正在输出或关闭中"
        else:
            age = p.get("peripheral_age_ms")
            raw = p.get("peripheral_raw", "")
            if type(age) is not int or age+(now-self.received_at)*1000 >= 3000 or not raw.startswith("PERIPH t="):
                return False, "等待 STM32 新鲜状态"
            fields = dict(part.split("=", 1) for part in raw.split() if "=" in part)
            legacy = fields.get("buzzer_proto") != "1"
            if not stop and fields.get("buzzer") != "0":
                return False, "蜂鸣器正在鸣叫"
            if legacy:
                return True, "旧版兼容：可短响和停止，但无法按编号确认结果，请现场听音"
        return True, ""

    def _finish_sound(self, status, detail, error=None):
        if self.sound_pending:
            self.sound_last = self._sound_public(self.sound_pending)
            self.sound_last.update(status=status, result=detail, error=error)
            self.sound_pending = None

    @staticmethod
    def _sound_public(command):
        return {k: copy.deepcopy(v) for k, v in command.items()
                if k not in ("identity", "route", "deadline", "send_deadline", "device_deadline", "line")}

    def _update_sound(self, now):
        c = self.sound_pending
        if not c:
            return
        if c["identity"] != self._sound_identity() or c["route"] != self.active_connection() or not self._sound_fresh(now):
            self._finish_sound("CANCELLED", "主板重启、连接改变或数据过期；旧请求不重放")
            return
        if now >= c["deadline"] or (c["status"] == "QUEUED" and now >= c["send_deadline"]):
            self._finish_sound("TIMED_OUT", "未在时限内确认执行结果；不会自动重试")
            return
        if c["status"] == "QUEUED":
            return
        ack = (self.payload or {}).get("sound_test", {}).get(c["target"], {})
        if ack.get("id") != c["id"]:
            return
        status = ack.get("status")
        if status == "UNCONFIRMED" and c["target"] == "buzzer":
            self._finish_sound("UNCONFIRMED", "已交给 ESP32 转发；旧版 STM32 无编号回执，是否鸣叫请现场确认")
            return
        if status in TERMINAL:
            expected = "STOPPED" if c["action"].endswith("_stop") else "COMPLETED"
            if status in ("STOPPED", "COMPLETED") and status != expected:
                self._finish_sound("FAILED", "板端结果与请求动作不一致")
            else:
                self._finish_sound(status, "板端已确认停止；请听音确认扬声器/蜂鸣器是否正常" if status == expected
                                   else "板端执行失败或未完成，请检查设备状态", ack.get("error") or None)
        elif status == "STARTED":
            c.update(status="STARTED", result="板端正在执行")

    def _sound_control(self, now):
        self._update_sound(now)
        result = {}
        for target in ("speaker", "buzzer"):
            ready, reason = self._sound_ready(target, now)
            can_stop, _ = self._sound_ready(target, now, stop=True)
            busy = self.sound_pending is not None
            if busy and self.sound_pending["target"] != target:
                can_stop = False
            result.update({target+"_ready": ready and not busy, target+"_busy": busy,
                           target+"_can_stop": can_stop, target+"_reason": "等待当前声音指令完成" if busy else reason})
        result["command"] = self._sound_public(self.sound_pending) if self.sound_pending else copy.deepcopy(self.sound_last)
        return result

    def request_sound(self, data):
        from robot_server import Rejected
        if not isinstance(data, dict):
            raise Rejected("Expected sound test object")
        action = data.get("action")
        keys = {"speaker": {"action", "hz", "duration_ms", "volume_percent"},
                "buzzer": {"action", "duration_ms"}, "speaker_stop": {"action"}, "buzzer_stop": {"action"}}
        if not isinstance(action, str) or action not in keys or set(data) != keys[action]:
            raise Rejected("Invalid sound action or fields")
        for key, lower, upper in (("hz", 100, 4000), ("duration_ms", 20, 1000), ("volume_percent", 1, 20)):
            if key in data and (type(data[key]) is not int or not lower <= data[key] <= upper):
                raise Rejected(f"{key} must be {lower}..{upper}")
        target, stop = action.split("_")[0], action.endswith("_stop")
        with self.lock:
            now = self.monotonic()
            self._update_sound(now)
            ready, reason = self._sound_ready(target, now, stop=stop)
            if not ready:
                raise Rejected(reason, 409)
            if self.sound_pending:
                if not stop or self.sound_pending["target"] != target:
                    raise Rejected("已有声音指令在执行", 409)
                self._finish_sound("CANCELLED", "被停止指令取代")
            p = self.payload
            boot = self._sound_identity()[0]
            if not isinstance(boot, str) or not re.fullmatch(r"[a-f0-9]{32}", boot):
                raise Rejected("等待有效的主板启动标识", 409)
            # Keep the original clock basis. A paused host must not renew TTL.
            snapshot_age = 0 if self._using_wifi() else p.get("esp_status_age_ms", 0)
            send_deadline = now+1.5
            absolute = (p["uptime_ms"]+int((send_deadline-self.received_at)*1000)+snapshot_age-150) & 0xffffffff
            identifier = secrets.token_hex(8)
            line = f"ESP SOUND {identifier} {absolute} {boot[:8]} {action.upper()}"
            if action == "speaker":
                line += f" {data['hz']} {data['duration_ms']} {data['volume_percent']}"
            elif action == "buzzer":
                line += f" {data['duration_ms']}"
            self.sound_pending = {**data, "id": identifier, "target": target, "status": "QUEUED", "error": None,
                                  "result": "等待下发", "identity": self._sound_identity(), "route": self.active_connection(),
                                  "deadline": now+7, "send_deadline": send_deadline, "device_deadline": absolute, "line": line}
            return {"ok": True, "command": self._sound_public(self.sound_pending), "sound_control": self._sound_control(now)}

    def outgoing_sound(self, route):
        with self.lock:
            self._update_sound(self.monotonic())
            c = self.sound_pending
            return copy.deepcopy(c) if c and c["status"] == "QUEUED" and c["route"] == route else None

    def send_sound(self, identifier, route):
        with self.lock:
            self._update_sound(self.monotonic())
            c = self.sound_pending
            if not c or c["id"] != identifier or c["status"] != "QUEUED" or c["route"] != route:
                return False
            c.update(status="SENT", result="已发送，等待板端确认")
            return True

    def fail_sound(self, identifier):
        with self.lock:
            if self.sound_pending and self.sound_pending["id"] == identifier:
                self._finish_sound("FAILED", "发送未获确认，不自动重试；请检查连接")
