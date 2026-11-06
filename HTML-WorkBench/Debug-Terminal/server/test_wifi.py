import http.client
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

from robot_server import Rejected, RobotServer, TelemetryState, validate_wifi_request, private_camera_ip
from serial_bridge import SerialReceiver
from test_server import TOKEN, ready_payload
from wifi_camera import WifiCameraProxy, fetch, PersistentFetcher, FrameStream, StreamUnavailable, MAX_FRAME_BYTES

BOOT = "abcdef0100000000"
JPEG = b"\xff\xd8real-frame-test\xff\xd9"


def wifi(**changes):
    return {"state":"READY","ssid":"Test 2.4G","ip":"192.168.1.38","rssi":-45,"request_id":"",
            "request_status":"","error":0,"save_error":0,"saved":True,"api_port":81,"api_ready":True,"disconnect_reason":0,**changes}


class WifiTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.state = TelemetryState(monotonic=lambda:self.now,wall=lambda:1000+self.now,transport="serial",serial_port="COM9")
        self.receiver = SerialReceiver(self.state,"COM9")
        self.sequence = 0

    def usb(self, **changes):
        data = ready_payload(boot_id=BOOT,seq=self.sequence,uptime_ms=100+int(self.now*1000),wifi=wifi())
        data.update(changes)
        self.sequence += 1
        self.receiver.feed(("ESP_STATUS "+json.dumps(data)+"\n").encode())

    def request(self, action="connect", **changes):
        payload = {"action":action}
        if action == "connect":
            payload.update(ssid="测试网络",password="test-pass-123")
        payload.update(changes)
        return self.state.request_wifi(payload)["wifi_control"]["pending"]

    def network(self, seq=100, **changes):
        target = self.state.wifi_camera_target()
        data = ready_payload(device_id=target["device"],boot_id=target["boot"],seq=seq,
                             uptime_ms=100+int(self.now*1000),wifi=wifi())
        data["control_proto"] = 1
        data.update(changes)
        headers = {"x-device-id":target["device"],"x-boot-id":target["boot"]}
        self.state.ingest_wifi_status(target,headers,data)
        return target,headers

    def frame(self, seq=1, **changes):
        target = self.state.wifi_camera_target()
        headers = {"x-device-id":target["device"],"x-boot-id":target["boot"],"x-frame-seq":str(seq),
                   "x-frame-timestamp-ms":str(100+int(self.now*1000)),**changes}
        self.state.ingest_wifi_frame(target,headers,JPEG)

    def test_scan_keeps_live_endpoint_and_sends_no_pairing_key(self):
        self.usb(public_test_build=True,wifi=wifi(scan_proto=1))
        target = self.state.wifi_camera_target()
        command = self.request('scan')
        self.assertEqual(self.state.wifi_camera_target(),target)
        fields = self.receiver.commands()[0].decode().split()
        self.assertEqual(fields,["ESP","WIFI_SCAN",command['id'],"4800","abcdef01"])
        self.usb(wifi=wifi(scan_proto=1,request_id=command['id'],request_status='SUCCEEDED',
                          scan={'request_id':'0'*16,'completed':True,'error':0,'networks':[]}))
        self.assertIsNotNone(self.state.status()['wifi_control']['pending'])
        self.usb(wifi=wifi(scan_proto=1,request_id=command['id'],request_status='SUCCEEDED',
                          scan={'request_id':command['id'],'completed':True,'error':0,'networks':[]}))
        self.assertEqual(self.state.status()['wifi_control']['last']['status'],'COMPLETED')

    def test_offline_scan_result_and_safe_utf8_ssid_normalization(self):
        self.usb(wifi=wifi(state='OFF',api_ready=False,scan_proto=1))
        command = self.request('scan')
        self.receiver.commands()
        ap = {'ssid_hex':'测试热点'.encode().hex(),'rssi':-55,'channel':6,'secure':True,'password':'must-not-leak'}
        self.usb(wifi=wifi(state='OFF',api_ready=False,scan_proto=1,request_id=command['id'],request_status='SUCCEEDED',
                          scan={'request_id':command['id'],'completed':True,'error':0,'password':'must-not-leak',
                                'networks':[ap,{**ap,'ssid_hex':'ff'}]}))
        status = self.state.status()
        self.assertEqual(self.receiver.invalid_status,0)
        self.assertEqual(status['wifi_control']['last']['status'],'COMPLETED')
        self.assertEqual(status['wifi']['scan']['networks'],[{'ssid':'测试热点','rssi':-55,'channel':6,'secure':True}])
        self.assertNotIn('must-not-leak',json.dumps(status))

    def test_scan_rejects_old_firmware_and_extra_credentials(self):
        self.usb()
        with self.assertRaises(Rejected): self.request('scan')
        with self.assertRaises(Rejected): validate_wifi_request({'action':'scan','password':'12345678'})

    def test_scan_uses_usb_while_motion_remains_on_active_wifi(self):
        self.usb(wifi=wifi(scan_proto=1))
        self.network(wifi=wifi(scan_proto=1))
        self.assertEqual(self.state.active_connection(),'wifi')
        command = self.request('scan')
        wire = self.receiver.commands()[0]
        self.assertTrue(wire.startswith(('ESP WIFI_SCAN '+command['id']).encode()))
        self.assertTrue(self.receiver.should_send(wire))
        self.assertFalse(self.receiver.should_send(b'PERIPH STOP\n'))
        self.assertEqual(self.receiver.commands(),[])

    def test_credentials_validation_utf8_byte_limits_open_and_psk(self):
        for ssid,password in (("华牧全安","12345678"),("a"*32,""),("n","a"*63),("n","a1"*32),("n","密码密码密码")):
            action,encoded = validate_wifi_request({"action":"connect","ssid":ssid,"password":password})
            self.assertEqual(action,"connect")
            self.assertEqual(bytes.fromhex(encoded[0]).decode(),ssid)
        invalid = [{},[],{"action":"unknown"},{"action":"reconnect","password":"secret"}]
        for ssid,password in (("","12345678"),("中"*11,"12345678"),("n","short"),("n","g"*64),
                              ("n","a"*65),("n","a\0bbbbbb"),("n",None),("\ud800","12345678")):
            invalid.append({"action":"connect","ssid":ssid,"password":password})
        for data in invalid:
            with self.subTest(data=data),self.assertRaises(Rejected): validate_wifi_request(data)

    def test_fresh_usb_is_required_even_with_battery_lines_or_wifi(self):
        with self.assertRaises(Rejected): self.request()
        self.usb()
        self.now = 3
        self.receiver.feed(b"BAT t=5 valid=1\n")
        with self.assertRaises(Rejected): self.request()
        self.usb()
        self.assertTrue(self.state.status()["wifi_control"]["can_control"])

    def test_command_exact_deadline_credentials_claimed_once_and_not_echoed(self):
        self.usb()
        command = self.request()
        wire = self.receiver.commands()[0]
        fields = wire.decode().split()
        self.assertEqual(fields[:5],["ESP","WIFI_CONNECT",command["id"],"4800","abcdef01"])
        self.assertEqual(bytes.fromhex(fields[5]).decode(),"测试网络")
        self.assertEqual(bytes.fromhex(fields[6]).decode(),"test-pass-123")
        self.assertTrue(self.receiver.should_send(wire))
        self.assertNotIn("credentials",self.state.pending_wifi)
        self.assertEqual(self.receiver.commands(),[])
        self.receiver.feed(wire)
        self.assertNotIn(fields[6],json.dumps(self.state.status()))
        self.assertNotIn("WIFI_CONNECT",json.dumps(self.state.logs()))
        self.now = 5
        self.assertFalse(self.receiver.should_send(wire))
        self.assertEqual(self.receiver.commands(),[])

    def test_pause_after_claim_cannot_extend_deadline(self):
        self.usb()
        self.request()
        original = self.state.take_wifi_command
        def paused(boot):
            result = original(boot)
            self.now += 0.5
            return result
        with patch.object(self.state,"take_wifi_command",side_effect=paused):
            self.assertEqual(self.receiver.commands()[0].decode().split()[3],"4800")

    def test_public_pairing_key_is_sent_once_only_over_usb_and_never_exposed(self):
        key = "1234567890abcdef" * 4
        self.state._wifi_pairing_token = key
        self.usb(public_test_build=True, wifi_pairing_proto=1)
        command = self.request()
        self.assertNotIn(key, json.dumps(self.state.pending_wifi))
        self.assertNotIn(key, json.dumps(self.state.status()))
        wire = self.receiver.commands()[0]
        fields = wire.decode().split()
        self.assertEqual(fields[-1], key)
        self.assertEqual(len(fields), 8)
        self.assertEqual(fields[:5], ["ESP", "WIFI_CONNECT", command["id"], "4800", "abcdef01"])
        self.assertTrue(self.receiver.should_send(wire))
        self.assertEqual(self.receiver.commands(), [])
        self.receiver.feed(wire)
        self.assertNotIn(key, json.dumps(self.state.status()))
        self.assertNotIn(key, json.dumps(self.state.logs()))
        self.now = 5
        self.assertFalse(self.receiver.should_send(wire))

    def test_pairing_reconnect_and_disconnect_keep_distinct_protocols(self):
        key = "abcd" * 16
        self.state._wifi_pairing_token = key
        self.usb(public_test_build=True, wifi_pairing_proto=1)
        command = self.request("reconnect")
        fields = self.receiver.commands()[0].decode().split()
        self.assertEqual(fields[-1], key)
        self.assertEqual(len(fields), 6)
        self.usb(public_test_build=True, wifi_pairing_proto=1,
                 wifi=wifi(request_id=command["id"], request_status="SUCCEEDED"))
        self.request("disconnect")
        fields = self.receiver.commands()[0].decode().split()
        self.assertEqual(len(fields), 5)
        self.assertNotIn(key, fields)

    def test_public_pairing_requires_capability_and_valid_host_key(self):
        self.usb(public_test_build=True)
        with self.assertRaisesRegex(Rejected, "固件"):
            self.request()
        self.assertIsNone(self.state.pending_wifi)
        self.usb(public_test_build=True, wifi_pairing_proto=1)
        for key in (None, "", "a" * 63, "a" * 65, "A" * 64):
            self.state._wifi_pairing_token = key
            with self.assertRaisesRegex(Rejected, "认证"):
                self.request()
            self.assertIsNone(self.state.pending_wifi)
        before = self.receiver.invalid_status
        self.usb(public_test_build=True, wifi_pairing_proto=True)
        self.assertEqual(self.receiver.invalid_status, before + 1)

    def test_legacy_firmware_never_receives_host_pairing_key(self):
        key = "abcd" * 16
        self.state._wifi_pairing_token = key
        self.usb()
        self.request()
        fields = self.receiver.commands()[0].decode().split()
        self.assertEqual(len(fields), 7)
        self.assertNotIn(key, fields)

    def test_pairing_key_never_enters_telemetry_recorder(self):
        key = "abcdef01" * 8
        with tempfile.TemporaryDirectory() as directory:
            state = TelemetryState(directory, transport="serial", serial_port="COM9")
            state._wifi_pairing_token = key
            receiver = SerialReceiver(state, "COM9")
            payload = ready_payload(boot_id=BOOT, wifi=wifi(), public_test_build=True, wifi_pairing_proto=1)
            receiver.feed(("ESP_STATUS " + json.dumps(payload) + "\n").encode())
            state.request_wifi({"action": "reconnect"})
            wire = receiver.commands()[0]
            self.assertIn(key.encode(), wire)
            receiver.feed(wire)
            state.logger.handlers[0].close()
            self.assertNotIn(key, (Path(directory) / "telemetry.jsonl").read_text(encoding="utf-8"))
            self.assertNotIn(key, json.dumps(state.status()))

    def test_unsent_expires_and_matching_final_only(self):
        self.usb()
        command = self.request()
        self.usb(wifi=wifi(request_id="f"*16,request_status="SUCCEEDED"))
        self.assertIsNotNone(self.state.status()["wifi_control"]["pending"])
        self.assertIsNone(self.state.wifi_camera_target())
        self.receiver.commands()
        self.usb(wifi=wifi(request_id=command["id"],request_status="SUCCEEDED",state="CONNECTING"))
        self.assertIsNotNone(self.state.status()["wifi_control"]["pending"])
        self.usb(wifi=wifi(request_id=command["id"],request_status="SUCCEEDED"))
        self.assertEqual(self.state.status()["wifi_control"]["last"]["status"],"COMPLETED")
        self.assertIsNotNone(self.state.wifi_camera_target())
        self.request("reconnect")
        self.now = 5
        self.assertEqual(self.receiver.commands(),[])
        self.assertEqual(self.state.status()["wifi_control"]["last"]["status"],"TIMED_OUT")

    def test_disconnect_boot_and_flow_timeout_do_not_replay(self):
        self.usb()
        command = self.request("disconnect")
        wire = self.receiver.commands()[0]
        self.assertEqual(wire.decode().split()[:3],["ESP","WIFI_DISCONNECT",command["id"]])
        self.usb(wifi=wifi(request_id=command["id"],request_status="SUCCEEDED",state="OFF",ip="",api_ready=False))
        self.assertEqual(self.state.status()["wifi_control"]["last"]["status"],"COMPLETED")
        self.assertIsNone(self.state.wifi_camera_target())
        self.request("reconnect"); self.receiver.commands()
        self.now = 45
        self.assertEqual(self.state.status()["wifi_control"]["last"]["status"],"TIMED_OUT")
        self.usb(); self.request()
        self.usb(boot_id="1234567800000000")
        self.assertEqual(self.state.status()["wifi_control"]["last"]["status"],"CANCELLED_BOOT")
        self.assertEqual(self.receiver.commands(),[])
        self.request(); self.receiver.disconnected("USB removed"); self.receiver.connected()
        self.assertEqual(self.state.status()["wifi_control"]["last"]["status"],"CANCELLED_TRANSPORT")
        self.assertEqual(self.receiver.commands(),[])

    def test_failed_result_and_busy_rejection(self):
        self.usb()
        command = self.request("reconnect")
        with self.assertRaises(Rejected): self.request("disconnect")
        self.receiver.commands()
        self.usb(wifi=wifi(request_id=command["id"],request_status="FAILED",state="ERROR",error=259,ip="",api_ready=False))
        self.assertEqual(self.state.status()["wifi_control"]["last"]["status"],"FAILED")
        self.assertIn("259",self.state.status()["wifi_control"]["last"]["detail"])

    def test_password_never_recorded_and_unknown_status_fields_dropped(self):
        with tempfile.TemporaryDirectory() as directory:
            state = TelemetryState(directory,transport="serial",serial_port="COM9")
            receiver = SerialReceiver(state,"COM9")
            receiver.feed(("ESP_STATUS "+json.dumps(ready_payload(boot_id=BOOT,wifi=wifi(password="private-test-password",future_metric=123)))+"\n").encode())
            state.request_wifi({"action":"connect","ssid":"Network","password":"private-test-password"})
            wire = receiver.commands()[0]
            receiver.feed(wire)
            state.logger.handlers[0].close()
            recorded = (Path(directory)/"telemetry.jsonl").read_text(encoding="utf-8")
            for value in ("private-test-password",b"private-test-password".hex()):
                self.assertNotIn(value,recorded)
                self.assertNotIn(value,json.dumps(state.status()))
        self.usb(wifi=wifi(password="secret"))
        self.assertEqual(self.receiver.invalid_status,0)
        self.assertNotIn("password",self.state.status()["wifi"])

    def test_full_firmware_wifi_schema_save_error_and_future_fields(self):
        complete = wifi(state="OFF",ssid="",ip="",rssi=0,request_id="0123456789abcdef",
                        request_status="FAILED",error=259,save_error=4354,saved=False,api_ready=False,
                        stream_port=82,stream_ready=False,disconnect_reason=201,future_field={"some_new_counter":1})
        self.usb(wifi=complete)
        self.assertEqual(self.receiver.invalid_status,0)
        status = self.state.status()
        self.assertTrue(status["camera"]["ready"])
        self.assertEqual(status["wifi"]["save_error"],4354)
        self.assertEqual(status["wifi"]["stream_port"],82)
        self.assertFalse(status["wifi"]["stream_ready"])
        self.assertNotIn("future_field",status["wifi"])
        for invalid in (True,"0",0.5,None,2**31):
            self.usb(wifi=wifi(save_error=invalid))
        self.assertEqual(self.receiver.invalid_status,5)
        self.usb(wifi=wifi(stream_ready=1))
        self.usb(wifi=wifi(stream_port=True))
        self.assertEqual(self.receiver.invalid_status,7)

    def test_stream_latest_only_fps_and_generation_invalidation(self):
        self.usb(); target,headers = self.network(wifi=wifi(stream_port=82,stream_ready=True))
        self.state.accept_wifi_stream_identity(target,{**headers,"x-wifi-generation":"1"})
        self.frame()
        first = self.state.next_stream_frame(timeout=0)
        self.assertIsNone(self.state.next_stream_frame(first[:2],timeout=0))
        self.now = 0.04; self.frame(seq=2)
        self.now = 0.08; self.frame(seq=3)
        latest = self.state.next_stream_frame(first[:2],timeout=0)
        self.assertEqual(latest[1],3)
        self.assertAlmostEqual(self.state.status()["camera_wifi"]["fps"],25)
        self.state.accept_wifi_stream_identity(target,{**headers,"x-wifi-generation":"2"})
        self.assertIsNone(self.state.wifi_frame)
        with self.assertRaises(Rejected): self.state.next_stream_frame(latest[:2],timeout=0)
        self.frame(seq=4)
        self.assertNotEqual(self.state.next_stream_frame(timeout=0)[0],latest[0])
        self.network(seq=101,wifi=wifi(stream_port=82,stream_ready=False))
        self.assertFalse(self.state.status()["camera_wifi"]["stream_available"])
        with self.assertRaises(Rejected): self.frame(seq=5)

    def test_declared_stream_failure_never_falls_back(self):
        for declared in (False,True):
            with self.subTest(declared=declared):
                self.setUp(); self.usb()
                self.network(wifi=wifi(**({"stream_port":82,"stream_ready":True} if declared else {})))
                calls = []
                proxy = WifiCameraProxy(self.state,TOKEN,lambda *args:calls.append(args))
                original = self.state.wifi_camera_failed
                def fail(*args,**kwargs):
                    original(*args,**kwargs); proxy.stopping.set()
                with patch("wifi_camera.FrameStream",side_effect=StreamUnavailable("not supported")),patch.object(self.state,"wifi_camera_failed",side_effect=fail):
                    proxy._capture_loop()
                self.assertEqual(len(calls),0 if declared else 1)

    def test_one_hz_status_poll_does_not_reuse_expiring_socket(self):
        self.usb()
        with patch("wifi_camera.fetch") as one_shot:
            one_shot.side_effect = [
                ({"content-type":"application/json","x-device-id":"robot-dog","x-boot-id":BOOT},
                 json.dumps(ready_payload(boot_id=BOOT,seq=seq,wifi=wifi(stream_port=82,stream_ready=True))).encode())
                for seq in (10,11)]
            proxy = WifiCameraProxy(self.state,TOKEN)
            proxy._poll_status()
            self.now = 1
            proxy._poll_status()
            self.assertEqual(one_shot.call_count,2)
            self.assertIsInstance(proxy.capture_fetcher,PersistentFetcher)
            self.assertTrue(self.state.status()["camera_wifi"]["reachable"])

    def test_endpoint_only_from_private_usb_identity_and_fixed_port(self):
        for ip in ("127.0.0.1","169.254.1.1","8.8.8.8","::1","http://192.168.1.1","192.168.1.1:81","0.0.0.0"):
            self.assertIsNone(private_camera_ip(ip))
            self.usb(wifi=wifi(ip=ip))
            self.assertIsNone(self.state.wifi_camera_target())
        self.usb(wifi=wifi(api_port=80))
        self.assertIsNone(self.state.wifi_camera_target())
        self.usb()
        self.assertEqual(self.state.wifi_camera_target()["ip"],"192.168.1.38")

    def test_usb_unplug_keeps_full_network_state_and_controls(self):
        self.usb(); self.network(); self.frame()
        self.receiver.disconnected("USB unplugged")
        self.now = 1
        self.network(seq=101); self.frame(seq=2)
        status = self.state.status()
        self.assertTrue(status["device_online"])
        self.assertEqual(status["connection"]["active"],"wifi")
        self.assertFalse(status["wifi_control"]["can_control"])
        self.assertTrue(status["charge_control"]["can_apply"])
        self.assertEqual(status["preview_transport"],"wifi_proxy")
        self.assertTrue(status["camera_wifi"]["reachable"])
        self.assertTrue(status["frame_available"])
        self.assertEqual(self.state.capture()[0],JPEG)
        self.now = 4
        self.assertFalse(self.state.status()["frame_available"])
        with self.assertRaises(Rejected): self.state.capture()

    def test_frames_cannot_cross_boot_endpoint_or_reuse_sequence(self):
        self.usb(); target,headers = self.network(); self.frame()
        with self.assertRaises(Rejected): self.frame()
        with self.assertRaises(Rejected): self.frame(seq=2,**{"x-boot-id":"other"})
        with self.assertRaises(Rejected): self.state.ingest_wifi_status(target,headers,ready_payload(boot_id="new-boot",seq=101))
        self.usb(boot_id="1234567800000000")
        self.assertFalse(self.state.status()["frame_available"])
        with self.assertRaises(Rejected): self.state.capture()
        self.state.wifi_camera_failed(target,"OLD_FAILURE")
        self.assertIsNone(self.state.status()["camera_wifi"]["error"])

    def test_capture_timestamp_age_is_not_reset_by_receiving_old_frame(self):
        self.usb(); self.network()
        self.now = 2
        self.frame(**{"x-frame-timestamp-ms":"100"})
        self.assertEqual(self.state.status()["camera_wifi"]["frame_age_ms"],2000)
        self.now = 3
        self.network(seq=101)
        with self.assertRaises(Rejected): self.frame(seq=2,**{"x-frame-timestamp-ms":"100"})
        with self.assertRaises(Rejected): self.state.capture()

    def test_proxy_order_identity_failure_disconnect_and_no_redirect(self):
        self.usb()
        calls = []
        def getter(ip,path,token,limit,timeout):
            calls.append((ip,path,limit,timeout))
            headers = {"x-device-id":"robot-dog","x-boot-id":BOOT}
            if path == "/api/status":
                return {**headers,"content-type":"application/json"},json.dumps(ready_payload(boot_id=BOOT,wifi=wifi())).encode()
            return {**headers,"content-type":"image/jpeg","x-frame-seq":"1","x-frame-timestamp-ms":"100"},JPEG
        proxy = WifiCameraProxy(self.state,TOKEN,getter)
        proxy.poll_once()
        self.assertEqual([call[1] for call in calls],["/api/status","/capture.jpg"])
        self.assertEqual(calls[1][3],6)
        self.assertEqual(self.state.capture()[0],JPEG)
        proxy.fetcher = lambda *args: (_ for _ in ()).throw(OSError("not logged"))
        proxy.poll_once()
        self.assertFalse(self.state.status()["camera_wifi"]["reachable"])
        self.assertIsNone(self.state.wifi_frame)


