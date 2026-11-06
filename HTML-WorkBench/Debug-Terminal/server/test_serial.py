import base64
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from robot_server import Rejected, TelemetryState
from serial_bridge import MAX_LINE, SerialReceiver
from test_server import PROFILE, ready_payload


class SerialTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.state = TelemetryState(monotonic=lambda:self.now,wall=lambda:1000+self.now,transport="serial",serial_port="COM18")
        self.receiver = SerialReceiver(self.state,"COM18")

    def status_line(self, **changes):
        self.receiver.feed(("ESP_STATUS "+json.dumps(ready_payload(**{"logs":[],"boot_id":"abcdef0100000000",**changes}))+"\r\n").encode())

    def bat(self, ma=1850):
        self.receiver.feed(f"BAT t=1 awake=1 valid=1 link=READY mode=VBUS_LIMIT set_mA={ma} verified=1\r\n".encode())

    def test_fragmented_rows_and_snapshot_preserve_real_sample_age(self):
        self.receiver.feed(b"BAT t=1 valid=1 link=")
        self.assertFalse(self.state.status()["device_online"])
        self.receiver.feed(b"READY\r\nPERIPH t=1 aht_valid=1\r\n")
        self.assertTrue(self.state.status()["uart_valid"])
        self.now = 1
        self.status_line(uart_age_ms=2000,peripheral_age_ms=3000)
        status = self.state.status()
        self.assertEqual(status["uart_age_ms"],1000)
        self.assertEqual(status["raw"],"BAT t=1 valid=1 link=READY")
        self.assertEqual(status["peripheral_age_ms"],1000)
        self.assertEqual(status["board_device_id"],"robot-dog")
        self.assertEqual(status["server_id"],"robot-monitor")
        self.assertEqual(status["transport"],"serial")
        self.assertEqual(status["serial_baud"],921600)
        self.now = 4
        self.status_line(seq=1,uart_age_ms=5000,peripheral_age_ms=6000)
        self.assertEqual(self.state.status()["uart_age_ms"],4000)
        self.assertFalse(self.state.status()["uart_valid"])
        self.assertTrue(self.state.status()["device_online"])
        self.assertNotIn("ESP_STATUS", "\n".join(self.state.logs()["logs"]))

    def test_ack_is_final_only_and_commands_write_once(self):
        self.status_line()
        self.bat()
        current = self.state.request_current(850,PROFILE)["charge_control"]["pending"]
        command = f"ESP CHARGE_CURRENT {current['id']} 850 14800 abcdef01\n".encode()
        self.assertEqual(self.receiver.commands(),[command])
        self.assertTrue(self.receiver.should_send(command))
        self.assertEqual(self.receiver.commands(),[])
        self.receiver.feed(f"BAT_CURRENT_ACK id={current['id']} target_mA=850 status=PENDING reason=WAIT applied_mA=1850\n".encode())
        self.assertEqual(self.state.status()["charge_control"]["pending"]["status"],"SENT")
        self.receiver.feed(f"BAT_CURRENT_ACK id={current['id']} target_mA=850 status=APPLIED reason=OK applied_mA=850\n".encode())
        self.assertEqual(self.state.status()["charge_control"]["pending"]["status"],"AWAITING_READBACK")
        self.bat(850)
        self.assertEqual(self.state.status()["charge_control"]["last"]["status"],"CONFIRMED")
        self.assertEqual(self.receiver.commands(),[])

    def test_charge_requires_fresh_board_clock_and_never_renews_expired_deadline(self):
        self.bat()
        self.state.request_current(850,PROFILE)
        self.assertEqual(self.receiver.commands(),[])
        self.status_line()
        command = self.receiver.commands()[0]
        self.assertTrue(self.receiver.should_send(command))
        self.now = 15
        self.assertFalse(self.receiver.should_send(command))
        self.assertEqual(self.receiver.commands(),[])
        self.assertEqual(self.state.status()["charge_control"]["last"]["status"],"TIMED_OUT")

    def test_pause_after_ttl_snapshot_cannot_extend_device_deadline(self):
        self.status_line()
        self.state.request_current(850,PROFILE)
        original = self.state.serial_commands
        def pause_after_snapshot():
            result = original()
            self.now += 0.5
            return result
        with patch.object(self.state,"serial_commands",side_effect=pause_after_snapshot):
            command = self.receiver.commands()[0].decode().split()
        self.assertEqual(command[4],"14800")

    def test_disconnect_reconnect_and_board_boot_change_cancel_without_replay(self):
        self.status_line()
        self.state.request_current(850,PROFILE)
        self.receiver.commands()
        self.receiver.disconnected("USB removed")
        self.assertFalse(self.state.status()["device_online"])
        self.assertEqual(self.state.status()["charge_control"]["last"]["status"],"CANCELLED_TRANSPORT")
        self.receiver.connected()
        self.assertFalse(self.state.status()["device_online"])
        self.assertEqual(self.receiver.commands(),[])
        self.status_line(seq=1)
        self.state.request_scan()
        old_boot = self.state.boot_id
        self.status_line(boot_id="boot-b")
        self.assertNotEqual(self.state.boot_id,old_boot)
        self.assertEqual(self.state.status()["i2c_scan_control"]["last"]["status"],"CANCELLED_BOOT")
        seq = self.state.seq
        self.status_line(seq=2)
        self.assertEqual(self.state.seq,seq)  # retired board boot ignored

    def test_bounded_lines_bad_json_and_duplicate_status_never_refresh(self):
        self.status_line()
        self.now = 1
        self.status_line()
        self.assertEqual(self.state.status()["device_age_ms"],1000)
        self.receiver.feed(b"ESP_STATUS {\n")
        self.receiver.feed(b"X"*(MAX_LINE+10)+b"\n")
        self.assertEqual(self.receiver.oversize,1)
        self.assertEqual(self.receiver.invalid_status,1)
        self.assertEqual(len(self.receiver.buffer),0)
        self.bat()
        self.assertTrue(self.state.status()["uart_valid"])
        self.assertLessEqual(len(self.state.logs()["logs"]),20)
        self.assertNotIn("X"*10,"\n".join(self.state.logs()["logs"]))

    def test_scan_one_write_matching_result_and_timeout(self):
        self.status_line()
        command = self.state.request_scan()["i2c_scan_control"]["pending"]
        self.assertEqual(self.receiver.commands(),[f"ESP I2C_SCAN {command['id']}\n".encode()])
        self.assertEqual(self.receiver.commands(),[])
        completed = {"id":"a"*16,"completed":True,"running":False,"addresses":[16,32],"count":2,"error":0}
        self.status_line(seq=1,i2c_scan=completed)
        self.assertIsNotNone(self.state.status()["i2c_scan_control"]["pending"])
        completed["id"] = command["id"]
        self.status_line(seq=2,i2c_scan=completed)
        self.assertEqual(self.state.status()["i2c_scan_control"]["last"]["status"],"COMPLETED")
        self.state.request_scan()
        self.now = 15
        self.assertEqual(self.receiver.commands(),[])
        self.assertEqual(self.state.status()["i2c_scan_control"]["last"]["status"],"TIMED_OUT")

    def test_camera_check_preserves_hardware_state_without_enabling_usb_preview(self):
        self.status_line(camera={"ready":True,"state":"READY","sensor_pid":0x5640})
        status = self.state.status()
        self.assertTrue(status["camera"]["ready"])
        self.assertEqual(status["camera"]["state"],"READY")
        self.assertEqual(status["preview_transport"],"unavailable")
        self.assertFalse(status["frame_available"])
        command = self.state.request_camera_check()["camera_check_control"]["pending"]
        wire = f"ESP CAMERA_CHECK {command['id']}\n".encode()
        self.assertEqual(self.receiver.commands(),[wire])
        self.assertTrue(self.receiver.should_send(wire))
        self.assertEqual(self.receiver.commands(),[])
        self.receiver.disconnected("USB removed")
        self.assertFalse(self.receiver.should_send(wire))
        self.assertEqual(self.state.status()["camera_check_control"]["last"]["status"],"CANCELLED_TRANSPORT")

    def test_usb_audio_lease_sequence_pcm_queue_and_no_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            state = TelemetryState(data_dir=directory,monotonic=lambda:self.now,wall=lambda:1000+self.now,transport="serial",serial_port="COM18")
            receiver = SerialReceiver(state,"COM18")
            receiver.feed(("ESP_STATUS "+json.dumps(ready_payload(esp_peripherals={"audio":{"configured":True}}))+"\n").encode())
            session = state.request_audio("browser-a",True)["audio_monitor"]["session_id"]
            self.assertEqual(receiver.commands(),[f"ESP AUDIO_START {session} 15000\n".encode()])
            self.assertEqual(receiver.commands(),[])
            pcm = b"\x00\x80\xff\x7f"
            packet = {"session_id":session,"seq":0,"sample_rate":16000,"channels":1,"format":"s16le","data":base64.b64encode(pcm).decode()}
            line = ("ESP_AUDIO "+json.dumps(packet)+"\r\n").encode()
            receiver.feed(line[:30]); receiver.feed(line[30:])
            self.assertEqual(state.read_audio("browser-a",session,-1)[2],pcm)
            receiver.feed(line)
            self.assertEqual(state.audio_received,1)
            bad = copy.deepcopy(packet); bad["seq"]=1; bad["data"]="###not-base64"
            receiver.feed(("ESP_AUDIO "+json.dumps(bad)+"\n").encode())
            self.assertEqual(state.audio_received,1)
            self.now = 5
            self.assertEqual(receiver.commands(),[f"ESP AUDIO_START {session} 10000\n".encode()])
            state.request_audio("browser-a",False)
            self.assertEqual(receiver.commands(),[f"ESP AUDIO_STOP {session}\n".encode()])
            self.assertEqual(receiver.commands(),[])
            receiver.feed(line)
            self.assertEqual(len(state.audio_packets),0)
            self.assertNotIn("ESP_AUDIO","\n".join(state.logs()["logs"]))
            state.logger.handlers[0].close()
            recorded = (Path(directory)/"telemetry.jsonl").read_text(encoding="utf-8")
            self.assertNotIn(packet["data"],recorded)
            self.assertNotIn("ESP_AUDIO",recorded)

    def test_no_codec_no_audio_and_boot_change_clears_lease(self):
        self.status_line(esp_peripherals={"audio":{"configured":False}})
        session = self.state.request_audio("browser-a",True)["audio_monitor"]["session_id"]
        packet = {"session_id":session,"seq":0,"sample_rate":16000,"channels":1,"format":"s16le","data":"AAA="}
        self.receiver.feed(("ESP_AUDIO "+json.dumps(packet)+"\n").encode())
        self.assertEqual(self.state.audio_received,0)
        self.status_line(boot_id="boot-b")
        self.assertFalse(self.state.status()["audio_monitor"]["enabled"])


if __name__ == "__main__":
    unittest.main()
