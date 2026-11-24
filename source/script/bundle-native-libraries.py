#!/usr/bin/env python3
"""Collect ARM64 ELF dependencies without executing target binaries."""
import argparse
import hashlib
import pathlib
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=pathlib.Path)
parser.add_argument('output', type=pathlib.Path)
parser.add_argument('--vendor-root', type=pathlib.Path)
args = parser.parse_args()
binary, output = args.binary, args.output
vendor = args.vendor_root.resolve() if args.vendor_root else None
vendor_hashes = {}
if vendor:
    for line in (vendor / 'LIB-SHA256SUMS').read_text().splitlines():
        digest, name = line.split(None, 1)
        vendor_hashes[name.strip()] = digest
    if not (vendor / 'SOURCES.json').is_file() or not any((vendor / 'licenses').glob('*')):
        raise RuntimeError('Vendor provenance/licenses missing')
search = [binary.parent, binary.parent / 'shadowlink', output / 'lib',
          pathlib.Path('/usr/lib/aarch64-linux-gnu'),
          pathlib.Path('/lib/aarch64-linux-gnu'), pathlib.Path('/usr/aarch64-linux-gnu/lib'),
          pathlib.Path('/usr/lib'), pathlib.Path('/lib')]
if vendor:
    search.append(vendor / 'lib')
visited = set()


def collect(path, copy=True):
    name = path.name
    if name in visited:
        return
    visited.add(name)
    header = path.read_bytes()[:20]
    if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183:
        raise RuntimeError(f'Expected ARM64 ELF: {path}')
    if copy:
        destination = output / 'lib' / name
        if destination.exists():
            if path.read_bytes() != destination.read_bytes():
                raise RuntimeError(f'Conflicting library: {name}')
        else:
            shutil.copyfile(path, destination)
            destination.chmod(0o755)
        if vendor and path.resolve().parent == vendor / 'lib':
            if hashlib.sha256(path.read_bytes()).hexdigest() != vendor_hashes.get(name):
                raise RuntimeError(f'Vendor library checksum mismatch: {path}')
        elif name != 'libshadowlink_core.so':
            owners = subprocess.run(['dpkg-query', '-S', str(path), str(path.resolve())],
                                    capture_output=True, text=True).stdout
            owner = next((line.split(': /')[0] for line in owners.splitlines()
                          if ': /' in line and not line.startswith('diversion ')), None)
            if not owner:
                raise RuntimeError(f'No package license for {path}')
            package = owner.split(':')[0]
            shutil.copyfile(pathlib.Path('/usr/share/doc') / package / 'copyright',
                            output / 'licenses' / (package + '.copyright'))
            with (output / 'PACKAGES').open('a') as packages:
                subprocess.run(['dpkg-query', '-W', '-f=${binary:Package} ${Version}\n', owner],
                               stdout=packages, check=True)
    dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
    # FFmpeg's libpulse uses $ORIGIN/pulseaudio for libpulsecommon. Follow
    # declared ELF paths rather than assuming every dependency is in /usr/lib.
    runpaths = []
    for group in re.findall(r'\((?:RUNPATH|RPATH)\).*?\[(.*?)\]', dynamic):
        for item in group.split(':'):
            if not item:
                continue
            expanded = item.replace('${ORIGIN}', str(path.resolve().parent)).replace('$ORIGIN', str(path.resolve().parent))
            directory = pathlib.Path(expanded)
            if directory.is_absolute():
                runpaths.append(directory)
    candidates = runpaths + search
    for needed in re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic):
        dependency = next((directory / needed for directory in candidates if (directory / needed).is_file()), None)
        if dependency is None:
            raise RuntimeError(f'Missing {needed}, required by {path}')
        # Already bundled libraries still need their original package ownership.
        if dependency.parent == output / 'lib' and needed != 'libshadowlink_core.so':
            dependency = next((d / needed for d in runpaths + search[3:] if (d / needed).is_file()), dependency)
        collect(dependency)
    program = subprocess.check_output(['readelf', '-l', str(path)], text=True)
    interpreter = re.search(r'Requesting program interpreter: (.*?)\]', program)
    if interpreter:
        collect(pathlib.Path(interpreter[1]))


collect(binary, copy=False)
