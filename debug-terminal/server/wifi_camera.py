"""Bounded outbound camera polling; no listeners, redirects, DNS or proxy env."""
import http.client
import json
import socket
import threading
import time


MAX_FRAME_BYTES = 512 * 1024  # Same bound as RobotServer and the ESP32 stream.


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def _fetch_on_connection(connection, path, token, limit, timeout):
    # Caller supplies only a USB-paired RFC1918 literal and one of two fixed paths.
    connection.timeout = timeout
    if connection.sock is not None:
        connection.sock.settimeout(timeout)
    expired = threading.Event()
    socket_ref = [None]

    def abort():
        expired.set()
        # HTTPResponse can keep a makefile reference after getresponse() clears
        # connection.sock for Connection: close. Keep the socket for the absolute
        # deadline so a slowly trickling body cannot outlive that deadline.
        sock = socket_ref[0] or connection.sock
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        connection.close()

    timer = threading.Timer(timeout, abort)
    timer.daemon = True
    timer.start()
    try:
        connection.request("GET", path, headers={"Authorization":"Bearer "+token,
                           "Connection":"keep-alive", "Accept-Encoding":"identity"})
        socket_ref[0] = connection.sock
        response = connection.getresponse()
        headers = {key.lower(): value for key,value in response.getheaders()}
        if response.status != 200:  # Never follow a redirect, even to another private IP.
            raise ValueError("Camera HTTP response was not successful")
        lengths = response.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            raise ValueError("A single bounded Content-Length is required")
        length = int(lengths[0])
        if not 1 <= length <= limit or response.getheader("Transfer-Encoding"):
            raise ValueError("Invalid camera body length")
        if response.getheader("Content-Encoding", "identity") != "identity":
            raise ValueError("Encoded camera body is not supported")
        parts, received = [], 0
        while received < length:
            if expired.is_set():
                raise TimeoutError("Camera request timed out")
            chunk = response.read1(min(8192,length-received))
            if not chunk:
                raise ValueError("Incomplete camera response")
            parts.append(chunk)
            received += len(chunk)
        if expired.is_set():
            raise TimeoutError("Camera request timed out")
        return headers, b"".join(parts)
    finally:
        timer.cancel()
        # A timer already entering abort() must finish before this connection
        # can serve another frame; otherwise it could close the next request.
        timer.join()


class PersistentFetcher:
    """One worker-owned connection, reused only after a complete bounded body."""
    def __init__(self):
        self.connection, self.ip = None, None

    def close(self):
        if self.connection is not None:
            self.connection.close()
        self.connection, self.ip = None, None

    def __call__(self, ip, path, token, limit, timeout):
        if ip != self.ip or self.connection is None:
            self.close()
            self.connection = http.client.HTTPConnection(ip,81,timeout=timeout)
            self.ip = ip
        try:
            return _fetch_on_connection(self.connection,path,token,limit,timeout)
        except Exception:
            self.close()  # Never reuse a partially read/failed HTTP response.
            raise


def fetch(ip, path, token, limit, timeout):
    """One-shot wrapper retained for bounded transport tests/tools."""
    client = PersistentFetcher()
    try:
        return client(ip,path,token,limit,timeout)
    finally:
        client.close()


class StreamUnavailable(ValueError):
    """Only an explicit old-firmware 404 permits single-JPEG compatibility."""


