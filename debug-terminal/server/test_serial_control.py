import http.client
import json
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from robot_server import Rejected, RobotServer, TelemetryState
from serial_bridge import SerialBridge, SerialReceiver
from test_server import PROFILE, TOKEN, ready_payload


def peripheral(pulses=(0,0,0,0), mask=0, allowed=1, tick=1):
    return f"PERIPH t={tick} servo_allowed={allowed} servo_mask={mask} "+" ".join(f"servo{i}_us={p}" for i,p in enumerate(pulses))


class ControlTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.state = TelemetryState(monotonic=lambda:self.now,transport="serial",serial_port="COM9")
        self.receiver = SerialReceiver(self.state,"COM9")
        self.snapshot()

    def snapshot(self, **changes):
        data = ready_payload(boot_id="abcdef0100000000",peripheral_raw=peripheral(),peripheral_age_ms=0,
                             esp_peripherals={"audio":{"configured":True}},**changes)
        self.receiver.feed(("ESP_STATUS "+json.dumps(data)+"\n").encode())

    def request(self, channel=0, pulse_us=1500, epoch=None):
        return self.state.request_servo({"channel":channel,"pulse_us":pulse_us,
                                         "expected_epoch":epoch or self.state.status()["servo_control"]["epoch"]})

    def send(self, wire):
        self.assertIn(wire,self.receiver.commands())
        self.assertTrue(self.receiver.should_send(wire))
        self.assertFalse(self.receiver.should_send(wire))

    def test_no_motion_on_connection_and_readback_is_not_position(self):
        self.assertEqual(self.receiver.commands(),[])
        control = self.state.status()["servo_control"]
        self.assertTrue(control["can_apply"])
        self.assertEqual([c["pulse_us"] for c in control["channels"]],[0]*4)
        self.assertEqual((control["min_us"],control["max_us"],control["period_us"]),(500,2500,20000))

    def test_extended_endpoints_send_and_confirm_real_output_readback(self):
        for pulse in (500,2500):
            with self.subTest(pulse=pulse):
                self.request(1,pulse)
                self.send(f"PERIPH SERVO 1 {pulse}\n".encode())
                self.receiver.feed(b"PERIPH_ACK cmd=SERVO ok=1 reason=OK\n")
                self.receiver.feed((peripheral((0,pulse,0,0),2,tick=pulse)+"\n").encode())
                control = self.state.status()["servo_control"]
                self.assertIsNone(control["pending"])
                self.assertEqual(control["last"]["status"],"CONFIRMED")
                self.assertEqual(control["channels"][1],{"channel":1,"pulse_us":pulse,"enabled":True})

    def test_out_of_range_readback_cannot_confirm_endpoint(self):
        self.request(0,500)
        self.send(b"PERIPH SERVO 0 500\n")
        self.receiver.feed(b"PERIPH_ACK cmd=SERVO ok=1 reason=OK\n")
        for pulse in (499,2501):
            self.receiver.feed((peripheral((pulse,0,0,0),1,tick=pulse)+"\n").encode())
            control = self.state.status()["servo_control"]
            self.assertEqual(control["channels"],[])
            self.assertEqual(control["pending"]["status"],"AWAITING_READBACK")
        self.receiver.feed((peripheral((500,0,0,0),1,tick=2502)+"\n").encode())
        self.assertEqual(self.state.status()["servo_control"]["last"]["status"],"CONFIRMED")

    def test_ack_and_new_direct_readback_required(self):
        self.request(2,1600)
        self.receiver.feed(b"PERIPH_ACK cmd=SERVO ok=1 reason=OK\n")
        self.assertEqual(self.state.status()["servo_control"]["pending"]["status"],"QUEUED")
        self.send(b"PERIPH SERVO 2 1600\n")
        self.receiver.feed((peripheral((0,0,1600,0),4,tick=2)+"\n").encode())
        self.assertEqual(self.state.status()["servo_control"]["pending"]["status"],"SENT")
        self.receiver.feed(b"PERIPH_ACK cmd=SERVO ok=1 reason=OK\n")
        self.assertEqual(self.state.status()["servo_control"]["pending"]["status"],"AWAITING_READBACK")
        self.receiver.feed((peripheral((0,0,1600,0),4,tick=3)+"\n").encode())
        control = self.state.status()["servo_control"]
        self.assertIsNone(control["pending"])
        self.assertEqual(control["last"]["status"],"CONFIRMED")
        self.assertIn("没有舵机位置反馈",control["last"]["detail"])
        self.assertEqual(self.receiver.commands(),[])

    def test_cached_ack_wrong_mask_and_rejected_ack(self):
        self.request()
        self.send(b"PERIPH SERVO 0 1500\n")
        self.snapshot(seq=1,logs=["PERIPH_ACK cmd=SERVO ok=1 reason=OK"])
        self.assertEqual(self.state.status()["servo_control"]["pending"]["status"],"SENT")
        self.receiver.feed(b"PERIPH_ACK cmd=STOP ok=1 reason=OK\n")
        self.assertEqual(self.state.status()["servo_control"]["pending"]["status"],"SENT")
        self.receiver.feed(b"PERIPH_ACK cmd=SERVO ok=0 reason=BATTERY\n")
        self.assertEqual(self.state.status()["servo_control"]["last"]["status"],"FAILED")

    def test_stop_preempts_and_invalidates_delayed_apply(self):
        epoch = self.state.status()["servo_control"]["epoch"]
        self.request()
        old = self.receiver.commands()[0]
        self.state.request_servo({"action":"stop"})
        self.assertFalse(self.receiver.should_send(old))
        self.send(b"PERIPH STOP\n")
        self.receiver.feed(b"PERIPH_ACK cmd=STOP ok=1 reason=OK\n")
        self.receiver.feed((peripheral()+"\n").encode())
        self.assertEqual(self.state.status()["servo_control"]["last"]["status"],"CONFIRMED")
        with self.assertRaises(Rejected):
            self.request(epoch=epoch)
        self.request()

    def test_interlock_blocks_apply_but_not_stop_and_expires_channels(self):
        self.receiver.feed((peripheral(allowed=0)+"\n").encode())
        self.assertFalse(self.state.status()["servo_control"]["can_apply"])
        with self.assertRaises(Rejected): self.request()
        self.state.request_servo({"action":"stop"})
        self.send(b"PERIPH STOP\n")
        self.now = 3.1
        self.assertEqual(self.state.status()["servo_control"]["channels"],[])
        self.assertFalse(self.state.status()["servo_control"]["can_apply"])

    def test_timeout_never_resends_and_bounds_are_strict(self):
        for channel,pulse in ((True,1500),(4,1500),(0,499),(0,2501),(0,1500.0),(0,500.0),(0,2500.0),(0,True),(0,False)):
            with self.subTest(channel=channel,pulse=pulse),self.assertRaises(Rejected): self.request(channel,pulse)
        self.request()
        wire = self.receiver.commands()[0]
        self.now = 5
        self.assertFalse(self.receiver.should_send(wire))
        self.assertEqual(self.state.status()["servo_control"]["last"]["status"],"TIMED_OUT")
        self.assertEqual(self.receiver.commands(),[])

    def test_disconnect_boot_cancel_and_refresh_epoch(self):
        epoch = self.state.status()["servo_control"]["epoch"]
        self.request()
        self.receiver.disconnected("unplugged")
        control = self.state.status()["servo_control"]
        self.assertNotEqual(control["epoch"],epoch)
        self.assertEqual(control["last"]["status"],"CANCELLED_TRANSPORT")
        self.receiver.connected()
        self.snapshot(seq=1)
        self.request()
        old_epoch = self.state.status()["servo_control"]["epoch"]
        data = ready_payload(boot_id="1234567800000000",peripheral_raw=peripheral(),peripheral_age_ms=0)
        self.receiver.feed(("ESP_STATUS "+json.dumps(data)+"\n").encode())
        self.assertEqual(self.state.status()["servo_control"]["last"]["status"],"CANCELLED_BOOT")
        self.assertNotEqual(self.state.status()["servo_control"]["epoch"],old_epoch)

    def test_switch_discards_old_data_and_pending_without_replay(self):
        self.request()
        old_wire = self.receiver.commands()[0]
        self.state.request_audio("owner",True)
        self.state.request_scan()
        bridge = SerialBridge(self.state,"COM9")
        result = bridge.select_port("COM18")
        self.assertTrue(result["serial_control"]["switching"])
        status = self.state.status()
        self.assertEqual(status["raw"],"")
        self.assertFalse(status["device_online"])
        self.assertFalse(status["audio_monitor"]["enabled"])
        self.assertIsNone(status["i2c_scan_control"]["pending"])
        self.assertIsNone(status["servo_control"]["pending"])
        self.receiver.feed((peripheral((1500,0,0,0),1)+"\n").encode())
        self.receiver.disconnected("old port failed")
        self.assertIsNone(self.state.status()["serial_error"])
        self.assertFalse(self.receiver.should_send(old_wire))
        new = SerialReceiver(self.state,"COM18")
        new.feed(("ESP_STATUS "+json.dumps(ready_payload(boot_id="fedcba9800000000"))+"\n").encode())
        self.assertTrue(self.state.status()["device_online"])
        self.assertEqual(self.state.status()["device_id"],"serial-COM18")
        self.assertEqual(new.commands(),[])

    def test_selected_port_persists_without_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            state = TelemetryState(data_dir=directory,transport="serial",serial_port="")
            try:
                state.select_serial_port("COM18")
                self.assertEqual(json.loads((Path(directory)/"serial-port.json").read_text()),{"port":"COM18"})
            finally:
                for handler in state.logger.handlers: handler.close()


