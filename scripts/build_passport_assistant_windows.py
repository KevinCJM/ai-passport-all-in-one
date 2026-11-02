#!/usr/bin/env python3
"""Build the Windows preview (x64): a PyInstaller folder in a zip, self-tested, with checksums and notices.

Run on Windows after scripts/build_windows_helpers.sh (MSYS2 UCRT64) has put passport-ppp.exe, ffmpeg.exe and
their DLLs in experiments/passport-connectivity-tools/bin. CI: .github/workflows/windows-assistant.yml.
No installer: unzip, run "AI Passport Assistant.exe"; it sets up start-with-Windows itself (per-user Run key).
"""
import hashlib
import importlib.metadata as metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from build_passport_assistant import VERSION,BUILD_NUMBER  # one version for both platforms

TOOLS=ROOT/'experiments/passport-connectivity-tools/bin'
WORK=ROOT/'experiments/passport-assistant-build/windows'
DIST=ROOT/f'releases/ai-passport-assistant-{VERSION}-preview-windows'
NAME='AI Passport Assistant'
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()


def run(args,**options):
    print('BUILD',Path(str(args[0])).name,flush=True)
    subprocess.run(list(map(str,args)),check=True,**options)


def notices():
    """Python packages' licence files and the MSYS2 helpers' (copied by build_windows_helpers.sh)."""
    target=WORK/'licenses'
    if target.exists():shutil.rmtree(target)
    shutil.copytree(TOOLS/'licenses',target/'native')
    packages=[]
    for dist in metadata.distributions():
        name=dist.metadata['Name']
        if name.lower().startswith(('pyinstaller','setuptools','pip','altgraph','pefile','pywin32-ctypes','packaging')):continue
        folder=target/name;folder.mkdir(exist_ok=True)
        for file in dist.files or []:
            if any(part.upper().startswith(('LICENSE','COPYING','NOTICE')) for part in file.parts):
                path=Path(dist.locate_file(file))
                if path.is_file():shutil.copy2(path,folder/path.name)
        packages.append({'name':name,'version':dist.version,'license':dist.metadata.get('License-Expression') or dist.metadata.get('License','')})
    (target/'components.json').write_text(json.dumps({'python_packages':packages,'native':sorted(p.name for p in TOOLS.iterdir() if p.is_file())},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return target


def main():
    if sys.platform!='win32':raise SystemExit('build on Windows (see .github/workflows/windows-assistant.yml)')
    helpers=[TOOLS/'passport-ppp.exe',TOOLS/'ffmpeg.exe',*sorted(TOOLS.glob('*.dll'))]
    missing=[p.name for p in helpers[:2]+[TOOLS/'libopus-0.dll'] if not p.is_file()]
    if missing:raise SystemExit('run scripts/build_windows_helpers.sh first; missing '+', '.join(missing))
    WORK.mkdir(parents=True,exist_ok=True)
    if DIST.exists():shutil.rmtree(DIST)
    hashes={p.name:sha(p) for p in (ROOT/'scripts').glob('*') if p.suffix in ('.py','.cpp','.js')}
    (WORK/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    command=[sys.executable,'-m','PyInstaller','--noconfirm','--windowed','--onedir','--name',NAME,
             '--icon',ROOT/'scripts/assets/assistant_icon.png','--python-option','X utf8',  # files and pipes are UTF-8, not GBK
             '--distpath',WORK/'dist','--workpath',WORK/'pyinstaller','--specpath',WORK,
             '--hidden-import','bleak.backends.winrt.client','--hidden-import','bleak.backends.winrt.scanner',
             '--hidden-import','pystray._win32','--hidden-import','sounddevice','--hidden-import','websockets.sync.client',
             '--collect-all','_sounddevice_data','--add-data',f'{WORK/"source-hashes.json"};.',
             '--add-data',f'{notices()};licenses','--add-data',f'{ROOT/"scripts/passport_menu.js"};.',
             '--add-data',f'{ROOT/"scripts/assets/assistant_icon.png"};.']
    for file in helpers:command.extend(['--add-binary',f'{file};helpers'])
    command.append(ROOT/'scripts/passport_assistant.py')
    run(command,cwd=ROOT,env=dict(os.environ,PYINSTALLER_CONFIG_DIR=str(WORK/'cache'),PYTHONNOUSERSITE='1'))
    app=WORK/'dist'/NAME
    # The packaged app checks its own imports, the Opus encoder and the PPP gateway, opening no device.
    with tempfile.TemporaryDirectory() as data:
        report=Path(data)/'self-test.json'
        run([app/f'{NAME}.exe','--self-test',report],env=dict(os.environ,PASSPORT_DATA_DIR=data),timeout=120)
        result=json.loads(report.read_text(encoding='utf-8'))
    print(json.dumps(result,ensure_ascii=False,indent=2))
    if not (result['packaged'] and result['dependencies']['voice'] and 'PPP_FCS_FRAMING_OK' in result['ppp_self_test']):
        raise SystemExit('self-test failed')
    DIST.mkdir(parents=True)
    archive=DIST/f'AI-Passport-Assistant-{VERSION}-preview-windows-x64.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as package:
        for path in sorted(app.rglob('*')):
            if path.is_file():package.write(path,Path(NAME)/path.relative_to(app))
    manifest={'version':VERSION+'-preview','build':BUILD_NUMBER,'platform':'Windows x64','minimum_windows':'10',
              'official_apps_bundled':False,'private_config_bundled':False,'code_signed':False,'firmware_flashed_by_build':False,
              'self_test':result,'source_hashes':hashes,'files':{archive.name:{'bytes':archive.stat().st_size,'sha256':sha(archive)}}}
    (DIST/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    # LF line endings, so shasum -c works on any system (write_text would give CRLF on Windows).
    (DIST/'SHA256SUMS').write_bytes(''.join(f'{sha(p)}  {p.name}\n' for p in sorted(DIST.iterdir()) if p.name!='SHA256SUMS').encode())
    print('ASSISTANT_WINDOWS_BUILT',DIST,flush=True)


if __name__=='__main__':main()