class FrameStream:
    """Bounded multipart parser over HTTPResponse (including chunk decoding)."""
    def __init__(self, ip, token):
        self.connection = http.client.HTTPConnection(ip,82,timeout=2)
        self.response, self.headers, self.sock = None, None, None
        try:
            def begin():
                self.connection.request("GET","/stream.mjpg",headers={"Authorization":"Bearer "+token,
                                        "Accept-Encoding":"identity","Connection":"keep-alive"})
                self.sock = self.connection.sock
                self.response = self.connection.getresponse()
                if self.response.status == 404:
                    raise StreamUnavailable("Stream is not supported")
                if self.response.status != 200:
                    raise ValueError("Stream HTTP response failed")
                self.headers = {key.lower():value for key,value in self.response.getheaders()}
                content = self.response.headers
                if content.get_content_type() != "multipart/x-mixed-replace" or content.get_param("boundary") != "robotframe":
                    raise ValueError("Invalid multipart boundary")
                if self.response.getheader("Content-Encoding","identity") != "identity":
                    raise ValueError("Encoded camera stream is not supported")
            self._bounded(begin,2)
        except Exception:
            self.close()
            raise

    def close(self):
        sock = self.sock or self.connection.sock
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.connection.close()
        if self.response is not None:
            self.response.close()

    def _bounded(self, operation, timeout):
        expired = threading.Event()
        def abort():
            expired.set()
            self.close()
        timer = threading.Timer(timeout,abort)
        timer.daemon = True
        timer.start()
        try:
            result = operation()
            if expired.is_set():
                raise TimeoutError("Camera stream deadline exceeded")
            return result
        finally:
            timer.cancel(); timer.join()

    def next_frame(self):
        def read_frame():
            # Firmware may prepend exactly one CRLF to the first/next boundary.
            line = self.response.readline(257)
            if line == b"\r\n":
                line = self.response.readline(257)
            if line in (b"",b"--robotframe--\r\n"):
                raise EOFError("Camera stream ended")
            if line != b"--robotframe\r\n":
                raise ValueError("Invalid camera frame boundary")
            headers, total = {}, 0
            for _ in range(17):
                line = self.response.readline(513)
                total += len(line)
                if total>4096 or len(line)>512 or not line.endswith(b"\r\n"):
                    raise ValueError("Camera part headers too large")
                if line == b"\r\n":
                    break
                name,separator,value = line[:-2].partition(b":")
                if not separator:
                    raise ValueError("Malformed camera part header")
                key = name.decode("ascii").lower()
                if key in headers:
                    raise ValueError("Duplicate camera part header")
                headers[key] = value.decode("ascii").strip()
            else:
                raise ValueError("Too many camera part headers")
            length = headers.get("content-length","")
            if not length.isascii() or not length.isdigit() or not 4<=int(length)<=MAX_FRAME_BYTES:
                raise ValueError("Invalid camera part length")
            if headers.get("content-type") != "image/jpeg":
                raise ValueError("Expected a JPEG part")
            for key in ("x-device-id","x-boot-id","x-wifi-generation"):
                if key in headers and headers[key] != self.headers.get(key):
                    raise ValueError("Camera part identity changed")
                headers[key] = self.headers.get(key,"")
            chunks, remaining = [], int(length)
            while remaining:
                chunk = self.response.read1(min(8192,remaining))
                if not chunk:
                    raise EOFError("Incomplete camera JPEG")
                chunks.append(chunk); remaining -= len(chunk)
            return headers,b"".join(chunks)
        if self.sock is not None:
            self.sock.settimeout(6)
        return self._bounded(read_frame,6)


