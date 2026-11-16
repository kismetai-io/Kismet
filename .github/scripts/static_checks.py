#!/usr/bin/env python3
"""Static checks that need no Unreal Engine. Run from the repository root.

They catch broken configuration and mistakes a macOS or Windows build hides.
They do not compile the plugin or run its automation tests.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PLUGIN = ROOT / 'KismetAI'
SOURCE = PLUGIN / 'Source'
errors: list[str] = []


def fail(message: str) -> None:
    errors.append(message)


def check_json() -> int:
    """Every configuration file and the plugin descriptor parse as JSON."""
    files = sorted(PLUGIN.glob('Config/*.json')) + [PLUGIN / 'KismetAI.uplugin']
    for path in files:
        try:
            json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError) as error:
            fail(f'{path.relative_to(ROOT)}: invalid JSON: {error}')
    return len(files)


def check_modules() -> int:
    """Each module in the descriptor has a folder and a matching Build.cs."""
    descriptor = json.loads((PLUGIN / 'KismetAI.uplugin').read_text(encoding='utf-8'))
    modules = [module['Name'] for module in descriptor.get('Modules', [])]
    for name in modules:
        if not (SOURCE / name / f'{name}.Build.cs').is_file():
            fail(f'module {name}: KismetAI/Source/{name}/{name}.Build.cs not found')
    declared = set(modules)
    for folder in sorted(p for p in SOURCE.iterdir() if p.is_dir()):
        if folder.name not in declared:
            fail(f'KismetAI/Source/{folder.name}: not listed in KismetAI.uplugin')
    return len(modules)


def check_python() -> int:
    """The helper scripts are valid Python."""
    files = sorted(PLUGIN.glob('Scripts/*.py')) + sorted((ROOT / '.github' / 'scripts').glob('*.py'))
    for path in files:
        try:
            compile(path.read_text(encoding='utf-8'), str(path), 'exec')
        except SyntaxError as error:
            fail(f'{path.relative_to(ROOT)}: {error}')
    return len(files)


def check_panel_scripts() -> int:
    """Every inline script in the panel UI parses."""
    html = (PLUGIN / 'Config' / 'ui.html').read_text(encoding='utf-8')
    scripts = [body for body in re.findall(r'<script\b[^>]*>(.*?)</script>', html, re.S) if body.strip()]
    checker = (
        "const vm=require('node:vm');let d='';process.stdin.on('data',c=>d+=c).on('end',()=>{"
        "let bad=0;JSON.parse(d).forEach((s,i)=>{try{new vm.Script(s)}catch(e){bad++;"
        "console.error('script block '+(i+1)+': '+e.message)}});process.exit(bad?1:0)})"
    )
    try:
        result = subprocess.run(['node', '-e', checker], input=json.dumps(scripts), text=True, capture_output=True)
    except FileNotFoundError:
        fail('node is required to check Config/ui.html')
        return 0
    if result.returncode:
        fail('KismetAI/Config/ui.html: ' + (result.stderr.strip() or 'a script block does not parse'))
    return len(scripts)


def check_include_case() -> int:
    """An #include of a plugin header uses the file's exact letter case.

    macOS and Windows file systems ignore case, so a wrong-case include builds
    there and fails on Linux.
    """
    headers = [p for p in SOURCE.rglob('*') if p.suffix in {'.h', '.inl'}]
    exact = {p.name for p in headers}
    by_lower: dict[str, set[str]] = {}
    for p in headers:
        by_lower.setdefault(p.name.lower(), set()).add(p.name)
    pattern = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
    checked = 0
    for path in SOURCE.rglob('*'):
        if path.suffix not in {'.h', '.cpp', '.inl'}:
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        for match in pattern.finditer(text):
            name = match.group(1).rsplit('/', 1)[-1]
            checked += 1
            if name in exact or name.lower() not in by_lower:
                continue  # exact match, or an engine header this check cannot see
            line = text.count('\n', 0, match.start()) + 1
            wanted = ', '.join(sorted(by_lower[name.lower()]))
            fail(f'{path.relative_to(ROOT)}:{line}: #include "{match.group(1)}" does not match the case of {wanted}')
    return checked


def check_no_generated_files() -> int:
    """Build output and local files are not committed."""
    tracked = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], capture_output=True, text=True, check=True
                             ).stdout.splitlines()
    forbidden_parts = {'Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '__pycache__'}
    forbidden_suffixes = {'.dll', '.dylib', '.so', '.pdb', '.pem', '.key', '.p12', '.env', '.uasset', '.umap'}
    for rel in tracked:
        path = Path(rel)
        if forbidden_parts & set(path.parts) or path.suffix.lower() in forbidden_suffixes or path.name == '.env':
            fail(f'{rel}: build output or local file is committed')
    return len(tracked)


CHECKS = [
    ('configuration parses as JSON', check_json, 'files'),
    ('modules match the descriptor', check_modules, 'modules'),
    ('helper scripts are valid Python', check_python, 'files'),
    ('panel UI scripts parse', check_panel_scripts, 'script blocks'),
    ('include letter case', check_include_case, 'includes'),
    ('no build output committed', check_no_generated_files, 'tracked files'),
]


def main() -> int:
    for title, check, unit in CHECKS:
        before = len(errors)
        count = check()
        status = 'FAIL' if len(errors) > before else 'ok'
        print(f'{status:4}  {title} ({count} {unit})')
    for message in errors:
        print(f'error: {message}', file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
