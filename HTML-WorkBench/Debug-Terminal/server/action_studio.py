"""Explicit, leased TTL pose teaching. Read-only requests never advance motion."""
import copy
import json
import math
import secrets


def validate_action(value):
    from robot_server import Rejected
    if not isinstance(value, dict) or set(value) != {"schema", "name", "frames"} or type(value["schema"]) is not int or value["schema"] != 1:
        raise Rejected("动作文件需要 schema=1、name 和 frames")
    def name(text):
        if not isinstance(text, str) or not text.strip() or len(text) > 60 or any(ord(c) < 32 for c in text):
            raise Rejected("动作和帧名称需要 1–60 个可见字符")
        return text.strip()
    result = {"schema": 1, "name": name(value["name"]), "frames": []}
    if not isinstance(value["frames"], list) or len(value["frames"]) > 120:
        raise Rejected("每个动作最多 120 帧")
    for frame in value["frames"]:
        if not isinstance(frame, dict) or set(frame) != {"name", "angles", "hold_ms"}:
            raise Rejected("每帧需要 name、四路 angles 和 hold_ms")
        angles = frame["angles"]
        if not isinstance(angles, list) or len(angles) != 4 or any(
                type(a) not in (int, float) or not math.isfinite(a) or not 0 <= a <= 180 for a in angles):
            raise Rejected("四路角度必须为 0–180° 的有限数值")
        if type(frame["hold_ms"]) is not int or not 200 <= frame["hold_ms"] <= 10000:
            raise Rejected("到位后停留时间需要 200–10000 ms")
        result["frames"].append({"name": name(frame["name"]), "angles": [round(a, 1) for a in angles], "hold_ms": frame["hold_ms"]})
    return result


