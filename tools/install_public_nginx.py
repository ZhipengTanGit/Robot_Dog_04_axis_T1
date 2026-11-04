"""Install a versioned static release and its private form-login service."""
from pathlib import Path, PurePosixPath
import fcntl
import hashlib
import importlib.util
import json
import os
import pwd
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import time
import urllib.request

BASE = Path('/var/www/robot-dog-studio')
BACKUP_BASE = Path('/var/backups/robot-dog-studio')
MAIN = Path('/etc/nginx/sites-available/robot-control')
SNIPPET = Path('/etc/nginx/snippets/robot-dog-studio.conf')
INCLUDE = '    include /etc/nginx/snippets/robot-dog-studio.conf;'
AUTH_APP = Path('/opt/robot-studio-auth/app.py')
AUTH_CONFIG = Path('/etc/robot-studio-auth.json')
AUTH_UNIT = Path('/etc/systemd/system/robot-studio-auth.service')
SERVICE = 'robot-studio-auth.service'
PORT = 4329
MARKER = '# robot-dog-studio managed static route'
UNIT_MARKER = '# robot-studio managed session authentication'


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('{} failed: {}'.format(args[0], result.stderr.strip()))
    return result.stdout


def systemctl_status(command):
    return subprocess.run(['systemctl', command, SERVICE], capture_output=True, text=True).stdout.strip()


def server_blocks(text):
    for match in re.finditer(r'(?m)^\s*server\s*\{', text):
        start = text.index('{', match.start())
        depth, quote, comment, escaped = 0, None, False, False
        for i in range(start, len(text)):
            c = text[i]
            if comment:
                if c == '\n': comment = False
                continue
            if quote:
                if escaped: escaped = False
                elif c == '\\': escaped = True
                elif c == quote: quote = None
                continue
            if c == '#': comment = True
            elif c in ('"', "'"): quote = c
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    yield start, i + 1, text[start:i + 1]
                    break


def nginx_snippet():
    proxy = '''    auth_basic off;
    auth_request off;
    proxy_pass http://127.0.0.1:4329/ENDPOINT;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header Origin $http_origin;
    proxy_set_header Cookie $http_cookie;
    proxy_connect_timeout 3s;
    proxy_read_timeout 10s;
    client_max_body_size 4k;
    add_header Cache-Control "no-store" always;
'''
    snippet = MARKER + '''
# Form login applies only to this app. Other routes keep their original auth.
location = /robot-studio {
    auth_basic off;
    return 308 /robot-studio/;
}
location = /__robot_studio_auth {
    internal;
    auth_basic off;
    auth_request off;
    proxy_pass http://127.0.0.1:4329/check;
    proxy_method GET;
    proxy_pass_request_body off;
    proxy_set_header Content-Length "";
    proxy_set_header Cookie $http_cookie;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_connect_timeout 3s;
    proxy_read_timeout 5s;
}
'''
    for endpoint in ('session', 'login', 'logout'):
        snippet += 'location = /robot-studio/auth/' + endpoint + ' {\n' + proxy.replace('ENDPOINT', endpoint) + '}\n'
    for asset in ('login.html', 'login.css', 'login.js'):
        snippet += '''location = /robot-studio/ASSET {
    auth_basic off;
    auth_request off;
    alias /var/www/robot-dog-studio/current/ASSET;
    charset utf-8;
    add_header Cache-Control "no-store" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header Referrer-Policy "same-origin" always;
    add_header Content-Security-Policy "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'" always;
}
'''.replace('ASSET', asset)
    snippet += '''location ^~ /robot-studio/ {
    auth_basic off;
    auth_request /__robot_studio_auth;
    error_page 401 = @robot_studio_login;
    alias /var/www/robot-dog-studio/current/;
    index index.html;
    autoindex off;
    charset utf-8;
    expires -1;
    add_header Cache-Control "private, no-store" always;
    gzip on;
    gzip_vary on;
    gzip_comp_level 5;
    gzip_types text/css application/javascript text/javascript application/json application/octet-stream;
}
location @robot_studio_login {
    auth_basic off;
    return 302 /robot-studio/login.html?next=$request_uri;
}
'''
    return snippet


def unit_text():
    return UNIT_MARKER + '''
[Unit]
Description=Robot Studio private session authentication
After=network.target

[Service]
Type=simple
User=www-data
Group=www-data
ExecStart=/usr/bin/python3 /opt/robot-studio-auth/app.py --config /etc/robot-studio-auth.json --port 4329
Restart=on-failure
RestartSec=2
UMask=0077
NoNewPrivileges=true
PrivateTmp=true
PrivateDevices=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
RestrictAddressFamilies=AF_INET AF_INET6
CapabilityBoundingSet=
LockPersonality=true
MemoryMax=128M
TasksMax=32

[Install]
WantedBy=multi-user.target
'''


