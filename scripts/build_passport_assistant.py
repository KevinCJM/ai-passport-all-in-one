#!/usr/bin/env python3
"""Build a self-contained Mac preview with an explicit, credential-free payload."""
import hashlib
import importlib.metadata as metadata
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from passport_signing import identity,sign,requirement

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'experiments/passport-assistant-build'
SOURCES=WORK/'sources'
DIST=ROOT/'releases/ai-passport-assistant-0.1.1-preview'
TOOLS=ROOT/'experiments/passport-connectivity-tools/bin'
OPUS=Path('/opt/homebrew/opt/opus/lib/libopus.0.dylib').resolve()  # 语音输入's encoder, loaded with ctypes


def run(args,cwd=None,env=None):
    print('BUILD',Path(str(args[0])).name,flush=True)
    subprocess.run(list(map(str,args)),cwd=cwd,env=env,check=True)


def source(name,checksum,url):
    archive=SOURCES/name
    if not archive.exists():
        SOURCES.mkdir(parents=True,exist_ok=True);part=archive.with_name(name+'.part')
        for attempt in range(3):  # a cut-off transfer raises; try again from the start
            print('DOWNLOAD',url,flush=True)
            try:urllib.request.urlretrieve(url,part);part.rename(archive);break
            except OSError:
                if attempt==2:raise
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest()!=checksum:
        raise ValueError('source archive missing or checksum differs: '+name)
    folder=WORK/name.split('.tar')[0]
    if not folder.exists():
        with tarfile.open(archive) as package:package.extractall(WORK,filter='data')
    return folder


def media():
    target=WORK/'media/bin/ffmpeg'
    if target.exists():return target
    prefix=WORK/'media';prefix.mkdir(exist_ok=True)
    lame=source('lame-3.100.tar.gz','ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e','https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz')
    run(['./configure','--prefix='+str(prefix),'--disable-shared','--enable-static','--disable-frontend'],lame)
    run(['make','-j4'],lame);run(['make','install'],lame)
    ffmpeg=source('ffmpeg-7.1.2.tar.xz','089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304','https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz')
    environment=dict(os.environ,PKG_CONFIG_PATH=str(prefix/'lib/pkgconfig'))
    run(['./configure','--prefix='+str(prefix),'--disable-autodetect','--disable-doc','--disable-debug','--disable-shared','--enable-static',
         '--disable-everything','--disable-x86asm','--enable-ffmpeg','--enable-securetransport','--enable-libmp3lame',
         '--enable-protocol=file,pipe,http,https,tcp,tls,httpproxy','--enable-demuxer=mp3,mov,aac,ogg,wav,flac,matroska',
         '--enable-decoder=mp3,mp3float,aac,flac,opus,vorbis,pcm_s16le','--enable-encoder=libmp3lame,pcm_s16le',
         '--enable-parser=mpegaudio,aac,flac,opus,vorbis','--enable-muxer=mp3,pcm_s16le',
         '--enable-filter=aresample,anull,aformat','--extra-cflags=-I'+str(prefix/'include'),
         '--extra-ldflags=-L'+str(prefix/'lib')],ffmpeg,environment)
    run(['make','-j4'],ffmpeg,environment);run(['make','install'],ffmpeg,environment)
    return target