class ActionStudioMixin:
    def _init_actions(self):
        self.action_library = {"schema": 1, "name": "我的动作", "frames": []}
        self.action_revision = secrets.token_hex(8)
        self.action_run = None
        self.action_last = {"mode": "IDLE", "detail": "开始示教后，手动摆腿并记录姿态"}
        self.action_storage_error = None
        if self.data_dir and (self.data_dir / "action-library.json").exists():
            try:
                path = self.data_dir / "action-library.json"
                if path.stat().st_size > 65536:
                    raise ValueError("oversized library")
                self.action_library = validate_action(json.loads(path.read_text(encoding="utf-8")))
            except (ValueError, OSError):
                self.action_storage_error = "已保存的动作文件无效，请导入有效备份后重新保存"

    def _action_identity(self):
        p = self.payload or {}
        return (p.get("board_boot_id") or p.get("boot_id"), self.motion_boot_epoch, self.active_connection())

    def _action_capable(self, now):
        fields = self._motion_fields(now) or {}
        return fields.get("teach_proto") == "1" and fields.get("pose_proto") == "1"

    def _action_angles(self, now, released=False):
        channels = (self._servo_feedback(now) or {}).get("channels", [])
        if len(channels) != 4 or any(not c.get("valid") or c.get("status") != 0 or c.get("baud") != 115200
                or released and c.get("torque_state") != 4 for c in channels):
            return None
        return [c["angle_deg"] for c in channels]

    def _action_busy(self):
        return self.action_run is not None

    def _action_cancel(self, detail, mode="STOPPED"):
        if self.action_run:
            self.action_last = {"mode": mode, "detail": detail, "run_id": self.action_run["run_id"],
                                "frame_index": self.action_run.get("index"), "frame_count": len(self.action_run.get("frames", []))}
            self.action_run = None

    def _action_halt(self, detail, mode="STOPPED"):
        self._action_cancel(detail, mode)
        self.live_servo = None
        self.live_targets.clear()
        self.servo_epoch = secrets.token_hex(8)
        self.motion_pending = self._new_motion("STOP") if self._servo_ready(self.monotonic(), stop=True)[0] else None

    def _action_status(self, now):
        r = self.action_run
        fields = self._motion_fields(now) or {}
        capable = self._action_capable(now)
        angles = self._action_angles(now)
        released = self._action_angles(now, released=True)
        lease_ok = bool(r and now < r["deadline"] and r["identity"] == self._action_identity())
        teach = bool(lease_ok and r["mode"] == "TEACH" and fields.get("teach") == "1")
        public = dict(self.action_last) if not r else {
            "mode": r["mode"], "detail": r["detail"], "run_id": r["run_id"],
            "frame_index": r.get("index"), "frame_count": len(r.get("frames", [])),
            "phase": r.get("phase"), "remaining_ms": max(0, round((r.get("hold_until", now)-now)*1000))}
        public.update(capable=capable, angles=angles, feedback=(self._servo_feedback(now) or {}).get("channels", []),
                      can_capture=bool(teach and released is not None),
                      can_teach=bool(capable and self._servo_ready(now, stop=True)[0] and not r
                          and angles is not None
                          and not self.pending_servo and not self.motion_pending and not self.live_servo
                          and fields.get("servo_mode") == "IDLE"),
                      can_play=bool(capable and self._servo_ready(now)[0] and angles is not None
                          and not self.pending_servo and not self.motion_pending and not self.live_servo
                          and (not r or teach) and (fields.get("servo_mode") == "IDLE" or teach)),
                      storage_error=self.action_storage_error)
        public["can_center"] = public["can_play"]
        return public

    def action_document(self):
        with self.lock:
            return {"document": copy.deepcopy(self.action_library), "revision": self.action_revision}

    def _action_pose(self, now):
        r = self.action_run
        frame = r["frames"][r["index"]]
        pulses = tuple(int(a*2000/180+500+0.5) for a in frame["angles"])
        self.motion_pending = self._new_motion("POSE", pulses)
        r.update(command_id=self.motion_pending["id"], ack=False, phase="MOVING", arrived=0,
                 frame_deadline=now+10, detail="四腿回中：等待实际角度到达 90°" if r["mode"] == "CENTERING" else f"第 {r['index']+1} 帧：等待四条腿实际到位")
        r.pop("hold_until", None)

    def _action_ack(self, command, ok):
        r = self.action_run
        if r and r.get("command_id") == command["id"]:
            r["ack"] = ok
            if not ok:
                r["failed"] = "STM32 拒绝动作指令"

    def _action_tick(self, now):
        r = self.action_run
        if not r:
            return
        if now >= r["deadline"] or r["identity"] != self._action_identity():
            self._action_halt("页面离开、连接变化或控制续期中断，动作已停止")
            return
        if r.get("failed"):
            self._action_halt(r["failed"], "FAILED")
            return
        if r["mode"] in ("RELEASING", "TEACH"):
            fields = self._motion_fields(now) or {}
            if r["ack"] and fields.get("teach") == "1" and self._action_angles(now, released=True) is not None:
                r.update(mode="TEACH", detail="四路扭矩已回读为关闭，可以手动摆腿并记录")
            elif r["mode"] == "RELEASING" and now > r["started"]+8:
                self._action_halt("未确认四路扭矩关闭，请检查舵机供电和连接", "FAILED")
            elif r["mode"] == "TEACH":
                r["detail"] = "正在等待四路有效角度与扭矩关闭回读，暂时不能记录"
            return
        if not self.live_servo or not self._servo_ready(now)[0] or self._action_angles(now) is None:
            self._action_halt("角度回读过期或舵机联锁中断，动作已停止", "FAILED")
            return
        frame = r["frames"][r["index"]]
        if r["phase"] == "MOVING":
            angles = self._action_angles(now)
            channels = (self._servo_feedback(now) or {}).get("channels", [])
            if r["ack"] and all(c.get("torque_state") == 2 for c in channels) and all(abs(a-b) <= 3 for a,b in zip(angles, frame["angles"])):
                r.update(phase="HOLDING", hold_until=now+frame["hold_ms"]/1000,
                         detail="四腿已回中，短暂保持后恢复手动示教" if r["mode"] == "CENTERING" else f"第 {r['index']+1} 帧已到位，正在停留")
            elif now >= r["frame_deadline"]:
                self._action_halt("10 秒内未确认姿态到位，已停止", "FAILED")
        elif now >= r["hold_until"]:
            if r["index"]+1 == len(r["frames"]):
                if r["mode"] == "CENTERING":
                    self.live_servo = None
                    self.live_targets.clear()
                    self.motion_pending = self._new_motion("TEACH")
                    r.update(mode="RELEASING", started=now, ack=False,
                             command_id=self.motion_pending["id"], detail="已回中，正在关闭扭矩，准备继续手动示教")
                    for key in ("frames", "index", "phase", "hold_until"):
                        r.pop(key, None)
                else:
                    self._action_halt("动作播放完成，已发送关闭扭矩指令", "COMPLETED")
            elif self.motion_pending is None:
                r["index"] += 1
                self._action_pose(now)

    def request_action(self, owner, data):
        from robot_server import Rejected
        if not isinstance(data, dict) or not isinstance(data.get("action"), str):
            raise Rejected("需要动作调试请求")
        action = data["action"]
        expected = {"save": {"document", "revision"}, "teach": {"expected_epoch"},
                    "center": {"expected_epoch"},
                    "play": {"document", "expected_epoch"}, "renew": {"run_id"},
                    "capture": {"run_id", "name", "hold_ms"}, "stop": set()}.get(action)
        if expected is None or set(data) != expected | {"action"}:
            raise Rejected("动作调试参数不完整")
        with self.lock:
            now = self.monotonic()
            self._expire_live(now)
            r = self.action_run
            if action == "save":
                document = validate_action(data["document"])
                if data["revision"] != self.action_revision:
                    raise Rejected("另一页面已保存动作，请导出当前编辑内容后重新载入", 409)
                if self.data_dir is None:
                    raise Rejected("动作保存目录未配置", 503)
                try:
                    self.data_dir.mkdir(parents=True, exist_ok=True)
                    temporary = self.data_dir / "action-library.json.tmp"
                    temporary.write_text(json.dumps(document, ensure_ascii=False, indent=2), encoding="utf-8")
                    temporary.replace(self.data_dir / "action-library.json")
                except OSError:
                    raise Rejected("动作文件写入失败，请先导出备份", 503)
                self.action_library = document
                self.action_revision = secrets.token_hex(8)
                self.action_storage_error = None
                return {"ok": True, **self.action_document()}
            if action == "stop":
                self._action_halt("已停止，正在关闭四路扭矩")
                return {"ok": True, "action_studio": self._action_status(now)}
            if action in ("renew", "capture"):
                if not r or r["owner"] != owner or r["run_id"] != data["run_id"] or now >= r["deadline"] or r["identity"] != self._action_identity():
                    raise Rejected("示教或播放已经结束，请重新开始", 409)
                if action == "capture":
                    if not self._action_status(now)["can_capture"]:
                        raise Rejected("尚未确认四路扭矩关闭与有效角度，不能记录", 409)
                    frame = validate_action({"schema": 1, "name": "capture", "frames": [{
                        "name": data["name"], "hold_ms": data["hold_ms"], "angles": self._action_angles(now, released=True)}]})["frames"][0]
                    return {"ok": True, "frame": frame}
                r["deadline"] = now+1.2
                if r["mode"] in ("PLAYING", "CENTERING") and self.live_servo:
                    self.live_servo["deadline"] = now+1.2
                return {"ok": True, "action_studio": self._action_status(now)}
            status = self._action_status(now)
            if data["expected_epoch"] != self.servo_epoch:
                raise Rejected("舵机控制状态已变化，请等待新数据", 409)
            if (action == "teach" and not status["can_teach"] or action == "play" and not status["can_play"]
                    or action == "center" and not status["can_center"]):
                raise Rejected("请等待新鲜的四路舵机状态，或先停止其他控制；固件需支持动作调试", 409)
            if r and r["owner"] != owner:
                raise Rejected("另一页面正在示教", 409)
            document = (validate_action(data["document"]) if action == "play" else
                        {"frames": [{"name": "回中", "angles": [90, 90, 90, 90], "hold_ms": 200}]} if action == "center" else None)
            if action == "play" and not document["frames"]:
                raise Rejected("至少记录一帧后再播放")
            self.live_targets.clear()
            self.servo_epoch = secrets.token_hex(8)
            self.action_run = {"run_id": r["run_id"] if action == "center" and r else secrets.token_hex(8), "owner": owner, "identity": self._action_identity(),
                               "deadline": now+1.2, "started": now, "ack": False}
            r = self.action_run
            if action == "teach":
                r.update(mode="RELEASING", detail="正在关闭四路扭矩，等待实际寄存器回读")
                self.motion_pending = self._new_motion("TEACH")
                r["command_id"] = self.motion_pending["id"]
            else:
                r.update(mode="CENTERING" if action == "center" else "PLAYING", frames=copy.deepcopy(document["frames"]), index=0)
                self.live_servo = {"owner": owner, "deadline": now+1.2}
                self._action_pose(now)
            return {"ok": True, "action_studio": self._action_status(now)}
