import copy
import json
import unittest

from robot_server import TelemetryState, Rejected
from serial_bridge import SerialReceiver
from test_server import ready_payload
from test_servo_live_wifi import motion
from test_wifi import wifi
BOOT = "abcdef01000000000000000000000000"
from wifi_control_bridge import WifiControlBridge


class SoundTests(unittest.TestCase):
    def setUp(self):
        self.now, self.seq = 0.0, 0
        self.state = TelemetryState(monotonic=lambda: self.now, transport="serial", serial_port="COM9")
        self.receiver = SerialReceiver(self.state, "COM9")
        self.usb()

    def payload(self, **changes):
        self.seq += 1
        p = ready_payload(boot_id=BOOT, seq=self.seq, uptime_ms=1000+int(self.now*1000), wifi=wifi(),
                          control_proto=1, peripheral_raw=motion()+" buzzer_proto=1 buzzer=0", peripheral_age_ms=0,
                          sound_test={"proto": 1, "speaker": {}, "buzzer": {}},
                          esp_peripherals={"audio": {"configured": True, "speaker_active": False, "tone_state": "IDLE"}})
        p.update(changes)
        return p

    def usb(self, **changes):
        self.receiver.feed(("ESP_STATUS "+json.dumps(self.payload(**changes))+"\n").encode())

    def request(self, action="speaker", **changes):
        data = {"action": action}
        if action == "speaker":
            data.update(hz=1000, duration_ms=500, volume_percent=5)
        elif action == "buzzer":
            data.update(duration_ms=200)
        data.update(changes)
        return self.state.request_sound(data)

    def wire(self):
        return next(x for x in self.receiver.commands() if x.startswith(b"ESP SOUND "))

    def network(self, **changes):
        target = self.state.wifi_camera_target()
        p = self.payload(device_id=target["device"], boot_id=target["boot"], **changes)
        self.state.ingest_wifi_status(target, {"x-device-id": p["device_id"], "x-boot-id": p["boot_id"]}, p)

    def test_idle_never_sends_sound(self):
        self.assertFalse(any(x.startswith(b"ESP SOUND ") for x in self.receiver.commands()))

    def test_usb_once_and_real_completion(self):
        result = self.request()
        identifier = result["command"]["id"]
        wire = self.wire()
        self.assertIn(b"SPEAKER 1000 500 5", wire)
        self.assertTrue(self.receiver.should_send(wire))
        self.assertFalse(self.receiver.should_send(wire))
        self.assertEqual(self.state.status()["sound_control"]["command"]["status"], "SENT")
        self.usb(sound_test={"proto": 1, "speaker": {"id": "f"*16, "status": "COMPLETED"}})
        self.assertEqual(self.state.status()["sound_control"]["command"]["status"], "SENT")
        self.usb(sound_test={"proto": 1, "speaker": {"id": identifier, "status": "COMPLETED", "error": 0}})
        self.assertEqual(self.state.status()["sound_control"]["command"]["status"], "COMPLETED")

    def test_buzzer_and_stop_do_not_send_servo_stop(self):
        self.request("buzzer")
        old = self.wire()
        self.request("buzzer_stop")
        self.assertFalse(self.receiver.should_send(old))
        wire = self.wire()
        self.assertIn(b"BUZZER_STOP", wire)
        self.assertNotIn(b"PERIPH STOP", wire)
        self.assertTrue(self.receiver.should_send(wire))

    def test_speaker_stop_when_not_configured(self):
        self.usb(esp_peripherals={"audio": {"configured": False}})
        with self.assertRaises(Rejected):
            self.request()
        self.request("speaker_stop")
        self.assertIn(b"SPEAKER_STOP", self.wire())

    def test_invalid_ranges_types_and_extra_fields(self):
        for data in ({"hz": 99}, {"hz": 4001}, {"duration_ms": 0}, {"duration_ms": 1001},
                     {"volume_percent": 21}, {"volume_percent": True}, {"extra": 1}):
            with self.subTest(data=data), self.assertRaises(Rejected):
                self.request(**data)

    def test_old_esp_blocks_and_old_stm_allows_explicit_legacy(self):
        self.usb(sound_test={})
        with self.assertRaises(Rejected): self.request()
        self.usb(boot_id="c"*32, peripheral_raw=motion()+" buzzer=0")
        self.assertIn("旧版兼容", self.state.status()["sound_control"]["buzzer_reason"])
        self.request("buzzer")
        self.assertTrue(self.receiver.should_send(self.wire()))
        identifier = self.state.sound_pending["id"]
        self.usb(boot_id="c"*32, peripheral_raw=motion()+" buzzer=0", sound_test={"proto":1,"buzzer":{"id":identifier,"status":"UNCONFIRMED","error":0}})
        self.assertEqual(self.state.status()["sound_control"]["command"]["status"], "UNCONFIRMED")

    def test_offline_and_stale_block(self):
        self.now = 10
        with self.assertRaises(Rejected): self.request()
        self.usb()
        self.receiver.disconnected("removed")
        with self.assertRaises(Rejected): self.request("buzzer")

    def test_deadline_pause_never_renews(self):
        self.request()
        wire = self.wire()
        self.now = 1.51
        self.assertFalse(self.receiver.should_send(wire))
        self.assertEqual(self.state.status()["sound_control"]["command"]["status"], "TIMED_OUT")

    def test_disconnect_cancels_no_replay(self):
        self.request()
        wire = self.wire()
        self.receiver.disconnected("gone")
        self.assertFalse(self.receiver.should_send(wire))
        self.usb()
        self.assertFalse(any(x.startswith(b"ESP SOUND ") for x in self.receiver.commands()))

    def test_reboot_cancels_old_request(self):
        self.request()
        wire = self.wire()
        self.usb(boot_id="b"*32)
        self.assertFalse(self.receiver.should_send(wire))

    def test_wifi_delivery_and_no_usb_duplicate(self):
        self.network()
        self.request("buzzer")
        sent = []
        def request(ip, token, method, path, payload=None):
            sent.append(copy.deepcopy(payload))
            target = self.state.wifi_camera_target()
            return {"x-device-id": target["device"], "x-boot-id": target["boot"]}, {"id": payload["id"], "accepted": True}
        bridge = WifiControlBridge(self.state, "test-token", request)
        bridge.poll_once(); bridge.poll_once()
        sound = [v for v in sent if v["line"].startswith("ESP SOUND ")]
        self.assertEqual(len(sound), 1)
        self.assertIn("BUZZER 200", sound[0]["line"])
        self.assertFalse(self.receiver.commands())
        self.assertEqual(self.state.status()["sound_control"]["command"]["status"], "SENT")

    def test_wifi_failure_is_not_completion_or_retried(self):
        self.network(); self.request()
        calls = []
        def fail(*args):
            calls.append(1); raise OSError("offline")
        bridge = WifiControlBridge(self.state, "test-token", fail)
        with self.assertRaises(OSError): bridge.poll_once()
        bridge.poll_once()
        self.assertEqual(len(calls), 1)
        self.assertEqual(self.state.status()["sound_control"]["command"]["status"], "FAILED")

    def test_usb_stm_log_does_not_make_device_deadline_past(self):
        self.now = 1.0
        self.receiver.feed((motion(tick=2000)+" buzzer_proto=1 buzzer=0\n").encode())
        self.request()
        self.assertEqual(int(self.wire().split()[3]), 3350)

    def test_invalid_sound_shapes_do_not_crash_status(self):
        from robot_server import validate
        for bad in ([], {"speaker": []}, {"buzzer": {"id": []}}, {"speaker": {"error": False}}, {"proto": True}):
            with self.subTest(bad=bad), self.assertRaises(Rejected):
                validate(self.payload(sound_test=bad))
        self.assertIn("sound_control", self.state.status())

    def test_other_target_stop_not_offered_while_pending(self):
        self.request()
        control = self.state.status()["sound_control"]
        self.assertTrue(control["speaker_can_stop"])
        self.assertFalse(control["buzzer_can_stop"])

    def test_route_change_does_not_replay(self):
        self.request(); wire = self.wire()
        self.network()
        self.assertFalse(self.receiver.should_send(wire))
        self.assertIsNone(self.state.outgoing_sound("wifi"))


if __name__ == "__main__":
    unittest.main()
