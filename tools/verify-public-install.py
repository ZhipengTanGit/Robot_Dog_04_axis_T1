"""Exercise release install/rollback in a temporary directory, with no real SSH,
systemd, Nginx or privileged filesystem changes. Runs on Windows and Linux.
"""
from pathlib import Path
from unittest.mock import patch
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import types

ROOT = Path(__file__).resolve().parents[1]
fcntl_stub = types.ModuleType('fcntl')
pwd_stub = types.ModuleType('pwd')
pwd_stub.getpwnam = lambda name: types.SimpleNamespace(pw_uid=0, pw_gid=0)
with patch.dict(sys.modules, {'fcntl': fcntl_stub, 'pwd': pwd_stub}):
    spec = importlib.util.spec_from_file_location('installer', ROOT / 'tools/install_public_nginx.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)

checks = []


def check(name, condition):
    if not condition:
        raise AssertionError(name)
    checks.append(name)


def case(failure=None, prior_service=False):
    with tempfile.TemporaryDirectory(prefix='robot-studio-install-') as temp:
        root = Path(temp)
        installer.BASE = root / 'www'
        installer.BACKUP_BASE = root / 'backups'
        installer.MAIN = root / 'etc/robot-control'
        installer.SNIPPET = root / 'etc/robot-dog-studio.conf'
        installer.AUTH_APP = root / 'opt/app.py'
        installer.AUTH_CONFIG = root / 'etc/auth.json'
        installer.AUTH_UNIT = root / 'etc/auth.service'
        installer.INCLUDE = '    include ' + str(installer.SNIPPET) + ';'
        installer.MAIN.parent.mkdir()
        main_original = ('server { listen 80; server_name 110.42.234.162; return 301 https://$host$request_uri; }\n'
                         'server { listen 443 ssl; server_name 110.42.234.162;\n' + installer.INCLUDE + '\n'
                         ' auth_basic "Robot Control"; auth_basic_user_file /etc/nginx/.htpasswd-robot;\n'
                         ' location / { proxy_pass http://127.0.0.1:8000; }\n'
                         ' location /learn/ { auth_basic off; proxy_pass http://127.0.0.1:4318; }\n}\n')
        installer.MAIN.write_text(main_original)
        snippet_original = installer.MARKER + '\nlocation ^~ /robot-studio/ { alias /old/; }\n'
        installer.SNIPPET.write_text(snippet_original)
        original_config = None
        if prior_service:
            installer.AUTH_APP.parent.mkdir()
            installer.AUTH_APP.write_text('# previous managed auth code')
            installer.AUTH_UNIT.write_text(installer.UNIT_MARKER + '\n[Service]\nUser=www-data')
            auth_spec = importlib.util.spec_from_file_location('old_auth', ROOT / 'tools/robot_studio_auth.py')
            old_auth = importlib.util.module_from_spec(auth_spec)
            auth_spec.loader.exec_module(old_auth)
            original_config = json.dumps(old_auth.initial_config())
            installer.AUTH_CONFIG.write_text(original_config)
        old = installer.BASE / 'releases/previous'
        old.mkdir(parents=True)
        current = installer.BASE / 'current'
        links = {str(current): str(old)}
        staged = root / 'stage'
        staged.mkdir()
        shutil.copyfile(ROOT / 'tools/robot_studio_auth.py', staged / 'auth.py')
        release = '20261029T120000Z-aabbccddeeff'
        files = {}
        with tarfile.open(staged / 'site.tar.gz', 'w:gz') as archive:
            for name in ('index.html', 'login.html', 'login.css', 'login.js'):
                data = ('fixture ' + name).encode()
                member = tarfile.TarInfo(name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
                files[name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        request = {'release': release, 'accessMode': 'form-session', 'publicAccess': False, 'files': files,
                   'archiveSha256': hashlib.sha256((staged / 'site.tar.gz').read_bytes()).hexdigest(),
                   'authSha256': hashlib.sha256((staged / 'auth.py').read_bytes()).hexdigest()}
        request_path = staged / 'request.json'
        request_path.write_text(json.dumps(request))
        system = {'active': prior_service, 'enabled': prior_service, 'nginxTests': 0, 'reloads': 0}

        def command(args, **kwargs):
            args = list(args)
            code, out, err = 0, '', ''
            if args[:2] == ['nginx', '-V']:
                err = '--with-http_auth_request_module'
            elif args[:2] == ['nginx', '-t']:
                system['nginxTests'] += 1
                if failure == 'nginx-test' and system['nginxTests'] == 2:
                    code, err = 1, 'injected nginx configuration failure'
            elif args[:2] == ['systemctl', 'is-active']:
                active = system['active'] if args[2] == installer.SERVICE else True
                code, out = (0, 'active\n') if active else (3, 'inactive\n')
            elif args[:2] == ['systemctl', 'is-enabled']:
                code, out = (0, 'enabled\n') if system['enabled'] else (1, 'disabled\n')
            elif args[:2] == ['systemctl', 'enable']:
                system['enabled'] = True
            elif args[:2] == ['systemctl', 'disable']:
                system['enabled'] = False
            elif args[:2] == ['systemctl', 'restart']:
                system['active'] = True
            elif args[:2] == ['systemctl', 'stop']:
                system['active'] = False
            elif args[:2] == ['systemctl', 'reload']:
                system['reloads'] += 1
                if failure == 'nginx-reload' and system['reloads'] == 1:
                    code, err = 1, 'injected nginx reload failure'
            return subprocess.CompletedProcess(args, code, out, err)

        original_is_symlink = Path.is_symlink
        original_replace = os.replace

        def is_symlink(path):
            return str(path) in links or original_is_symlink(path)

        def symlink(source, destination):
            if str(destination) in links:
                raise FileExistsError(str(destination))
            links[str(destination)] = str(source)

        def replace(source, destination):
            if str(source) in links:
                links[str(destination)] = links.pop(str(source))
            else:
                original_replace(source, destination)

        def health(*args, **kwargs):
            result = io.BytesIO(b'{"ok":true}')
            result.status = 200
            return result

        with patch.object(installer.subprocess, 'run', command), \
             patch.object(installer.socket, 'create_connection', side_effect=ConnectionRefusedError()), \
             patch.object(installer.urllib.request, 'urlopen', health), \
             patch.object(Path, 'is_symlink', is_symlink), \
             patch.object(installer.os, 'symlink', symlink), \
             patch.object(installer.os, 'readlink', lambda path: links[str(path)]), \
             patch.object(installer.os, 'replace', replace), \
             patch.object(installer.os, 'chown', lambda *args: None, create=True):
            if failure:
                try:
                    installer.install(request_path)
                except RuntimeError as error:
                    check(failure + ': expected failure surfaced', 'injected' in str(error))
                else:
                    raise AssertionError('Expected injected failure')
                check(failure + ': original main site and Basic Auth restored', installer.MAIN.read_text() == main_original)
                check(failure + ': original static route restored', installer.SNIPPET.read_text() == snippet_original)
                check(failure + ': original release pointer restored', links[str(current)] == str(old))
                check(failure + ': prior service lifecycle restored', system['active'] == prior_service and system['enabled'] == prior_service)
                if prior_service:
                    check(failure + ': private config retained exactly', installer.AUTH_CONFIG.read_text() == original_config)
                    check(failure + ': original auth code restored', installer.AUTH_APP.read_text() == '# previous managed auth code')
                else:
                    check(failure + ': newly installed private service files removed', not any(p.exists() for p in (installer.AUTH_APP, installer.AUTH_CONFIG, installer.AUTH_UNIT)))
            else:
                result = installer.install(request_path)
                check('Successful publish switches pointer to new release', links[str(current)] == str(installer.BASE / 'releases' / release))
                check('Successful publish preserves original main site byte for byte', installer.MAIN.read_text() == main_original)
                check('Successful publish enables working session backend', result['authService'] == 'active' and system['enabled'])
                check('Successful publish protects entire app path', 'auth_request /__robot_studio_auth;' in installer.SNIPPET.read_text())
                private = json.loads(installer.AUTH_CONFIG.read_text())
                check('Successful publish stores salted hash outside dist', private['username'] == 'admin' and private['password']['algorithm'] == 'pbkdf2-sha256')
                check('Successful publish saves complete rollback records', (installer.BACKUP_BASE / release / 'restore-files.json').is_file())


case()
case('nginx-test')
case('nginx-reload')
case('nginx-reload', prior_service=True)
print(json.dumps({'passed': len(checks), 'checks': checks}, indent=2))