def notices():
    target=WORK/'licenses';target.mkdir(exist_ok=True)
    packages=[]
    for dist in metadata.distributions():
        name=dist.metadata['Name'];version=dist.version
        if name.lower().startswith(('bumble','pandora','grpc','clang','pyinstaller','setuptools','pip','pillow')):continue
        folder=target/name;folder.mkdir(exist_ok=True)
        for file in dist.files or []:
            if any(part.upper().startswith(('LICENSE','COPYING','NOTICE')) for part in file.parts):
                path=Path(dist.locate_file(file))
                if path.is_file():shutil.copy2(path,folder/path.name)
        packages.append({'name':name,'version':version,'license':dist.metadata.get('License-Expression') or dist.metadata.get('License','')})
    for path in [OPUS.parents[1]/'COPYING',Path('/opt/homebrew/opt/libslirp/LICENSE'),Path('/opt/homebrew/opt/glib/LGPL-2.1-or-later.txt'),Path('/opt/homebrew/opt/gettext/COPYING'),Path('/opt/homebrew/opt/pcre2/COPYING')]:
        shutil.copy2(path,target/(path.parts[4]+'-'+path.name))
    for name,folder in [('ffmpeg','ffmpeg-7.1.2'),('lame','lame-3.100')]:
        for path in (WORK/folder).glob('COPYING*'):shutil.copy2(path,target/(name+'-'+path.name))
    links={'libopus':'https://downloads.xiph.org/releases/opus/opus-%s.tar.gz'%OPUS.parents[1].name,'libslirp':'https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.5/libslirp-v4.9.5.tar.gz',
           'glib':'https://download.gnome.org/sources/glib/2.90/glib-2.90.1.tar.xz',
           'libintl':'https://ftp.gnu.org/gnu/gettext/gettext-1.0.tar.gz',
           'pcre2':'https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.49/pcre2-10.49.tar.bz2'}
    (target/'components.json').write_text(json.dumps({'python_packages':packages,'shared_library_sources':links},ensure_ascii=False,indent=2)+'\n')
    (target/'README.txt').write_text('FFmpeg and LAME sources accompany this preview. Shared-library source links and notices are listed in components.json.\nCodex and its installer are not included.\n')
    return target


