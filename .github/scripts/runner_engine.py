import argparse
import hashlib
import json
import logging
from logging.handlers import QueueListener
from multiprocessing import Queue
import os
from pathlib import Path
import shutil
import time

from legendary.models.manifest import Manifest
from legendary.downloader.mp.manager import DLManager


def selected(f, with_content=False):
    path = f.filename.lower()
    if f.install_tags and not set(f.install_tags).issubset({'', 'engine_source'}):
        return False
    if path.startswith(('featurepacks/', 'samples/', 'templates/', 'engine/extras/',
                        'engine/documentation/', 'engine/deriveddatacache/')):
        return False
    if ('/content/' in path and not with_content) or '.dsym/' in path or path.endswith(('.pdb', '.dsym')):
        return False
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('manifest', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--analyze', action='store_true')
    parser.add_argument('--with-content', action='store_true')
    args = parser.parse_args()
    manifest = Manifest.read_all(args.manifest.read_bytes())
    original = manifest.file_manifest_list.elements
    files = [f for f in original if selected(f, args.with_content)]
    total = sum(f.file_size for f in files)
    print(json.dumps({'version': manifest.meta.build_version, 'files': len(files),
                      'original_gib': sum(f.file_size for f in original) / 2**30,
                      'selected_gib': total / 2**30}), flush=True)
    if args.analyze:
        return
    args.destination.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(args.destination).free
    if free < total + 8 * 2**30:
        raise RuntimeError(f'Need {total / 2**30 + 8:.1f} GiB including build headroom; have {free / 2**30:.1f}')
    manifest.file_manifest_list.elements = files
    for f in files:
        resolved = (args.destination / f.filename).resolve()
        if not resolved.is_relative_to(args.destination.resolve()):
            raise RuntimeError('Manifest path escapes destination')
    manager = DLManager(str(args.destination),
                        'https://epicgames-download1.akamaized.net/Builds/UE5/Releases/CloudDir',
                        {}, max_workers=8, max_shared_memory=1024 * 2**20,
                        update_interval=30, dl_timeout=60)
    manager.run_analysis(manifest, resume=False, processing_optimization=True)
    manager.logging_queue = Queue()
    listener = QueueListener(manager.logging_queue, logging.StreamHandler())
    listener.start()
    manager.start()
    started = time.monotonic()
    while manager.is_alive():
        manager.join(timeout=30)
        print(f'Download elapsed {time.monotonic() - started:.0f}s; free disk {shutil.disk_usage(args.destination).free / 2**30:.1f} GiB', flush=True)
    if manager.exitcode != 0:
        raise RuntimeError(f'Downloader failed: {manager.exitcode}')
    listener.stop()
    for index, f in enumerate(files, 1):
        if index % 10000 == 0:
            print(f'Verifying engine files: {index}/{len(files)}', flush=True)
        path = args.destination / f.filename
        if f.symlink_target:
            if not path.is_symlink():
                raise RuntimeError(f'Missing symlink: {f.filename}')
            continue
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha1').digest()
        if digest != f.sha_hash:
            raise RuntimeError(f'Hash mismatch: {f.filename}')
        if f.executable and os.name != 'nt':
            path.chmod(path.stat().st_mode | 0o111)
    print(f'Verified {len(files)} selected engine files', flush=True)


if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO)
    main()