class PortThreadTests(unittest.TestCase):
    def test_empty_port_switch_error_recovery_and_no_reset_lines(self):
        opened,closed,writes = [],[],[]
        class FakeSerial:
            def __init__(self,**kwargs): self.port=None; self.dtr=True; self.rts=True
            def open(self):
                if self.port == "COM7": raise OSError("Access is denied: port occupied")
                opened.append((self.port,self.dtr,self.rts))
            def close(self): closed.append(self.port)
            @property
            def in_waiting(self): return 0
            def read(self,count): threading.Event().wait(0.01); return b""
            def write(self,data): writes.append((self.port,data))
        def until(predicate):
            deadline=time.monotonic()+2
            while not predicate() and time.monotonic()<deadline: threading.Event().wait(0.01)
            self.assertTrue(predicate())
        state = TelemetryState(transport="serial",serial_port="")
        bridge = SerialBridge(state,"")
        with patch("serial.Serial",FakeSerial):
            bridge.start()
            try:
                until(lambda:state.status()["serial_error"] == "请选择串口")
                self.assertEqual(opened,[])
                bridge.select_port("COM7")
                until(lambda:"occupied" in (state.status()["serial_error"] or ""))
                bridge.select_port("COM18")
                until(lambda:state.status()["serial_connected"])
                bridge.select_port("COM19")
                until(lambda:any(port == "COM19" for port,_,_ in opened))
                self.assertIn("COM18",closed)
                self.assertTrue(all(not dtr and not rts for _,dtr,rts in opened))
                self.assertTrue(all(wire == b"ESP STATUS\n" for _,wire in writes))
            finally: bridge.stop()


