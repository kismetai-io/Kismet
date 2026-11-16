#!/usr/bin/env python3
"""Create a deterministic, source-only plugin archive and scan its exact contents."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TREES = ('Source', 'Config', 'Resources', 'ThirdPartyNotices')
# Contributor docs that sit at the repository root, one level above the plugin.
# They are added to the archive beside the plugin's own documents.
REPO_ROOT = ROOT.parent
REPO_FILES = ('CONTRIBUTING.md', 'MAINTAINING.md', 'SECURITY.md')
FILES = ('KismetAI.uplugin', 'README.md', 'INSTALL.md', 'LICENSE', 'NOTICE.md',
         'SERVICE_TERMS.md', 'package.ps1',
         'Scripts/embed_config.py', 'Scripts/set-embed.sh', 'Scripts/rebuild.sh',
         'Scripts/package_source.py')


def source_files() -> list[Path]:
    candidates = [ROOT / name for name in FILES if (ROOT / name).is_file()]
    for name in TREES:
        candidates.extend(p for p in (ROOT / name).rglob('*') if p.is_file())
    for path in candidates:
        rel = path.relative_to(ROOT)
        if path.is_symlink() or any(part.startswith('.') or part == '__pycache__' for part in rel.parts):
            raise ValueError(f'Unexpected hidden file or symlink: {rel}')
        if path.suffix.lower() in {'.env', '.pem', '.key', '.p12', '.uasset', '.umap', '.dylib', '.dll', '.so', '.pyc'}:
            raise ValueError(f'Unexpected private/generated/engine file: {rel}')
    return sorted(set(candidates))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='New ZIP path (existing files are never overwritten)')
    args = parser.parse_args()
    scanner = shutil.which('gitleaks')
    if not scanner:
        parser.error('Install gitleaks before preparing a public source archive')
    files = source_files()
    for required in ('LICENSE', 'NOTICE.md', 'README.md', 'INSTALL.md'):
        if ROOT / required not in files:
            parser.error(f'Missing required public document: {required}')
    with tempfile.TemporaryDirectory(prefix='kismet-source-') as temp:
        stage = Path(temp) / 'KismetAI'
        for path in files:
            dest = stage / path.relative_to(ROOT)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, dest)
        repo_docs = [REPO_ROOT / name for name in REPO_FILES if (REPO_ROOT / name).is_file()]
        for path in repo_docs:
            shutil.copyfile(path, stage / path.name)
        subprocess.run([scanner, 'dir', str(stage), '--redact', '--no-banner'], check=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.output, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in files:
                name = 'KismetAI/' + path.relative_to(ROOT).as_posix()
                info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o100755 if path.suffix in {'.sh', '.py'} else 0o100644) << 16
                archive.writestr(info, (stage / path.relative_to(ROOT)).read_bytes())
            for path in sorted(repo_docs):
                info = zipfile.ZipInfo('KismetAI/' + path.name, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, (stage / path.name).read_bytes())
    print(json.dumps({'archive': str(args.output.resolve()), 'files': len(files) + len(repo_docs),
                      'sha256': hashlib.sha256(args.output.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
