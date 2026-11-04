"""Loopback-only session authentication for the robot-studio Nginx route.

Only the generated /etc configuration contains password hashes or signing keys.
The HTTP endpoints are deliberately independent of the existing main website.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from http.cookies import SimpleCookie, CookieError
from pathlib import Path
import argparse
import hashlib
import hmac
import ipaddress
import json
import secrets
import threading
import time
from urllib.parse import unquote, urlsplit

PREFIX = '/robot-studio/'
SESSION_COOKIE = 'robot_studio_session'
CSRF_COOKIE = 'robot_studio_csrf'
PASSWORD_ITERATIONS = 600000
SESSION_SECONDS = 8 * 60 * 60
IDLE_SECONDS = 60 * 60
CSRF_SECONDS = 10 * 60
MAX_BODY = 4096


def initial_config(username='admin', password='admin'):
    salt = secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(salt), PASSWORD_ITERATIONS).hex()
    return {'version': 1, 'username': username,
            'password': {'algorithm': 'pbkdf2-sha256', 'iterations': PASSWORD_ITERATIONS, 'salt': salt, 'digest': digest},
            'csrfKey': secrets.token_hex(32), 'origin': 'https://110.42.234.162'}


def validate_config(config):
    password = config['password']
    if (config.get('version') != 1 or not isinstance(config['username'], str)
            or password['algorithm'] != 'pbkdf2-sha256'
            or not isinstance(password['iterations'], int) or not 300000 <= password['iterations'] <= 2000000
            or len(bytes.fromhex(password['salt'])) != 16 or len(bytes.fromhex(password['digest'])) != 32
            or len(bytes.fromhex(config['csrfKey'])) != 32):
        raise ValueError('Invalid private authentication configuration')
    origin = urlsplit(config['origin'])
    if origin.scheme != 'https' or not origin.netloc or origin.path or origin.query or origin.fragment:
        raise ValueError('Authentication origin must be an HTTPS origin')
    return config


def safe_next(value):
    """Accept only normalized app URLs, never auth routes or external redirects."""
    if not isinstance(value, str) or len(value) > 2048:
        return PREFIX
    decoded = value
    for _ in range(4):
        newer = unquote(decoded)
        if newer == decoded:
            break
        decoded = newer
    if any(ord(c) < 32 or ord(c) == 127 for c in decoded) or '\\' in decoded or '%' in decoded:
        return PREFIX
    try:
        parsed = urlsplit(decoded)
    except ValueError:
        return PREFIX
    if (parsed.scheme or parsed.netloc or not parsed.path.startswith(PREFIX)
            or any(part in {'.', '..', ''} for part in parsed.path[len(PREFIX):].split('/')[:-1])
            or any(part in {'.', '..'} for part in parsed.path.split('/'))
            or parsed.path.startswith(PREFIX + 'auth/')
            or parsed.path in {PREFIX + 'login.html', PREFIX + 'login.js', PREFIX + 'login.css'}):
        return PREFIX
    return value


def same_token(left, right):
    return hmac.compare_digest(left.encode('utf-8'), right.encode('utf-8'))


class AuthState:
    def __init__(self, config, clock=time.time):
        self.config = validate_config(config)
        self.clock = clock
        self.lock = threading.Lock()
        self.sessions = {}
        self.failures = {}

    def _prune(self):
        now = self.clock()
        self.sessions = {key: session for key, session in self.sessions.items()
                         if now < session['expires'] and now - session['seen'] < IDLE_SECONDS}
        self.failures = {key: [stamp for stamp in stamps if now - stamp < 600]
                         for key, stamps in self.failures.items() if any(now - stamp < 600 for stamp in stamps)}

    def session(self, token, touch=False):
        if not token or len(token) > 128:
            return None
        key = hashlib.sha256(token.encode()).hexdigest()
        with self.lock:
            self._prune()
            session = self.sessions.get(key)
            if session and touch:
                session['seen'] = self.clock()
            return dict(session) if session else None

    def create_session(self, old_token=None):
        token = secrets.token_urlsafe(32)
        now = self.clock()
        session = {'username': self.config['username'], 'csrf': secrets.token_urlsafe(32),
                   'expires': now + SESSION_SECONDS, 'seen': now}
        with self.lock:
            self._prune()
            if old_token:
                self.sessions.pop(hashlib.sha256(old_token.encode()).hexdigest(), None)
            # Bound memory even if an authorized client creates excessive sessions.
            if len(self.sessions) >= 1000:
                oldest = min(self.sessions, key=lambda key: self.sessions[key]['seen'])
                del self.sessions[oldest]
            self.sessions[hashlib.sha256(token.encode()).hexdigest()] = session
        return token, dict(session)

    def logout(self, token):
        with self.lock:
            self.sessions.pop(hashlib.sha256(token.encode()).hexdigest(), None)

    def csrf_challenge(self):
        value = str(int(self.clock())) + '.' + secrets.token_urlsafe(24)
        signature = hmac.new(bytes.fromhex(self.config['csrfKey']), value.encode(), hashlib.sha256).hexdigest()
        return value + '.' + signature

    def check_challenge(self, challenge):
        try:
            value, signature = challenge.rsplit('.', 1)
            stamp = int(value.split('.', 1)[0])
            expected = hmac.new(bytes.fromhex(self.config['csrfKey']), value.encode(), hashlib.sha256).hexdigest()
            return 0 <= self.clock() - stamp <= CSRF_SECONDS and same_token(signature, expected)
        except (ValueError, AttributeError):
            return False

    def authenticate(self, username, password, ip):
        with self.lock:
            self._prune()
            if len(self.failures.get(ip, [])) >= 5:
                retry = max(1, int(601 - (self.clock() - self.failures[ip][0])))
                return False, retry
            # Reserve an attempt before expensive hashing so parallel requests
            # cannot all pass the same five-attempt check.
            if ip not in self.failures and len(self.failures) >= 10000:
                del self.failures[next(iter(self.failures))]
            self.failures.setdefault(ip, []).append(self.clock())
        entry = self.config['password']
        actual = hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(entry['salt']), entry['iterations']).hex()
        accepted = (hmac.compare_digest(actual, entry['digest'])
                    and hmac.compare_digest(username.encode(), self.config['username'].encode()))
        with self.lock:
            if accepted:
                self.failures.pop(ip, None)
        return accepted, 0


def handler_for(state):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'RobotStudioAuth'
        sys_version = ''

        def log_message(self, *args):
            # Never log passwords, cookies, request bodies or URL query strings.
            pass

        def cookie(self, name):
            try:
                cookies = SimpleCookie(self.headers.get('Cookie', ''))
                return cookies[name].value if name in cookies else ''
            except CookieError:
                return ''

        def send_json(self, status, value, cookies=(), retry=None):
            data = json.dumps(value, ensure_ascii=False).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            for cookie in cookies:
                self.send_header('Set-Cookie', cookie)
            if retry:
                self.send_header('Retry-After', str(retry))
            self.end_headers()
            self.wfile.write(data)

        def set_cookie(self, name, value, seconds):
            return '{}={}; Path={}; Max-Age={}; Secure; HttpOnly; SameSite=Strict'.format(name, value, PREFIX, seconds)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == '/health':
                self.send_json(200, {'ok': True})
                return
            session = state.session(self.cookie(SESSION_COOKIE), touch=True)
            if path == '/check':
                self.send_response(204 if session else 401)
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Content-Length', '0')
                self.end_headers()
            elif path == '/session':
                if session:
                    self.send_json(200, {'authenticated': True, 'username': session['username'], 'csrfToken': session['csrf']})
                else:
                    challenge = state.csrf_challenge()
                    self.send_json(401, {'authenticated': False, 'csrfToken': challenge},
                                   [self.set_cookie(CSRF_COOKIE, challenge, CSRF_SECONDS)])
            else:
                self.send_json(404, {'error': '接口不存在'})

        def do_POST(self):
            path = urlsplit(self.path).path
            if path not in {'/login', '/logout'}:
                self.send_json(404, {'error': '接口不存在'})
                return
            if self.headers.get('Origin') != state.config['origin']:
                self.send_json(403, {'error': '请从本站登录页面重试'})
                return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= MAX_BODY or self.headers.get('Transfer-Encoding'):
                    raise ValueError()
                if self.headers.get('Content-Type', '').split(';')[0].strip().lower() != 'application/json':
                    raise ValueError()
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError()
            except (ValueError, UnicodeError):
                self.send_json(400, {'error': '请求格式不正确'})
                return
            token = self.cookie(SESSION_COOKIE)
            session = state.session(token)
            csrf = self.headers.get('X-CSRF-Token', '')
            if path == '/logout':
                if not session:
                    self.send_json(401, {'error': '登录已过期，请重新登录'})
                elif not same_token(csrf, session['csrf']):
                    self.send_json(403, {'error': '安全校验失效，请刷新后重试'})
                else:
                    state.logout(token)
                    self.send_json(200, {'ok': True, 'redirectTo': PREFIX + 'login.html'},
                                   [self.set_cookie(SESSION_COOKIE, '', 0), self.set_cookie(CSRF_COOKIE, '', 0)])
                return
            challenge = self.cookie(CSRF_COOKIE)
            if not csrf or not same_token(csrf, challenge) or not state.check_challenge(challenge):
                self.send_json(403, {'error': '登录页面已过期，请刷新后重试'})
                return
            username, password = body.get('username'), body.get('password')
            if (not isinstance(username, str) or not isinstance(password, str)
                    or not 1 <= len(username) <= 128 or not 1 <= len(password) <= 1024):
                self.send_json(400, {'error': '请输入账号和密码'})
                return
            try:
                username.encode('utf-8')
                password.encode('utf-8')
            except UnicodeError:
                self.send_json(400, {'error': '账号或密码格式不正确'})
                return
            try:
                ip = str(ipaddress.ip_address(self.headers.get('X-Real-IP', self.client_address[0])))
            except ValueError:
                ip = self.client_address[0]
            accepted, retry = state.authenticate(username, password, ip)
            if retry:
                self.send_json(429, {'error': '尝试次数过多，请稍后再试', 'retryAfter': retry}, retry=retry)
            elif not accepted:
                self.send_json(401, {'error': '账号或密码不正确'})
            else:
                new_token, new_session = state.create_session(token)
                self.send_json(200, {'ok': True, 'username': new_session['username'], 'redirectTo': safe_next(body.get('next'))},
                               [self.set_cookie(SESSION_COOKIE, new_token, SESSION_SECONDS), self.set_cookie(CSRF_COOKIE, '', 0)])
    return Handler


def make_server(config, port=4329):
    server = ThreadingHTTPServer(('127.0.0.1', port), handler_for(AuthState(config)))
    server.daemon_threads = True
    return server


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--port', type=int, default=4329)
    args = parser.parse_args()
    config = validate_config(json.loads(args.config.read_text()))
    make_server(config, args.port).serve_forever(poll_interval=0.5)