def snapshot_files(paths, backup):
    result = {}
    for path in paths:
        if path.is_symlink():
            raise RuntimeError('Managed configuration must not be a symlink: ' + str(path))
        if path.exists():
            stat = path.stat()
            target = backup / path.name
            shutil.copyfile(path, target)
            target.chmod(0o600)
            result[str(path)] = {'name': path.name, 'mode': stat.st_mode & 0o777, 'uid': stat.st_uid, 'gid': stat.st_gid}
        else:
            result[str(path)] = None
    (backup / 'restore-files.json').write_text(json.dumps(result, indent=2))
    (backup / 'restore-files.json').chmod(0o600)
    return result


def restore_files(snapshot, backup):
    for name, record in snapshot.items():
        path = Path(name)
        if record is None:
            if path.exists(): path.unlink()
        else:
            shutil.copyfile(backup / record['name'], path)
            path.chmod(record['mode'])
            os.chown(path, record['uid'], record['gid'])


def install(request_path):
    request = json.loads(request_path.read_text())
    release_id = request['release']
    if not re.fullmatch(r'[0-9]{8}T[0-9]{6}Z-[a-f0-9]{8,16}', release_id):
        raise ValueError('Invalid release identity')
    if request.get('accessMode') != 'form-session' or request.get('publicAccess'):
        raise ValueError('This installer requires private form-session authentication')
    archive = request_path.parent / 'site.tar.gz'
    auth_source = request_path.parent / 'auth.py'
    if hashlib.sha256(archive.read_bytes()).hexdigest() != request['archiveSha256']:
        raise ValueError('Upload checksum mismatch')
    if hashlib.sha256(auth_source.read_bytes()).hexdigest() != request['authSha256']:
        raise ValueError('Authentication upload checksum mismatch')
    run('nginx', '-t')
    version = subprocess.run(['nginx', '-V'], capture_output=True, text=True)
    if '--with-http_auth_request_module' not in version.stderr + version.stdout:
        raise RuntimeError('Nginx auth_request module is required')
    main_before = MAIN.read_text()
    if re.search(r'location\s+[^\n{]*(?:robot-studio|__robot_studio_auth|robot_studio_login)', main_before):
        raise RuntimeError('An unmanaged robot-studio route already exists')
    snippet_before = SNIPPET.read_bytes() if SNIPPET.exists() else None
    if snippet_before and MARKER.encode() not in snippet_before:
        raise RuntimeError('Snippet is not owned by this deployment')
    if AUTH_UNIT.exists() and UNIT_MARKER not in AUTH_UNIT.read_text():
        raise RuntimeError('Authentication service is not owned by this deployment')
    active_before = systemctl_status('is-active') == 'active'
    enabled_before = systemctl_status('is-enabled') == 'enabled'
    if not AUTH_UNIT.exists():
        if active_before:
            raise RuntimeError('An unmanaged authentication service already exists')
        try:
            with socket.create_connection(('127.0.0.1', PORT), timeout=1):
                raise RuntimeError('Authentication port is already occupied')
        except (ConnectionRefusedError, socket.timeout):
            pass
    https = [(a, b) for a, b, block in server_blocks(main_before)
             if re.search(r'listen\s+(?:\[::\]:)?443\b', block)
             and re.search(r'server_name\s+110\.42\.234\.162\s*;', block)]
    if len(https) != 1:
        raise RuntimeError('Expected one existing IP HTTPS server')
    if INCLUDE in main_before:
        if main_before.count(INCLUDE) != 1:
            raise RuntimeError('Duplicate managed include')
        main_after = main_before
    else:
        start = https[0][0] + 1
        main_after = main_before[:start] + '\n' + INCLUDE + main_before[start:]
    current = BASE / 'current'
    if current.exists() and not current.is_symlink():
        raise RuntimeError('Current path is not a release symlink')
    previous = os.readlink(current) if current.is_symlink() else None
    if previous:
        try:
            (current.parent / previous).resolve().relative_to((BASE / 'releases').resolve())
        except ValueError:
            raise RuntimeError('Previous release is outside the managed directory')
    release = BASE / 'releases' / release_id
    release.mkdir(parents=True, mode=0o755, exist_ok=False)
    with tarfile.open(archive, 'r:gz') as bundle:
        seen = set()
        for member in bundle.getmembers():
            path = PurePosixPath(member.name)
            if (not member.isfile() or path.is_absolute() or '..' in path.parts or any(p.startswith('.') for p in path.parts)
                    or member.name not in request['files'] or member.size > 100 * 1024 * 1024):
                raise ValueError('Unsafe or unexpected archive entry')
            if member.name in seen: raise ValueError('Duplicate archive entry')
            seen.add(member.name)
            data = bundle.extractfile(member).read()
            expected = request['files'][member.name]
            if len(data) != expected['size'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
                raise ValueError('Release file checksum mismatch')
            target = release / str(path)
            target.parent.mkdir(parents=True, mode=0o755, exist_ok=True)
            target.write_bytes(data)
            target.chmod(0o644)
        if seen != set(request['files']): raise ValueError('Release is incomplete')
    if not all((release / name).is_file() for name in ('index.html', 'login.html', 'login.css', 'login.js')):
        raise ValueError('Login page is missing from release')
    backup = BACKUP_BASE / release_id
    backup.mkdir(parents=True, mode=0o700)
    snapshot = snapshot_files((MAIN, SNIPPET, AUTH_APP, AUTH_CONFIG, AUTH_UNIT), backup)
    (backup / 'deployment.json').write_text(json.dumps(request, indent=2))
    (backup / 'deployment.json').chmod(0o600)
    (backup / 'service-before.json').write_text(json.dumps({'active': active_before, 'enabled': enabled_before, 'previousRelease': previous}))
    (backup / 'service-before.json').chmod(0o600)
    staged_link = BASE / ('.current-' + release_id)
    try:
        AUTH_APP.parent.mkdir(parents=True, mode=0o755, exist_ok=True)
        shutil.copyfile(auth_source, AUTH_APP)
        AUTH_APP.chmod(0o644)
        spec = importlib.util.spec_from_file_location('robot_studio_auth', str(AUTH_APP))
        auth = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(auth)
        if AUTH_CONFIG.exists():
            auth.validate_config(json.loads(AUTH_CONFIG.read_text()))
        else:
            # User explicitly selected admin/admin. Only its salted hash is stored.
            AUTH_CONFIG.write_text(json.dumps(auth.initial_config(), indent=2))
        AUTH_CONFIG.chmod(0o600)
        owner = pwd.getpwnam('www-data')
        os.chown(AUTH_CONFIG, owner.pw_uid, owner.pw_gid)
        AUTH_UNIT.write_text(unit_text())
        AUTH_UNIT.chmod(0o644)
        run('systemctl', 'daemon-reload')
        run('systemctl', 'enable', SERVICE)
        run('systemctl', 'restart', SERVICE)
        healthy = False
        for _ in range(20):
            try:
                with urllib.request.urlopen('http://127.0.0.1:4329/health', timeout=1) as response:
                    healthy = response.status == 200 and json.load(response).get('ok') is True
                if healthy: break
            except (OSError, ValueError):
                time.sleep(0.25)
        if not healthy or systemctl_status('is-active') != 'active':
            raise RuntimeError('Authentication service did not start')
        SNIPPET.write_text(nginx_snippet())
        SNIPPET.chmod(0o644)
        MAIN.write_text(main_after)
        run('nginx', '-t')
        os.symlink(str(release), staged_link)
        os.replace(staged_link, current)
        run('systemctl', 'reload', 'nginx')
        if run('systemctl', 'is-active', 'nginx').strip() != 'active':
            raise RuntimeError('Nginx did not remain active')
    except Exception:
        subprocess.run(['systemctl', 'stop', SERVICE], capture_output=True)
        if not enabled_before:
            subprocess.run(['systemctl', 'disable', SERVICE], capture_output=True)
        restore_files(snapshot, backup)
        if previous:
            rollback = BASE / ('.rollback-' + release_id)
            os.symlink(previous, rollback)
            os.replace(rollback, current)
        elif current.is_symlink():
            current.unlink()
        run('systemctl', 'daemon-reload')
        if enabled_before: run('systemctl', 'enable', SERVICE)
        if active_before: run('systemctl', 'restart', SERVICE)
        run('nginx', '-t')
        run('systemctl', 'reload', 'nginx')
        raise
    finally:
        if staged_link.is_symlink(): staged_link.unlink()
    return {'release': release_id, 'directory': str(release), 'url': 'https://110.42.234.162/robot-studio/',
            'accessMode': 'form-session', 'publicAccess': False, 'files': len(request['files']),
            'authService': systemctl_status('is-active'), 'nginx': 'active', 'backup': str(backup), 'previousRelease': previous}


if __name__ == '__main__':
    with open('/var/lock/robot-dog-studio-deploy.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        print(json.dumps(install(Path(sys.argv[1])), ensure_ascii=True))
