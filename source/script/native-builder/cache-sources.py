#!/usr/bin/env python3
"""Cache pinned native source archives outside disposable Docker build layers."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def obtain(entry, cache):
    target = cache / (entry['sha256'] + '.tar.gz')
    if target.exists():
        if digest(target) != entry['sha256']:
            raise ValueError('Invalid cached source: ' + entry['name'])
        print('Cache hit: ' + entry['name'], flush=True)
        return target
    partial = target.with_suffix('.part')
    urls = [entry['url']]
    mirror = 'https://storage.googleapis.com/mirror.tensorflow.org/'
    if entry['url'].startswith(mirror):
        # Retry the original upstream named by TensorFlow's mirror; keep the same checksum.
        urls.append('https://' + entry['url'][len(mirror):])
    for attempt in range(4):
        print(f'Download {entry["name"]}: attempt {attempt + 1}', flush=True)
        result = subprocess.run([
            'curl', '-fL', '--connect-timeout', '20', '--max-time', '180',
            '--speed-limit', '1024', '--speed-time', '30', '--continue-at', '-',
            urls[attempt % len(urls)], '-o', str(partial),
        ])
        if result.returncode == 0:
            if digest(partial) != entry['sha256']:
                raise ValueError('Source checksum mismatch: ' + entry['name'])
            partial.replace(target)
            return target
        if result.returncode == 33:  # The server does not support resuming this response.
            partial.unlink(missing_ok=True)
        elif result.returncode not in (5, 6, 7, 18, 28, 35, 52, 56):
            raise RuntimeError(f'Source download failed: {entry["name"]}, curl={result.returncode}')
        if attempt < 3:
            time.sleep(2 ** attempt)
    raise RuntimeError('Source download exhausted retries; partial retained: ' + entry['name'])


def prepare(lock, cache, output):
    cache.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    entries = json.loads(lock.read_text())
    with ThreadPoolExecutor(max_workers=2) as workers:
        archives = list(workers.map(lambda entry: obtain(entry, cache), entries))
    for archive in archives:
        shutil.copyfile(archive, output / archive.name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('lock', type=Path)
    parser.add_argument('cache', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    prepare(args.lock, args.cache, args.output)
