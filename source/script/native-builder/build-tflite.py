#!/usr/bin/env python3
"""Build the pinned, Python-free TensorFlow Lite C SDK inside the PC builder."""
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile

from tflite_compat import apply_patches

root = pathlib.Path('/opt/clawstage-xmos')
sources = root / 'sources'
build = root / 'build'
lock = pathlib.Path(__file__).with_name('openwakeword-sources.json')
entries = json.loads(lock.read_text())
sources.mkdir(parents=True, exist_ok=True)
for entry in entries:
    archive = pathlib.Path('/opt/clawstage-source-cache') / (entry['sha256'] + '.tar.gz')
    if hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest() != entry['sha256']:
        sys.exit('Source checksum mismatch: ' + entry['name'])
    destination = sources / entry['name']
    destination.mkdir()
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            parts = pathlib.PurePosixPath(member.name).parts
            if len(parts) < 2:
                continue
            member.name = str(pathlib.PurePosixPath(*parts[1:]))
            package.extract(member, destination, filter='data')
    archive.unlink()

apply_patches(sources)

options = [f'-DFETCHCONTENT_SOURCE_DIR_{entry["name"].upper()}={sources / entry["name"]}'
           for entry in entries if entry['name'] not in ('tensorflow', 'fxdiv', 'pthreadpool')]
subprocess.run([
    'cmake', '-S', str(sources / 'tensorflow/tensorflow/lite/c'), '-B', str(build),
    '-DCMAKE_TOOLCHAIN_FILE=/opt/clawstage-toolchain.cmake', '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_CXX_STANDARD=17', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
    # Preserve the pinned projects' existing FetchContent and install behavior
    # explicitly on CMake >= 3.30/3.31, without hiding compiler diagnostics.
    '-DCMAKE_POLICY_DEFAULT_CMP0169=OLD', '-DCMAKE_POLICY_DEFAULT_CMP0177=OLD',
    # Sources are extracted above, never downloaded by CMake. Declare modern
    # timestamp behavior and avoid exporting this disposable SDK to a user's
    # CMake package registry (also supports builders running without a home).
    '-DCMAKE_POLICY_DEFAULT_CMP0135=NEW', '-DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF',
    '-DTFLITE_C_BUILD_SHARED_LIBS=OFF', '-DTFLITE_ENABLE_XNNPACK=OFF',
    '-DTFLITE_ENABLE_GPU=OFF', '-DTFLITE_ENABLE_NNAPI=OFF',
    '-DTFLITE_ENABLE_RUY=ON', '-DBUILD_TESTING=OFF',
    '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', '-DFLATBUFFERS_BUILD_TESTS=OFF',
    '-DFLATBUFFERS_BUILD_FLATC=OFF', '-DABSL_PROPAGATE_CXX_STD=ON',
    '-DCPUINFO_BUILD_TOOLS=OFF', '-DCPUINFO_BUILD_UNIT_TESTS=OFF',
    '-DCPUINFO_BUILD_MOCK_TESTS=OFF', '-DCPUINFO_BUILD_BENCHMARKS=OFF',
    '-DRUY_FIND_CPUINFO=ON', f'-DCPUINFO_SOURCE_DIR={sources / "cpuinfo"}',
    f'-DFXDIV_SOURCE_DIR={sources / "fxdiv"}', '-DFXDIV_BUILD_TESTS=OFF', '-DFXDIV_BUILD_BENCHMARKS=OFF',
    f'-DPTHREADPOOL_SOURCE_DIR={sources / "pthreadpool"}', *options,
], check=True)
subprocess.run(['cmake', '--build', str(build), '--target', 'tensorflowlite_c', '-j', '2'], check=True)

# Merge static dependency archives so consumers do not need the upstream build tree.
library = root / 'lib/libtensorflowlite.a'
library.parent.mkdir()
archives = sorted(build.rglob('*.a'))
if not any(p.name == 'libtensorflowlite_c.a' for p in archives):
    sys.exit('TensorFlow Lite C archive was not built')
script = f'CREATE {library}\n' + ''.join(f'ADDLIB {p}\n' for p in archives) + 'SAVE\nEND\n'
subprocess.run(['aarch64-linux-gnu-ar', '-M'], input=script, text=True, check=True)
include = root / 'include'
for header in (sources / 'tensorflow/tensorflow').rglob('*.h'):
    dest = include / header.relative_to(sources / 'tensorflow')
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(header, dest)
licenses = root / 'licenses'
licenses.mkdir()
for entry in entries:
    source = sources / entry['name']
    notices = [p for p in source.iterdir() if p.is_file() and
               (p.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')) or p.name == 'readme2d.txt')]
    if not notices:
        sys.exit('Missing license for ' + entry['name'])
    for notice in notices:
        shutil.copyfile(notice, licenses / (entry['name'] + '-' + notice.name))
shutil.copyfile(lock, root / 'SOURCES.json')
shutil.rmtree(build)
shutil.rmtree(sources)
