"""Local HTTP and pure-function checks; never accesses the public server."""
from pathlib import Path
import ast
import http.client
import importlib.util
import json
import re
import threading

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('robot_studio_auth', ROOT / 'tools/robot_studio_auth.py')
auth = importlib.util.module_from_spec(spec)
spec.loader.exec_module(auth)
checks = []


def check(name, condition):
    if not condition:
        raise AssertionError(name)
    checks.append(name)


config = auth.initial_config()
check('Private config contains salted hash and no plaintext password', config['password']['digest'] != 'admin' and config['password']['salt'] and 'admin' not in json.dumps(config['password']))
for value in ('https://evil.example/', '//evil.example/', '/learn/', '/robot-studio/../learn/', '/robot-studio/%2e%2e/learn/',
              '/robot-studio/%252e%252e/learn/', '/robot-studio/\\evil', '/robot-studio/auth/logout', '/robot-studio/login.html',
              '/robot-studio/%0d%0aLocation:https://evil.example', None, '/robot-studio//evil', '/robot-studio/./'):
    check('Unsafe redirect rejected: ' + str(value), auth.safe_next(value) == auth.PREFIX)
for value in ('/robot-studio/#assembly', '/robot-studio/index.html?x=1#choreography', '/robot-studio/assets/model.bin'):
    check('App redirect retained: ' + value, auth.safe_next(value) == value)

now = [1000000.0]
state = auth.AuthState(config, clock=lambda: now[0])
token, session = state.create_session()
check('Random session token and independent CSRF', len(token) >= 40 and token != session['csrf'])
check('Session memory stores token hash only', token not in state.sessions)
now[0] += auth.IDLE_SECONDS
check('Idle session expires', state.session(token) is None)
token, session = state.create_session()
for _ in range(8):
    now[0] += 3500
    state.session(token, touch=True)
now[0] += 801
check('Absolute expiry applies despite activity', state.session(token) is None)
challenge = state.csrf_challenge()
check('Signed CSRF challenge accepted', state.check_challenge(challenge))
check('Tampered CSRF challenge rejected', not state.check_challenge(challenge[:-1] + ('0' if challenge[-1] != '0' else '1')))
now[0] += auth.CSRF_SECONDS + 1
check('CSRF challenge expires', not state.check_challenge(challenge))
check('Non ASCII token does not crash comparison', not auth.same_token('é', 'abc') and not state.check_challenge('100.abc.é'))

server = auth.make_server(config, port=0)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
port = server.server_address[1]


def request(method, path, body=None, cookie='', csrf='', origin=config['origin'], ip='192.0.2.4', raw=False):
    client = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    headers = {'Origin': origin, 'X-Real-IP': ip}
    if cookie: headers['Cookie'] = cookie
    if csrf: headers['X-CSRF-Token'] = csrf
    if body is not None:
        headers['Content-Type'] = 'application/json'
        body = body if raw else json.dumps(body)
    client.request(method, path, body, headers)
    response = client.getresponse()
    data = response.read()
    result = (response.status, response.getheaders(), json.loads(data) if data else None)
    client.close()
    return result


