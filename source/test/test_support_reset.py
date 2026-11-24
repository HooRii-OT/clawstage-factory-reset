"""Support transport cannot turn an unknown result or corrupt payload into success."""
import hashlib
from concurrent.futures import ThreadPoolExecutor
import time
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('support_reset', ROOT / 'script/support-reset/device-reset.py')
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)


class SupportResetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def archive(self, extra=None, corrupt=False):
        archive = self.root / 'payload.tar.gz'
        files = {name: b'fixture' for name in ('factory-reset.sh', 'health-check.sh', 'stack-install.sh',
                                             'shadowlink/runtime/shadowlink-server')}
        manifest = {'source': {'commit': 'fixture'}, 'files': {
            k: hashlib.sha256(v).hexdigest() for k, v in files.items()}}
        if corrupt:
            files['factory-reset.sh'] = b'corrupt'
        files['manifest.json'] = json.dumps(manifest).encode()
        with tarfile.open(archive, 'w:gz') as tar:
            for name, data in files.items():
                info = tarfile.TarInfo(name); info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
            if extra:
                tar.addfile(extra)
        return archive

    def test_payload_validates_inventory_and_bytes(self):
        support.unpack(self.archive(), self.root / 'good')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            support.unpack(self.archive(corrupt=True), self.root / 'bad')

    def test_payload_rejects_traversal_links_and_duplicates_before_extracting(self):
        for name, kind in (('../escape', tarfile.REGTYPE), ('/escape', tarfile.REGTYPE),
                           ('link', tarfile.SYMTYPE), ('hardlink', tarfile.LNKTYPE),
                           ('factory-reset.sh', tarfile.REGTYPE)):
            with self.subTest(name=name):
                entry = tarfile.TarInfo(name); entry.type = kind; entry.linkname = '/etc/shadow'
                with self.assertRaisesRegex(ValueError, 'Unsafe'):
                    support.unpack(self.archive(entry), self.root / 'bad')
                self.assertFalse((self.root / 'bad').exists())

    def test_preflight_accepts_versions_by_installed_contract(self):
        for name in ('user/.hoorii/config.json', 'openclaw/hoorii_claw/script/clear.sh',
                     'homeassistant/stage-stack/compose.release.yaml', 'homeassistant/stage-stack/install.sh'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        for version in ('0.2.6', '0.2.16-test4', 'future-version', None):
            with self.subTest(version=version):
                path = self.root / 'VERSION'
                if version is None:
                    path.unlink()
                else:
                    path.write_text(version)
                with patch.object(support.os, 'uname', return_value=SimpleNamespace(machine='aarch64', nodename='fixture')), \
                        patch.object(support.shutil, 'which', return_value='/fixture/bin'), \
                        patch.object(support.subprocess, 'run') as run:
                    self.assertEqual(support.preflight(self.root), self.root)
                    run.assert_called_once_with(['docker', 'compose', 'version'], check=True,
                                                stdout=subprocess.DEVNULL)

    def test_missing_installed_contract_still_refuses_reset(self):
        (self.root / 'VERSION').write_text('0.2.16-test4')
        with patch.object(support.os, 'uname', return_value=SimpleNamespace(machine='aarch64', nodename='fixture')), \
                patch.object(support.shutil, 'which', return_value='/fixture/bin'), \
                patch.object(support.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'Missing required installed file'):
                support.preflight(self.root)
            run.assert_not_called()

    def test_existing_run_cannot_start_again(self):
        with patch.object(support, 'preflight') as preflight:
            with self.assertRaisesRegex(ValueError, 'already exists'):
                support.start(self.root, self.root / 'unused', 'unused')
            preflight.assert_not_called()

    def test_follow_replays_full_log_and_preserves_failure(self):
        (self.root / 'request.json').write_text('{}')
        (self.root / 'reset.log').write_bytes(b'STEP detail\nlast error\n')
        support.save_result(self.root, 41)
        output = io.TextIOWrapper(io.BytesIO(), encoding='utf-8')
        with patch.object(support.sys, 'stdout', output), patch.object(support.subprocess, 'run') as run:
            self.assertEqual(support.follow(self.root), 41)
            self.assertEqual(output.buffer.getvalue(), b'STEP detail\nlast error\n')
            run.assert_not_called()

    def test_follow_dead_service_without_result_is_unknown(self):
        (self.root / 'request.json').write_text('{}')
        with patch.object(support.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'failed\n')):
            self.assertEqual(support.follow(self.root), 3)

    def test_failed_service_start_does_not_claim_success(self):
        archive = self.archive()
        run = self.root / 'runs' / ('a' * 32)
        with patch.object(support, 'BASE', run.parent), patch.object(support, 'preflight', return_value=self.root), \
                patch.object(support.subprocess, 'run', side_effect=[subprocess.CompletedProcess([], 0, ''), None, subprocess.CalledProcessError(1, 'systemd-run')]):
            with self.assertRaises(subprocess.CalledProcessError):
                support.start(run, archive, support.digest(archive))
        self.assertFalse((run / 'result.json').exists())

    def test_running_task_rejects_new_start_before_payload_or_cleanup(self):
        run = self.root / ('a' * 32)
        output = 'clawstage-support-reset-' + 'b' * 32 + '.service loaded active running reset'
        with patch.object(support, 'BASE', self.root), \
                patch.object(support.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, output)), \
                patch.object(support, 'start_locked') as launch:
            with self.assertRaises(support.ResetBusyError) as error:
                support.start(run, self.root / 'unused', 'unused')
            self.assertEqual(error.exception.run_id, 'b' * 32)
            launch.assert_not_called()
        self.assertFalse(run.exists())

    def test_parallel_launch_requests_admit_only_one_worker(self):
        active = [None]
        def launch(run, *_):
            time.sleep(0.1)  # Simulate payload setup before systemd starts the unit.
            active[0] = run.name
        def attempt(name):
            try:
                support.start(self.root / name, self.root / 'unused', 'unused')
                return 'started'
            except support.ResetBusyError:
                return 'busy'
        with patch.object(support, 'BASE', self.root), \
                patch.object(support, 'active_support_run', side_effect=lambda: active[0]), \
                patch.object(support, 'start_locked', side_effect=launch) as start:
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(attempt, c * 32) for c in ('a', 'b')]
                self.assertCountEqual([f.result(timeout=5) for f in futures], ['started', 'busy'])
            self.assertEqual(start.call_count, 1)
            active[0] = None  # A completed unit no longer blocks the next reset.
            self.assertEqual(attempt('c' * 32), 'started')
            self.assertEqual(start.call_count, 2)

    def test_systemd_query_failure_refuses_launch(self):
        with patch.object(support, 'BASE', self.root), \
                patch.object(support.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'systemctl')), \
                patch.object(support, 'start_locked') as launch:
            with self.assertRaises(subprocess.CalledProcessError):
                support.start(self.root / ('a' * 32), self.root / 'unused', 'unused')
            launch.assert_not_called()

    def test_worker_logs_in_beijing_time_without_changing_parent_timezone(self):
        (self.root / 'request.json').write_text(json.dumps({'release': str(self.root), 'source': {}}))
        payload = self.root / 'payload'
        payload.mkdir()
        (payload / 'factory-reset.sh').write_text(
            '#!/bin/bash\nprintf "[%(%Y-%m-%dT%H:%M:%S%z)T] timezone-check\\n" 0\n'
            '[ "$TZ" = Asia/Shanghai ]\n')
        # worker redirects its process FDs, so exercise it in a disposable child.
        result = subprocess.run([sys.executable, '-c', '''
import importlib.util, os, pathlib, sys
spec = importlib.util.spec_from_file_location('support', sys.argv[1])
support = importlib.util.module_from_spec(spec); spec.loader.exec_module(support)
run = pathlib.Path(sys.argv[2])
support.preflight = lambda: run
os.environ['TZ'] = 'Etc/GMT+8'
code = support.worker(run)
assert os.environ['TZ'] == 'Etc/GMT+8'
sys.exit(code)
''', str(ROOT / 'script/support-reset/device-reset.py'), str(self.root)],
                                capture_output=True, text=True, timeout=5)
        log = (self.root / 'reset.log').read_text()
        self.assertEqual(result.returncode, 0, result.stderr + log)
        self.assertIn('[1970-01-01T08:00:00+0800] timezone-check', log)
        self.assertEqual(json.loads((self.root / 'result.json').read_text())['exit_code'], 0)


if __name__ == '__main__':
    unittest.main()
