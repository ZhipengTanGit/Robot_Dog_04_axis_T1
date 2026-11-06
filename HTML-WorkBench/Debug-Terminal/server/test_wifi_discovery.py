import hashlib
import hmac
import json
import unittest
import tempfile
from pathlib import Path
from robot_server import TelemetryState
from serial_bridge import SerialReceiver
from test_server import ready_payload
from test_wifi import wifi
from wifi_discovery import parse_announcement, WifiDiscovery

KEY='1234567890abcdef'*4
BOOT='a'*32
NEW_BOOT='b'*32


def packet(boot=NEW_BOOT,ip='192.168.1.47',device='robot-dog',key=KEY):
    body=f'RDW1 {device} {boot} {ip} 1234'.encode()
    return body+b' '+hmac.new(key.encode(),body,hashlib.sha256).hexdigest().encode()


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.state=TelemetryState(transport='serial',serial_port='COM9')
        self.receiver=SerialReceiver(self.state,'COM9')
        self.receiver.feed(('ESP_STATUS '+json.dumps(ready_payload(boot_id=BOOT,wifi=wifi()))+'\n').encode())

    def getter(self,ip,path,key,limit,timeout):
        self.assertEqual((path,key),('/api/status',KEY))
        return {'content-type':'application/json','x-device-id':'robot-dog','x-boot-id':NEW_BOOT},json.dumps(
            ready_payload(boot_id=NEW_BOOT,seq=100,wifi=wifi(ip=ip))).encode()

    def test_authenticated_lan_announcement(self):
        self.assertEqual(parse_announcement(packet(),'192.168.1.47',KEY),{'device':'robot-dog','boot':NEW_BOOT,'ip':'192.168.1.47'})
        for data,source,key in [(packet()[:-1]+b'0','192.168.1.47',KEY),(packet(),'192.168.1.48',KEY),
                                (packet(ip='8.8.8.8'),'8.8.8.8',KEY),(packet(),'192.168.1.47','c'*64),
                                (packet(boot='bad'),'192.168.1.47',KEY),(b'bad','192.168.1.47',KEY)]:
            self.assertIsNone(parse_announcement(data,source,key))

    def test_reboot_and_new_ip_without_serial(self):
        self.state.select_serial_port(None)
        self.assertIsNotNone(self.state.wifi_camera_target())
        with self.state.lock:
            self.state._reset_wifi_target(None)
        self.assertIsNotNone(self.state.wifi_discovery_peer())
        discovery=WifiDiscovery(self.state,KEY,self.getter)
        self.assertTrue(discovery.receive(packet(),'192.168.1.47'))
        target=self.state.wifi_camera_target()
        self.assertEqual((target['ip'],target['boot']),('192.168.1.47',NEW_BOOT))
        self.assertEqual(self.state.status()['connection']['active'],'wifi')
        self.assertFalse(self.state.status()['serial_connected'])

    def test_other_board_and_live_http_identity_are_required(self):
        discovery=WifiDiscovery(self.state,KEY,lambda *a:self.fail('unpaired board must not be probed'))
        self.assertFalse(discovery.receive(packet(device='another-board'),'192.168.1.47'))
        def wrong_boot(*args):
            headers,body=self.getter(*args)
            payload=json.loads(body);payload['boot_id']=BOOT
            return headers,json.dumps(payload).encode()
        discovery.getter=wrong_boot
        self.assertFalse(discovery.receive(packet(),'192.168.1.47'))
        self.assertEqual(self.state.wifi_camera_target()['boot'],BOOT)

    def test_manual_serial_disconnect_preserves_live_wifi(self):
        target=self.state.wifi_camera_target()
        payload=ready_payload(boot_id=BOOT,seq=99,wifi=wifi())
        self.state.ingest_wifi_status(target,{'x-device-id':'robot-dog','x-boot-id':BOOT},payload)
        self.state.select_serial_port(None)
        self.assertTrue(self.state.status()['device_online'])
        self.assertEqual(self.state.status()['connection']['active'],'wifi')
        self.state.select_serial_port('COM12')
        self.assertIsNone(self.state.wifi_discovery_peer())

    def test_temporary_link_loss_keeps_peer_for_next_server_start(self):
        with tempfile.TemporaryDirectory() as directory:
            state=TelemetryState(directory,transport='serial',serial_port=None)
            try:
                with state.lock:
                    state._reset_wifi_target({'device':'robot-dog','boot':BOOT,'ip':'192.168.1.38'})
                    state._reset_wifi_target(None)
                self.assertTrue((Path(directory)/'wifi-pairing.json').exists())
                restored=TelemetryState(directory,transport='serial',serial_port=None)
                try:
                    self.assertEqual(restored.wifi_discovery_peer()['device'],'robot-dog')
                    restored.select_serial_port('COM12')
                    self.assertFalse((Path(directory)/'wifi-pairing.json').exists())
                finally:
                    for handler in restored.logger.handlers:handler.close()
            finally:
                for handler in state.logger.handlers:handler.close()


if __name__=='__main__':
    unittest.main()