class WifiCameraProxy(threading.Thread):
    def __init__(self, state, token, fetcher=None):
        super().__init__(name="robot-wifi-camera",daemon=True)
        self.state, self.token = state, token
        # Port 81 closes an idle HTTP socket after one second. A 1 Hz status
        # poll cannot safely reuse that socket: its next request races the
        # device's idle close and would unnecessarily interrupt a healthy 82
        # stream. Status therefore uses a bounded one-shot connection; JPEGs
        # still use the independent persistent stream (or legacy fetcher).
        self.fetcher = fetcher if fetcher is not None else fetch
        self.capture_fetcher = fetcher if fetcher is not None else PersistentFetcher()
        self.last_target, self.next_status, self.ready = None, 0.0, False
        self.stopping = threading.Event()

    def stop(self):
        self.stopping.set()
        if self.is_alive():
            self.join(timeout=8)

    def poll_once(self):
        target = self.state.wifi_camera_target()
        if target != self.last_target:
            if isinstance(self.fetcher,PersistentFetcher):
                self.fetcher.close()
            self.last_target, self.next_status, self.ready = target, 0.0, False
        if target is None:
            return False
        status_valid = self.ready
        try:
            if self.state.monotonic() >= self.next_status:
                status_valid = False
                headers, body = self.fetcher(target["ip"],"/api/status",self.token,64*1024,2)
                if headers.get("content-type","").split(";",1)[0] != "application/json":
                    raise ValueError("Expected camera JSON")
                payload = json.loads(body,object_pairs_hook=unique_object)
                self.ready = self.state.ingest_wifi_status(target,headers,payload)
                self.next_status = self.state.monotonic()+1
                status_valid = True
            if not self.ready or self.stopping.is_set():
                return False
            headers, jpeg = self.fetcher(target["ip"],"/capture.jpg",self.token,MAX_FRAME_BYTES,6)
            if headers.get("content-type","").split(";",1)[0] != "image/jpeg":
                raise ValueError("Expected camera JPEG")
            self.state.ingest_wifi_frame(target,headers,jpeg)
            return True
        except (OSError,ValueError,TypeError,KeyError,RecursionError,http.client.HTTPException) as error:
            # No request bodies, credentials, HTTP errors or arbitrary remote text
            # enter a log; the UI receives a fixed diagnostic code only.
            self.state.wifi_camera_failed(target,"FRAME_UNAVAILABLE" if status_valid else "STATUS_UNAVAILABLE",
                                          unreachable=not status_valid or isinstance(error,(OSError,http.client.HTTPException)))
            self.next_status, self.ready = 0.0, False
            return False

    def run(self):
        capture = threading.Thread(target=self._capture_loop,name="robot-camera-frames",daemon=True)
        capture.start()
        try:
            while not self.stopping.is_set():
                start = time.monotonic()
                self._poll_status()
                self.stopping.wait(max(0.05,0.5-(time.monotonic()-start)))
        finally:
            self.stopping.set()
            capture.join(timeout=7)
            if isinstance(self.fetcher,PersistentFetcher):
                self.fetcher.close()

    def _poll_status(self):
        target = self.state.wifi_camera_target()
        if target != self.last_target:
            if isinstance(self.fetcher,PersistentFetcher): self.fetcher.close()
            self.last_target = target
        if target is None: return
        try:
            headers,body = self.fetcher(target["ip"],"/api/status",self.token,64*1024,2)
            if headers.get("content-type","").split(";",1)[0] != "application/json":
                raise ValueError("Expected camera JSON")
            self.state.ingest_wifi_status(target,headers,json.loads(body,object_pairs_hook=unique_object))
        except (OSError,ValueError,TypeError,KeyError,RecursionError,http.client.HTTPException):
            self.state.wifi_camera_failed(target,"STATUS_UNAVAILABLE",unreachable=True)

    def _capture_loop(self):
        target, stream, fallback, blocked_seq = None, None, False, -1
        try:
            while not self.stopping.is_set():
                context = self.state.wifi_capture_context()
                if context is None:
                    if stream is not None: stream.close(); stream = None
                    self.stopping.wait(0.05); continue
                if context["target"] != target:
                    if stream is not None: stream.close(); stream = None
                    if isinstance(self.capture_fetcher,PersistentFetcher): self.capture_fetcher.close()
                    target, fallback, blocked_seq = context["target"],False,-1
                if context["status_seq"] <= blocked_seq or (context["stream_declared"] and not context["stream_ready"]):
                    if stream is not None: stream.close(); stream = None
                    self.stopping.wait(0.05); continue
                if context["stream_declared"]:
                    fallback = False
                start = time.monotonic()
                try:
                    if not fallback:
                        if stream is None:
                            try:
                                stream = FrameStream(target["ip"],self.token)
                            except (StreamUnavailable,ConnectionRefusedError):
                                if context["stream_declared"]: raise
                                fallback = True
                            if stream is not None:
                                self.state.accept_wifi_stream_identity(target,stream.headers)
                        if stream is not None:
                            headers,jpeg = stream.next_frame()
                    if fallback:
                        headers,jpeg = self.capture_fetcher(target["ip"],"/capture.jpg",self.token,MAX_FRAME_BYTES,6)
                        if headers.get("content-type","").split(";",1)[0] != "image/jpeg":
                            raise ValueError("Expected camera JPEG")
                    self.state.ingest_wifi_frame(target,headers,jpeg)
                except (OSError,ValueError,TypeError,KeyError,EOFError,RecursionError,http.client.HTTPException):
                    if stream is not None: stream.close(); stream = None
                    self.state.wifi_camera_failed(target,"STREAM_UNAVAILABLE" if not fallback else "FRAME_UNAVAILABLE",unreachable=False)
                    fresh = self.state.wifi_capture_context()
                    blocked_seq = fresh["status_seq"] if fresh else context["status_seq"]
                    self.stopping.wait(0.1)
                    continue
                # Always drain a continuous stream; limiting reads would queue
                # old JPEGs in TCP. The sensor controls its actual frame rate.
                if fallback:
                    self.stopping.wait(max(0,1/30-(time.monotonic()-start)))
        finally:
            if stream is not None: stream.close()
            if isinstance(self.capture_fetcher,PersistentFetcher): self.capture_fetcher.close()
