"""Find the already-paired board after DHCP changes, without USB or LAN scans."""
import hashlib
import hmac
import ipaddress
import json
import re
import socket
import threading
import time
import http.client
from wifi_camera import fetch, unique_object

PORT = 32321


def parse_announcement(packet, source_ip, token):
    if not isinstance(packet,bytes) or len(packet)>255 or not re.fullmatch(r'[a-f0-9]{64}',token or ''):
        return None
    try:
        body, signature = packet.rsplit(b' ',1)
        if not hmac.compare_digest(hmac.new(token.encode('ascii'),body,hashlib.sha256).hexdigest().encode(),signature):
            return None
        kind, device, boot, address, uptime = body.decode('ascii').split(' ')
        ip = ipaddress.IPv4Address(address)
        private = any(ip in ipaddress.IPv4Network(net) for net in ('10.0.0.0/8','172.16.0.0/12','192.168.0.0/16'))
        if (kind!='RDW1' or not re.fullmatch(r'[A-Za-z0-9_-]{1,24}',device)
                or not re.fullmatch(r'[a-f0-9]{32}',boot) or str(ip)!=source_ip or not private
                or not re.fullmatch(r'[0-9]{1,10}',uptime) or int(uptime)>0xffffffff):
            return None
        return {'device':device,'boot':boot,'ip':str(ip)}
    except (ValueError,UnicodeError):
        return None


class WifiDiscovery(threading.Thread):
    def __init__(self,state,token,getter=fetch):
        super().__init__(name='robot-wifi-discovery',daemon=True)
        self.state,self.token,self.getter=state,token,getter
        self.stopping=threading.Event()
        self.connection=None
        self.last_probe=-10.0

    def stop(self):
        self.stopping.set()
        if self.connection is not None:
            self.connection.close()

    def receive(self,packet,source_ip):
        announcement=parse_announcement(packet,source_ip,self.token)
        peer=self.state.wifi_discovery_peer()
        if not announcement or not peer or announcement['device']!=peer['device']:
            return False
        target=self.state.wifi_camera_target()
        if target and all(target[key]==announcement[key] for key in ('device','boot','ip')):
            return False
        if time.monotonic()-self.last_probe<2:
            return False
        self.last_probe=time.monotonic()
        try:
            headers,body=self.getter(announcement['ip'],'/api/status',self.token,64*1024,2)
            if headers.get('content-type','').split(';',1)[0]!='application/json':
                return False
            payload=json.loads(body,object_pairs_hook=unique_object)
            target=self.state.adopt_wifi_discovery(announcement,headers,payload)
            if target is None:
                return False
            self.state.ingest_wifi_status(target,headers,payload)
            return True
        except (OSError,ValueError,TypeError,KeyError,RecursionError,http.client.HTTPException):
            return False

    def run(self):
        connection=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.connection=connection
        try:
            connection.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            connection.bind(('0.0.0.0',PORT))
            connection.settimeout(.5)
            while not self.stopping.is_set():
                try:
                    packet,source=connection.recvfrom(512)
                    self.receive(packet,source[0])
                except socket.timeout:
                    pass
        except OSError:
            pass
        finally:
            connection.close()
