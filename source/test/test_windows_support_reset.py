"""Run the actual PowerShell entry against fake SSH/SFTP processes, never a device."""
import hashlib
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PWSH = os.environ.get('PWSH') or shutil.which('pwsh')


@unittest.skipUnless(PWSH, 'Set PWSH to an installed PowerShell executable')
class WindowsSupportResetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='reset kit with spaces ')
        self.addCleanup(self.temp.cleanup)
        self.kit = Path(self.temp.name)
        self.env = dict(os.environ, DOTNET_EnableDiagnostics='0')
        if os.name != 'nt':
            for key in ('XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_CONFIG_HOME', 'XDG_RUNTIME_DIR'):
                path = self.kit / key
                path.mkdir(mode=0o700)
                self.env[key] = str(path)
        for name in ('Reset-ClawStage.ps1', 'Reset-Transport.ps1', 'device-reset.py'):
            shutil.copyfile(ROOT / 'script/support-reset' / name, self.kit / name)
        self.discovery(1)
        (self.kit / 'payload.tar.gz').write_bytes(b'fixture')
        self.events = self.kit / 'events.jsonl'
        fake = '''#!/usr/bin/env python3
import json,os,pathlib,sys,time
args=sys.argv[1:]
assert '-batch' in args and '-pwfile' in args and '-hostkey' in args
assert 'goodlife' not in args
assert pathlib.Path(args[args.index('-pwfile')+1]).read_text().strip()=='goodlife'
key=args[args.index('-hostkey')+1]
if key=='SHA256:'+'A'*43:
 print('The server host key is: ssh-ed25519 255 SHA256:'+'B'*43,file=sys.stderr)
 sys.exit(1)
assert key=='SHA256:'+'B'*43
if pathlib.Path(sys.argv[0]).name=='pscp.exe':
 assert '-sftp' in args
 print('Upload complete');sys.exit(0)
assert sys.stdin.read()=='goodlife\\n'
command=args[-1]
with open(os.environ['RESET_EVENTS'],'a') as stream:stream.write(json.dumps(command)+'\\n')
if command=='hostname':print('ClawStage-ABCD')
elif command.endswith(' inspect'):
 print('Device=ClawStage-ABCD version='+os.environ.get('DEVICE_VERSION','0.2.6'))
 sys.exit(1 if os.environ.get('MISSING_PREREQUISITE') else 0)
elif ' start --run-id ' in command:
 if os.environ.get('RESET_BUSY'):
  print('BUSY run_id='+'b'*32);sys.exit(4)
 print('STARTED')
 if os.environ.get('LOST_START'):sys.exit(1)
elif ' follow --run-id ' in command:
 print('STEP START live-output',flush=True)
 print('stderr diagnostic goodlife',file=sys.stderr,flush=True)
 time.sleep(0.1)
 print('SUPPORT RESULT='+('FAILED' if os.environ.get('RESET_FAILURE') else 'SUCCESS'),flush=True)
 sys.exit(41 if os.environ.get('RESET_FAILURE') else 0)
'''
        for name in ('plink.exe', 'pscp.exe'):
            (self.kit / name).write_text(fake); (self.kit / name).chmod(0o755)
        self.metadata()

    def discovery(self, count):
        text = 'param([switch]$AsObject)\n'
        for i in range(count):
            name = 'ABCD' if i == 0 else 'EF12'
            text += f"[pscustomobject]@{{Hostname='ClawStage-{name}.local';IPv4='192.0.2.{10+i}'}}\n"
        (self.kit / 'Find-ClawStage.ps1').write_text(text)

    def metadata(self):
        (self.kit / 'package.json').write_text(json.dumps({'files': {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in self.kit.iterdir()
            if p.is_file() and p.name in {'Reset-ClawStage.ps1', 'Reset-Transport.ps1', 'device-reset.py',
                                          'Find-ClawStage.ps1', 'payload.tar.gz', 'plink.exe', 'pscp.exe'}}}))

    def run_tool(self, *args, **env):
        return subprocess.run([PWSH, '-NoProfile', '-NonInteractive', '-File',
                               str(self.kit / 'Reset-ClawStage.ps1'), *args], capture_output=True,
                              text=True, timeout=25,
                              env=dict(self.env, RESET_EVENTS=str(self.events), **env))

    def commands(self):
        return [json.loads(line) for line in self.events.read_text().splitlines()] if self.events.exists() else []

    def test_verify_only_does_not_connect(self):
        result = self.run_tool('-VerifyOnly')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.commands(), [])
        self.assertIn('Verification only', result.stdout)

    def test_each_double_click_starts_a_new_full_reset(self):
        first = self.run_tool(TZ='Etc/GMT+8')
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        second = self.run_tool()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(sum(' start --run-id ' in cmd for cmd in self.commands()), 2)
        self.assertEqual(sum(' follow --run-id ' in cmd for cmd in self.commands()), 2)
        self.assertEqual(len(set(cmd for cmd in self.commands() if ' start --run-id ' in cmd)), 2)
        logs = list(self.kit.glob('*.log')); self.assertEqual(len(logs), 2)
        for path in logs:
            log = path.read_text()
            timestamp = datetime.strptime(log.split(']', 1)[0][1:], '%Y-%m-%dT%H:%M:%S%z')
            self.assertEqual(timestamp.utcoffset(), timedelta(hours=8))
            self.assertLess(abs((timestamp - datetime.now(timezone.utc)).total_seconds()), 60)
            file_time = datetime.strptime(path.name[len('Reset-ClawStage-'):len('Reset-ClawStage-') + 15],
                                          '%Y%m%d-%H%M%S').replace(tzinfo=timestamp.tzinfo)
            self.assertLessEqual(abs((file_time - timestamp).total_seconds()), 1)
            self.assertIn('live-output', log); self.assertIn('stderr diagnostic [REDACTED]', log)
            self.assertIn('Tool exit=0', log); self.assertNotIn('goodlife', log)
        self.assertFalse((self.kit / 'logs').exists())

    def test_lost_launch_response_can_be_followed_without_relaunch(self):
        first = self.run_tool(LOST_START='1')
        self.assertNotEqual(first.returncode, 0)
        second = self.run_tool('-Follow')
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn('NO new reset was performed', second.stdout)
        self.assertEqual(sum(' start --run-id ' in cmd for cmd in self.commands()), 1)

    def test_follow_without_session_and_conflicting_modes_never_start(self):
        for args in (('-Follow',), ('-Follow', '-NewReset'), ('-Follow', '-InspectOnly')):
            self.assertNotEqual(self.run_tool(*args).returncode, 0)
        self.assertFalse(any(' start --run-id ' in cmd for cmd in self.commands()))

    def test_busy_device_is_not_queued_and_can_be_followed(self):
        result = self.run_tool(RESET_BUSY='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No new reset was queued', result.stdout)
        self.assertFalse(any(' follow --run-id ' in cmd for cmd in self.commands()))
        session = json.loads(next(self.kit.glob('.clawstage-session-*')).read_text())
        self.assertEqual(session['runId'], 'b' * 32)
        result = self.run_tool('-Follow')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('NO new reset was performed', result.stdout)

    def test_failure_exit_code_and_stderr_are_logged(self):
        result = self.run_tool(RESET_FAILURE='1')
        self.assertEqual(result.returncode, 41, result.stdout + result.stderr)
        log = next(self.kit.glob('*.log')).read_text()
        self.assertIn('SUPPORT RESULT=FAILED', log); self.assertIn('Tool exit=41', log)

    def test_newer_firmware_uses_bundled_reset(self):
        result = self.run_tool(DEVICE_VERSION='0.2.16-test4')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('version=0.2.16-test4', result.stdout)
        self.assertIn('starting the bundled factory reset', result.stdout)
        self.assertEqual(sum(' start --run-id ' in cmd for cmd in self.commands()), 1)

    def test_missing_prerequisite_inspect_and_no_devices_never_start(self):
        for args, env in [((), {'MISSING_PREREQUISITE': '1'}), (('-InspectOnly',), {})]:
            result = self.run_tool(*args, **env)
            self.assertEqual(result.returncode, 1 if env else 0, result.stdout + result.stderr)
            self.assertFalse(any(' start --run-id ' in cmd for cmd in self.commands()))
        self.discovery(0); self.metadata()
        previous_logs = set(self.kit.glob('*.log'))
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No ClawStage found', result.stdout)
        self.assertIn('No ClawStage found', (set(self.kit.glob('*.log')) - previous_logs).pop().read_text())

    def test_multiple_devices_do_not_silently_choose(self):
        self.discovery(2); self.metadata()
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Multiple devices found', result.stdout)
        self.assertEqual(self.commands(), [])

    def test_corrupt_payload_aborts_before_ssh_and_logs_reason(self):
        (self.kit / 'payload.tar.gz').write_bytes(b'corrupted')
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Package checksum mismatch', next(self.kit.glob('*.log')).read_text())
        self.assertEqual(self.commands(), [])

    def test_hostname_mismatch_and_saved_key_change_never_start(self):
        path = self.kit / 'Find-ClawStage.ps1'
        original = path.read_text()
        path.write_text(original.replace('ABCD', 'EF12')); self.metadata()
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('hostname does not match', result.stdout)
        path.write_text(original); self.metadata()
        self.assertEqual(self.run_tool('-InspectOnly').returncode, 0)
        key = next(self.kit.glob('.clawstage-key-*.json'))
        key.write_text(json.dumps({'fingerprint': 'SHA256:' + 'C' * 43}))
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('First connection: pinning', result.stdout)
        self.assertFalse(any(' start --run-id ' in cmd for cmd in self.commands()))

    def test_real_discovery_parser_and_all_powershell_syntax(self):
        script = self.kit / 'dns-test.ps1'
        script.write_text('''param($Source)
foreach ($path in (Get-ChildItem -LiteralPath $Source -Filter '*.ps1')) {
    $tokens = $null; $errors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile($path.FullName, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
}
$text = Get-Content -Raw (Join-Path $Source 'Find-ClawStage.ps1')
. ([scriptblock]::Create($text.Substring(0, $text.IndexOf('$addresses = @('))))
$wire = [Collections.Generic.List[byte]]::new()
foreach ($part in 'ClawStage-ABCD.local'.Split('.')) { $wire.Add([byte]$part.Length); $wire.AddRange([Text.Encoding]::ASCII.GetBytes($part)) }
$wire.Add(0); $directLength = $wire.Count; $wire.Add(192); $wire.Add(0)
$offset = 0
if ((Read-DnsName $wire.ToArray() ([ref]$offset)) -cne 'ClawStage-ABCD.local' -or $offset -ne $directLength) { throw 'direct name failed' }
if ((Read-DnsName $wire.ToArray() ([ref]$offset)) -cne 'ClawStage-ABCD.local' -or $offset -ne $wire.Count) { throw 'compressed name failed' }
$offset = 0
try { Read-DnsName ([byte[]]@(192)) ([ref]$offset); throw 'missing truncation check' }
catch { if ($_ -notmatch 'Truncated DNS pointer') { throw } }
''')
        result = subprocess.run([PWSH, '-NoProfile', '-File', str(script), str(ROOT / 'script/support-reset')],
                                capture_output=True, text=True, timeout=10, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_transport_timeout_and_argument_quoting(self):
        script = self.kit / 'transport-test.ps1'
        script.write_text('''param($Root)
. (Join-Path $Root 'Reset-Transport.ps1')
function Write-ResetLog($Message) { Write-Host $Message }
$result = Invoke-ResetTool '/usr/bin/python3' @('-c', 'import sys,json; print(json.dumps(sys.argv[1:]))', 'a b', 'a"b', 'C:\\folder with space\\')
$values = $result.Lines[0] | ConvertFrom-Json
if ($values[0] -cne 'a b' -or $values[1] -cne 'a"b' -or $values[2] -cne 'C:\\folder with space\\') { throw 'argv changed' }
try { Invoke-ResetTool '/usr/bin/python3' @('-c', 'import time;time.sleep(30)') -TimeoutSeconds 1; throw 'missed timeout' }
catch { if ($_ -notmatch 'timed out') { throw } }
function Read-Host($Prompt) { '2' }
$selected = Select-ClawStageDevice @([pscustomobject]@{Hostname='ClawStage-A.local';IPv4='192.0.2.1'}, [pscustomobject]@{Hostname='ClawStage-B.local';IPv4='192.0.2.2'})
if ($selected.IPv4 -ne '192.0.2.2') { throw 'wrong selection' }
''')
        result = subprocess.run([PWSH, '-NoProfile', '-File', str(script), str(self.kit)],
                                capture_output=True, text=True, timeout=10, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
