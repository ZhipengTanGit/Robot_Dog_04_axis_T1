import copy
import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from robot_server import ALLOWED_MA, BOARD_LIMIT_MA, MAX_FRAME, MAX_TELEMETRY, Rejected, RobotServer, TelemetryState


TOKEN = "test-only-not-a-deployment-secret-12345678"
PROFILE = {"cell_capacity_mah":700,"parallel_cells":2,"cell_max_charge_ma":1000,
           "nominal_voltage_v":3.7,"usable_percent":90,"efficiency_percent":85,"output_voltage_v":5}


def payload(**changes):
    result = {"device_id":"robot-dog", "boot_id":"boot-a", "seq":0, "uptime_ms":100,
              "uart_age_ms":100, "uart_valid":True, "raw":"BAT t=1 valid=1 link=READY",
              "peripheral_raw":"PERIPH t=1 aht_valid=1", "peripheral_age_ms":100,
              "logs":["BAT t=1 valid=1 link=READY"], "log_seq":1, "uart_lines":1,
              "camera":{"ready":True, "state":"READY"}}
    result.update(changes)
    return result


def ready_payload(**changes):
    result = payload(raw="BAT t=1 awake=1 valid=1 link=READY mode=VBUS_LIMIT set_mA=1850 verified=1")
    result.update(changes)
    return result


class StateTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.state = TelemetryState(monotonic=lambda:self.now, wall=lambda:1000+self.now)

    def test_no_data_is_offline(self):
        status = self.state.status()
        self.assertFalse(status["device_online"])
        self.assertFalse(status["uart_valid"])
        self.assertIsNone(status["device_age_ms"])
        self.assertEqual(status["raw"], "")

    def test_age_continues_without_device_and_recovery(self):
        self.state.ingest(payload())
        self.now = 3
        status = self.state.status()
        self.assertTrue(status["device_online"])
        self.assertEqual(status["uart_age_ms"], 3100)
        self.assertFalse(status["uart_valid"])
        self.assertFalse(status["peripheral_valid"])
        self.now = 5
        self.assertFalse(self.state.status()["device_online"])
        self.assertEqual(self.state.status()["raw"], payload()["raw"])
        self.state.ingest(payload(seq=1))
        self.assertTrue(self.state.status()["uart_valid"])
        self.assertEqual(self.state.status()["device_age_ms"], 0)

    def test_duplicate_old_sequence_and_retired_boot_never_refresh(self):
        self.state.ingest(payload(seq=3))
        self.now = 2
        for seq in (3, 2):
            with self.assertRaises(Rejected) as error:
                self.state.ingest(payload(seq=seq))
            self.assertEqual(error.exception.status, 409)
        self.assertEqual(self.state.status()["device_age_ms"], 2000)
        self.state.ingest(payload(boot_id="boot-b"))
        with self.assertRaises(Rejected):
            self.state.ingest(payload(seq=4))
        with self.assertRaises(Rejected):
            self.state.ingest(payload(device_id="other-device", boot_id="boot-c"))

    def test_schema_rejects_invalid_types_nonfinite_and_oversized_lines(self):
        invalid = [dict(seq=True), dict(seq=-1), dict(uptime_ms=1.5), dict(boot_id="../bad"),
                   dict(raw=[]), dict(raw="x"*2049), dict(logs=["x"]*21), dict(logs=[1]),
                   dict(uart_valid="true"), dict(camera={"ready":1}), dict(camera=[]),
                   dict(esp_peripherals={"bad":float("nan")}), dict(uart_age_ms=-1)]
        for changed in invalid:
            with self.subTest(changed=changed), self.assertRaises(Rejected):
                self.state.ingest(payload(**changed))
        nested = {}
        for _ in range(20):
            nested = {"nested":nested}
        with self.assertRaises(Rejected):
            self.state.ingest(payload(esp_peripherals=nested))

    def test_logs_snapshot_and_mutability(self):
        source = payload()
        self.state.ingest(source)
        source["logs"].append("not really received")
        self.assertEqual(len(self.state.logs()["logs"]), 1)
        result = self.state.status()
        result["camera"]["ready"] = False
        self.assertTrue(self.state.status()["camera"]["ready"])
        self.state.ingest(payload(seq=1))
        self.assertEqual(self.state.logs()["log_seq"], 1)
        self.assertEqual(len(self.state.logs()["logs"]), 1)

    def test_frame_session_camera_state_and_expiry(self):
        jpeg = b"\xff\xd8sample\xff\xd9"
        self.state.ingest(payload())
        self.state.ingest_frame("robot-dog", "boot-a", jpeg)
        self.assertEqual(self.state.capture()[0], jpeg)
        self.assertTrue(self.state.status()["frame_available"])
        with self.assertRaises(Rejected):
            self.state.ingest_frame("robot-dog", "wrong", jpeg)
        with self.assertRaises(Rejected):
            self.state.ingest_frame("robot-dog", "boot-a", b"not jpeg")
        self.now = 5
        with self.assertRaises(Rejected):
            self.state.capture()
        self.state.ingest(payload(seq=1, camera={"ready":False}))
        with self.assertRaises(Rejected):
            self.state.ingest_frame("robot-dog", "boot-a", jpeg)
        self.state.ingest(payload(boot_id="boot-b"))
        self.assertFalse(self.state.status()["frame_available"])

    def test_rotating_jsonl_records_only_accepted_payloads(self):
        with tempfile.TemporaryDirectory() as directory:
            state = TelemetryState(data_dir=directory)
            state.logger.handlers[0].maxBytes = 900
            state.logger.handlers[0].backupCount = 2
            for seq in range(15):
                state.ingest(payload(seq=seq))
            with self.assertRaises(Rejected):
                state.ingest(payload(seq=14))
            for handler in state.logger.handlers:
                handler.close()
            paths = list(Path(directory).glob("telemetry.jsonl*"))
            self.assertLessEqual(len(paths), 3)
            for path in paths:
                for line in path.read_text().splitlines():
                    record = json.loads(line)
                    self.assertIn("received_unix_ms", record)
                    self.assertNotIn(TOKEN, line)

    def test_recorder_failure_is_visible_and_recovers_without_dropping_telemetry(self):
        with tempfile.TemporaryDirectory() as directory:
            state = TelemetryState(data_dir=directory)
            handler = state.logger.handlers[0]
            with patch.object(handler, "shouldRollover", side_effect=OSError("disk unavailable")):
                state.ingest(payload())
            self.assertEqual(state.status()["recording_error"], "disk unavailable")
            self.assertTrue(state.status()["device_online"])
            state.ingest(payload(seq=1))
            self.assertIsNone(state.status()["recording_error"])
            handler.close()


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.server = RobotServer(("127.0.0.1", 0), TOKEN)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def request(self, path, data=None, token=TOKEN, content_type="application/json", extra=None):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=2)
        headers = {"Content-Type":content_type}
        if token:
            headers["Authorization"] = "Bearer " + token
        headers.update(extra or {})
        connection.request("POST" if data is not None else "GET", path, data, headers)
        response = connection.getresponse()
        result = response.status, response.read(), dict(response.headers)
        connection.close()
        return result

    def test_get_is_readonly_and_post_authentication(self):
        status, body, _ = self.request("/api/status", token=None)
        self.assertEqual(status, 200)
        self.assertFalse(json.loads(body)["device_online"])
        for token in (None, "wrong"):
            self.assertEqual(self.request("/api/telemetry", json.dumps(payload()), token=token)[0], 401)
        self.assertEqual(self.request("/api/telemetry", json.dumps(payload()))[0], 200)
        self.assertEqual(self.request("/api/telemetry", json.dumps(payload()))[0], 409)
        self.assertEqual(self.request("/api/control", "{}")[0], 404)
        self.assertEqual(self.request("/../robot_server.py")[0], 404)

    def test_scan_requires_csrf_origin_online_and_is_bounded(self):
        self.assertEqual(self.request("/api/i2c-scan","{}",token=None)[0],403)
        browser = self.browser_session()
        self.assertEqual(self.request("/api/i2c-scan","{}",token=None,extra=browser)[0],409)
        self.server.state.ingest(ready_payload())
        self.assertEqual(self.request("/api/i2c-scan","{}",token=None,extra={**browser,"Origin":"http://evil.example"})[0],403)
        self.assertEqual(self.request("/api/i2c-scan",'{"id":"chosen"}',token=None,extra=browser)[0],400)
        status, body, _ = self.request("/api/i2c-scan","{}",token=None,extra=browser)
        self.assertEqual(status,202)
        command = json.loads(body)["i2c_scan_control"]["pending"]
        self.assertRegex(command["id"],r"^[a-f0-9]{16}$")
        response = self.server.state.ingest(ready_payload(seq=1))
        self.assertEqual(response["i2c_scan"]["id"],command["id"])
        self.assertEqual(self.request("/api/i2c-scan","{}",token=None,extra=browser)[0],409)

    def test_serial_receiver_rejects_network_device_overwrite(self):
        self.server.state = TelemetryState(transport="serial",serial_port="COM18")
        for path, body in (("/api/telemetry",json.dumps(payload())),("/api/frame",b"jpeg"),("/api/audio",b"pcm")):
            self.assertEqual(self.request(path,body)[0],409)
        status = json.loads(self.request("/api/status",token=None)[1])
        self.assertEqual(status["server_id"],"robot-monitor")
        self.assertEqual(status["transport"],"serial")
        self.assertFalse(status["device_online"])

    def test_camera_check_requires_origin_csrf_and_empty_json(self):
        self.server.state.ingest(ready_payload())
        browser = self.browser_session()
        self.assertEqual(self.request("/api/camera-check","{}",token=None)[0],403)
        self.assertEqual(self.request("/api/camera-check","{}",token=None,extra={**browser,"Origin":"http://evil.example"})[0],403)
        self.assertEqual(self.request("/api/camera-check",'{"id":"chosen"}',token=None,extra=browser)[0],400)
        self.assertEqual(self.request("/api/camera-check","{}",token=None,extra=browser,content_type="text/plain")[0],415)
        status, body, _ = self.request("/api/camera-check","{}",token=None,extra=browser)
        self.assertEqual(status,202)
        command = json.loads(body)["camera_check_control"]["pending"]
        self.assertRegex(command["id"],r"^[a-f0-9]{16}$")
        self.assertEqual(self.request("/api/camera-check","{}",token=None,extra=browser)[0],409)

    def test_bad_payload_and_length_are_rejected(self):
        for body in ("{", "[]", '{"seq":0,"seq":1}', json.dumps(payload(esp_peripherals={"n":float("nan")}))):
            self.assertEqual(self.request("/api/telemetry", body)[0], 400)
        self.assertEqual(self.request("/api/telemetry", "{}", content_type="text/plain")[0], 415)
        # Rejection happens from the declared length before reading/allocating
        # the body. Avoid a platform-specific TCP reset from unread huge bodies.
        self.assertEqual(self.request("/api/telemetry", b"", extra={"Content-Length":str(MAX_TELEMETRY+1)})[0], 413)
        self.assertEqual(self.request("/api/frame", b"", content_type="image/jpeg", extra={"Content-Length":str(MAX_FRAME+1)})[0], 413)

    def test_frame_upload_roundtrip_and_headers(self):
        self.request("/api/telemetry", json.dumps(payload()))
        frame = b"\xff\xd8sample\xff\xd9"
        status, _, _ = self.request("/api/frame", frame, content_type="image/jpeg", extra={"X-Device-Id":"robot-dog", "X-Boot-Id":"boot-a"})
        self.assertEqual(status, 200)
        status, body, headers = self.request("/capture.jpg?t=1")
        self.assertEqual(status, 200)
        self.assertEqual(body, frame)
        self.assertEqual(headers["Content-Type"], "image/jpeg")
        self.assertIn("X-Frame-Age-Ms", headers)
        self.assertEqual(headers["Cache-Control"], "no-store")

    def browser_session(self):
        origin = "http://%s:%d" % self.server.server_address
        status, body, headers = self.request("/api/session", token=None, extra={"Origin":origin})
        self.assertEqual(status, 200)
        self.assertIn("SameSite=Strict", headers["Set-Cookie"])
        self.assertIn("HttpOnly", headers["Set-Cookie"])
        self.assertNotIn(TOKEN, body.decode())
        return {"Origin":origin,"Cookie":headers["Set-Cookie"].split(";",1)[0],"X-CSRF-Token":json.loads(body)["csrf_token"]}

    def test_charge_requires_same_origin_csrf_cookie_json_and_profile(self):
        self.server.state.ingest(ready_payload())
        headers = self.browser_session()
        body = json.dumps({"ma":650,"profile":PROFILE})
        for missing in ("Origin","Cookie","X-CSRF-Token"):
            bad = {key:value for key,value in headers.items() if key != missing}
            self.assertEqual(self.request("/api/charge-current",body,token=None,extra=bad)[0],403)
        for changed in ({"Origin":"http://evil.example"},{"Origin":"null"},{"X-CSRF-Token":"bad"}, {"Sec-Fetch-Site":"cross-site"}, {"Host":"evil.example:%d" % self.server.server_address[1]}):
            self.assertEqual(self.request("/api/charge-current",body,token=None,extra={**headers,**changed})[0],403)
        self.assertEqual(self.request("/api/session",token=None,extra={"Origin":"http://evil.example"})[0],403)
        self.assertEqual(self.request("/api/charge-current",body,token=None,extra=headers,content_type="text/plain")[0],415)
        self.assertEqual(self.request("/api/charge-current",json.dumps({"ma":650}),token=None,extra=headers)[0],400)
        self.assertEqual(self.request("/api/charge-current",body,token=None,extra=headers)[0],202)
        self.assertEqual(self.request("/api/charge-current",body,token=None,extra=headers)[0],409)

    def test_audio_session_authentication_and_pcm_roundtrip(self):
        self.server.state.ingest(ready_payload(esp_peripherals={"audio":{"configured":True}}))
        browser = self.browser_session()
        self.assertEqual(self.request("/api/audio-monitor",'{"enabled":true}',token=None)[0],403)
        status, body, _ = self.request("/api/audio-monitor",'{"enabled":true}',token=None,extra=browser)
        self.assertEqual(status,200)
        session = json.loads(body)["audio_monitor"]["session_id"]
        headers = {"X-Device-Id":"robot-dog","X-Boot-Id":"boot-a","X-Audio-Session":session,
                   "X-Audio-Seq":"0","X-Sample-Rate":"16000","X-Channels":"1","X-Sample-Format":"s16le"}
        pcm = b"\x00\x00\xff\x7f\x00\x80"
        self.assertEqual(self.request("/api/audio",pcm,token=None,content_type="application/octet-stream",extra=headers)[0],401)
        self.assertEqual(self.request("/api/audio",pcm,content_type="application/octet-stream",extra=headers)[0],200)
        path = "/api/audio?session="+session+"&after=-1"
        self.assertEqual(self.request(path,token=None)[0],403)
        another = self.browser_session()
        self.assertEqual(self.request(path,token=None,extra=another)[0],403)
        status, data, response = self.request(path,token=None,extra=browser)
        self.assertEqual((status,data),(200,pcm))
        self.assertEqual(response["X-Audio-Seq"],"0")
        self.assertEqual(response["X-Sample-Rate"],"16000")
        self.assertEqual(self.request(path.replace("after=-1","after=0"),token=None,extra=browser)[0],204)
        self.assertEqual(self.request("/api/audio-monitor",'{"enabled":false}',token=None,extra=browser)[0],200)
        self.assertEqual(self.request(path,token=None,extra=browser)[0],410)


class CameraCheckTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.state = TelemetryState(monotonic=lambda:self.now,wall=lambda:1000+self.now)
        self.state.ingest(ready_payload())

    def result(self, check_id, **changes):
        return {"id":check_id,"running":False,"completed":True,"state":"PASSED","stage":"COMPLETE",
                "usable":True,"expander_ok":True,"sccb_ack":True,"id_ok":True,"init_ok":True,"frame_ok":True,
                "sensor_pid":0x5640,"width":320,"height":240,"frame_bytes":12000,"error":0,**changes}

    def test_matching_real_frame_result_only_confirms_usable_camera(self):
        command = self.state.request_camera_check()["camera_check_control"]["pending"]
        self.assertEqual(command["expires_in_ms"],30000)
        response = self.state.ingest(ready_payload(seq=1))
        self.assertEqual(response["camera_check"]["id"],command["id"])
        with self.assertRaises(Rejected): self.state.request_camera_check()
        self.state.ingest(ready_payload(seq=2,camera_check=self.result("f"*16)))
        self.assertIsNotNone(self.state.status()["camera_check_control"]["pending"])
        self.state.ingest(ready_payload(seq=3,camera_check=self.result(command["id"],running=True,completed=False)))
        self.assertIsNotNone(self.state.status()["camera_check_control"]["pending"])
        self.state.ingest(ready_payload(seq=4,camera_check=self.result(command["id"])))
        self.assertEqual(self.state.status()["camera_check_control"]["last"]["status"],"PASSED")

    def test_ack_or_initialized_sensor_without_frame_is_not_usable(self):
        for changes in ({"frame_ok":False,"usable":False,"frame_bytes":0,"frame_error":263},
                        {"id_ok":False,"init_ok":False,"frame_ok":False,"usable":False},
                        {"frame_bytes":0},{"usable":False}):
            state = TelemetryState()
            state.ingest(ready_payload())
            command = state.request_camera_check()["camera_check_control"]["pending"]
            state.ingest(ready_payload(seq=1,camera_check=self.result(command["id"],**changes)))
            self.assertEqual(state.status()["camera_check_control"]["last"]["status"],"FAILED")

    def test_timeout_late_result_and_boot_restart_do_not_replay(self):
        command = self.state.request_camera_check()["camera_check_control"]["pending"]
        self.now = 30
        response = self.state.ingest(ready_payload(seq=1,camera_check=self.result(command["id"])))
        self.assertIsNone(response["camera_check"])
        self.assertEqual(self.state.status()["camera_check_control"]["last"]["status"],"TIMED_OUT")
        self.state.request_camera_check()
        response = self.state.ingest(ready_payload(boot_id="boot-b"))
        self.assertIsNone(response["camera_check"])
        self.assertEqual(self.state.status()["camera_check_control"]["last"]["status"],"CANCELLED_BOOT")
        fresh = TelemetryState()
        self.assertIsNone(fresh.ingest(ready_payload())["camera_check"])

    def test_invalid_camera_results_are_rejected_without_refreshing_state(self):
        for check in ({"id":"bad"},{"running":1},{"completed":"true"},{"usable":1},{"frame_bytes":-1},{"width":float("nan")}):
            with self.subTest(check=check), self.assertRaises(Rejected):
                self.state.ingest(ready_payload(seq=1,camera_check=check))


class ChargeTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.state = TelemetryState(monotonic=lambda:self.now, wall=lambda:1000+self.now)
        self.state.ingest(ready_payload())

    def test_all_32_codes_are_listed_but_board_and_profile_limits_are_enforced(self):
        expected = [50+sum(weight for bit,weight in enumerate((100,200,500,1200,2300)) if code & 1<<bit) for code in range(32)]
        self.assertEqual(list(ALLOWED_MA),expected)
        control = self.state.status()["charge_control"]
        self.assertEqual(control["allowed_ma"],expected)
        self.assertEqual(control["board_limit_ma"],3000)
        for ma in expected:
            state = TelemetryState()
            state.ingest(ready_payload())
            if ma <= 2000:
                self.assertTrue(state.request_current(ma,PROFILE)["ok"])
            else:
                with self.assertRaises(Rejected): state.request_current(ma,PROFILE)
        high_profile = {**PROFILE,"cell_max_charge_ma":10000}
        self.assertTrue(self.state.request_current(2950,high_profile)["ok"])
        for ma in expected:
            if ma > BOARD_LIMIT_MA:
                with self.assertRaises(Rejected): self.state.request_current(ma,high_profile)
        for ma in (0,1000,1500,3000,650.0,True):
            with self.assertRaises(Rejected): self.state.request_current(ma,PROFILE)

    def test_invalid_profile_and_unready_device_are_rejected(self):
        bad = [{}, {**PROFILE,"parallel_cells":1.5}, {**PROFILE,"parallel_cells":True},
               {**PROFILE,"cell_capacity_mah":0}, {**PROFILE,"output_voltage_v":0},
               {**PROFILE,"nominal_voltage_v":float("nan")}, {**PROFILE,"efficiency_percent":101},
               {**PROFILE,"cell_max_charge_ma":float("inf")}, {**PROFILE,"extra":"bad"}]
        for profile in bad:
            with self.subTest(profile=profile), self.assertRaises(Rejected): self.state.request_current(650,profile)
        self.now = 6
        with self.assertRaises(Rejected): self.state.request_current(650,PROFILE)
        for raw in ("BAT t=1 awake=0 valid=1 link=READY mode=VBUS_LIMIT", "BAT t=1 awake=1 valid=0 link=BUS_ERROR mode=VBUS_LIMIT", "BAT t=1 awake=1 valid=1 link=READY mode=MONITOR"):
            state = TelemetryState()
            state.ingest(ready_payload(raw=raw))
            with self.assertRaises(Rejected): state.request_current(650,PROFILE)

    def test_one_pending_id_repeats_with_decreasing_ttl_and_never_after_expiry(self):
        created = self.state.request_current(650,PROFILE)["charge_control"]["pending"]
        self.assertRegex(created["id"],r"^[a-f0-9]{16}$")
        self.assertEqual(created["status"],"QUEUED")
        first = self.state.ingest(ready_payload(seq=1))["command"]
        with self.assertRaises(Rejected): self.state.request_current(850,PROFILE)
        self.now = 2.5
        again = self.state.ingest(ready_payload(seq=2))["command"]
        self.assertEqual(first["id"],again["id"])
        self.assertEqual(again["expires_in_ms"],12500)
        self.now = 15
        self.assertIsNone(self.state.ingest(ready_payload(seq=3))["command"])
        self.assertEqual(self.state.status()["charge_control"]["last"]["status"],"TIMED_OUT")

    def test_ack_requires_matching_id_ma_and_new_verified_readback(self):
        command = self.state.request_current(650,PROFILE)["charge_control"]["pending"]
        changed_raw = "BAT t=2 awake=1 valid=1 link=READY mode=VBUS_LIMIT set_mA=650 verified=1"
        wrong = {"id":"0"*16,"ma":650,"status":"APPLIED"}
        self.state.ingest(ready_payload(seq=1,raw=changed_raw,command_ack=wrong))
        self.assertIsNotNone(self.state.status()["charge_control"]["pending"])
        ack = {"id":command["id"],"ma":850,"status":"APPLIED"}
        self.state.ingest(ready_payload(seq=2,raw=changed_raw,command_ack=ack))
        self.assertIsNotNone(self.state.status()["charge_control"]["pending"])
        ack["ma"] = 650
        self.assertIsNone(self.state.ingest(ready_payload(seq=3,command_ack=ack))["command"])
        self.assertEqual(self.state.status()["charge_control"]["pending"]["status"],"AWAITING_READBACK")
        self.state.ingest(ready_payload(seq=4,raw=changed_raw.replace("verified=1","verified=0")))
        self.assertIsNotNone(self.state.status()["charge_control"]["pending"])
        self.state.ingest(ready_payload(seq=5,raw=changed_raw))
        result = self.state.status()["charge_control"]
        self.assertIsNone(result["pending"])
        self.assertEqual(result["last"]["status"],"CONFIRMED")

    def test_failure_restart_and_late_ack_do_not_replay_or_confirm(self):
        command = self.state.request_current(650,PROFILE)["charge_control"]["pending"]
        self.state.ingest(ready_payload(seq=1,command_ack={"id":command["id"],"ma":650,"status":"FAILED","detail":"READBACK"}))
        self.assertEqual(self.state.status()["charge_control"]["last"]["status"],"FAILED")
        self.state.request_current(850,PROFILE)
        self.assertIsNone(self.state.ingest(ready_payload(boot_id="boot-b"))["command"])
        self.assertEqual(self.state.status()["charge_control"]["last"]["status"],"CANCELLED_BOOT")
        command = self.state.request_current(650,PROFILE)["charge_control"]["pending"]
        self.now = 16
        self.state.ingest(ready_payload(seq=1,boot_id="boot-b",raw="BAT t=2 awake=1 valid=1 link=READY mode=VBUS_LIMIT set_mA=650 verified=1",command_ack={"id":command["id"],"ma":650,"status":"APPLIED"}))
        self.assertEqual(self.state.status()["charge_control"]["last"]["status"],"TIMED_OUT")
        fresh_server = TelemetryState()
        self.assertIsNone(fresh_server.ingest(ready_payload())["command"])


class AudioTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.state = TelemetryState(monotonic=lambda:self.now,wall=lambda:1000+self.now)
        self.state.ingest(ready_payload(esp_peripherals={"audio":{"configured":True}}))
        self.session = self.state.request_audio("browser-a",True)["audio_monitor"]["session_id"]

    def upload(self, sequence, **changes):
        args = dict(device="robot-dog",boot="boot-a",session=self.session,sequence=sequence,pcm=b"\x12\x34"*3200,sample_rate="16000",channels="1",sample_format="s16le")
        args.update(changes)
        return self.state.ingest_audio(**args)

    def test_bounded_queue_discards_old_audio_and_never_repeats_played_cursor(self):
        for seq in range(10):
            self.now = seq/10
            self.upload(seq)
        self.assertEqual(len(self.state.audio_packets),5)
        self.assertLessEqual(sum(len(item[2]) for item in self.state.audio_packets),32000)
        packet = self.state.read_audio("browser-a",self.session,-1)
        self.assertEqual(packet[0],8)
        self.assertEqual(self.state.read_audio("browser-a",self.session,8)[0],9)
        self.assertIsNone(self.state.read_audio("browser-a",self.session,9))
        self.now = 2
        self.assertIsNone(self.state.read_audio("browser-a",self.session,9))
        self.assertEqual(len(self.state.audio_packets),0)

    def test_session_identity_format_duplicate_and_codec_readiness(self):
        with self.assertRaises(Rejected): self.state.request_audio("browser-b",True)
        with self.assertRaises(Rejected): self.state.read_audio("browser-b",self.session,-1)
        for changed in ({"boot":"other"},{"device":"other"},{"session":"0"*16},{"pcm":b"\x00"},{"pcm":b"\x00"*6402},{"channels":"2"},{"sample_rate":"44100"},{"sample_format":"float"}):
            with self.subTest(changed=changed), self.assertRaises(Rejected): self.upload(0,**changed)
        self.upload(0)
        with self.assertRaises(Rejected): self.upload(0)
        self.state.ingest(ready_payload(seq=1,esp_peripherals={"audio":{"configured":False,"state":"INIT_ERROR"}}))
        with self.assertRaises(Rejected): self.upload(1)

    def test_stop_expiry_and_boot_change_clear_audio_without_replay(self):
        self.upload(0)
        monitor = self.state.ingest(ready_payload(seq=1,esp_peripherals={"audio":{"configured":True}}))["audio_monitor"]
        self.assertEqual(monitor["session_id"],self.session)
        self.assertEqual(monitor["expires_in_ms"],15000)
        self.state.request_audio("browser-a",False)
        self.assertEqual(len(self.state.audio_packets),0)
        with self.assertRaises(Rejected): self.upload(1)
        fresh = self.state.request_audio("browser-a",True)["audio_monitor"]["session_id"]
        self.assertNotEqual(fresh,self.session)
        with self.assertRaises(Rejected): self.upload(1)
        self.session = fresh
        self.upload(0)
        self.now = 15
        self.assertFalse(self.state.status()["audio_monitor"]["enabled"])
        self.assertEqual(len(self.state.audio_packets),0)
        with self.assertRaises(Rejected): self.upload(1)
        self.state.ingest(ready_payload(seq=2,esp_peripherals={"audio":{"configured":True}}))
        self.state.request_audio("browser-a",True)
        result = self.state.ingest(ready_payload(boot_id="boot-b",esp_peripherals={"audio":{"configured":True}}))
        self.assertFalse(result["audio_monitor"]["enabled"])

    def test_pcm_never_enters_recorded_jsonl(self):
        with tempfile.TemporaryDirectory() as directory:
            state = TelemetryState(data_dir=directory)
            state.ingest(ready_payload(esp_peripherals={"audio":{"configured":True}}))
            session = state.request_audio("owner",True)["audio_monitor"]["session_id"]
            for seq in range(4):
                state.ingest_audio("robot-dog","boot-a",session,seq,b"\x12\x34"*100,"16000","1","s16le")
            state.logger.handlers[0].close()
            paths = list(Path(directory).iterdir())
            self.assertEqual(len(paths),1)
            self.assertEqual(len(paths[0].read_text().splitlines()),1)


if __name__ == "__main__":
    unittest.main()
