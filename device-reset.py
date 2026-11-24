#!/usr/bin/env python3
"""Transport/diagnostics only; account cleanup belongs to factory-reset.sh."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import time

BASE = Path('/var/lib/clawstage-support-reset')
CURRENT = Path('/home/pi/clawstage/current')
MAX_PAYLOAD = 256 * 1024 * 1024


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def inspect_release(current=CURRENT):
    release = current.resolve(strict=True)
    try:
        version = (release / 'VERSION').read_text().strip() or 'unknown'
    except FileNotFoundError:
        version = 'unknown'
    print(f'Device={os.uname().nodename} architecture={os.uname().machine} release={release} version={version}', flush=True)
    return release, version


def preflight(current=CURRENT):
    # Firmware version is diagnostic only. The bundled reset implementation is
    # selected independently; installed paths and commands define compatibility.
    release, _ = inspect_release(current)
    if os.uname().machine != 'aarch64':
        raise ValueError('Requires ARM64 Raspberry Pi OS')
    for name in ('bash', 'systemctl', 'systemd-run', 'jq', 'openssl', 'docker', 'flock', 'runuser', 'curl'):
        if not shutil.which(name):
            raise ValueError(f'Missing command: {name}')
    for name in ('user/.hoorii/config.json', 'openclaw/hoorii_claw/script/clear.sh',
                 'homeassistant/stage-stack/compose.release.yaml', 'homeassistant/stage-stack/install.sh'):
        if not (release / name).is_file():
            raise ValueError(f'Missing required installed file: {name}')
    subprocess.run(['docker', 'compose', 'version'], check=True, stdout=subprocess.DEVNULL)
    return release


def unpack(archive, destination):
    """No links, device nodes, duplicate names or archive paths outside payload."""
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        seen = set()
        total = 0
        for member in members:
            name = PurePosixPath(member.name)
            if (name.is_absolute() or '..' in name.parts or not name.parts
                    or member.name in seen or not (member.isfile() or member.isdir())):
                raise ValueError('Unsafe payload member: ' + member.name)
            seen.add(member.name)
            total += member.size
        if total > MAX_PAYLOAD:
            raise ValueError('Payload is too large')
        for member in members:
            path = destination / member.name
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, path.open('xb') as target:
                    shutil.copyfileobj(source, target)
                path.chmod(0o755 if member.mode & 0o111 else 0o644)
    manifest = json.loads((destination / 'manifest.json').read_text())
    actual = {p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file()}
    if actual != set(manifest['files']) | {'manifest.json'}:
        raise ValueError('Payload manifest inventory mismatch')
    for name, expected in manifest['files'].items():
        if digest(destination / name) != expected:
            raise ValueError('Payload checksum mismatch: ' + name)
    for name in ('factory-reset.sh', 'health-check.sh', 'stack-install.sh', 'shadowlink/runtime/shadowlink-server'):
        if name not in manifest['files']:
            raise ValueError('Missing payload helper: ' + name)
    return manifest


def unit_name(run):
    return 'clawstage-support-reset-' + run.name


class ResetBusyError(Exception):
    def __init__(self, run_id):
        self.run_id = run_id


def active_support_run():
    result = subprocess.run([
        'systemctl', 'list-units', '--type=service',
        '--state=active,activating,reloading,deactivating', '--no-legend', '--plain', '--no-pager',
        'clawstage-support-reset-*.service',
    ], capture_output=True, text=True, check=True)
    for line in result.stdout.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r'clawstage-support-reset-([a-f0-9]{32})\.service', line.split()[0])
        if not match:
            raise ValueError('Cannot identify active support task; refusing another launch')
        return match.group(1)
    return None


def start(run, archive, expected):
    if run.exists():
        raise ValueError('Run already exists. Use follow; never automatically repeat a reset.')
    BASE.mkdir(mode=0o700, parents=True, exist_ok=True)
    # Serialize admission through systemd-run, including requests from other PCs.
    # The active unit then owns the reservation until its worker exits. Existing
    # workers from older packages are detected too; no second reset is queued.
    with (BASE / '.launch.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        active = active_support_run()
        if active:
            raise ResetBusyError(active)
        start_locked(run, archive, expected)


def start_locked(run, archive, expected):
    release = preflight()
    BASE.mkdir(mode=0o700, parents=True, exist_ok=True)
    run.mkdir(mode=0o700)
    # Freeze all executable input before the detached privileged worker starts.
    frozen = run / 'payload.tar.gz'
    shutil.copyfile(archive, frozen)
    if digest(frozen) != expected:
        raise ValueError('Upload checksum mismatch; no reset started')
    manifest = unpack(frozen, run / 'payload')
    shutil.copyfile(Path(__file__), run / 'device-reset.py')
    (run / 'request.json').write_text(json.dumps({'release': str(release), 'source': manifest['source']}) + '\n')
    subprocess.run([str(run / 'payload/shadowlink/runtime/shadowlink-server'), '--version'], check=True)
    subprocess.run(['systemd-run', '--unit=' + unit_name(run), '--service-type=exec',
                    '--property=TimeoutStartSec=infinity', '--property=RuntimeMaxSec=2400',
                    '/usr/bin/python3', '-u', str(run / 'device-reset.py'), 'worker', '--run-id', run.name], check=True)
    print(f'STARTED run_id={run.name} logs={run}/reset.log', flush=True)


def save_result(run, code):
    path = run / 'result.json.tmp'
    with path.open('w') as stream:
        json.dump({'exit_code': code, 'result': 'SUCCESS' if code == 0 else 'FAILED'}, stream)
        stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
    path.replace(run / 'result.json')


def worker(run):
    code = 1
    with (run / 'reset.log').open('a', buffering=1) as log:
        os.dup2(log.fileno(), 1); os.dup2(log.fileno(), 2)
        try:
            request = json.loads((run / 'request.json').read_text())
            release = preflight()
            if str(release) != request['release']:
                raise ValueError('Active release changed after confirmation')
            print('Support source: ' + json.dumps(request['source']), flush=True)
            print('Installed component cleanup entries are reused; installed software version is unchanged.', flush=True)
            print('Reset log timezone: Asia/Shanghai (UTC+08:00).', flush=True)
            code = subprocess.call(['/bin/bash', str(run / 'payload/factory-reset.sh'),
                                    '--release-dir', str(release), '--expected-release', str(release),
                                    '--support-dir', str(run / 'payload')],
                                   env=dict(os.environ, SUDO_USER='pi', PYTHONUNBUFFERED='1',
                                            TZ='Asia/Shanghai'))
            if code < 0:
                code = 128 - code
        except Exception as error:
            print(f'SUPPORT ERROR: {error}', flush=True)
        finally:
            print(f'SUPPORT RESULT={"SUCCESS" if code == 0 else "FAILED"} exit_code={code}', flush=True)
            print('Device log on ClawStage (remote copy): ' + str(run / 'reset.log'), flush=True)
            save_result(run, code)
    return code


def follow(run):
    if not (run / 'request.json').exists():
        raise ValueError('Unknown run ID; no reset will be started')
    offset = 0
    while True:
        result = run / 'result.json'
        complete = result.exists()
        log = run / 'reset.log'
        if log.exists():
            with log.open('rb') as stream:
                stream.seek(offset)
                data = stream.read(); offset = stream.tell()
                sys.stdout.buffer.write(data); sys.stdout.buffer.flush()
        if complete:
            return int(json.loads(result.read_text())['exit_code'])
        state = subprocess.run(['systemctl', 'show', unit_name(run), '--property=ActiveState', '--value'],
                               capture_output=True, text=True, check=False).stdout.strip()
        if state not in ('active', 'activating', 'reloading'):
            # A worker may have finished between reading the result and systemctl.
            if result.exists():
                continue
            print(f'RESULT UNKNOWN: service={state or "missing"}; no completion record. Do not assume success.', flush=True)
            return 3
        time.sleep(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=('inspect', 'start', 'worker', 'follow', 'log'))
    parser.add_argument('--run-id')
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--sha256')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run with sudo')
    if args.action == 'inspect':
        preflight(); return 0
    if not args.run_id or not re.fullmatch(r'[a-f0-9]{32}', args.run_id):
        parser.error('Invalid run ID')
    run = BASE / args.run_id
    if args.action == 'start':
        if not args.archive or not re.fullmatch(r'[a-f0-9]{64}', args.sha256 or ''):
            parser.error('start requires --archive and --sha256')
        start(run, args.archive, args.sha256); return 0
    if args.action == 'worker':
        return worker(run)
    if args.action == 'log':
        sys.stdout.buffer.write((run / 'reset.log').read_bytes()); return 0
    return follow(run)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except ResetBusyError as error:
        print('BUSY run_id=' + error.run_id, flush=True)
        raise SystemExit(4)
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        print(f'SUPPORT ERROR: {error}', file=sys.stderr, flush=True)
        raise SystemExit(1)