class ControlHttpTests(unittest.TestCase):
    def setUp(self):
        state=TelemetryState(transport="serial",serial_port="")
        self.server=RobotServer(("127.0.0.1",0),TOKEN,state)
        self.server.serial_bridge=SerialBridge(state,"")
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True)
        self.thread.start()
        self.origin=f"http://127.0.0.1:{self.server.server_address[1]}"
        self.ports=patch("serial.tools.list_ports.comports",return_value=[SimpleNamespace(device="COM18",description="CH340",hwid="USB")])
        self.ports.start()

    def tearDown(self):
        self.ports.stop(); self.server.shutdown(); self.server.server_close(); self.thread.join()

    def request(self,path,data=None,headers=None):
        connection=http.client.HTTPConnection("127.0.0.1",self.server.server_address[1])
        request_headers={"Content-Type":"application/json",**(headers or {})}
        connection.request("GET" if data is None else "POST",path,body=None if data is None else json.dumps(data),headers=request_headers)
        response=connection.getresponse(); result=(response.status,dict(response.getheaders()),json.loads(response.read()))
        connection.close(); return result

    def auth(self):
        _,headers,body=self.request("/api/session",headers={"Origin":self.origin})
        return {"Origin":self.origin,"Cookie":headers["Set-Cookie"].split(";",1)[0],"X-CSRF-Token":body["csrf_token"]}

    def test_ports_and_csrf_protected_strict_switch(self):
        code,_,body=self.request("/api/serial-ports")
        self.assertEqual(code,200); self.assertEqual(body["ports"][0]["device"],"COM18")
        self.assertEqual(self.request("/api/serial-port",{"port":"COM18"})[0],403)
        headers=self.auth()
        for payload in ({"port":"socket://example"},{"port":"COM99"},{"port":"COM18","extra":1},{"port":18}):
            self.assertEqual(self.request("/api/serial-port",payload,headers)[0],400)
        code,_,body=self.request("/api/serial-port",{"port":"COM18"},headers)
        self.assertEqual(code,202); self.assertTrue(body["serial_control"]["switching"])
        self.assertEqual(self.server.state.status()["serial_port"],"COM18")

    def test_servo_requires_origin_csrf_and_online(self):
        self.assertEqual(self.request("/api/servo",{"action":"stop"})[0],403)
        self.assertEqual(self.request("/api/servo",{"action":"stop"},self.auth())[0],409)
        self.assertEqual(self.request("/api/servo",{"action":"sweep"},self.auth())[0],400)


if __name__ == "__main__": unittest.main()
