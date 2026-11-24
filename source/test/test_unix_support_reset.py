"""Real Unix client subprocesses with fake OpenSSH; never erase a real device."""
import base64
import hashlib
import importlib.util
import ipaddress
import io
import pty
import select
import time
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / 'script/support-reset'
sys.path.insert(0, str(CLIENT))
import discover_devices as dns
spec = importlib.util.spec_from_file_location('reset_client', CLIENT / 'reset-client.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class DiscoveryTests(unittest.TestCase):
    def test_linux_and_mac_interface_selection(self):
        def linux(args):
            return json.dumps([{'dev': 'wlan0'}]) if 'route' in args else json.dumps([
                {'addr_info': [{'family': 'inet', 'scope': 'global', 'local': '192.0.2.8', 'prefixlen': 24}]}])
        self.assertEqual(str(dns.local_address(system='Linux', run=linux)), '192.0.2.8/24')
        def mac(args):
            return ' interface: en0\n' if args[0] == 'route' else ' inet 192.0.2.8 netmask 0xffffff00 broadcast 192.0.2.255\n'
        self.assertEqual(str(dns.local_address(system='Darwin', run=mac)), '192.0.2.8/24')
        with self.assertRaises(ValueError):
            dns.local_address(system='Linux', run=lambda _: '[{"dev":"eth0"},{"dev":"tun0"}]')
        with self.assertRaises(ValueError):
            dns.local_address(system='Darwin', run=lambda _: ' interface: en0\n inet 192.0.2.8 netmask 0xffff0000\n')

    def test_dns_compression_and_malformed_packets(self):
        reverse = '10.2.0.192.in-addr.arpa'
        question = dns.query(reverse, 1)[12:]
        target = b''.join(bytes([len(p)]) + p.encode() for p in 'ClawStage-ABCD.local'.split('.')) + b'\0'
        packet = struct.pack('!6H', 1, 0x8400, 1, 1, 0, 0) + question + b'\xc0\x0c' + struct.pack('!HHIH', 12, 1, 120, len(target)) + target
        self.assertEqual(dns.parse_reply(packet, {reverse: '192.0.2.10'}), [('ClawStage-ABCD.local', '192.0.2.10')])
        self.assertEqual(dns.parse_reply(packet, {}), [])
        for malformed in (b'\xc0\x00', b'\x40bad', b'\x03ab'):
            with self.assertRaises(ValueError):
                dns.read_name(malformed, 0)
        with self.assertRaises(ValueError):
            dns.parse_reply(packet[:-1], {reverse: '192.0.2.10'})

    def test_multiple_devices_require_explicit_selection(self):
        devices = [('ClawStage-A.local', '192.0.2.1'), ('ClawStage-B.local', '192.0.2.2')]
        with patch('builtins.input', return_value='2'):
            self.assertEqual(client.select_device(devices, lambda _: None), devices[1])
        with patch('builtins.input', return_value=''):
            with self.assertRaises(ValueError):
                client.select_device(devices, lambda _: None)


class UnixClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='reset kit with spaces ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ('reset-client.py', 'discover_devices.py', 'ssh-askpass.sh', 'device-reset.py',
                     'Reset-ClawStage.sh', 'Reset-ClawStage.command'):
            shutil.copyfile(CLIENT / name, self.root / name)
        (self.root / 'payload.tar.gz').write_bytes(b'fixture')
        self.bin = self.root / 'bin'; self.bin.mkdir()
        self.events = self.root / 'events.jsonl'
        fake = '''#!/usr/bin/env python3
import os,sys,json,pathlib,base64,subprocess
args=sys.argv[1:]; name=pathlib.Path(sys.argv[0]).name
if args==['-V']:
 print('OpenSSH_'+os.environ.get('SSH_VERSION','9.2p1'), file=sys.stderr);sys.exit(0)
if name=='ssh-keyscan':
 raw=b'\\0\\0\\0\\x0bssh-ed25519\\0\\0\\0\\x20'+b'k'*32
 print('192.0.2.10 ssh-ed25519 '+base64.b64encode(raw).decode());sys.exit(0)
assert 'goodlife' not in args
assert os.environ['SSH_ASKPASS_REQUIRE']=='force'
assert subprocess.check_output([os.environ['SSH_ASKPASS']]).decode()=='goodlife\\n'
assert 'StrictHostKeyChecking=yes' in args and 'PubkeyAuthentication=no' in args
if name=='scp':
 print('Upload done');sys.exit(0)
assert '-T' in args and sys.stdin.read()=='goodlife\\n'
command=args[-1]
with open(os.environ['RESET_EVENTS'],'a') as f:f.write(json.dumps(command)+'\\n')
if command=='hostname':print(os.environ.get('HOSTNAME_REPLY','ClawStage-ABCD'))
elif command.endswith(' inspect'):
 print('Device version='+os.environ.get('DEVICE_VERSION','0.2.6'))
 sys.exit(1 if os.environ.get('MISSING_PREREQUISITE') else 0)
elif ' start --run-id ' in command:
 if os.environ.get('RESET_BUSY'):
  print('BUSY run_id='+'b'*32);sys.exit(4)
 print('STARTED')
 if os.environ.get('LOST_START'):sys.exit(1)
elif ' follow --run-id ' in command:
 print('STEP START live-output',flush=True)
 print('stderr diagnostic goodlife',file=sys.stderr,flush=True)
 print('SUPPORT RESULT=SUCCESS',flush=True)
 sys.exit(41 if os.environ.get('RESET_FAILURE') else 0)
'''
        for name in ('ssh', 'scp', 'ssh-keyscan'):
            path = self.bin / name; path.write_text(fake); path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ['PATH'], RESET_EVENTS=str(self.events))
        self.metadata()

    def metadata(self):
        (self.root / 'package.json').write_text(json.dumps({'files': {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in self.root.iterdir()
            if p.is_file() and p.name != 'package.json'}}))

    def run_tool(self, *args, **env):
        return subprocess.run([sys.executable, str(self.root / 'reset-client.py'), '--device', '192.0.2.10', *args],
                              capture_output=True, text=True, timeout=15, env=dict(self.env, **env))

    def commands(self):
        return [json.loads(line) for line in self.events.read_text().splitlines()] if self.events.exists() else []

    def test_each_default_launch_starts_new_and_follow_is_explicit(self):
        for args in ((), (), ('--follow',), ('--new-reset',)):
            result = self.run_tool(*args)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sum(' start --run-id ' in c for c in self.commands()), 3)
        self.assertEqual(sum(' follow --run-id ' in c for c in self.commands()), 4)
        starts = [c for c in self.commands() if ' start --run-id ' in c]
        self.assertEqual(len(set(starts)), 3)
        for path in self.root.glob('*.log'):
            log = path.read_text()
            self.assertIn('+0800]', log)
            self.assertNotIn('goodlife', log)
            self.assertIn('stderr diagnostic [REDACTED]', log)

    def test_lost_launch_reply_can_be_followed_without_reset(self):
        self.assertNotEqual(self.run_tool(LOST_START='1').returncode, 0)
        self.assertEqual(self.run_tool('--follow').returncode, 0)
        self.assertEqual(sum(' start --run-id ' in c for c in self.commands()), 1)

    def test_follow_without_session_never_starts_and_modes_conflict(self):
        for args in (('--follow',), ('--follow', '--new-reset')):
            self.assertNotEqual(self.run_tool(*args).returncode, 0)
        self.assertFalse(any(' start --run-id ' in c for c in self.commands()))

    def test_busy_device_is_not_queued_and_can_be_followed(self):
        result = self.run_tool(RESET_BUSY='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No new reset was queued', result.stdout)
        self.assertFalse(any(' follow --run-id ' in c for c in self.commands()))
        session = json.loads(next(self.root.glob('.clawstage-session-*')).read_text())
        self.assertEqual(session['runId'], 'b' * 32)
        result = self.run_tool('--follow')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('NO new reset was performed', result.stdout)
        self.assertIn('follow --run-id ' + 'b' * 32, self.commands()[-1])

    def test_inspect_missing_prerequisite_old_ssh_and_wrong_hostname(self):
        for args, env, expected in [(('--inspect-only',), {}, 0), ((), {'MISSING_PREREQUISITE':'1'}, 1),
                                    ((), {'SSH_VERSION':'8.3p1'}, 1), ((), {'HOSTNAME_REPLY':'unrelated'}, 1)]:
            result = self.run_tool(*args, **env)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertFalse(any(' start --run-id ' in c for c in self.commands()))

    def test_changed_host_key_stops_before_authentication(self):
        self.assertEqual(self.run_tool('--inspect-only').returncode, 0)
        count = len(self.commands())
        next(self.root.glob('.clawstage-key-*')).write_text('{"fingerprint":"different"}')
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('host key changed', result.stdout)
        self.assertEqual(len(self.commands()), count)

    def test_failure_exit_and_firmware_not_restricted(self):
        result = self.run_tool(RESET_FAILURE='1', DEVICE_VERSION='0.2.16-test4')
        self.assertEqual(result.returncode, 41, result.stdout + result.stderr)
        self.assertIn('version=0.2.16-test4', result.stdout)

    def test_verify_only_and_corruption_never_connect(self):
        self.assertEqual(self.run_tool('--verify-only').returncode, 0)
        (self.root / 'payload.tar.gz').write_bytes(b'corrupt')
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertEqual(self.commands(), [])

    def test_mac_launcher_from_unrelated_cwd_saves_log_beside_it(self):
        with tempfile.TemporaryDirectory(prefix='unrelated cwd ') as cwd:
            result = subprocess.run(['/bin/sh', str(self.root / 'Reset-ClawStage.command'), '--verify-only'],
                                    cwd=cwd, env=dict(self.env, PATH='/usr/bin:/bin'),
                                    text=True, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            logs = list(self.root.glob('*.log'))
            self.assertEqual(len(logs), 1)
            self.assertFalse(list(Path(cwd).glob('*.log')))
            self.assertIn('Local log saved on this computer', logs[0].read_text())
            self.assertIn(str(logs[0]), result.stdout)
            self.assertEqual(self.commands(), [])

    def test_mac_terminal_stays_open_and_preserves_failure_exit(self):
        (self.root / 'payload.tar.gz').write_bytes(b'corrupt')
        master, slave = pty.openpty()
        process = subprocess.Popen(['/bin/sh', str(self.root / 'Reset-ClawStage.command'), '--verify-only'],
                                   stdin=slave, stdout=slave, stderr=slave, env=self.env)
        os.close(slave)
        try:
            output = b''
            deadline = time.monotonic() + 10
            while b'Press Enter' not in output and time.monotonic() < deadline:
                if select.select([master], [], [], 0.1)[0]:
                    output += os.read(master, 8192)
            self.assertIn(b'Press Enter', output)
            self.assertIsNone(process.poll())
            self.assertIn(b'Package checksum mismatch', output)
            os.write(master, b'\n')
            self.assertEqual(process.wait(timeout=5), 1)
            self.assertEqual(self.commands(), [])
        finally:
            if process.poll() is None:
                process.kill(); process.wait()
            os.close(master)

    def test_finder_reveals_exact_local_log_and_failure_keeps_log(self):
        log = client.Logger(self.root, 'goodlife')
        with patch.object(client.sys, 'platform', 'darwin'):
            with patch.object(client.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as reveal:
                client.finish_log(log, reveal=True)
                self.assertEqual(reveal.call_args.args[0], ['/usr/bin/open', '-R', str(log.path)])
            with patch.object(client.subprocess, 'run', side_effect=OSError('Finder unavailable')):
                client.finish_log(log, reveal=True)
        self.assertIn('Could not reveal', log.path.read_text())
        self.assertIn(str(log.path), log.path.read_text())

    def test_log_creation_failure_stops_before_device_connection(self):
        with patch.object(client, 'ROOT', self.root / 'missing-directory'), \
                patch.object(client.sys, 'argv', ['reset-client.py', '--verify-only']), \
                patch.object(client, 'execute') as execute, patch('sys.stderr', new_callable=io.StringIO) as stderr:
            self.assertEqual(client.main(), 1)
            execute.assert_not_called()
            self.assertIn('Cannot create a log beside the launcher', stderr.getvalue())

    def test_timeout_stops_local_transport(self):
        log = client.Logger(self.root, 'goodlife')
        with self.assertRaises(TimeoutError):
            client.Runner(log, os.environ).run([sys.executable, '-c', 'import time; time.sleep(10)'], timeout=0.2)


if __name__ == '__main__':
    unittest.main()
