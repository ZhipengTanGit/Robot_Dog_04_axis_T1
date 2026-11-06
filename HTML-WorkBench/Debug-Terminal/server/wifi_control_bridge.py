"""Outbound authenticated controls/audio for the USB-paired ESP32 endpoint."""
import base64
import http.client
import json
import secrets
import socket
import threading

from robot_server import unique_object


class ControlHTTPError(ValueError):
    def __init__(self,status):
        self.status = status
        super().__init__("CONTROL_HTTP_FAILED")


def exchange(ip, token, method, path, payload=None, timeout=1.0):
    connection = http.client.HTTPConnection(ip,81,timeout=timeout)
    sock_ref = [None]
    expired = threading.Event()
    def abort():
        expired.set()
        sock = sock_ref[0] or connection.sock
        if sock is not None:
            try: sock.shutdown(socket.SHUT_RDWR)
            except OSError: pass
        connection.close()
    timer = threading.Timer(timeout,abort)
    timer.daemon = True
    timer.start()
    try:
        body = None if payload is None else json.dumps(payload,separators=(",",":")).encode("ascii")
        headers = {"Authorization":"Bearer "+token,"Connection":"close","Accept-Encoding":"identity"}
        if body is not None: headers["Content-Type"] = "application/json"
        connection.request(method,path,body=body,headers=headers)
        sock_ref[0] = connection.sock
        response = connection.getresponse()
        received_headers = {key.lower():value for key,value in response.getheaders()}
        if response.status == 204 and method == "GET":
            return received_headers,None
        if response.status not in (200,202):
            raise ControlHTTPError(response.status)
        lengths = response.headers.get_all("Content-Length",[])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit() or not 1<=int(lengths[0])<=16384:
            raise ValueError("CONTROL_LENGTH_INVALID")
        if response.getheader("Transfer-Encoding") or response.getheader("Content-Encoding","identity") != "identity":
            raise ValueError("CONTROL_ENCODING_INVALID")
        if response.getheader("Content-Type","").split(";",1)[0] != "application/json":
            raise ValueError("CONTROL_JSON_REQUIRED")
        raw = response.read(int(lengths[0]))
        if len(raw) != int(lengths[0]) or expired.is_set():
            raise ValueError("CONTROL_RESPONSE_INCOMPLETE")
        return received_headers,json.loads(raw,object_pairs_hook=unique_object)
    finally:
        timer.cancel()
        timer.join()
        connection.close()


