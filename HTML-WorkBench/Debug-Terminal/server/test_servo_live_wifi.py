import base64
import json
import tempfile
import unittest

from robot_server import TelemetryState, Rejected, SERVO_BROWSER_LEASE_MS
from serial_bridge import SerialReceiver
from test_server import ready_payload, PROFILE
from test_serial_control import peripheral
from test_wifi import BOOT, wifi
from wifi_control_bridge import WifiControlBridge


def motion(tick=1000, **changes):
    fields = dict(servo_proto=2,servo_mode="IDLE",last_id=0,test_id=0,test_state="IDLE",
                  test_result="NONE",test_left_ms=0,test_delay_ms=0,test_mask=0,test_lo=1300,test_hi=1700)
    fields.update(changes)
    return peripheral(tick=tick)+" "+" ".join(f"{k}={v}" for k,v in fields.items())


class LiveWifiTests(unittest.TestCase):
    def setUp(self):
        self.now,self.seq = 0.0,0
        self.state = TelemetryState(monotonic=lambda:self.now,transport="serial",serial_port="COM9")
        self.receiver = SerialReceiver(self.state,"COM9")
        self.usb()

    def usb(self,raw=None):
        self.seq += 1
        data = ready_payload(boot_id=BOOT,seq=self.seq,uptime_ms=1000+int(self.now*1000),wifi=wifi(),
                             peripheral_raw=raw or motion(),peripheral_age_ms=0,esp_peripherals={"audio":{"configured":True}})
        self.receiver.feed(("ESP_STATUS "+json.dumps(data)+"\n").encode())

    def network(self,raw=None,**changes):
        self.seq += 1
        target = self.state.wifi_camera_target()
        data = ready_payload(device_id=target["device"],boot_id=target["boot"],seq=self.seq,
                             uptime_ms=1000+int(self.now*1000),wifi=wifi(),peripheral_raw=raw or motion(),peripheral_age_ms=0,
                             esp_peripherals={"audio":{"configured":True}})
        data["control_proto"] = 1
        data.update(changes)
        headers = {"x-device-id":data["device_id"],"x-boot-id":data["boot_id"]}
        self.state.ingest_wifi_status(target,headers,data)

    def live(self,channel=0,pulse=1500,owner="browser",epoch=None):
        return self.state.request_live(owner,{"action":"set","channel":channel,"pulse_us":pulse,
                      "expected_epoch":epoch or self.state.status()["servo_control"]["epoch"]})

    def test_battery_bus_fault_reports_cause_and_still_rejects_motion(self):
        raw = motion().replace("servo_allowed=1", "servo_allowed=0")
        self.network(raw)
        self.state.wifi_payload["raw"] = "BAT t=1000 link=BUS_ERROR valid=0 awake=1 io_last=NACK:ADDR_W:00:R:7:1000"
        state = self.state.status()["servo_control"]
        self.assertFalse(state["can_live"])
        self.assertFalse(state["can_test"])
        self.assertIn("0x75 无应答",state["reason"])
        with self.assertRaises(Rejected): self.live()
        self.network()
        self.assertTrue(self.state.status()["servo_control"]["can_live"])

    def test_ttl_speed_is_capability_gated_and_transmitted_atomically(self):
        payload = {"action":"set","channel":2,"pulse_us":1700,"speed":50,
                   "expected_epoch":self.state.servo_epoch}
        with self.assertRaises(Rejected): self.state.request_live("browser",payload)
        self.now += 0.2
        self.usb(motion(tick=1200)+" servo_bus=TTL_CONTROL ttl_speed_proto=1")
        payload["expected_epoch"] = self.state.servo_epoch
        self.state.request_live("browser",payload)
        command = self.state.outgoing_motion("serial")
        self.assertEqual(command["args"],(2,1700,50))
        self.assertTrue(command["line"].endswith(" 2 1700 50"))

    def test_ttl_speed_rejects_zero_out_of_range_and_nonintegers(self):
        self.now += 0.2
        self.usb(motion(tick=1200)+" servo_bus=TTL_CONTROL ttl_speed_proto=1")
        self.assertTrue(self.state.status()["servo_control"]["capability"]["speed"])
        for speed in (0,1024,-1,True,1.5,"50"):
            with self.subTest(speed=speed), self.assertRaises(Rejected):
                self.state.request_live("browser",{"action":"set","channel":0,"pulse_us":1500,
                    "speed":speed,"expected_epoch":self.state.servo_epoch})
        self.assertFalse(self.state.live_targets)
        self.assertIsNone(self.state.live_servo)

    def test_wifi_speed_requires_esp_support_as_well_as_stm_support(self):
        raw = motion()+" servo_bus=TTL_CONTROL ttl_speed_proto=1"
        self.network(raw)
        payload = {"action":"set","channel":1,"pulse_us":1600,"speed":300,
                   "expected_epoch":self.state.servo_epoch}
        self.assertFalse(self.state.status()["servo_control"]["capability"]["speed"])
        with self.assertRaises(Rejected): self.state.request_live("browser",payload)
        self.network(raw,servo_speed_proto=1)
        payload["expected_epoch"] = self.state.servo_epoch
        self.state.request_live("browser",payload)
        command = self.state.outgoing_motion("wifi")
        self.assertEqual(command["args"],(1,1600,300))

    def test_advertised_charge_limit_rejects_high_requests_on_usb_and_wifi(self):
        for use_wifi in (False, True):
            with self.subTest(wifi=use_wifi):
                if use_wifi: self.network()
                active = self.state.wifi_payload if use_wifi else self.state._payload
                active["raw"] += " limit_mA=350"
                self.assertEqual(self.state.status()["charge_control"]["board_limit_ma"],350)
                with self.assertRaisesRegex(Rejected,"350 mA"):
                    self.state.request_current(1850,PROFILE)
                self.assertIsNone(self.state.pending_command)
                self.state.request_current(350,PROFILE)
                self.assertEqual(self.state.pending_command["ma"],350)
                self.state.pending_command = None

    def test_ttl_readback_blocks_pwm_and_decodes_real_position(self):
        raw = motion(servo_proto=0).replace("servo_allowed=1", "servo_allowed=0") + " servo_bus=TTL_READONLY"
        raw += " ttl0=1,1000000,512,0,20,0,0 ttl1=1,1000000,NA,0,4294967295,2,-1"
        raw += " ttl2=1,115200,200,36,30,0,0 ttl3=1,1000000,5000,0,20,0,0"
        self.network(raw)
        control = self.state.status()["servo_control"]
        self.assertFalse(control["can_live"])
        self.assertFalse(control["can_stop"])
        self.assertIn("TTL", control["reason"])
        channels = control["feedback"]["channels"]
        self.assertEqual(channels[0]["angle_deg"],90.1)
        self.assertTrue(channels[0]["valid"])
        self.assertEqual(channels[1]["state"],"NO_RESPONSE")
        self.assertIsNone(channels[1]["angle_deg"])
        self.assertEqual(channels[2]["state"],"FAULT")
        self.assertEqual(channels[3]["state"],"INVALID")
        with self.assertRaisesRegex(Rejected,"TTL"): self.live()
        self.now += 2
        channels = self.state.status()["servo_control"]["feedback"]["channels"]
        self.assertFalse(channels[0]["valid"])
        self.assertIsNone(channels[0]["angle_deg"])

    def test_ttl_pin_conflict_is_explicit_and_never_a_valid_angle(self):
        raw=motion(servo_proto=0).replace("servo_allowed=1","servo_allowed=0")+" servo_bus=TTL_READONLY"
        raw+="".join(f" ttl{i}=1,1000000,NA,0,4294967295,1,-9 tx{i}=0 rx{i}=0" for i in range(4))
        self.network(raw)
        control=self.state.status()["servo_control"]
        self.assertFalse(control["can_live"])
        for item in control["feedback"]["channels"]:
            self.assertEqual(item["state"],"PIN_CONFLICT")
            self.assertFalse(item["valid"])
            self.assertIsNone(item["angle_deg"])

    def test_ttl_control_advertises_live_control_without_legacy_test(self):
        raw=motion()+" servo_bus=TTL_CONTROL"+"".join(f" ttl{i}=1,115200,512,0,20,0,0 tq{i}=0" for i in range(4))
        self.network(raw)
        control=self.state.status()["servo_control"]
        self.assertTrue(control["can_live"])
        self.assertTrue(control["can_stop"])
        self.assertFalse(control["can_test"])
        self.assertEqual(control["feedback"]["channels"][0]["torque_state"],0)
        self.live()

    def test_ttl_180_degree_endpoints_and_midpoint(self):
        raw=motion()+" servo_bus=TTL_CONTROL ttl_span=180 ttl0=1,115200,0,0,20,0,0 ttl1=1,115200,512,0,20,0,0 ttl2=1,115200,1023,0,20,0,0"
        self.network(raw)
        feedback=self.state.status()["servo_control"]["feedback"]
        self.assertEqual(feedback["angle_max_deg"],180)
        self.assertEqual([x["angle_deg"] for x in feedback["channels"][:3]],[0.0,90.1,180.0])

    def test_latest_target_per_channel_and_rate_limit(self):
        self.live(0,1400); self.live(0,1600); self.live(1,1700)
        command = self.state.outgoing_motion("serial")
        self.assertTrue(command["line"].endswith("0 1600"))
        self.assertTrue(self.state.send_motion(command["id"],"serial"))
        self.assertIsNone(self.state.outgoing_motion("serial"))
        self.now = .11
        command = self.state.outgoing_motion("serial")
        self.assertTrue(command["line"].endswith("1 1700"))
        self.assertTrue(self.state.send_motion(command["id"],"serial"))
        self.now = .25
        self.assertIsNone(self.state.outgoing_motion("serial"))
        self.now = .52
        self.assertEqual(self.state.outgoing_motion("serial")["kind"],"KEEP")

    def test_release_owner_epoch_and_test_barrier(self):
        epoch = self.state.servo_epoch
        self.live()
        with self.assertRaises(Rejected): self.live(owner="other")
        self.state.request_live("browser",{"action":"release","expected_epoch":epoch})
        with self.assertRaises(Rejected): self.live(epoch=epoch)
        stop = self.state.outgoing_motion("serial")
        self.assertEqual(stop["kind"],"STOP")
        self.state.send_motion(stop["id"],"serial")
        self.state.accept_motion_ack({"id":stop["id"],"cmd":"STOP","ok":"1"})
        epoch = self.state.servo_epoch
        result = self.state.request_servo_test({"mask":3,"min_us":1300,"max_us":1700,"expected_epoch":epoch})
        self.assertEqual(result["servo_control"]["motion_pending"]["kind"],"TEST")
        with self.assertRaises(Rejected): self.live(epoch=epoch)
        with self.assertRaises(Rejected): self.live()
        command = self.state.outgoing_motion("serial")
        self.assertTrue(command["line"].endswith("3 1300 1700 0"))

    def test_lease_expiry_stop_and_no_unsent_replay(self):
        self.live()
        old = self.state.outgoing_motion("serial")
        epoch = self.state.servo_epoch
        self.now = SERVO_BROWSER_LEASE_MS/1000+.01
        self.usb(motion(tick=1000+int(self.now*1000)))
        self.assertFalse(self.state.send_motion(old["id"],"serial"))
        self.assertNotEqual(epoch,self.state.servo_epoch)
        self.assertEqual(self.state.outgoing_motion("serial")["kind"],"STOP")
        with self.assertRaises(Rejected):
            self.state.request_live("browser",{"action":"renew","expected_epoch":epoch})

    def test_manual_hold_survives_webgl_pause_but_not_abandoned_browser(self):
        self.live()
        first=self.state.outgoing_motion("serial")
        self.assertTrue(self.state.send_motion(first["id"],"serial"))
        epoch=self.state.servo_epoch
        # The bridge still runs while the browser spends three seconds loading
        # the assembly. KEEP renews the unchanged 1.5 s firmware watchdog.
        for i in range(1,13):
            self.now=i*.25
            self.usb(motion(tick=1000+int(self.now*1000),servo_mode="LIVE"))
            command=self.state.outgoing_motion("serial")
            if command:
                self.assertEqual(command["kind"],"KEEP")
                self.assertTrue(self.state.send_motion(command["id"],"serial"))
            self.assertTrue(self.state.status()["servo_control"]["live"]["active"])
        self.state.request_live("browser",{"action":"renew","expected_epoch":epoch})
        self.now=3+SERVO_BROWSER_LEASE_MS/1000+.01
        self.usb(motion(tick=1000+int(self.now*1000),servo_mode="LIVE"))
        self.assertFalse(self.state.status()["servo_control"]["live"]["active"])
        self.assertEqual(self.state.outgoing_motion("serial")["kind"],"STOP")
        with self.assertRaises(Rejected):
            self.state.request_live("browser",{"action":"renew","expected_epoch":epoch})

    def test_wifi_full_state_usb_unplug_and_motion_post(self):
        self.network(logs=["new wifi log"])
        self.live(2,1600)
        self.receiver.disconnected("unplugged")
        sent = []
        def request(ip,token,method,path,payload=None):
            sent.append((method,path,payload))
            target = self.state.wifi_camera_target()
            return {"x-device-id":target["device"],"x-boot-id":target["boot"]},{"id":payload["id"],"accepted":True}
        bridge = WifiControlBridge(self.state,"token",request)
        bridge.poll_once()
        self.assertEqual(sent[0][1],"/api/control")
        self.assertTrue(sent[0][2]["line"].endswith("2 1600"))
        self.assertEqual(sent[0][2]["deadline_ms"],2200)
        self.assertTrue(self.state.status()["device_online"])
        self.assertEqual(self.state.logs()["logs"],["new wifi log"])
        self.assertEqual(self.receiver.commands(),[])
        self.assertEqual(self.state.status()["connection"]["active"],"wifi")

    def test_sent_command_not_replayed_between_routes(self):
        self.state.request_current(650,PROFILE)
        wire = next(line for line in self.receiver.commands() if line.startswith(b"ESP CHARGE_CURRENT"))
        self.assertTrue(self.receiver.should_send(wire))
        self.network()
        sent = []
        bridge = WifiControlBridge(self.state,"token",lambda *args:sent.append(args))
        bridge.poll_once()
        self.assertEqual(sent,[])

    def test_wifi_test_result_is_board_authoritative(self):
        self.network()
        epoch = self.state.servo_epoch
        self.state.request_servo_test({"mask":1,"min_us":1300,"max_us":1700,"expected_epoch":epoch})
        cmd = self.state.outgoing_motion("wifi")
        self.state.send_motion(cmd["id"],"wifi")
        self.network(motion(1100,last_id=cmd["id"],test_id=cmd["id"],test_state="RUN",test_result="RUNNING",test_left_ms=9000),
                     periph_ack=f"PERIPH_ACK cmd=TEST id={cmd['id']} ok=1 reason=OK",periph_ack_age_ms=0)
        self.receiver.disconnected("unplugged")
        self.assertFalse(self.state.status()["servo_control"]["can_live"])
        self.now = 10
        self.assertFalse(self.state.status()["servo_control"]["test"]["authoritative"])
        self.network(motion(11100,last_id=cmd["id"],test_id=cmd["id"],test_state="DONE",test_result="OK"))
        self.assertEqual(self.state.status()["servo_control"]["test"]["state"],"DONE")

    def test_stm_reset_cancels_old_motion_and_restarts_ids(self):
        self.network(motion(20000,last_id=40))
        self.live()
        command = self.state.outgoing_motion("wifi")
        self.assertEqual(command["id"],41)
        old_epoch = self.state.servo_epoch
        self.now = .5
        self.network(motion(100))
        self.assertNotEqual(old_epoch,self.state.servo_epoch)
        self.assertFalse(self.state.send_motion(command["id"],"wifi"))
        self.live()
        self.assertEqual(self.state.outgoing_motion("wifi")["id"],1)

    def test_pairing_survives_restart_and_new_esp_boot_cancels_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            state = TelemetryState(directory,monotonic=lambda:self.now,transport="serial",serial_port="COM9")
            with state.lock:
                state._reset_wifi_target({"ip":"192.168.1.38","device":"board","boot":BOOT})
            for handler in state.logger.handlers: handler.close()
            restored = TelemetryState(directory,monotonic=lambda:self.now,transport="serial",serial_port="COM9")
            target = restored.wifi_camera_target()
            self.assertEqual(target["device"],"board")
            new_boot = "1234567800000000"
            data = ready_payload(device_id="board",boot_id=new_boot,seq=0,peripheral_raw=motion(),peripheral_age_ms=0,wifi=wifi())
            restored.ingest_wifi_status(target,{"x-device-id":"board","x-boot-id":new_boot},data)
            self.assertEqual(restored.wifi_camera_target()["boot"],new_boot)
            self.assertTrue(restored.status()["device_online"])
            for handler in restored.logger.handlers: handler.close()

    def test_audio_pull_uses_existing_private_lease(self):
        self.network()
        audio = self.state.request_audio("browser",True)["audio_monitor"]
        target = self.state.wifi_camera_target()
        packet = {"session_id":audio["session_id"],"seq":0,"sample_rate":16000,"channels":1,"format":"s16le","data":base64.b64encode(b"\0\0"*20).decode()}
        bridge = WifiControlBridge(self.state,"token",lambda *args:({"x-device-id":target["device"],"x-boot-id":target["boot"]},packet))
        bridge.audio_once()
        self.assertEqual(self.state.read_audio("browser",audio["session_id"],-1)[2],b"\0\0"*20)

    def test_http_acceptance_does_not_confirm_test_or_unlock_live(self):
        self.network()
        self.state.request_servo_test({"mask":1,"min_us":1300,"max_us":1700,"expected_epoch":self.state.servo_epoch})
        def accept(ip,token,method,path,envelope):
            target = self.state.wifi_camera_target()
            return {"x-device-id":target["device"],"x-boot-id":target["boot"]},{"id":envelope["id"],"accepted":True}
        bridge = WifiControlBridge(self.state,"token",accept)
        bridge.poll_once()
        self.assertEqual(self.state.status()["servo_control"]["motion_pending"]["status"],"SENT")
        command = self.state.motion_pending.copy()
        self.network(periph_ack=f"PERIPH_ACK cmd=TEST id={command['id']} ok=1 reason=OK",periph_ack_age_ms=0)
        control = self.state.status()["servo_control"]
        self.assertIsNone(control["motion_pending"])
        self.assertFalse(control["can_live"])
        self.assertEqual(control["test"]["state"],"IDLE")

    def test_old_wifi_firmware_is_read_only(self):
        self.network(control_proto=0)
        status = self.state.status()
        self.assertTrue(status["device_online"])
        self.assertFalse(status["servo_control"]["can_live"])
        self.assertIn("升级",status["servo_control"]["reason"])
        self.assertFalse(status["charge_control"]["can_apply"])
        with self.assertRaises(Rejected): self.state.request_scan()
        with self.assertRaises(Rejected): self.state.request_audio("browser",True)

    def test_usb_replug_does_not_cancel_same_wifi_board_live(self):
        self.network()
        self.live()
        epoch = self.state.servo_epoch
        self.receiver.disconnected("unplugged")
        self.receiver = SerialReceiver(self.state,"COM9")
        self.usb()
        self.assertEqual(self.state.servo_epoch,epoch)
        self.assertTrue(self.state.status()["servo_control"]["live"]["active"])

    def test_failed_http_motion_is_not_replayed(self):
        self.network()
        self.state.request_servo_test({"mask":1,"min_us":1300,"max_us":1700,"expected_epoch":self.state.servo_epoch})
        calls = []
        def failed(*args):
            calls.append(args)
            raise TimeoutError()
        bridge = WifiControlBridge(self.state,"token",failed)
        with self.assertRaises(TimeoutError): bridge.poll_once()
        bridge.poll_once()
        self.assertEqual(len(calls),1)
        self.now = 1.3
        self.network()
        self.assertEqual(self.state.status()["servo_control"]["motion_last"]["status"],"UNKNOWN")

    def test_wifi_stop_needs_matching_ack_and_later_readback(self):
        self.network()
        self.state.request_servo({"action":"stop"})
        command = self.state.outgoing_servo(self.state.boot_id)
        self.state.send_servo(command["id"],self.state.boot_id)
        ack = f"PERIPH_ACK cmd=STOP id={command['firmware_id']} ok=1 reason=OK"
        self.network(periph_ack="PERIPH_ACK cmd=STOP id=0 ok=1 reason=OK",periph_ack_age_ms=0)
        self.assertEqual(self.state.pending_servo["status"],"SENT")
        self.network(periph_ack=ack,periph_ack_age_ms=0,peripheral_age_ms=100)
        self.assertEqual(self.state.pending_servo["status"],"AWAITING_READBACK")
        self.network(periph_ack=ack,periph_ack_age_ms=100,peripheral_age_ms=0)
        self.assertIsNone(self.state.pending_servo)
        self.assertEqual(self.state.last_servo["status"],"CONFIRMED")

    def start_test(self):
        self.network()
        self.state.request_servo_test({"mask":1,"min_us":1300,"max_us":1700,"expected_epoch":self.state.servo_epoch})
        command = self.state.outgoing_motion("wifi")
        self.state.send_motion(command["id"],"wifi")
        self.now += .1
        self.network(motion(1100,last_id=command["id"],test_id=command["id"],test_state="RUN",test_result="RUNNING",test_left_ms=9000),
                     periph_ack=f"PERIPH_ACK cmd=TEST id={command['id']} ok=1 reason=OK",periph_ack_age_ms=0)
        return command

    def test_running_history_retired_on_esp_boot_and_cached_ack_ignored(self):
        command = self.start_test()
        previous = self.state.status()["servo_control"]["motion_last"]
        self.now += 1
        self.network(motion(100),boot_id="1234567800000000",seq=0)
        history = self.state.status()["servo_control"]["motion_last"]
        self.assertEqual(history["status"],"CANCELLED_BOOT")
        self.assertEqual(history["kind"],"TEST")
        self.assertTrue(history["previous_boot"])
        self.assertEqual(history["boot_id"],previous["boot_id"])
        self.assertEqual(history["test_state"],"RUN")
        self.assertNotEqual(history["boot_epoch"],self.state.motion_boot_epoch)
        self.state.accept_motion_ack({"id":command["id"],"cmd":"TEST","ok":"1","reason":"OK"})
        self.assertEqual(self.state.motion_last["status"],"CANCELLED_BOOT")
        self.assertEqual(self.state.status()["servo_control"]["test"]["id"],0)

    def test_done_history_remains_completed_but_previous_boot(self):
        command = self.start_test()
        self.now += 10
        self.network(motion(11100,last_id=command["id"],test_id=command["id"],test_state="DONE",test_result="OK"))
        # No status request is needed to retain a board-observed terminal state.
        self.assertEqual(self.state.motion_last["status"],"COMPLETED")
        self.state.accept_motion_ack({"id":command["id"],"cmd":"TEST","ok":"1","reason":"OK"})
        self.assertEqual(self.state.motion_last["status"],"COMPLETED")
        self.now += 1
        self.network(motion(100),boot_id="1234567800000000",seq=0)
        history = self.state.status()["servo_control"]["motion_last"]
        self.assertEqual(history["status"],"COMPLETED")
        self.assertTrue(history["previous_boot"])
        self.assertEqual(history["test_result"],"OK")
        self.assertEqual(self.state.status()["servo_control"]["test"]["state"],"IDLE")

    def test_stm_only_reset_and_port_change_retire_test(self):
        self.start_test()
        boot = self.state.wifi_camera_target()["boot"]
        self.now += 1
        self.network(motion(100))
        self.assertEqual(self.state.wifi_camera_target()["boot"],boot)
        self.assertEqual(self.state.motion_last["status"],"CANCELLED_BOOT")
        self.assertTrue(self.state.motion_last["previous_boot"])
        self.start_test()
        self.state.select_serial_port("COM10")
        self.assertEqual(self.state.motion_last["status"],"CANCELLED_BOOT")
        self.assertTrue(self.state.motion_last["previous_boot"])

    def test_wifi_survives_usb_disconnect_without_retiring_history(self):
        self.start_test()
        self.receiver.disconnected("unplugged")
        history = self.state.status()["servo_control"]["motion_last"]
        self.assertEqual(history["status"],"ACKNOWLEDGED")
        self.assertFalse(history["previous_boot"])

    def test_waveform_timings_old_defaults_and_new_reported_values(self):
        self.network()
        test = self.state.status()["servo_control"]["test"]
        self.assertEqual((test["ramp_ms"],test["hold_ms"]),(1000,0))
        self.network(motion(test_ramp_ms=700,test_hold_ms=300))
        test = self.state.status()["servo_control"]["test"]
        self.assertEqual((test["ramp_ms"],test["hold_ms"]),(700,300))
        self.network(motion(test_ramp_ms="invalid",test_hold_ms=300))
        self.assertFalse(self.state.status()["servo_control"]["test"]["authoritative"])


if __name__ == "__main__": unittest.main()
