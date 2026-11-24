#!/usr/bin/env python3
"""Auditable Linux/macOS client. Uses stdlib + OpenSSH; never cleans PC data."""
import argparse
import base64
from datetime import datetime, timedelta, timezone
import fcntl
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid

from discover_devices import discover, local_address

ROOT = Path(__file__).resolve().parent
BEIJING = timezone(timedelta(hours=8))
HOSTNAME = re.compile(r'^ClawStage-[a-zA-Z0-9-]+$', re.I)


def digest(path):
    with Path(path).open('rb') as stream:
        h = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def verify_package(root):
    manifest = json.loads((root / 'package.json').read_text())
    required = {'payload.tar.gz', 'device-reset.py', 'reset-client.py', 'discover_devices.py', 'ssh-askpass.sh'}
    if not required <= set(manifest['files']):
        raise ValueError('Incomplete package manifest')
    for name, expected in manifest['files'].items():
        path = root / name
        if not path.resolve().is_relative_to(root.resolve()) or not path.is_file() or digest(path) != expected:
            raise ValueError('Package checksum mismatch: ' + name)
    return manifest


def save_json(path, value):
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


class Logger:
    def __init__(self, root, password):
        self.password = password
        self.path = root / ('Reset-ClawStage-' + datetime.now(BEIJING).strftime('%Y%m%d-%H%M%S')
                            + '-' + uuid.uuid4().hex[:8] + '.log')
        # Fail before any device connection if the requested directory is not writable.
        with self.path.open('x', encoding='utf-8'):
            pass

    def clean(self, text):
        text = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', text)
        return text.replace(self.password, '[REDACTED]') if self.password else text

    def __call__(self, message):
        line = f'[{datetime.now(BEIJING):%Y-%m-%dT%H:%M:%S%z}] {self.clean(str(message))}'
        with self.path.open('a', encoding='utf-8') as stream:
            stream.write(line + '\n')
        print(line, flush=True)


class Runner:
    """Stream both channels, enforce a deadline, never put a password in argv."""
    def __init__(self, log, env):
        self.log, self.env = log, env

    def run(self, args, input_text='', timeout=90, quiet=False):
        lines, buffers = [], {}
        with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=self.env, start_new_session=True) as process:
            try:
                if input_text:
                    process.stdin.write(input_text.encode())
                    process.stdin.flush()
                process.stdin.close()
                deadline = time.monotonic() + timeout
                with selectors.DefaultSelector() as selector:
                    for stream in (process.stdout, process.stderr):
                        selector.register(stream, selectors.EVENT_READ)
                        buffers[stream] = b''
                    while selector.get_map() or process.poll() is None:
                        if time.monotonic() > deadline:
                            raise TimeoutError('Connection/command timed out; completion is UNKNOWN')
                        for key, _ in selector.select(0.1):
                            stream = key.fileobj
                            data = os.read(stream.fileno(), 8192)
                            buffers[stream] += data.replace(b'\r', b'\n')
                            chunks = buffers[stream].split(b'\n')
                            buffers[stream] = chunks.pop()
                            if not data:
                                selector.unregister(stream)
                                if buffers[stream]:
                                    chunks.append(buffers[stream])
                                    buffers[stream] = b''
                            for chunk in chunks:
                                if not chunk:
                                    continue
                                line = self.log.clean(chunk.decode('utf-8', errors='replace'))
                                lines.append(line)
                                if not quiet:
                                    self.log(line)
                    code = process.wait()
                return code, lines
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def select_device(candidates, log):
    candidates = sorted(set(candidates))
    if not candidates:
        raise ValueError('No ClawStage found. Check power/network, or specify --device IPv4')
    for hostname, addr in candidates:
        if not hostname.lower().endswith('.local') or not HOSTNAME.fullmatch(hostname[:-6]):
            raise ValueError('Invalid discovery hostname')
        ipaddress.IPv4Address(addr)
    if len(candidates) == 1:
        return candidates[0]
    log('Multiple devices found. Select exactly ONE device to erase:')
    for index, (hostname, addr) in enumerate(candidates, 1):
        log(f'{index}: {hostname}  {addr}')
    answer = input('Device number (Ctrl+C cancels): ')
    if not answer.isdecimal() or not 1 <= int(answer) <= len(candidates):
        raise ValueError('Invalid selection; no reset started')
    return candidates[int(answer) - 1]


