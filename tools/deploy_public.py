"""Publish only dist/ using the user's existing verified SSH handoff.

No credentials are copied to the project, command arguments, logs, or release.
This app uses its own form login. Other routes retain their original protection.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import shlex
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def connect(handoff):
    config = json.loads((handoff / 'server-access.json').read_text(encoding='utf-8-sig'))
    try:
        import paramiko
    except ModuleNotFoundError:
        sys.path.insert(0, config['paramiko_fallback_path'])
        import paramiko
    if config['host'] != '110.42.234.162' or int(config['port']) != 22 or config['username'] != 'ubuntu':
        raise ValueError('Handoff does not match the selected existing server')
    client = paramiko.SSHClient()
    client.load_host_keys(str(handoff / 'known_hosts'))
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    client.connect(config['host'], port=config['port'], username=config['username'], password=config['password'],
                   timeout=15, auth_timeout=15, banner_timeout=15, allow_agent=False, look_for_keys=False)
    return client


def response(url, accept_gzip=False):
    req = urllib.request.Request(url, method='GET', headers={'Accept-Encoding': 'gzip' if accept_gzip else 'identity'})
    try:
        result = urllib.request.urlopen(req, timeout=25)
    except urllib.error.HTTPError as error:
        result = error
    with result:
        data = result.read()
        return {'url': result.url, 'status': result.status, 'contentType': result.headers.get('Content-Type'),
                'encoding': result.headers.get('Content-Encoding'), 'bytes': len(data),
                'sha256': hashlib.sha256(data).hexdigest(), 'loginRequired': bool(result.headers.get('WWW-Authenticate'))}


def deploy(handoff):
    output = ROOT / 'review-output/public-deploy'
    output.mkdir(parents=True, exist_ok=True)
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    release = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + commit[:12]
    archive = output / (release + '.tar.gz')
    files = {}
    with tarfile.open(archive, 'w:gz', compresslevel=6) as bundle:
        for path in sorted((ROOT / 'dist').rglob('*')):
            if path.is_symlink(): raise ValueError('Symlink in public output')
            if not path.is_file(): continue
            relative = path.relative_to(ROOT / 'dist').as_posix()
            public_type = path.suffix in {'.html', '.css', '.js', '.json', '.bin', '.txt'}
            firmware_type = relative.startswith('firmware/') and path.suffix in {'.hex', '.zip'}
            if any(part.startswith('.') for part in Path(relative).parts) or not (public_type or firmware_type):
                raise ValueError('Unexpected public artifact')
            data = path.read_bytes()
            files[relative] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            bundle.add(path, arcname=relative, recursive=False)
    request = {'release': release, 'sourceCommit': commit, 'archiveSha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
               'accessMode': 'form-session', 'publicAccess': False, 'files': files,
               'authSha256': hashlib.sha256((ROOT / 'tools/robot_studio_auth.py').read_bytes()).hexdigest()}
    request_file = output / (release + '.json')
    request_file.write_text(json.dumps(request, indent=2), encoding='utf-8')
    client = connect(handoff)
    try:
        stage = '/tmp/robot-studio-' + release
        with client.open_sftp() as sftp:
            sftp.mkdir(stage, 0o700)
            for source, name in [(archive, 'site.tar.gz'), (request_file, 'request.json'),
                                 (ROOT / 'tools/install_public_nginx.py', 'install.py'),
                                 (ROOT / 'tools/robot_studio_auth.py', 'auth.py')]:
                sftp.put(str(source), stage + '/' + name)
                sftp.chmod(stage + '/' + name, 0o600)
        command = 'sudo -n python3 ' + shlex.quote(stage + '/install.py') + ' ' + shlex.quote(stage + '/request.json')
        _, stdout, stderr = client.exec_command(command, timeout=90)
        text = stdout.read().decode('utf-8', errors='replace')
        error = stderr.read().decode('utf-8', errors='replace')
        code = stdout.channel.recv_exit_status()
        if code: raise RuntimeError('Remote deployment failed: ' + error.strip())
        result = json.loads(text)
        (output / 'deployment.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=True))
    finally:
        client.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--handoff', type=Path, required=True)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    if args.verify:
        print(json.dumps([response('https://110.42.234.162' + path) for path in ['/robot-studio/', '/robot-studio/app.js', '/learn/', '/']], indent=2))
    else:
        deploy(args.handoff)
