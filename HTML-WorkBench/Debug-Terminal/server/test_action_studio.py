import copy
import json
import tempfile
import unittest
from pathlib import Path

from action_studio import validate_action
from robot_server import TelemetryState, Rejected
from serial_bridge import SerialReceiver
from test_server import ready_payload
from test_servo_live_wifi import motion
from test_wifi import BOOT, wifi
from wifi_control_bridge import WifiControlBridge

DOCUMENT = {"schema": 1, "name": "抬腿", "frames": [
    {"name": "姿态一", "angles": [90.1, 90.1, 90.1, 90.1], "hold_ms": 200},
    {"name": "姿态二", "angles": [100, 100, 100, 100], "hold_ms": 400}]}


class ActionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.now = 0.
        self.seq = 0
        self.state = TelemetryState(data_dir=self.directory.name, monotonic=lambda:self.now, transport="serial", serial_port="COM9")
        self.addCleanup(lambda: [h.close() for h in self.state.logger.handlers])
        self.receiver = SerialReceiver(self.state, "COM9")
        self.snapshot()

    def snapshot(self, torque=0, teach=0, position=512, valid=True, flags=0, **changes):
        self.now += .001
        self.seq += 1
        raw = motion(tick=1000+int(self.now*1000), **changes)+f" teach_proto=1 pose_proto=1 teach={teach} servo_bus=TTL_CONTROL"
        raw += ''.join(f" ttl{i}=1,115200,{position if valid else 'NA'},{flags},0,0,0 tq{i}={torque}" for i in range(4))
        data=ready_payload(boot_id=BOOT,seq=self.seq,uptime_ms=1000+int(self.now*1000),wifi=wifi(),peripheral_raw=raw,peripheral_age_ms=0)
        self.receiver.feed(("ESP_STATUS "+json.dumps(data)+"\n").encode())

    def start(self, action="teach", owner="browser"):
        args={"action":action,"expected_epoch":self.state.servo_epoch}
        if action=="play":args["document"]=DOCUMENT
        return self.state.request_action(owner,args)["action_studio"]["run_id"]

    def renew(self, run, dt=.1):
        self.now+=dt
        self.state.request_action("browser",{"action":"renew","run_id":run})

    def dispatch(self, kind, ok=True):
        c=self.state.outgoing_motion("serial")
        self.assertEqual(c["kind"],kind)
        self.assertTrue(self.state.send_motion(c["id"],"serial"))
        self.state.accept_motion_ack({"cmd":kind,"id":str(c["id"]),"ok":"1" if ok else "0"})
        return c

    def tick(self):
        with self.state.lock:self.state._action_tick(self.now)

    def test_record_requires_ack_and_actual_four_torque_off(self):
        run=self.start()
        request={"action":"capture","run_id":run,"name":"站立","hold_ms":1000}
        self.dispatch("TEACH")
        with self.assertRaises(Rejected):self.state.request_action("browser",request)
        self.snapshot(torque=4,teach=1);self.tick()
        self.assertEqual(self.state.request_action("browser",request)["frame"]["angles"],[90.1]*4)
        self.snapshot(torque=3,teach=1)
        with self.assertRaises(Rejected):self.state.request_action("browser",request)

    def test_failed_ack_cannot_authorize_capture(self):
        self.start();self.dispatch("TEACH",False);self.snapshot(torque=4,teach=1);self.tick()
        self.assertEqual(self.state.action_last["mode"],"FAILED")
        self.assertEqual(self.state.motion_pending["kind"],"STOP")

    def test_teaching_locks_manual_control_and_other_browser(self):
        run=self.start();self.dispatch("TEACH");self.snapshot(torque=4,teach=1);self.tick()
        epoch=self.state.servo_epoch
        with self.assertRaises(Rejected):self.state.request_live("browser",{"action":"set","channel":0,"pulse_us":1500,"expected_epoch":epoch})
        with self.assertRaises(Rejected):self.state.request_servo({"channel":0,"pulse_us":1500,"expected_epoch":epoch})
        with self.assertRaises(Rejected):self.start("play",owner="another")
        with self.assertRaises(Rejected):self.state.request_action("another",{"action":"renew","run_id":run})

    def test_play_pose_is_atomic_waits_actual_arrival_then_hold(self):
        run=self.start("play");c=self.dispatch("POSE")
        self.assertEqual(len(c["args"]),4)
        self.assertEqual(c["line"].split()[4:],["1501"]*4)
        self.snapshot(torque=1,servo_mode="LIVE");self.tick()
        self.assertEqual(self.state.action_run["phase"],"MOVING")
        self.snapshot(torque=2,servo_mode="LIVE");self.tick()
        self.assertEqual(self.state.action_run["phase"],"HOLDING")
        self.renew(run,.21);self.snapshot(torque=2,servo_mode="LIVE");self.tick()
        c=self.dispatch("POSE");self.assertEqual(c["args"],(1611,)*4)
        self.renew(run,.1);self.snapshot(torque=2,position=568,servo_mode="LIVE");self.tick()
        self.renew(run,.41);self.snapshot(torque=2,position=568,servo_mode="LIVE");self.tick()
        self.assertIsNone(self.state.action_run)
        self.assertEqual(self.state.action_last["mode"],"COMPLETED")
        self.assertEqual(self.state.motion_pending["kind"],"STOP")

    def test_center_works_without_frames_and_returns_to_verified_teaching(self):
        library=copy.deepcopy(self.state.action_library)
        run=self.start("center");c=self.dispatch("POSE")
        self.assertEqual(c["args"],(1500,1500,1500,1500))
        self.assertEqual(self.state.action_run["mode"],"CENTERING")
        self.snapshot(torque=1,position=512,servo_mode="LIVE");self.tick()
        self.assertEqual(self.state.action_run["phase"],"MOVING")
        self.snapshot(torque=2,position=512,servo_mode="LIVE");self.tick()
        self.assertEqual(self.state.action_run["phase"],"HOLDING")
        self.renew(run,.21);self.snapshot(torque=2,servo_mode="LIVE");self.tick()
        self.assertEqual(self.state.action_run["mode"],"RELEASING")
        self.assertIsNone(self.state.live_servo)
        self.dispatch("TEACH")
        self.snapshot(torque=3,teach=1);self.tick()
        self.assertFalse(self.state.status()["action_studio"]["can_capture"])
        self.snapshot(torque=4,teach=1);self.tick()
        self.assertEqual(self.state.action_run["run_id"],run)
        self.assertTrue(self.state.status()["action_studio"]["can_capture"])
        self.assertEqual(self.state.action_library,library)

    def test_center_from_teaching_keeps_lease_identity_and_rejects_other_owner(self):
        run=self.start();self.dispatch("TEACH");self.snapshot(torque=4,teach=1);self.tick()
        self.assertTrue(self.state.status()["action_studio"]["can_center"])
        with self.assertRaises(Rejected):self.start("center",owner="another")
        self.assertEqual(self.start("center"),run)
        self.renew(run)
        self.assertGreater(self.state.live_servo["deadline"],self.now)
        self.dispatch("POSE")
        self.assertFalse(self.state.status()["action_studio"]["can_capture"])

    def test_center_rejects_missing_feedback_and_motion_interlock(self):
        self.snapshot(valid=False)
        with self.assertRaises(Rejected):self.start("center")
        self.snapshot(servo_allowed=0)
        with self.assertRaises(Rejected):self.start("center")

    def test_center_stop_or_lease_loss_never_resumes_teaching(self):
        run=self.start("center");self.dispatch("POSE")
        self.state.request_action("browser",{"action":"stop"})
        self.snapshot(torque=2,servo_mode="LIVE");self.tick()
        self.assertIsNone(self.state.action_run)
        self.assertEqual(self.state.motion_pending["kind"],"STOP")
        self.dispatch("STOP");self.snapshot();run=self.start("center");self.dispatch("POSE")
        self.now+=1.3;self.snapshot(torque=2,servo_mode="LIVE");self.tick()
        self.assertIsNone(self.state.action_run)
        self.assertEqual(self.state.motion_pending["kind"],"STOP")
        with self.assertRaises(Rejected):self.renew(run)

    def test_readonly_gets_do_not_advance_frames(self):
        self.start("play");self.dispatch("POSE");self.snapshot(torque=2,servo_mode="LIVE")
        for _ in range(10):self.state.status();self.state.action_document()
        self.assertEqual(self.state.action_run["phase"],"MOVING")

    def test_lease_loss_and_old_renew_never_restart(self):
        run=self.start("play");self.dispatch("POSE")
        self.now=1.3;self.snapshot(torque=2,servo_mode="LIVE");self.state.outgoing_motion("serial")
        self.assertIsNone(self.state.action_run)
        with self.assertRaises(Rejected):self.state.request_action("browser",{"action":"renew","run_id":run})
        self.snapshot();self.assertIsNone(self.state.action_run)

    def test_bad_feedback_cancels_play(self):
        self.start("play");self.dispatch("POSE");self.snapshot(torque=2,valid=False,servo_mode="LIVE");self.tick()
        self.assertEqual(self.state.action_last["mode"],"FAILED")

    def test_arrival_timeout_and_fault_stop(self):
        run=self.start("play");self.dispatch("POSE")
        for _ in range(21):
            self.renew(run,.49);self.snapshot(torque=2,position=0,servo_mode="LIVE");self.tick()
        self.assertEqual(self.state.action_last["mode"],"FAILED")

    def test_stop_preempts_pose_and_fences_its_ack(self):
        self.start("play");c=self.state.outgoing_motion("serial");self.state.send_motion(c["id"],"serial")
        self.state.request_servo({"action":"stop"})
        self.state.accept_motion_ack({"cmd":"POSE","id":str(c["id"]),"ok":"1"})
        self.assertIsNone(self.state.action_run);self.assertIsNone(self.state.live_servo)

    def test_connection_change_cancels_and_preserves_document(self):
        self.start();self.state.select_serial_port("COM10")
        self.assertIsNone(self.state.action_run)

    def test_invalid_and_old_firmware_cannot_teach(self):
        self.snapshot(valid=False)
        with self.assertRaises(Rejected):self.start()
        self.snapshot();self.state._payload["peripheral_raw"]=self.state._payload["peripheral_raw"].replace("teach_proto=1","teach_proto=0")
        with self.assertRaises(Rejected):self.start()

    def test_save_reload_revision_and_invalid_import_preserves_file(self):
        self.state.request_action("browser",{"action":"save","revision":self.state.action_revision,"document":DOCUMENT})
        path=Path(self.directory.name)/"action-library.json";before=path.read_bytes()
        with self.assertRaises(Rejected):self.state.request_action("browser",{"action":"save","revision":"old","document":DOCUMENT})
        bad=copy.deepcopy(DOCUMENT);bad["frames"][0]["angles"][0]=float('nan')
        with self.assertRaises(Rejected):self.state.request_action("browser",{"action":"save","revision":self.state.action_revision,"document":bad})
        self.assertEqual(path.read_bytes(),before)
        restored=TelemetryState(data_dir=self.directory.name)
        self.addCleanup(lambda: [h.close() for h in restored.logger.handlers])
        self.assertEqual(restored.action_library,DOCUMENT);self.assertIsNone(restored.action_run)

    def test_strict_limits(self):
        for bad in [True,{},dict(DOCUMENT,frames=[DOCUMENT["frames"][0]]*121)]:
            with self.assertRaises(Rejected):validate_action(bad)

    def test_wifi_http_acceptance_is_not_torque_confirmation(self):
        def network(ack=None):
            self.now += .002
            self.seq += 1
            target = self.state.wifi_camera_target()
            raw = self.state._payload["peripheral_raw"]
            if ack:
                raw=raw.replace("teach=0","teach=1")
                for i in range(4):raw=raw.replace(f"tq{i}=0",f"tq{i}=4")
            data=ready_payload(device_id=target["device"],boot_id=target["boot"],seq=self.seq,
                uptime_ms=1000+int(self.now*1000),wifi=wifi(),peripheral_raw=raw,peripheral_age_ms=0,control_proto=1)
            if ack:data.update(periph_ack=ack,periph_ack_age_ms=0)
            headers={"x-device-id":target["device"],"x-boot-id":target["boot"]}
            self.state.ingest_wifi_status(target,headers,data)
        network()
        sent=[]
        def request(ip, token, method, path, body):
            sent.append(body)
            target=self.state.wifi_camera_target()
            return {"x-device-id":target["device"],"x-boot-id":target["boot"]},{"accepted":True,"id":body["id"]}
        bridge=WifiControlBridge(self.state,"a"*64,requester=request)
        self.start();bridge.poll_once()
        self.assertTrue(sent[0]["line"].startswith("PERIPH TEACH "))
        self.assertFalse(self.state.status()["action_studio"]["can_capture"])
        identifier=self.state.motion_pending["id"]
        network({"cmd":"TEACH","id":str(identifier),"ok":"1"});self.tick()
        self.assertTrue(self.state.status()["action_studio"]["can_capture"])
        for value in [True,-1,181,float('inf'),"90"]:
            bad=copy.deepcopy(DOCUMENT);bad["frames"][0]["angles"][0]=value
            with self.assertRaises(Rejected):validate_action(bad)


if __name__=="__main__":unittest.main()