class WifiHttpTests(unittest.TestCase):
    def setUp(self):
        self.state = TelemetryState(transport="serial",serial_port="COM9")
        self.receiver = SerialReceiver(self.state,"COM9")
        self.receiver.feed(("ESP_STATUS "+json.dumps(ready_payload(boot_id=BOOT,wifi=wifi()))+"\n").encode())
        self.server = RobotServer(("127.0.0.1",0),TOKEN,self.state)
        self.thread = threading.Thread(target=self.server.serve_forever,daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join()

    def request(self,path,data=None,extra=None):
        connection = http.client.HTTPConnection(*self.server.server_address,timeout=2)
        connection.request("POST" if data is not None else "GET",path,data,{"Content-Type":"application/json",**(extra or {})})
        response = connection.getresponse()
        result = response.status,response.read(),dict(response.headers)
        connection.close()
        return result

    def test_csrf_same_origin_and_no_password_response(self):
        request = json.dumps({"action":"connect","ssid":"Network","password":"private-http-password"})
        self.assertEqual(self.request("/api/wifi",request)[0],403)
        origin = "http://%s:%d" % self.server.server_address
        _,body,headers = self.request("/api/session",extra={"Origin":origin})
        browser = {"Origin":origin,"Cookie":headers["Set-Cookie"].split(";",1)[0],"X-CSRF-Token":json.loads(body)["csrf_token"]}
        self.assertEqual(self.request("/api/wifi",request,{**browser,"Origin":"http://evil.example"})[0],403)
        status,body,_ = self.request("/api/wifi",request,browser)
        self.assertEqual(status,202)
        self.assertNotIn(b"private-http-password",body)
        self.assertIn("wifi_control",json.loads(body))

    def test_native_mjpeg_serves_distinct_frames_then_ends_on_failure(self):
        self.assertEqual(self.request("/stream.mjpg",extra={"Origin":"http://evil.example"})[0],403)
        target = self.state.wifi_camera_target()
        identity = {"x-device-id":target["device"],"x-boot-id":target["boot"]}
        self.state.ingest_wifi_status(target,identity,ready_payload(boot_id=BOOT,seq=10,uptime_ms=100,wifi=wifi(stream_port=82,stream_ready=True)))
        def frame(seq):
            self.state.ingest_wifi_frame(target,{**identity,"x-frame-seq":str(seq),"x-frame-timestamp-ms":"100"},JPEG)
        frame(1)
        connection = http.client.HTTPConnection(*self.server.server_address,timeout=2)
        try:
            connection.request("GET","/stream.mjpg")
            response = connection.getresponse()
            self.assertEqual(response.status,200)
            self.assertIn("multipart/x-mixed-replace",response.getheader("Content-Type"))
            for seq in (1,2):
                self.assertEqual(response.readline(),b"--robotframe\r\n")
                headers = {}
                while True:
                    line = response.readline()
                    if line == b"\r\n": break
                    key,value = line.decode().split(":",1); headers[key.lower()] = value.strip()
                self.assertEqual(headers["x-frame-seq"],str(seq))
                self.assertEqual(response.read(int(headers["content-length"])),JPEG)
                self.assertEqual(response.read(2),b"\r\n")
                if seq == 1: frame(2)
            self.state.wifi_camera_failed(target,"TEST_DISCONNECT")
            self.assertEqual(response.read(),b"")
        finally:
            connection.close()


class FetchTests(unittest.TestCase):
    def response(self,body=b"ok",status=200,extra=None):
        raw = b"HTTP/1.1 "+str(status).encode()+b" OK\r\nContent-Length: "+str(len(body)).encode()+b"\r\n"
        raw += (extra or b"")+b"\r\n"+body
        class Sock:
            def makefile(self,*args): return io.BufferedReader(io.BytesIO(raw))
        response = http.client.HTTPResponse(Sock()); response.begin()
        return response

    def test_fetch_refuses_redirect_oversize_transfer_encoding_and_incomplete(self):
        for response,limit in ((self.response(status=302),20),(self.response(b"large"),3),
                               (self.response(extra=b"Transfer-Encoding: chunked\r\n"),20),
                               (self.response(extra=b"Content-Length: 2\r\n"),20)):
            with patch("wifi_camera.http.client.HTTPConnection") as factory:
                factory.return_value.getresponse.return_value = response
                with self.assertRaises(ValueError): fetch("192.168.1.38","/api/status",TOKEN,limit,2)
                self.assertEqual(factory.call_count,1)
        with patch("wifi_camera.http.client.HTTPConnection") as factory:
            factory.return_value.getresponse.return_value = self.response()
            _,body = fetch("192.168.1.38","/api/status",TOKEN,20,2)
            self.assertEqual(body,b"ok")
            self.assertEqual(factory.call_args.args,("192.168.1.38",81))

    def test_absolute_timeout_stops_trickling_connection_close_body(self):
        class SlowHandler(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Length","100")
                self.send_header("Connection","close")
                self.end_headers()
                try:
                    for _ in range(100):
                        self.wfile.write(b"x"); self.wfile.flush(); time.sleep(0.02)
                except OSError:
                    pass
        server = ThreadingHTTPServer(("127.0.0.1",0),SlowHandler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever,daemon=True); thread.start()
        real_connection = http.client.HTTPConnection
        try:
            with patch("wifi_camera.http.client.HTTPConnection",side_effect=lambda *a,**kw:real_connection(*server.server_address,**kw)):
                started = time.monotonic()
                with self.assertRaises((ValueError,OSError,http.client.HTTPException)):
                    fetch("192.168.1.38","/capture.jpg",TOKEN,1000,0.15)
                self.assertLess(time.monotonic()-started,0.8)
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_complete_requests_reuse_connection_but_errors_reset_it(self):
        with patch("wifi_camera.http.client.HTTPConnection") as factory:
            factory.return_value.getresponse.side_effect = [self.response(),self.response(),self.response(status=500)]
            client = PersistentFetcher()
            for _ in range(2):
                self.assertEqual(client("192.168.1.38","/api/status",TOKEN,20,2)[1],b"ok")
            self.assertEqual(factory.call_count,1)
            with self.assertRaises(ValueError): client("192.168.1.38","/api/status",TOKEN,20,2)
            self.assertIsNone(client.connection)


class MultipartTests(unittest.TestCase):
    def part(self,seq=1,extra=b"",body=JPEG):
        return (b"\r\n--robotframe\r\nContent-Type: image/jpeg\r\nContent-Length: "+str(len(body)).encode()+
                b"\r\nX-Device-Id: robot-dog\r\nX-Boot-Id: "+BOOT.encode()+b"\r\nX-Wifi-Generation: 2\r\nX-Frame-Seq: "+str(seq).encode()+
                b"\r\nX-Frame-Timestamp-Ms: 100\r\n"+extra+b"\r\n"+body)

    def response(self,body):
        # A real HTTPResponse parses arbitrarily fragmented chunk boundaries.
        chunks = b"".join(hex(len(body[n:n+7]))[2:].encode()+b"\r\n"+body[n:n+7]+b"\r\n" for n in range(0,len(body),7))+b"0\r\n\r\n"
        raw = (b"HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=robotframe\r\nTransfer-Encoding: chunked\r\n"
               b"X-Device-Id: robot-dog\r\nX-Boot-Id: "+BOOT.encode()+b"\r\nX-Wifi-Generation: 2\r\n\r\n"+chunks)
        class Sock:
            def makefile(self,*args): return io.BufferedReader(io.BytesIO(raw))
        response = http.client.HTTPResponse(Sock()); response.begin()
        return response

    def test_chunked_stream_two_frames_and_clean_end(self):
        with patch("wifi_camera.http.client.HTTPConnection") as factory:
            factory.return_value.getresponse.return_value = self.response(self.part()+self.part(2)+b"\r\n--robotframe--\r\n")
            stream = FrameStream("192.168.1.38",TOKEN)
            for sequence in (1,2):
                headers,body = stream.next_frame()
                self.assertEqual(body,JPEG); self.assertEqual(headers["x-frame-seq"],str(sequence))
                self.assertEqual(headers["x-wifi-generation"],"2")
            with self.assertRaises(EOFError): stream.next_frame()
            stream.close()
            self.assertEqual(factory.call_args.args,("192.168.1.38",82))

    def test_invalid_headers_identity_size_and_truncated_frame_rejected(self):
        bodies = [self.part(extra=b"Content-Length: 4\r\n"),self.part().replace(b"Generation: 2",b"Generation: 3"),
                  self.part().replace(str(len(JPEG)).encode()+b"\r\n",str(MAX_FRAME_BYTES+1).encode()+b"\r\n",1),self.part()[:-2]]
        for body in bodies:
            with self.subTest(body=body),patch("wifi_camera.http.client.HTTPConnection") as factory:
                factory.return_value.getresponse.return_value = self.response(body)
                stream = FrameStream("192.168.1.38",TOKEN)
                with self.assertRaises((ValueError,EOFError)): stream.next_frame()
                stream.close()

    def test_large_frames_through_512k_boundary_keep_multipart_alignment(self):
        # The previous 96 KiB cap rejected valid larger 1080p JPEGs. Exercise
        # both that boundary and the full permitted size through chunk decoding.
        for size in (96*1024+1, MAX_FRAME_BYTES):
            jpeg = b"\xff\xd8" + b"x"*(size-4) + b"\xff\xd9"
            with self.subTest(size=size),patch("wifi_camera.http.client.HTTPConnection") as factory:
                factory.return_value.getresponse.return_value = self.response(self.part(body=jpeg)+self.part(2))
                stream = FrameStream("192.168.1.38",TOKEN)
                try:
                    headers,body = stream.next_frame()
                    self.assertEqual(body,jpeg)
                    self.assertEqual(headers["x-frame-seq"],"1")
                    self.assertEqual(stream.next_frame()[1],JPEG)
                finally:
                    stream.close()

    def test_oversized_frame_rejected_before_reading_body(self):
        body = self.part().replace(str(len(JPEG)).encode()+b"\r\n",str(MAX_FRAME_BYTES+1).encode()+b"\r\n",1)
        with patch("wifi_camera.http.client.HTTPConnection") as factory:
            response = self.response(body)
            factory.return_value.getresponse.return_value = response
            stream = FrameStream("192.168.1.38",TOKEN)
            try:
                with patch.object(response,"read1",side_effect=AssertionError("Oversized body must not be read")):
                    with self.assertRaisesRegex(ValueError,"Invalid camera part length"):
                        stream.next_frame()
            finally:
                stream.close()

    def test_truncated_large_frame_never_delivered(self):
        jpeg = b"\xff\xd8" + b"x"*(100*1024-4) + b"\xff\xd9"
        with patch("wifi_camera.http.client.HTTPConnection") as factory:
            factory.return_value.getresponse.return_value = self.response(self.part(body=jpeg)[:-8])
            stream = FrameStream("192.168.1.38",TOKEN)
            try:
                with self.assertRaisesRegex(EOFError,"Incomplete camera JPEG"):
                    stream.next_frame()
            finally:
                stream.close()


if __name__ == "__main__":
    unittest.main()