class WifiControlBridge(threading.Thread):
    def __init__(self,state,token,requester=exchange):
        super().__init__(name="robot-wifi-controls",daemon=True)
        self.state,self.token,self.requester = state,token,requester
        self.stopping = threading.Event()
        self.audio_session = None
        self.audio_next_renew = 0

    def stop(self):
        self.stopping.set()
        if self.is_alive(): self.join(timeout=3)

    def _send(self,target,line,identifier,deadline):
        with self.state.lock:
            if target != self.state.wifi_target or not self.state._using_wifi():
                raise ValueError("CONTROL_ROUTE_CHANGED")
            payload = self.state.wifi_payload
            absolute = (payload["uptime_ms"]+int((deadline-self.state.wifi_at)*1000)) & 0xffffffff
            envelope = {"id":identifier,"boot_id":target["boot"],"deadline_ms":absolute,"line":line}
            self.state.wifi_command_diagnostic = {"id":identifier,"kind":line.split()[1],"status":"SENDING",
                "expires_in_ms":int((deadline-self.state.monotonic())*1000),
                "snapshot_age_ms":int((self.state.monotonic()-self.state.wifi_at)*1000),"http_status":None}
        headers,response = self.requester(target["ip"],self.token,"POST","/api/control",envelope)
        if (headers.get("x-device-id"),headers.get("x-boot-id")) != (target["device"],target["boot"]):
            raise ValueError("CONTROL_IDENTITY_CHANGED")
        if not isinstance(response,dict) or response.get("id") != identifier or response.get("accepted") is not True:
            raise ValueError("CONTROL_NOT_ACCEPTED")
        with self.state.lock:
            self.state.wifi_command_diagnostic.update(status="HTTP_ACCEPTED",error=None)

    def poll_once(self):
        self.state.wifi_control_heartbeat = self.state.monotonic()
        target = self.state.wifi_camera_target()
        if target is None or self.state.active_connection() != "wifi":
            return
        now = self.state.monotonic()
        sound = self.state.outgoing_sound("wifi")
        if sound and self.state.send_sound(sound["id"],"wifi"):
            try:
                self._send(target,sound["line"],sound["id"],sound["send_deadline"])
            except (OSError,ValueError,KeyError,TypeError,http.client.HTTPException):
                self.state.fail_sound(sound["id"])
                raise
        motion = self.state.outgoing_motion("wifi")
        if motion and self.state.send_motion(motion["id"],"wifi"):
            self._send(target,motion["line"],motion["request_id"],motion["deadline"])
        servo = self.state.outgoing_servo(self.state.boot_id)
        if servo and self.state.send_servo(servo["id"],self.state.boot_id):
            line = ("PERIPH STOP"+(" "+str(servo["firmware_id"]) if "firmware_id" in servo else "")
                    if servo["action"] == "stop" else f"PERIPH SERVO {servo['channel']} {servo['pulse_us']}")
            self._send(target,line,servo["id"],min(servo["deadline"],now+1.2))
        charge,scan,audio,camera = self.state.serial_commands()
        if charge and charge["expires_in_ms"] >= 1000 and self.state.claim_control(charge["id"],"wifi"):
            with self.state.lock:
                deadline = now+min(4,charge["expires_in_ms"]/1000)
                absolute = (self.state.wifi_payload["uptime_ms"]+int((deadline-self.state.wifi_at)*1000)) & 0xffffffff
            self._send(target,f"ESP CHARGE_CURRENT {charge['id']} {charge['ma']} {absolute} {target['boot'][:8]}",charge["id"],deadline)
        for command,verb in ((scan,"I2C_SCAN"),(camera,"CAMERA_CHECK")):
            if command and command["expires_in_ms"] >= 1000 and self.state.claim_control(command["id"],"wifi"):
                self._send(target,f"ESP {verb} {command['id']}",command["id"],now+1.2)
        session = audio["session_id"] if audio["enabled"] else None
        if self.audio_session is not None and self.audio_session != session:
            old,self.audio_session = self.audio_session,None
            self._send(target,f"ESP AUDIO_STOP {old}",secrets.token_hex(8),now+1.2)
        if session and (session != self.audio_session or now >= self.audio_next_renew):
            self.audio_session,self.audio_next_renew = session,now+5
            self._send(target,f"ESP AUDIO_START {session} {audio['expires_in_ms']}",secrets.token_hex(8),now+1.2)

    def audio_once(self):
        with self.state.lock:
            audio = self.state._audio_status(self.state.monotonic())
            if not audio["enabled"] or not self.state._using_wifi():
                return
            target = self.state.wifi_camera_target()
            session = audio["session_id"]
        headers,packet = self.requester(target["ip"],self.token,"GET","/api/audio?session="+session)
        if packet is None: return
        if (headers.get("x-device-id"),headers.get("x-boot-id")) != (target["device"],target["boot"]):
            raise ValueError("AUDIO_IDENTITY_CHANGED")
        if (not isinstance(packet,dict) or packet.get("session_id") != session or packet.get("sample_rate") != 16000
                or packet.get("channels") != 1 or packet.get("format") != "s16le" or not isinstance(packet.get("data"),str)):
            raise ValueError("AUDIO_FORMAT_INVALID")
        pcm = base64.b64decode(packet["data"],validate=True)
        with self.state.lock:
            if target != self.state.wifi_target or not self.state._using_wifi(): return
            self.state.ingest_audio(self.state.device_id,self.state.boot_id,session,packet["seq"],pcm,"16000","1","s16le")

    def _audio_loop(self):
        while not self.stopping.is_set():
            try: self.audio_once()
            except (OSError,ValueError,KeyError,TypeError,http.client.HTTPException): pass
            self.stopping.wait(0.04)

    def run(self):
        audio = threading.Thread(target=self._audio_loop,name="robot-wifi-audio",daemon=True)
        audio.start()
        try:
            while not self.stopping.is_set():
                try: self.poll_once()
                except (OSError,ValueError,KeyError,TypeError,http.client.HTTPException) as error:
                    with self.state.lock:
                        diagnostic = self.state.wifi_command_diagnostic or {}
                        safe_error = str(error) if isinstance(error,ValueError) and str(error).startswith("CONTROL_") else type(error).__name__
                        self.state.wifi_command_diagnostic = {**diagnostic,"status":"FAILED","error":safe_error,
                                                              "http_status":getattr(error,"status",None)}
                self.stopping.wait(0.05)
        finally:
            audio.join(timeout=2)
