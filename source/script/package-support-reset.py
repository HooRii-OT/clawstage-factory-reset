#!/usr/bin/env python3
"""Freeze an auditable Windows/Linux/macOS support kit from current sources and a matching ARM64 bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PUTTY_FILES = {
    'plink.exe': '969f36879d5716aa1a9811f43a6a6510e8f08372dbeb9695b810b9c776f39c75',
    'pscp.exe': '4275310c04b4dd00cc139f93b932267df7816fda8e362d4a845660129264f3a1',
}


CLIENT_FILES = (
    'device-reset.py', 'Reset-ClawStage.ps1', 'Reset-ClawStage.cmd',
    'Find-ClawStage.ps1', 'Reset-Transport.ps1', 'reset-client.py',
    'discover_devices.py', 'ssh-askpass.sh', 'Reset-ClawStage.sh',
    'Reset-ClawStage.command',
)
PUBLIC_DOCS = ('HELP.md', 'FILES.md', 'OPERATIONS.md', 'PRIVACY.md', 'THIRD_PARTY.md')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_sums(base, manifest):
    for line in manifest.read_text().splitlines():
        expected, name = line.split(None, 1)
        if digest(base / name.lstrip('*')) != expected:
            raise ValueError('Stale/corrupt runtime input: ' + name)


def package(runtime, output, windows_tools):
    for name, expected in PUTTY_FILES.items():
        if digest(windows_tools / name) != expected:
            raise ValueError('Requires official PuTTY 0.85 Windows x64 binary: ' + name)
    if digest(windows_tools / 'LICENCE') != '3b02f87e0063430289d9681727a98818c844fc8a4d79b24b76047fa061e77f13':
        raise ValueError('Requires the PuTTY 0.85 source LICENCE')
    verify_sums(runtime, runtime / 'SHA256SUMS')
    listed = {Path(line.split(None, 1)[1].lstrip('*')).as_posix()
              for line in (runtime / 'SHA256SUMS').read_text().splitlines()}
    actual = {p.relative_to(runtime).as_posix() for p in runtime.rglob('*') if p.is_file()}
    if actual != listed | {'SHA256SUMS'}:
        raise ValueError('Runtime contains unlisted or missing files')
    verify_sums(ROOT / 'resource/shadowlink/native', runtime / 'SOURCE-SHA256SUMS')
    if 'architecture=aarch64' not in (runtime / 'BUILD-INFO').read_text().splitlines():
        raise ValueError('Requires ARM64 runtime')
    def git(*args):
        try:
            return subprocess.check_output(['git', *args], cwd=ROOT, text=True,
                                           stderr=subprocess.DEVNULL).strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            return None

    source = {
        'commit': git('rev-parse', 'HEAD'),
        'branch': git('branch', '--show-current'),
        'status': git('status', '--short'),
        'version': (ROOT / 'VERSION').read_text().strip(),
        'target_policy': 'installed-reset-prerequisites; no firmware version restriction',
    }
    if source['commit'] is None:
        previous_manifest = ROOT.parent / 'package.json'
        if not previous_manifest.is_file():
            raise ValueError('Source needs Git metadata or the original exported package.json')
        previous = json.loads(previous_manifest.read_text())['source']
        source['original_source'] = previous.get('original_source', previous)
        source['status'] = 'Exported source without Git metadata; current contents recorded by file hashes'
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary); kit = work / 'ClawStage-Factory-Reset'; kit.mkdir()
        payload = kit / 'payload'; payload.mkdir()
        shutil.copytree(runtime, payload / 'shadowlink/runtime')
        for src, dst in [('factory-reset.sh', 'factory-reset.sh'), ('health-check.sh', 'health-check.sh'),
                         ('resource/homeassistant/stage-stack-bundle-online/install.sh', 'stack-install.sh')]:
            shutil.copyfile(ROOT / src, payload / dst); (payload / dst).chmod(0o755)
        manifest = {'source': source, 'files': {p.relative_to(payload).as_posix(): digest(p)
                    for p in sorted(payload.rglob('*')) if p.is_file()}}
        (payload / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with tarfile.open(kit / 'payload.tar.gz', 'w:gz') as tar:
            for path in sorted(payload.rglob('*')):
                if path.is_file():
                    tar.add(path, arcname=path.relative_to(payload).as_posix(), recursive=False)
        for name in CLIENT_FILES:
            shutil.copyfile(ROOT / 'script/support-reset' / name, kit / name)
            if name.endswith(('.sh', '.command')):
                (kit / name).chmod(0o755)
            if name.endswith('.cmd'):
                (kit / name).write_bytes((kit / name).read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        for name in PUTTY_FILES:
            shutil.copyfile(windows_tools / name, kit / name)
        shutil.copyfile(windows_tools / 'LICENCE', kit / 'PUTTY-LICENCE.txt')
        (kit / 'PUTTY-SOURCE.json').write_text(json.dumps({
            'version': '0.85',
            'source_url': 'https://the.earth.li/~sgtatham/putty/0.85/putty-0.85.tar.gz',
            'source_sha256': '13fd4db2936d03b73812a7bcc2a658e4dd29cc776a56c3670a7fc6f1a0ee8af8',
            'files': {name: {
                'url': 'https://the.earth.li/~sgtatham/putty/0.85/w64/' + name,
                'sha256': expected} for name, expected in PUTTY_FILES.items()}}, indent=2) + '\n')
        # Keep one public documentation tree, plus build notes with the source.
        # The same exporter also works from the lean public source/ directory.
        repository_docs = ROOT / 'docs/support-reset'
        if repository_docs.is_dir():
            readme_path = repository_docs / 'README.md'
            docs_dir = repository_docs / 'details'
            build_path = docs_dir / 'BUILD.md'
        else:
            readme_path = ROOT.parent / 'README.md'
            docs_dir = ROOT.parent / 'docs'
            build_path = ROOT / 'BUILD.md'
        readme = readme_path.read_text(encoding='utf-8')
        (kit / 'README.md').write_text(readme.replace('](details/', '](docs/'), encoding='utf-8')
        (kit / 'docs').mkdir()
        for name in PUBLIC_DOCS:
            contents = (docs_dir / name).read_text(encoding='utf-8')
            contents = contents.replace('](BUILD.md)', '](../source/BUILD.md)')
            (kit / 'docs' / name).write_text(contents, encoding='utf-8')
        (kit / 'source').mkdir()
        build_notes = build_path.read_text(encoding='utf-8')
        build_notes = build_notes.replace('](THIRD_PARTY.md)', '](../docs/THIRD_PARTY.md)')
        (kit / 'source/BUILD.md').write_text(build_notes, encoding='utf-8')
        (kit / '.gitignore').write_text('*.log\n.clawstage-*\n__pycache__/\n*.pyc\n.DS_Store\ndist/\n')
        # Preserve exact bytes: package hashes are over the published files.
        (kit / '.gitattributes').write_text('* -text\n*.exe binary\n*.gz binary\n')
        # Explicit source allowlist; never copy private config, git metadata or logs.
        inputs = {ROOT / 'VERSION', ROOT / 'factory-reset.sh', ROOT / 'health-check.sh',
                  ROOT / 'resource/homeassistant/stage-stack-bundle-online/install.sh',
                  ROOT / 'script/package-support-reset.py'}
        inputs.update(ROOT / 'script/support-reset' / name for name in CLIENT_FILES)
        native = ROOT / 'resource/shadowlink/native'
        for line in (runtime / 'SOURCE-SHA256SUMS').read_text().splitlines():
            _, name = line.split(None, 1)
            inputs.add((native / name.lstrip('*')).resolve())
        for folder, patterns in [
            ('resource/shadowlink/native/test', ('*.c', '*.h')),
        ]:
            for pattern in patterns:
                inputs.update((ROOT / folder).rglob(pattern))
        for name in ('bundle-validation.sh',):
            inputs.add(native / name)
        for name in ('test_support_reset.py', 'test_windows_support_reset.py', 'test_unix_support_reset.py'):
            inputs.add(ROOT / 'test' / name)
        for path in sorted(inputs):
            relative = path.resolve().relative_to(ROOT)
            destination = kit / 'source' / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        (kit / 'package.json').write_text(json.dumps({'source': source, 'files': {
            p.relative_to(kit).as_posix(): digest(p) for p in sorted(kit.rglob('*'))
            if p.is_file()}}, indent=2) + '\n')
        output.parent.mkdir(parents=True, exist_ok=True)
        # Refuse overwrite so an operator cannot accidentally delete customer logs.
        exported = output.with_suffix('')
        if output.exists() or exported.exists():
            raise ValueError('Output ZIP or folder exists; choose a fresh output path')
        shutil.copytree(kit, exported)
        with zipfile.ZipFile(output, 'x', zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(kit.rglob('*')):
                if path.is_file():
                    archive.write(path, 'ClawStage-Factory-Reset/' + path.relative_to(kit).as_posix())
        output.with_suffix(output.suffix + '.sha256').write_text(digest(output) + '  ' + output.name + '\n')
    print(f'{output}\nSHA256={digest(output)}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=ROOT / 'resource/shadowlink/runtime')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-tools', type=Path, required=True,
                        help='Directory containing official PuTTY 0.85 x64 plink.exe, pscp.exe and LICENCE')
    args = parser.parse_args()
    package(args.runtime.resolve(), args.output.resolve(), args.windows_tools.resolve())
