"""Portable package tests: local HTTP only, no physical serial port is opened."""
import argparse
import contextlib
import copy
import http.client
import io
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import portable_usb_launcher as launcher

sys.path.insert(0,str(Path(__file__).resolve().parent.parent/"server"))
import robot_server


def port(name, vid=0x1A86, description="USB-SERIAL CH340"):
    return SimpleNamespace(device=name,vid=vid,description=description)


class FakeBridge:
    instances = []
    def __init__(self,state,device,baud):
        self.state,self.device,self.baud = state,device,baud
        self.started = self.stopped = False
        self.instances.append(self)
    def start(self):
        self.started = True
    def stop(self):
        self.stopped = True


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        FakeBridge.instances = []
        self.server = SimpleNamespace(state=robot_server.TelemetryState(transport="serial"))

    def test_no_device_waits_then_unique_ch340_is_selected_without_fixed_com_number(self):
        ports = []
        manager = launcher.PortSupervisor(self.server,FakeBridge,ports=lambda:ports,interactive=False)
        with contextlib.redirect_stdout(io.StringIO()):
            manager.step()
            self.assertIsNone(manager.bridge)
            self.assertFalse(self.server.state.status()["device_online"])
            ports.extend([port("COM5",vid=0x10C4,description="CP2102"),port("COM27")])
            manager.step()
            manager.step()
        self.assertEqual(len(FakeBridge.instances),1)
        self.assertEqual((manager.bridge.device,manager.bridge.baud),("COM27",921600))
        manager.stop()
        self.assertTrue(manager.bridge.stopped)

    def test_multiple_devices_never_choose_implicitly_and_invalid_input_can_retry(self):
        ports = [port("COM7"),port("COM11")]
        choices = iter(["wrong", "", "2"])
        manager = launcher.PortSupervisor(self.server,FakeBridge,ports=lambda:ports,prompt=lambda _:next(choices),interactive=True)
        with contextlib.redirect_stdout(io.StringIO()):
            manager.step(); self.assertIsNone(manager.bridge)
            manager.step(); self.assertIsNone(manager.bridge)
            manager.step()
        self.assertEqual(manager.bridge.device,"COM11")
        headless = launcher.PortSupervisor(self.server,FakeBridge,ports=lambda:ports,interactive=False)
        with contextlib.redirect_stdout(io.StringIO()): headless.step()
        self.assertIsNone(headless.bridge)

    def test_explicit_port_does_not_enumerate_and_list_ports_never_opens_a_bridge(self):
        manager = launcher.PortSupervisor(self.server,FakeBridge,requested="COM8",ports=lambda: (_ for _ in ()).throw(AssertionError("must not enumerate")))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            manager.step()
            launcher.list_ports_command([port("COM23"),port("COM1",None,"系统串口")])
        self.assertEqual(manager.bridge.device,"COM8")
        self.assertEqual(len(FakeBridge.instances),1)
        self.assertIn("[CH340/WCH] COM23",output.getvalue())
        manager.stop()


class InstanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="机器狗 USB 调试 ")
        self.root = Path(self.temporary.name)
        self.server,self.metadata = launcher.create_service(robot_server,self.root,0)
        self.thread = threading.Thread(target=self.server.serve_forever,kwargs={"poll_interval":0.02},daemon=True)
        self.thread.start()
        self.state_file = self.root/"run"/"instance.json"
        self.state_file.parent.mkdir()
        launcher.write_metadata(self.state_file,self.metadata)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        launcher.close_recorders(self.server.state)
        self.temporary.cleanup()

    def args(self,**changes):
        return argparse.Namespace(list_ports=False,stop=False,no_browser=True,serial_port=None,port=8081,**changes)

    def test_identity_token_and_package_path_are_checked_and_secrets_never_returned(self):
        data = launcher.management_request(self.metadata)
        self.assertEqual(data["server_id"],launcher.PORTABLE_ID)
        self.assertEqual(data["package_id"],launcher.package_id(self.root))
        self.assertNotIn(self.metadata["stop_token"],json.dumps(data))
        self.assertNotIn(self.server.token,json.dumps(data))
        self.assertGreaterEqual(len(self.server.token),32)
        wrong = copy.deepcopy(self.metadata); wrong["stop_token"]="0"*64
        self.assertIsNone(launcher.management_request(wrong,stop=True))
        self.assertTrue(self.thread.is_alive())
        wrong = copy.deepcopy(self.metadata); wrong["package_id"]="0"*64
        self.assertIsNone(launcher.management_request(wrong))
        self.assertIsNone(launcher.read_metadata(self.state_file,launcher.package_id(self.root/"别的目录")))

    def test_repeated_launch_reuses_only_same_package_and_stop_does_not_kill_foreign_pid(self):
        lock = launcher.PackageLock(self.state_file.with_name("instance.lock"))
        self.assertTrue(lock.acquire())
        output = io.StringIO()
        try:
            with contextlib.redirect_stdout(output), patch.object(launcher,"create_service",side_effect=AssertionError("duplicate server")):
                self.assertEqual(launcher.run(self.args(),root=self.root),0)
            self.assertIn(str(self.metadata["port"]),output.getvalue())
            self.assertNotIn(self.metadata["stop_token"],output.getvalue())
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(launcher.run(argparse.Namespace(list_ports=False,stop=True),root=self.root/"other package"),0)
            self.assertTrue(self.thread.is_alive())
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(launcher.run(argparse.Namespace(list_ports=False,stop=True),root=self.root),0)
            self.thread.join(timeout=2)
            self.assertFalse(self.thread.is_alive())
        finally:
            lock.release()

    def test_stale_port_with_wrong_token_never_gets_reused(self):
        changed = dict(self.metadata,stop_token="a"*64)
        launcher.write_metadata(self.state_file,changed)
        self.assertIsNone(launcher.existing_instance(self.state_file,self.root))
        self.assertTrue(self.thread.is_alive())

    def test_occupied_port_falls_back_without_changing_existing_listener(self):
        other,metadata = launcher.create_service(robot_server,self.root/"第二份 包",self.metadata["port"])
        try:
            self.assertNotEqual(other.server_address[1],self.metadata["port"])
            self.assertEqual(other.server_address[0],"127.0.0.1")
            self.assertEqual(metadata["port"],other.server_address[1])
            self.assertIsNotNone(launcher.management_request(self.metadata))
        finally:
            other.server_close()
            launcher.close_recorders(other.state)


class LockAndCliTests(unittest.TestCase):
    def test_lock_is_exclusive_and_released_without_removing_lock_file(self):
        with tempfile.TemporaryDirectory() as directory:
            first = launcher.PackageLock(Path(directory)/"同一个 目录"/"instance.lock")
            second = launcher.PackageLock(first.path)
            self.assertTrue(first.acquire())
            self.assertFalse(second.acquire())
            first.release()
            self.assertTrue(second.acquire())
            second.release()

    def test_help_does_not_require_runtime_dependencies_or_serial_access(self):
        with contextlib.redirect_stdout(io.StringIO()) as output, self.assertRaises(SystemExit) as stopped:
            launcher.main(["--help"])
        self.assertEqual(stopped.exception.code,0)
        self.assertIn("--list-ports",output.getvalue())
        self.assertIn("--no-browser",output.getvalue())
        self.assertIn("--serial-port",output.getvalue())

    def test_cli_port_and_serial_name_validation(self):
        for arguments in (["--port","65536"],["--serial-port","COM0"],["--serial-port","other.exe"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped:
                launcher.main(arguments)
            self.assertEqual(stopped.exception.code,2)


if __name__ == "__main__":
    unittest.main()
