import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

engine = Path(os.environ['UE_ROOT'])
packages = Path(os.environ['PACKAGE_ROOT'])
project = Path(os.environ['RUNNER_TEMP']) / 'KismetAutomation'
project.mkdir(exist_ok=True)
plugins = project / 'Plugins'
plugins.mkdir(exist_ok=True)
archives = list(packages.glob('*-release-src.zip'))
assert len(archives) == 1, archives
with zipfile.ZipFile(archives[0]) as archive:
    archive.extractall(plugins)
uproject = project / 'KismetAutomation.uproject'
uproject.write_text(json.dumps({'FileVersion': 3, 'EngineAssociation': os.environ.get('ENGINE_VERSION', '5.8'),
    'Plugins': [{'Name': 'KismetAI', 'Enabled': True}]}))
report = packages / 'AutomationReport'
report.mkdir(exist_ok=True)
editor = engine / ('Engine/Binaries/Win64/UnrealEditor-Cmd.exe' if os.name == 'nt'
                   else 'Engine/Binaries/Mac/UnrealEditor-Cmd')
command = [str(editor), str(uproject), '-ExecCmds=Automation RunTests KismetAI+KismetLayoutCore',
           '-TestExit=Automation Test Queue Empty', '-unattended', '-nopause',
           '-NullRHI', '-nosplash', '-nosound', '-stdout', '-FullStdOutLogOutput',
           f'-ReportExportPath={report}', f'-abslog={report / "Editor.log"}']
if sys.platform == 'darwin':
    # The 7 GiB runner otherwise blocks on Unreal's modal 8 GiB hardware advisory.
    command.extend(['-skipminspeccheck', '-buildmachine'])
with (report / 'process.log').open('w') as log:
    try:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=600)
        except subprocess.TimeoutExpired:
            try:
                if sys.platform == 'darwin':
                    subprocess.run(['sample', str(process.pid), '5', '-file', str(report / 'startup-sample.txt')], timeout=30)
            finally:
                process.kill()
                process.wait()
            raise
        result = process
    except subprocess.TimeoutExpired:
        raise SystemExit('Automation exceeded 10 minutes; see process.log')
index = report / 'index.json'
if not index.exists():
    print((report / 'process.log').read_text(errors='replace')[-16000:])
    raise SystemExit(f'No automation report; editor exit {result.returncode}')
data = json.loads(index.read_text(encoding='utf-8-sig'))
print(json.dumps({key: data.get(key) for key in ('succeeded', 'succeededWithWarnings', 'failed', 'notRun', 'totalDuration')}, indent=2))
if result.returncode or data.get('failed', 0) or data.get('notRun', 0) or data.get('inProcess', 0):
    raise SystemExit('Automation failed or left tests unrun')
passed = data.get('succeeded', 0) + data.get('succeededWithWarnings', 0)
if passed < 1000:
    raise SystemExit(f'Incomplete discovery: only {passed} tests passed')