try:
    status, headers, data = request('GET', '/session')
    check('Anonymous session is 401 JSON with login CSRF', status == 401 and not data['authenticated'] and data['csrfToken'])
    csrf = data['csrfToken']
    challenge_cookie = next(v for k, v in headers if k.lower() == 'set-cookie')
    cookie = challenge_cookie.split(';')[0]
    check('Cookie has Secure HttpOnly Strict scoped attributes', all(item in challenge_cookie for item in ('Secure', 'HttpOnly', 'SameSite=Strict', 'Path=/robot-studio/')))
    check('Anonymous check denies all assets', request('GET', '/check')[0] == 401)
    body = {'username': 'admin', 'password': 'admin', 'next': '/robot-studio/#choreography'}
    check('Login requires CSRF', request('POST', '/login', body)[0] == 403)
    check('Cross origin login denied', request('POST', '/login', body, cookie, csrf, 'https://evil.example')[0] == 403)
    check('Missing Origin denied', request('POST', '/login', body, cookie, csrf, '')[0] == 403)
    check('Malformed JSON denied', request('POST', '/login', '{bad', cookie, csrf, raw=True)[0] == 400)
    check('Oversized body denied', request('POST', '/login', {'x': 'a' * 5000}, cookie, csrf)[0] == 400)
    check('Invalid Unicode denied cleanly', request('POST', '/login', {'username': 'admin', 'password': '\ud800'}, cookie, csrf)[0] == 400)
    wrong = dict(body, password='incorrect')
    check('Wrong password denies login', request('POST', '/login', wrong, cookie, csrf)[0] == 401)
    status, headers, data = request('POST', '/login', body, cookie, csrf)
    check('Correct credentials grant app redirect', status == 200 and data['redirectTo'] == '/robot-studio/#choreography')
    session_cookie = next(v for k, v in headers if k.lower() == 'set-cookie' and v.startswith(auth.SESSION_COOKIE + '=')).split(';')[0]
    check('Authenticated check allows protected resources', request('GET', '/check', cookie=session_cookie)[0] == 204)
    status, headers, data = request('GET', '/session', cookie=session_cookie)
    check('Authenticated session contract has admin and CSRF', status == 200 and data['authenticated'] and data['username'] == 'admin' and data['csrfToken'])
    csrf_session = data['csrfToken']
    check('Logout requires session CSRF', request('POST', '/logout', {}, session_cookie)[0] == 403)
    check('Cross origin logout denied', request('POST', '/logout', {}, session_cookie, csrf_session, 'https://evil.example')[0] == 403)
    check('Correct logout invalidates session immediately', request('POST', '/logout', {}, session_cookie, csrf_session)[0] == 200 and request('GET', '/check', cookie=session_cookie)[0] == 401)
    check('Logged out cookie cannot be reused', request('POST', '/logout', {}, session_cookie, csrf_session)[0] == 401)
    for _ in range(5):
        result = request('POST', '/login', wrong, cookie, csrf, ip='192.0.2.55')
        check('Failure counted toward rate limit', result[0] == 401)
    status, headers, data = request('POST', '/login', body, cookie, csrf, ip='192.0.2.55')
    check('Repeated attempts rate limited with Retry After', status == 429 and data['retryAfter'] > 0 and any(k.lower() == 'retry-after' for k, v in headers))
    check('Other client is not locked out', request('POST', '/login', body, cookie, csrf, ip='192.0.2.99')[0] == 200)
    check('Unknown endpoint is JSON 404', request('GET', '/not-real')[0] == 404)
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=3)

# Extract pure Nginx text functions without importing Linux-only installer modules.
tree = ast.parse((ROOT / 'tools/install_public_nginx.py').read_text(encoding='utf-8'))
pure = ast.Module(body=[node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in {'nginx_snippet', 'unit_text', 'server_blocks'}], type_ignores=[])
namespace = {'re': re, 'MARKER': '# robot-dog-studio managed static route', 'UNIT_MARKER': '# robot-studio managed session authentication'}
exec(compile(pure, '<installer-pure>', 'exec'), namespace)
snippet = namespace['nginx_snippet']()
protected = re.search(r'location \^~ /robot-studio/ \{(.*?)\n\}', snippet, re.S).group(1)
check('All app static resources including model require auth_request', 'auth_request /__robot_studio_auth;' in protected and 'alias /var/www/robot-dog-studio/current/;' in protected)
check('Anonymous redirect does not use a permanent status', 'return 302 /robot-studio/login.html?next=$request_uri;' in snippet)
check('Auth subrequest cannot be requested externally', 'location = /__robot_studio_auth {\n    internal;' in snippet)
check('Only intended exact login/API endpoints are public', len(re.findall(r'location = /robot-studio/(?:login\.(?:html|css|js)|auth/(?:login|logout|session)) \{', snippet)) == 6)
check('Login has restrictive CSP and no-store', "frame-ancestors 'none'" in snippet and 'Cache-Control "no-store"' in snippet)
check('Auth service drops privileges and restricts filesystem', all(v in namespace['unit_text']() for v in ('User=www-data', 'ProtectSystem=strict', 'NoNewPrivileges=true')))
sample = 'server { listen 80; server_name 110.42.234.162; }\nserver { listen 443 ssl; server_name 110.42.234.162; auth_basic "Robot Control"; location / { proxy_pass http://localhost:8000; } }'
blocks = list(namespace['server_blocks'](sample))
check('Existing original server blocks parsed without changing content', len(blocks) == 2 and 'auth_basic "Robot Control";' in blocks[1][2])
print(json.dumps({'passed': len(checks), 'checks': checks}, ensure_ascii=True, indent=2))