def key_from_scan(lines):
    keys = set()
    for line in lines:
        parts = line.split()
        if len(parts) == 3 and parts[1] == 'ssh-ed25519':
            raw = base64.b64decode(parts[2], validate=True)
            # SSH binary encoding: string "ssh-ed25519", then 32-byte key.
            if len(raw) != 51 or raw[:19] != b'\0\0\0\x0bssh-ed25519\0\0\0\x20':
                raise ValueError('Invalid Ed25519 host key')
            fingerprint = 'SHA256:' + base64.b64encode(hashlib.sha256(raw).digest()).decode().rstrip('=')
            keys.add((parts[2], fingerprint))
    if len(keys) != 1:
        raise ValueError('Expected exactly one Ed25519 SSH host key')
    return keys.pop()


def execute(args, root, log, password):
    metadata = verify_package(root)
    log('Package checksums verified.')
    if args.verify_only:
        return 0
    for command in ('ssh', 'scp', 'ssh-keyscan'):
        if not shutil.which(command):
            raise ValueError('Install OpenSSH client first; missing: ' + command)
    version = subprocess.run(['ssh', '-V'], capture_output=True, text=True, timeout=10)
    match = re.search(r'OpenSSH_(\d+)\.(\d+)', version.stdout + version.stderr)
    if not match or tuple(map(int, match.groups())) < (8, 4):
        raise ValueError('OpenSSH 8.4+ is required for automatic SSH_ASKPASS authentication')
    expected_name = None
    if args.device:
        device = str(ipaddress.IPv4Address(args.device))
    else:
        address = local_address(args.interface)
        log(f'Searching {address.network} via mDNS on {address.ip} (about 10 seconds)...')
        candidates = discover(address)
        if args.discover_only:
            for name, ip in candidates:
                log(f'{name} {ip}')
            if not candidates:
                raise ValueError('No ClawStage found')
            return 0
        expected_name, device = select_device(candidates, log)
    log(f'Selected device: {device} port={args.port} user=pi')
    target = 'pi@' + device
    with tempfile.TemporaryDirectory(prefix='clawstage-reset-ssh-') as temporary:
        temp = Path(temporary)
        # Password goes through the child environment/askpass and sudo stdin.
        # Same-user/admin processes may inspect it; it is not a secret store.
        helper = temp / 'askpass'
        shutil.copyfile(root / 'ssh-askpass.sh', helper)
        helper.chmod(0o700)
        env = dict(os.environ, SSH_ASKPASS=str(helper), SSH_ASKPASS_REQUIRE='force',
                   DISPLAY='clawstage-askpass:0', CLAWSTAGE_SSH_PASSWORD=password)
        runner = Runner(log, env)
        scan_code, scan = runner.run(['ssh-keyscan', '-T', '10', '-p', str(args.port), '-t', 'ed25519', device], quiet=True)
        try:
            key, fingerprint = key_from_scan(scan)
        except ValueError as error:
            for line in scan:
                log(line)
            raise ValueError(f'SSH host-key probe failed (exit={scan_code}): {error}. '
                             'Check device power, SSH port and network reachability') from error
        key_path = root / f'.clawstage-key-{expected_name or device}-{args.port}.json'
        if key_path.exists():
            if json.loads(key_path.read_text(encoding='utf-8-sig'))['fingerprint'] != fingerprint:
                raise ValueError('SSH host key changed. No reset started; contact support')
        else:
            log('First connection: pinning SSH host key ' + fingerprint)
        known_hosts = temp / 'known_hosts'
        host = device if args.port == 22 else f'[{device}]:{args.port}'
        known_hosts.write_text(f'{host} ssh-ed25519 {key}\n')
        known_option = 'UserKnownHostsFile="' + str(known_hosts).replace('\\', '\\\\').replace('"', '\\"') + '"'
        common = ['-F', '/dev/null', '-o', known_option, '-o', 'GlobalKnownHostsFile=/dev/null',
                  '-o', 'StrictHostKeyChecking=yes', '-o', 'HostKeyAlgorithms=ssh-ed25519',
                  '-o', 'UpdateHostKeys=no', '-o', 'IdentityAgent=none', '-o', 'PubkeyAuthentication=no',
                  '-o', 'PreferredAuthentications=password,keyboard-interactive', '-o', 'NumberOfPasswordPrompts=1',
                  '-o', 'ConnectTimeout=15', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=3']

        def remote(command, timeout=90, check=True):
            result = runner.run(['ssh', *common, '-p', str(args.port), '-T', target, command],
                                input_text=password + '\n', timeout=timeout)
            if check and result[0]:
                raise ValueError(f'Device command failed (exit={result[0]})')
            return result

        def upload(name, destination):
            code, _ = runner.run(['scp', *common, '-P', str(args.port), str(root / name),
                                  target + ':' + destination], timeout=180)
            if code:
                raise ValueError(f'Upload failed: {name} (exit={code})')

        _, identity = remote('hostname')
        names = [line for line in identity if HOSTNAME.fullmatch(line)]
        if len(names) != 1 or (expected_name and names[0].lower() + '.local' != expected_name.lower()):
            raise ValueError('SSH hostname does not match discovered ClawStage; no reset started')
        hostname = names[0]
        # Preserve the inode so two local windows cannot both enter. This small
        # lock file is intentionally retained after closing; flock owns the lock.
        with (root / f'.clawstage-lock-{hostname}-{args.port}').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('Another reset window is working with this device')
            save_json(key_path, {'fingerprint': fingerprint})
            session_path = root / f'.clawstage-session-{hostname}-{args.port}.json'
            if args.follow:
                if not session_path.exists():
                    raise ValueError('No saved task to follow; no reset started')
                session = json.loads(session_path.read_text(encoding='utf-8-sig'))
                run_id = session['runId']
                if session['fingerprint'] != fingerprint or not re.fullmatch(r'[a-f0-9]{32}', run_id):
                    raise ValueError('Saved session does not match this device')
                log(f'Reconnecting to saved run {run_id}; no new reset will be started.')
            else:
                if not args.inspect_only:
                    log('Starting a NEW full reset; previous saved results will not be reused.')
                run_id = uuid.uuid4().hex
                destination = '/home/pi/clawstage-support-upload-' + run_id
                remote('umask 077; mkdir ' + shlex.quote(destination))
                upload('device-reset.py', destination + '/device-reset.py')
                script = shlex.quote(destination + '/device-reset.py')
                remote(f"sudo -S -p '' /usr/bin/python3 {script} inspect")
                if args.inspect_only:
                    log('Preflight passed. No reset started.')
                    return 0
                upload('payload.tar.gz', destination + '/payload.tar.gz')
                # Save before launch so --follow works even if the launch reply is lost.
                save_json(session_path, {'runId': run_id, 'fingerprint': fingerprint,
                                         'device': device, 'hostname': hostname})
                log(f'Starting run {run_id}; use --follow to reconnect without resetting again.')
                archive = shlex.quote(destination + '/payload.tar.gz')
                code, lines = remote(f"sudo -S -p '' /usr/bin/python3 {script} start --run-id {run_id} "
                                     f"--archive {archive} --sha256 {metadata['files']['payload.tar.gz']}", check=False)
                if code == 4:
                    busy = [re.fullmatch(r'BUSY run_id=([a-f0-9]{32})', line) for line in lines]
                    busy = [match.group(1) for match in busy if match]
                    if len(busy) == 1:
                        save_json(session_path, {'runId': busy[0], 'fingerprint': fingerprint,
                                                 'device': device, 'hostname': hostname})
                    raise ValueError('A reset is already running on this device. No new reset was queued. Use --follow to read it.')
                if code:
                    raise ValueError(f'Device start failed (exit={code}); use --follow to check the requested task')
            worker = f'/var/lib/clawstage-support-reset/{run_id}/device-reset.py'
            log(f'Following durable device log for run {run_id}...')
            code, _ = remote(f"sudo -S -p '' /usr/bin/python3 {worker} follow --run-id {run_id}",
                             timeout=2700, check=False)
            if args.follow:
                log(f'Saved task result exit={code}; NO new reset was performed by this invocation.')
            else:
                log('SUCCESS: This new local factory reset completed; cloud account deletion is not guaranteed.' if code == 0
                    else f'FAILED or UNKNOWN (exit={code}); use --follow to read this task without resetting again.')
            return code if code >= 0 else 128 - code


def finish_log(log, reveal=False):
    """Identify the computer copy separately from the device's durable log."""
    log('Local log saved on this computer (beside the launcher): ' + str(log.path))
    with log.path.open('a', encoding='utf-8') as stream:
        stream.flush()
        os.fsync(stream.fileno())
    if reveal and sys.platform == 'darwin':
        try:
            result = subprocess.run(['/usr/bin/open', '-R', str(log.path)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                    text=True, timeout=10)
            if result.returncode:
                log('Could not reveal the log in Finder; open the local path printed above.')
        except (OSError, subprocess.SubprocessError):
            log('Could not reveal the log in Finder; open the local path printed above.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', help='Explicit IPv4; bypasses mDNS discovery')
    parser.add_argument('--port', type=int, default=22)
    parser.add_argument('--reveal-log', action='store_true', help='Reveal the local log in Finder on macOS')
    parser.add_argument('--interface', help='Discovery interface, for example wlan0 or en0')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--inspect-only', action='store_true')
    mode.add_argument('--discover-only', action='store_true')
    mode.add_argument('--verify-only', action='store_true')
    mode.add_argument('--follow', action='store_true', help='Read the saved task only; never start a reset')
    mode.add_argument('--new-reset', action='store_true', help='Compatibility alias for the default: start a new full reset')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('Invalid SSH port')
    if args.device and args.discover_only:
        parser.error('--discover-only cannot be combined with --device')
    password = os.environ.get('CLAWSTAGE_PASSWORD', 'goodlife')
    if not password or '\n' in password or '\r' in password:
        parser.error('CLAWSTAGE_PASSWORD must be a nonempty single line')
    try:
        log = Logger(ROOT, password)
    except OSError as error:
        print(f'Cannot create a log beside the launcher in {ROOT}: {error}', file=sys.stderr)
        print('No device connection started. Move the complete tool folder to a writable location.', file=sys.stderr)
        return 1
    try:
        log('ClawStage factory reset. A new reset erases device account/HA/Matter/Thread data.')
        log('Log timezone: Asia/Shanghai (UTC+08:00). Log file: ' + str(log.path))
        code = execute(args, ROOT, log, password)
    except KeyboardInterrupt:
        log('Interrupted. A launched device task continues; use --follow to read its status.')
        code = 130
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, EOFError) as error:
        log('ERROR: ' + str(error))
        log('No automatic retry. Use --follow to inspect a saved task; a normal launch requests a new reset.')
        code = 1
    log(f'Tool exit={code}. Logs are not uploaded automatically.')
    finish_log(log, reveal=args.reveal_log)
    return code


if __name__ == '__main__':
    sys.exit(main())
