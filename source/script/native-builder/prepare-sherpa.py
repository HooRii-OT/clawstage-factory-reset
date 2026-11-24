#!/usr/bin/env python3
"""Prepare pinned upstream C SDK/models from verified build-host downloads only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile


def prepare(lock, cache, output):
    entries = json.loads(lock.read_text())
    sources = {}
    for entry in entries:
        path = cache / (entry['sha256'] + '.tar.gz')
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Source checksum mismatch: ' + entry['name'])
        sources[entry['name']] = path
    output.mkdir(parents=True, exist_ok=True)
    for name in ('lib', 'include/sherpa-onnx/c-api', 'models', 'licenses'):
        (output / name).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        unpacked = Path(temporary)
        for name in ('sherpa-sdk', 'kws-model'):
            destination = unpacked / name
            destination.mkdir()
            with tarfile.open(sources[name]) as archive:
                for member in archive.getmembers():
                    path = Path(member.name)
                    if path.is_absolute() or '..' in path.parts or not (member.isfile() or member.isdir()):
                        raise ValueError('Unsafe archive member: ' + member.name)
                archive.extractall(destination, filter='data')
        sdk = next((unpacked / 'sherpa-sdk').iterdir())
        model = next((unpacked / 'kws-model').iterdir())
        for name in ('libsherpa-onnx-c-api.so', 'libonnxruntime.so'):
            shutil.copyfile(sdk / 'lib' / name, output / 'lib' / name)
        for target, source in {
            'encoder.onnx': 'encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx',
            'decoder.onnx': 'decoder-epoch-12-avg-2-chunk-16-left-64.onnx',
            'joiner.onnx': 'joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx',
            'tokens.txt': 'tokens.txt',
            'bpe.model': 'bpe.model',
        }.items():
            shutil.copyfile(model / source, output / 'models' / target)
        shutil.copyfile(model / 'README.md', output / 'licenses/kws-model.README.md')
        # Keep official speech fixtures in the builder, never in the device payload.
        shutil.copytree(model / 'test_wavs', output / 'test_wavs', dirs_exist_ok=True)
    shutil.copyfile(sources['sherpa-header'], output / 'include/sherpa-onnx/c-api/c-api.h')
    for name in ('sherpa-license', 'onnx-license', 'onnx-notices'):
        shutil.copyfile(sources[name], output / 'licenses' / name)
    shutil.copyfile(lock, output / 'SOURCES.json')
    (output / 'LIB-SHA256SUMS').write_text(''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n'
        for p in sorted((output / 'lib').iterdir())))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--lock', type=Path, default=Path(__file__).with_name('xmos-sources.json'))
    p.add_argument('--cache', type=Path, default=Path('/opt/clawstage-source-cache'))
    p.add_argument('--output', type=Path, default=Path('/opt/clawstage-sherpa'))
    args = p.parse_args()
    prepare(args.lock, args.cache, args.output)