def main():
    WORK.mkdir(parents=True,exist_ok=True);DIST.mkdir(parents=True,exist_ok=True);TOOLS.mkdir(parents=True,exist_ok=True)
    signer=identity()
    previous=DIST/'AI Passport Assistant.app'
    previous_requirement=requirement(previous) if previous.exists() else None
    if previous_requirement and 'cdhash ' in previous_requirement:previous_requirement=None
    for name,sourcefile,arguments in [('passport-dictation','mac_dictation_bridge.cpp',['-framework','CoreFoundation','-framework','ApplicationServices']),
                                      ('passport-ppp','passport_ppp_gateway.cpp',['-I/opt/homebrew/include','-L/opt/homebrew/lib','-lslirp'])]:
        run(['/usr/bin/clang++','-std=c++20','-O2',ROOT/'scripts'/sourcefile,*arguments,'-o',TOOLS/name])
    encoder=media();licenses=notices()
    opus=WORK/OPUS.name;shutil.copy2(OPUS,opus);opus.chmod(0o755)
    run(['/usr/bin/install_name_tool','-id','@loader_path/'+OPUS.name,opus])
    hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (ROOT/'scripts').glob('*') if p.suffix in ('.py','.cpp')}
    (WORK/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    command=[sys.executable,'-m','PyInstaller','--noconfirm','--windowed','--onedir','--name','AI Passport Assistant',
             '--osx-bundle-identifier','local.ai-passport.assistant','--target-arch','arm64',
             '--icon',ROOT/'scripts/assets/assistant_icon.icns',  # from assistant_icon.svg via rsvg-convert + iconutil
             '--distpath',DIST,'--workpath',WORK/'pyinstaller','--specpath',WORK,
             '--hidden-import','bleak.backends.corebluetooth.client','--hidden-import','bleak.backends.corebluetooth.scanner',
             '--hidden-import','sounddevice','--hidden-import','websockets.sync.client',
             '--collect-all','_sounddevice_data', '--add-data',str(WORK/'source-hashes.json')+':.',
             '--add-data',str(licenses)+':licenses']
    for file in [TOOLS/'passport-dictation',TOOLS/'passport-ppp',encoder,opus]:command.extend(['--add-binary',str(file)+':helpers'])
    command.append(ROOT/'scripts/passport_assistant.py');run(command,ROOT,dict(os.environ,PYINSTALLER_CONFIG_DIR=str(WORK/'cache'),PYTHONNOUSERSITE='1'))
    app=DIST/'AI Passport Assistant.app';info=app/'Contents/Info.plist'
    config=plistlib.loads(info.read_bytes());config.update(CFBundleShortVersionString='0.1.1',CFBundleVersion='4',LSUIElement=True,LSMinimumSystemVersion='15.0',
        NSBluetoothAlwaysUsageDescription='连接已配对的 AI Passport，接收设备麦克风语音和按键事件。')
    info.write_bytes(plistlib.dumps(config));sign(app,signer)
    signing_requirement=requirement(app)
    if 'cdhash ' in signing_requirement:raise ValueError('preview signing identity is not stable across updates')
    if previous_requirement and signing_requirement!=previous_requirement:raise ValueError('refusing to change the existing preview signing identity')
    payload=WORK/'payload'
    if payload.exists():shutil.rmtree(payload)
    (payload/'Applications').mkdir(parents=True);shutil.copytree(app,payload/'Applications'/app.name,symlinks=True)
    component_list=WORK/'components.plist'
    run(['/usr/bin/pkgbuild','--analyze','--root',payload,component_list])
    components=plistlib.loads(component_list.read_bytes())
    for component in components:component.update(BundleIsRelocatable=False,BundleOverwriteAction='upgrade')
    component_list.write_bytes(plistlib.dumps(components))
    run(['/usr/bin/pkgbuild','--root',payload,'--component-plist',component_list,'--install-location','/','--identifier','local.ai-passport.preview.components','--version','0.1.1',WORK/'components.pkg'])
    resources=WORK/'installer-resources';resources.mkdir(exist_ok=True)
    (resources/'welcome.txt').write_text('AI Passport 助手 · macOS Apple Silicon 预览版\n\n安装桌面助手、PPP 与蓝牙适配组件。\n语音输入用小智识别，无需安装输入法；Codex 由用户自行安装、登录。\n\nUSB 优先；进入电脑功能后拔线可切换蓝牙。首次配对请使用 USB 数据线。预览版请参阅实机验收记录。\n')
    distribution=WORK/'distribution.xml'
    distribution.write_text('''<?xml version="1.0"?><installer-gui-script minSpecVersion="2"><title>AI Passport 助手</title><welcome file="welcome.txt"/><options customize="never" require-scripts="false" hostArchitectures="arm64"/><volume-check><allowed-os-versions><os-version min="15.0"/></allowed-os-versions></volume-check><domains enable_localSystem="true" enable_currentUserHome="false"/><choices-outline><line choice="default"/></choices-outline><choice id="default" title="AI Passport 助手"><pkg-ref id="local.ai-passport.preview.components"/></choice><pkg-ref id="local.ai-passport.preview.components" version="0.1.1" auth="Root">components.pkg</pkg-ref></installer-gui-script>''')
    run(['/usr/bin/productbuild','--distribution',distribution,'--package-path',WORK,'--resources',resources,DIST/'AI-Passport-Assistant-0.1.1-preview-arm64.pkg'])
    with tarfile.open(DIST/'component-sources.tar.gz','w:gz') as archive:
        for path in SOURCES.iterdir():archive.add(path,arcname=path.name)
        archive.add(Path(__file__),arcname='build_passport_assistant.py')
        for path in (ROOT/'scripts').glob('passport*.py'):archive.add(path,arcname='assistant-source/'+path.name)
        for name in ['mac_dictation_bridge.cpp','passport_ppp_gateway.cpp']:archive.add(ROOT/'scripts'/name,arcname='assistant-source/'+name)
    manifest={'version':'0.1.1-preview','platform':'macOS arm64','minimum_macos':'15.0','official_apps_bundled':False,'private_config_bundled':False,
              'signed_with_developer_id':False,'notarized':False,'firmware_flashed_by_build':False,
              'signature':'persistent local development certificate','designated_requirement':signing_requirement,
              'source_hashes':hashes,'files':{p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in DIST.iterdir() if p.is_file()}}
    (DIST/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('ASSISTANT_PREVIEW_BUILT',str(DIST),flush=True)


if __name__=='__main__':main()
