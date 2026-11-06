"""Exercise the deployed form login and private asset delivery over HTTPS.

Session values and credentials stay in memory and are never included in output.
The user explicitly selected the workbench-only admin account.
"""
from pathlib import Path
import hashlib
import http.cookiejar
import importlib.util
import json
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ORIGIN = 'https://110.42.234.162'
PREFIX = '/robot-studio/'


def firmware_validation():
    spec = importlib.util.spec_from_file_location('firmware_catalog_verifier', Path(__file__).with_name('verify-firmware-catalog.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.verify(ROOT / 'dist')


def hardware_resources():
    paths = {'hardware.html', 'hardware.css', 'hardware.js', 'serial-session.js',
             'hardware-flash.js', 'hardware-flash-stm32.js', 'firmware-hash.js'}
    for folder in ('firmware', 'vendor/esptool', 'vendor/webstlink'):
        paths.update(path.relative_to(ROOT / 'dist').as_posix() for path in (ROOT / 'dist' / folder).rglob('*') if path.is_file())
    return sorted(paths)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def verify():
    # Fail locally before making requests if a release is inconsistent. This
    # reads only dist/ and never loads server access or other private config.
    firmware = firmware_validation()
    resources = hardware_resources()
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(NoRedirect(), urllib.request.HTTPCookieProcessor(jar))
    checks = []

    def request(path, body=None, csrf=None, extra=None):
        headers = {'Accept-Encoding': 'identity'}
        if body is not None:
            headers.update({'Origin': ORIGIN, 'Content-Type': 'application/json'})
        if csrf:
            headers['X-CSRF-Token'] = csrf
        headers.update(extra or {})
        req = urllib.request.Request(ORIGIN + path, headers=headers,
                                     data=None if body is None else json.dumps(body).encode())
        try:
            response = opener.open(req, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, response.headers, response.read()

    def check(condition, description):
        if not condition:
            raise AssertionError(description)
        checks.append(description)

    for path in ('', 'index.html', 'app.js', 'viewer.js', 'assets/model.json', 'assets/model.bin'):
        status, headers, _ = request(PREFIX + path)
        location = urllib.parse.urljoin(ORIGIN, headers.get('Location', ''))
        check(status == 302 and location.startswith(ORIGIN + PREFIX + 'login.html?next='),
              'Anonymous access denied: ' + (path or 'index'))
    anonymous_extra = {'hardware.html', 'hardware.js', 'serial-session.js', 'hardware-flash.js',
                       'hardware-flash-stm32.js', 'firmware-hash.js', 'vendor/esptool/esptool.js',
                       'vendor/webstlink/webstlink.js', 'vendor/webstlink/lib/stm32fp.js', 'firmware/catalog.json'}
    anonymous_extra.update(firmware['declaredFiles'])
    for path in sorted(anonymous_extra):
        status, headers, _ = request(PREFIX + path)
        location = urllib.parse.urljoin(ORIGIN, headers.get('Location', ''))
        check(status == 302 and location.startswith(ORIGIN + PREFIX + 'login.html?next='),
              'Anonymous hardware/firmware access denied: ' + path)
    for path in ('login.html', 'login.css', 'login.js'):
        status, headers, data = request(PREFIX + path)
        check(status == 200 and hashlib.sha256(data).digest() == hashlib.sha256((ROOT / 'dist' / path).read_bytes()).digest(),
              'Public login asset matches release: ' + path)
    status, _, data = request(PREFIX + 'auth/session')
    anonymous = json.loads(data)
    check(status == 401 and anonymous.get('authenticated') is False and anonymous.get('csrfToken'), 'Anonymous CSRF initialization')
    status, _, _ = request(PREFIX + 'auth/login', {'username': 'admin', 'password': 'incorrect-test-password'}, anonymous['csrfToken'])
    check(status == 401, 'Incorrect password rejected')
    status, _, data = request(PREFIX + 'auth/login', {'username': 'admin', 'password': 'admin', 'next': PREFIX + '#color'}, anonymous['csrfToken'])
    check(status == 200 and json.loads(data).get('redirectTo') == PREFIX + '#color', 'Admin login succeeds and returns workbench')
    session_cookie = next(cookie for cookie in jar if cookie.name == 'robot_studio_session')
    check(session_cookie.secure and session_cookie.path == PREFIX and session_cookie.has_nonstandard_attr('HttpOnly')
          and session_cookie.get_nonstandard_attr('SameSite') == 'Strict', 'Scoped Secure HttpOnly SameSite session cookie')
    token = session_cookie.value
    status, _, data = request(PREFIX + 'auth/session')
    session = json.loads(data)
    check(status == 200 and session.get('authenticated') is True and session.get('username') == 'admin', 'Authenticated session contract')
    for path in ('index.html', 'app.js', 'viewer.js', 'color-selection.js', 'color-selection.css', 'dance-format.js'):
        status, headers, data = request(PREFIX + path)
        check(status == 200 and hashlib.sha256(data).digest() == hashlib.sha256((ROOT / 'dist' / path).read_bytes()).digest(),
              'Authenticated asset matches release: ' + path)
        check('no-store' in ','.join(headers.get_all('Cache-Control', [])), 'Private asset is not cached: ' + path)
    for path in resources:
        status, headers, data = request(PREFIX + path)
        local = (ROOT / 'dist' / path).read_bytes()
        check(status == 200 and len(data) == len(local) and hashlib.sha256(data).digest() == hashlib.sha256(local).digest(),
              'Authenticated hardware/firmware bytes match release: ' + path)
        check('no-store' in ','.join(headers.get_all('Cache-Control', [])), 'Hardware/firmware asset is not cached: ' + path)
        if path in firmware['declaredFiles']:
            declared = firmware['declaredFiles'][path]
            check(len(data) == declared['size'] and hashlib.sha256(data).hexdigest() == declared['sha256'],
                  'Downloaded firmware matches catalog size and SHA: ' + path)
    status, _, data = request(PREFIX + 'assets/model.bin', extra={'Range': 'bytes=0-63'})
    check(status == 206 and data == (ROOT / 'dist/assets/model.bin').read_bytes()[:64], 'Authenticated model range request works')
    status, _, _ = request(PREFIX + 'auth/logout', {}, session['csrfToken'])
    check(status == 200, 'Logout succeeds')
    status, _, _ = request(PREFIX + 'app.js', extra={'Cookie': 'robot_studio_session=' + token})
    check(status == 302, 'Logged out session cannot access assets again')
    status, _, _ = request('/')
    check(status == 401, 'Original main-site login remains enabled')
    status, _, _ = request('/learn/')
    check(status == 200, 'Existing learn site remains available')
    report = {'passed': len(checks), 'url': ORIGIN + PREFIX, 'originalChecksRetained': 31,
              'localFirmwareChecks': firmware['passed'], 'hardwareResources': len(resources), 'checks': checks}
    output = ROOT / 'review-output/public-deploy/https-validation.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    return report


if __name__ == '__main__':
    print(json.dumps(verify(), indent=2))
