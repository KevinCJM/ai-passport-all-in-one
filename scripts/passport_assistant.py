#!/usr/bin/env python3
"""AI Passport Mac assistant. No official application is installed or bundled."""
import argparse
import fcntl
import http.client
import json
import os
from pathlib import Path
import plistlib
import re
import secrets
import signal
import subprocess
import sys
import threading
import time
import certifi

from passport_identity import paired_port
from passport_paths import helper, user_data, codex_command, PACKAGED

VERSION='0.1.1-preview'
LABEL='local.ai-passport.assistant'


def validate_config(config):
    if not isinstance(config,dict) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',config.get('mac','')):
        raise ValueError('invalid device identity')
    if not re.fullmatch('[0-9a-fA-F]{64}',config.get('api_token','')):
        raise ValueError('invalid device credential')
    # Imported development settings must not dictate executable paths or ports.
    result={k:config[k] for k in ('mac','api_token','xiaozhi') if k in config}
    result.update(http_forward=18763,audio_forward=19548,ui_port=8768)
    return result


def save_config(path,config):
    config=validate_config(config)
    temporary=path.with_suffix('.new')
    with os.fdopen(os.open(temporary,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w') as target:
        json.dump(config,target);target.flush();os.fsync(target.fileno())
    temporary.replace(path);path.chmod(0o600)


def devices(ports):
    # Fixed USB Serial/JTAG with a MAC serial; generic USB boards are not opened.
    return [{'mac':p.serial_number.lower(),'port':p.device} for p in ports
            if p.vid==0x303a and p.pid==0x1001 and re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',p.serial_number or '')]


def dependencies():
    try:codex_command();codex=True
    except FileNotFoundError:codex=False
    try:
        result=subprocess.run([str(helper('passport-dictation')),'status'],capture_output=True,timeout=4,check=True)
        audio=json.loads(result.stdout)
    except Exception:audio={}
    try:
        from CoreBluetooth import CBManager
        bluetooth=int(CBManager.authorization())
    except Exception:bluetooth=None
    try:
        from passport_dictation import Encoder
        Encoder().encode(bytes(1920));voice=True
    except Exception:voice=False
    return {'voice':voice,'codex':codex,
            'accessibility':audio.get('accessibility'),'post_event_access':audio.get('post_event_access'),
            'bluetooth_allowed':None if bluetooth is None else bluetooth==3,'bluetooth_denied':bluetooth in (1,2)}


class Controller:
    def __init__(self,directory=None,device_factory=None):
        self.directory=Path(directory) if directory else user_data();self.directory.mkdir(parents=True,exist_ok=True,mode=0o700)
        self.path=self.directory/'device.json';self.factory=device_factory;self.device=None;self.http=None
        self.stop=threading.Event();self.enabled=True;self.lock=threading.Lock();self.pair_mac=None
        self.status='等待连接 AI Passport';self.error='';self.code='';self.candidates=[];self.deps={};self.thread=None;self.paired_mac=None
        self.permission_state=None;self.permission_notified=False
        self.permission_polling=threading.Event();self.permission_refresh=threading.Event()

    def permission_event(self):
        if not all(isinstance(self.deps.get(k),bool) for k in ('accessibility','post_event_access')):
            self.permission_polling.set();return None
        granted=self.deps['accessibility'] and self.deps['post_event_access'] and not self.deps.get('bluetooth_denied');previous=self.permission_state;self.permission_state=granted
        if granted:
            self.permission_polling.clear();self.permission_notified=False
            if previous is False:self.permission_refresh.set();return 'granted'
        else:
            self.permission_polling.set()
            if not self.permission_notified:self.permission_notified=True;return 'required'
        return None

    def refresh_after_permission(self):
        if not self.permission_refresh.is_set():return
        if not self.device:return
        state=self.device.snapshot()
        if state.get('app')!=2:self.permission_refresh.clear();return
        if not state.get('link_ready') or state.get('dictation_phase'):return
        try:self.device.command('refresh_service')
        except Exception:return
        self.permission_refresh.clear()

    def snapshot(self):
        with self.lock:
            choices=[dict(d,paired=d['mac']==self.paired_mac,transport='USB') for d in self.candidates]
            if self.paired_mac and not any(d['mac']==self.paired_mac for d in choices):choices.append({'mac':self.paired_mac,'port':None,'paired':True,'transport':'蓝牙'})
            return {'version':VERSION,'status':self.status,'error':self.error,'code':self.code,'enabled':self.enabled,
                    'paired':self.path.exists(),'devices':choices,'dependencies':dict(self.deps)}

    def import_config(self,path):
        if Path(path).stat().st_size>16384:raise ValueError('configuration too large')
        save_config(self.path,json.loads(Path(path).read_text()));self.error=''

    def allow_ip_location(self,allowed):
        folder=self.directory/'live';folder.mkdir(exist_ok=True,mode=0o700)
        path=folder/'settings.json';settings=json.loads(path.read_text()) if path.exists() else {}
        settings['ip_location_allowed']=bool(allowed)
        with os.fdopen(os.open(path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w') as target:json.dump(settings,target)
        if self.device:self.device.services.settings['ip_location_allowed']=bool(allowed)

    def pair(self,mac):
        if not any(d['mac']==mac for d in self.candidates):raise ValueError('device not available')
        self.pair_mac=mac

    def pair_device(self,mac,ports):
        port=paired_port(mac,ports);gateway=None
        log=(self.directory/'gateway.log').open('a')
        try:
            gateway=subprocess.Popen([str(helper('passport-ppp')),'--serial',port,'--forward','18763','--audio-forward','19548'],stdout=log,stderr=subprocess.STDOUT)
            nonce=secrets.token_hex(16);self.code=nonce[:6];self.status='请核对设备配对码，并按设备确认键'
            started=time.monotonic();begun=False
            while not self.stop.wait(.5) and time.monotonic()-started<65:
                if gateway.poll() is not None:raise ConnectionError('PPP 配对连接失败')
                connection=http.client.HTTPConnection('127.0.0.1',18763,timeout=2)
                try:
                    connection.request('GET' if begun else 'POST','/pair',body=None if begun else json.dumps({'nonce':nonce}),headers={'X-Passport-Pair':nonce,'Content-Type':'application/json'})
                    response=connection.getresponse();raw=response.read(1024)
                    if response.status==404:raise ValueError('设备固件尚未支持电脑配对')
                    if response.status==403:raise ValueError('配对已取消或过期')
                    if response.status!=200:raise ValueError('配对请求被设备拒绝')
                    data=json.loads(raw);begun=True
                    if data.get('state')=='approved':
                        if data.get('mac','').lower()!=mac:raise ValueError('配对设备身份不一致')
                        save_config(self.path,data);return
                except (OSError,TimeoutError):pass
                finally:connection.close()
            raise TimeoutError('配对超时，请重新操作')
        finally:
            self.code=''
            if gateway and gateway.poll() is None:
                gateway.terminate()
                try:gateway.wait(4)
                except subprocess.TimeoutExpired:gateway.kill();gateway.wait()
            log.close()

    def disconnect(self):
        if self.http:
            self.http.shutdown();self.http.server_close();self.http=None
        if self.device:
            self.device.close();self.device=None

    def step(self,ports):
        with self.lock:self.candidates=devices(ports)
        if not self.enabled:
            self.disconnect();self.status='助手已暂停';return
        if self.pair_mac:
            mac=self.pair_mac;self.pair_mac=None;self.disconnect();self.pair_device(mac,ports)
        if not self.path.exists():self.status='请选择 AI Passport 并配对';return
        config=validate_config(json.loads(self.path.read_text()))
        self.paired_mac=config['mac'].lower()
        if self.device:
            transport=getattr(self.device,'transport','none')
            label='USB' if transport=='usb' else '蓝牙' if transport=='ble' else '等待连接'
            self.status=('AI Passport 已连接 · '+label if time.monotonic()-self.device.last_seen<15 else '等待已配对的 AI Passport · '+label)
            if transport=='ble' and self.deps.get('bluetooth_denied'):self.status='请允许助手使用蓝牙'
            hint='Mac 弹出蓝牙配对请求时，请点「连接」或「配对」'
            if transport=='ble' and time.monotonic()-self.device.last_seen>=15:self.error=hint
            elif self.error==hint:self.error=''
            return
        self.status='正在连接 AI Passport · USB 优先 / 蓝牙备用'
        if self.factory is None:
            from passport_device import Device
            self.factory=Device
        self.device=self.factory(config=config)
        self.device.assistant_permissions=lambda:{k:self.deps.get(k) for k in ('accessibility','post_event_access','bluetooth_allowed','bluetooth_denied')}
        from passport_device import Handler
        from http.server import ThreadingHTTPServer
        import passport_device
        passport_device.DEVICE=self.device
        try:
            self.http=ThreadingHTTPServer(('127.0.0.1',8768),Handler)
            threading.Thread(target=self.http.serve_forever,daemon=True).start()
        except Exception:self.disconnect();raise
        self.status='AI Passport 助手已启动 · 自动连接设备';self.error=''

    def start(self):
        def run():
            from serial.tools import list_ports
            retry=1;last_deps=0
            while not self.stop.is_set():
                try:
                    if time.monotonic()-last_deps>(2 if self.permission_polling.is_set() else 15):self.deps=dependencies();last_deps=time.monotonic()
                    self.step(list(list_ports.comports()));self.refresh_after_permission();retry=1
                except Exception as error:
                    self.disconnect();self.error=str(error) if isinstance(error,(ValueError,ConnectionError,TimeoutError)) else type(error).__name__
                    self.status='连接未就绪，稍后重试';retry=min(retry*2,15)
                self.stop.wait(retry)
            self.disconnect()
        self.thread=threading.Thread(target=run,daemon=True);self.thread.start()

    def close(self):
        self.stop.set()
        if self.thread:self.thread.join(40)


def login_item(enable):
    if not PACKAGED:raise ValueError('请在安装后的应用中设置登录启动')
    if enable and not any(Path(sys.executable).is_relative_to(root) for root in (Path('/Applications'),Path.home()/'Applications')):
        raise ValueError('请先将助手安装到应用程序文件夹')
    target=Path.home()/'Library/LaunchAgents'/f'{LABEL}.plist';domain=f'gui/{os.getuid()}'
    subprocess.run(['/bin/launchctl','bootout',f'{domain}/{LABEL}'],capture_output=True)
    if not enable:target.unlink(missing_ok=True);return
    target.parent.mkdir(parents=True,exist_ok=True)
    log=user_data()/'assistant.log'
    config={'Label':LABEL,'ProgramArguments':[sys.executable,'--background'],'RunAtLoad':True,
            'KeepAlive':{'SuccessfulExit':False},'ThrottleInterval':10,'LimitLoadToSessionType':'Aqua',
            'StandardOutPath':str(log),'StandardErrorPath':str(log)}
    target.write_bytes(plistlib.dumps(config));target.chmod(0o600)
    subprocess.run(['/bin/launchctl','bootstrap',domain,str(target)],check=True,capture_output=True)


def gui(controller,background=False):
    import AppKit as A
    from Foundation import NSObject,NSTimer
    class Delegate(NSObject):
        def applicationDidFinishLaunching_(self,notification):
            self.window=A.NSWindow.alloc().initWithContentRect_styleMask_backing_defer_(((0,0),(540,570)),15,A.NSBackingStoreBuffered,False)
            self.window.setTitle_('AI Passport 助手');self.window.center();self.labels={}
            content=self.window.contentView()
            def label(name,text,y,size=14):
                item=A.NSTextField.labelWithString_(text);item.setFrame_(((24,y),(492,48 if name=='message' else 28)));item.setFont_(A.NSFont.systemFontOfSize_(size));content.addSubview_(item);self.labels[name]=item
            def button(text,selector,x,y,w=150):
                item=A.NSButton.buttonWithTitle_target_action_(text,self,selector);item.setFrame_(((x,y),(w,32)));content.addSubview_(item);return item
            label('title','AI Passport',512,28);label('subtitle','设备连接与电脑功能',480)
            label('status','正在检查',431,17);label('message','',379)
            self.choices=A.NSPopUpButton.alloc().initWithFrame_pullsDown_(((24,335),(330,32)),False);self.choices.setTarget_(self);self.choices.setAction_('deviceSelected:');content.addSubview_(self.choices)
            self.pairButton=button('配对设备','pair:',370,335,145)
            for key,text,y in [('voice','语音输入',283),('codex','Codex',249),('permission','辅助功能与按键授权',215)]:label(key,text,y)
            button('连接与按键授权','permissionGuide:',24,133,160);self.toggle=button('暂停助手','toggle:',204,133,145)
            button('导入已有配对','import:',370,133,145)
            self.login=button('登录后启动','login:',24,88,150);self.login.setButtonType_(A.NSSwitchButton);self.login.setState_(int((Path.home()/'Library/LaunchAgents'/f'{LABEL}.plist').exists()))
            ip=button('允许 IP 定位天气','location:',194,88,170);ip.setButtonType_(A.NSSwitchButton)
            settings=controller.directory/'live/settings.json'
            try:ip.setState_(int(json.loads(settings.read_text()).get('ip_location_allowed',False)))
            except (OSError,ValueError):pass
            ip.setToolTip_('允许向 ipwho.is 提供当前公网 IP，以查询城市、天气和空气质量。')
            button('使用说明','help:',370,88,145)
            label('foot','语音输入用小智识别，无需安装输入法；Codex 请自行安装并登录。',40,12)
            button('设备内容','statusLabels:',370,36,145)
            self.item=A.NSStatusBar.systemStatusBar().statusItemWithLength_(A.NSVariableStatusItemLength);self.item.button().setTitle_('Passport')
            menu=A.NSMenu.alloc().init();menu.addItemWithTitle_action_keyEquivalent_('打开助手','show:','').setTarget_(self)
            menu.addItemWithTitle_action_keyEquivalent_('设备内容（状态牌 / 倒数日 / 二维码）','statusLabels:','').setTarget_(self)
            menu.addItemWithTitle_action_keyEquivalent_('打开 Mac 程序的口令…','macApps:','').setTarget_(self)
            menu.addItemWithTitle_action_keyEquivalent_('退出','quit:','q').setTarget_(self);self.item.setMenu_(menu)
            self.timer=NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(1,self,'refresh:',None,True)
            self.current_choices=[];self.refresh_(None)
            if not background:self.show_(None)
        def refresh_(self,_):
            state=controller.snapshot();self.labels['status'].setStringValue_(state['status']);self.labels['message'].setStringValue_(('配对码：'+state['code']) if state['code'] else state['error'])
            candidates=state['devices']
            if candidates!=self.current_choices:
                old=self.choices.indexOfSelectedItem();selected=self.current_choices[old]['mac'] if 0<=old<len(self.current_choices) else None
                self.current_choices=candidates;self.choices.removeAllItems();self.choices.addItemsWithTitles_([d['mac'].upper()+' · '+d['transport']+(' · 已配对' if d['paired'] else '') for d in candidates] or ['尚未发现 AI Passport'])
                self.choices.selectItemAtIndex_(next((i for i,d in enumerate(candidates) if d['mac']==selected),0))
            self.deviceSelected_(None)
            deps=state['dependencies']
            if deps.get('bluetooth_denied'):self.labels['message'].setStringValue_('系统设置中允许 AI Passport Assistant 使用蓝牙。')
            self.labels['voice'].setStringValue_('语音输入　'+('小智识别，文字粘贴到光标处' if deps.get('voice') else '编码组件不可用，请重新安装助手'))
            self.labels['codex'].setStringValue_('Codex　'+('已找到；登录状态由功能页检查' if deps.get('codex') else '未安装，请自行安装'))
            checked=all(isinstance(deps.get(k),bool) for k in ('accessibility','post_event_access'))
            permission_text='正在检查' if not checked else '已授权' if deps['accessibility'] and deps['post_event_access'] else '需要在系统设置中授权'
            self.labels['permission'].setStringValue_('辅助功能与按键授权　'+permission_text)
            self.toggle.setTitle_('暂停助手' if state['enabled'] else '启动助手')
            permission=controller.permission_event()
            if permission=='required':self.permissionGuide_(None)
            if permission=='granted':
                self.labels['message'].setStringValue_('授权完成，语音输入页面将自动恢复。')
                alert=getattr(self,'permissionAlert',None)
                if alert and alert.window().sheetParent():self.window.endSheet_(alert.window())
                self.permissionAlert=None
        def pair_(self,_):
            if self.current_choices and self.current_choices[self.choices.indexOfSelectedItem()].get('port'):
                try:controller.pair(self.current_choices[self.choices.indexOfSelectedItem()]['mac'])
                except ValueError as error:controller.error=str(error)
        def deviceSelected_(self,_):
            index=self.choices.indexOfSelectedItem();item=self.current_choices[index] if 0<=index<len(self.current_choices) else None
            self.pairButton.setEnabled_(bool(item and item.get('port')))
            self.pairButton.setTitle_('蓝牙自动连接' if item and not item.get('port') else '配对设备')
        def import_(self,_):
            panel=A.NSOpenPanel.openPanel();panel.setCanChooseDirectories_(False);panel.setAllowsMultipleSelection_(False)
            if panel.runModal()==A.NSModalResponseOK:
                try:controller.import_config(panel.URL().path())
                except Exception:controller.error='配对文件无效'
        def permissionGuide_(self,_):
            deps=controller.deps
            if not all(isinstance(deps.get(k),bool) for k in ('accessibility','post_event_access')):
                self.labels['message'].setStringValue_('正在检查权限，请稍候。');return
            if deps['accessibility'] and deps['post_event_access'] and not deps.get('bluetooth_denied'):
                self.labels['message'].setStringValue_('权限正常，无需重复授权。');return
            if self.window.attachedSheet():return
            self.show_(None)
            alert=A.NSAlert.alloc().init();alert.setMessageText_('完成 AI Passport 电脑授权')
            alert.setInformativeText_('蓝牙权限：用于设备与电脑传输。\n辅助功能：把识别出的文字粘贴到光标处。\n\n请在对应页面允许 AI Passport Assistant；完成后自动检测。\n\n若旧条目已开启仍无效，请移除旧条目，再添加 Finder 中的当前助手。')
            options=[]
            if controller.deps.get('bluetooth_denied'):options.append(('打开蓝牙权限','Privacy_Bluetooth'))
            if not (controller.deps.get('accessibility') and controller.deps.get('post_event_access')):options.append(('打开辅助功能','Privacy_Accessibility'))
            if not options:options.append(('查看辅助功能','Privacy_Accessibility'))
            for text,_ in options:alert.addButtonWithTitle_(text)
            for text in ['在 Finder 显示助手','稍后']:alert.addButtonWithTitle_(text)
            self.permissionAlert=alert
            def completed(response):
                index=response-A.NSAlertFirstButtonReturn
                if 0<=index<len(options):
                    A.NSWorkspace.sharedWorkspace().openURL_(A.NSURL.URLWithString_('x-apple.systempreferences:com.apple.preference.security?'+options[index][1]));self.window.orderBack_(None)
                elif index==len(options):
                    bundle=Path(sys.executable).resolve().parents[2] if PACKAGED else Path.home()/'Applications/AI Passport Assistant.app'
                    A.NSWorkspace.sharedWorkspace().selectFile_inFileViewerRootedAtPath_(str(bundle),'')
            alert.beginSheetModalForWindow_completionHandler_(self.window,completed)
        def location_(self,sender):controller.allow_ip_location(bool(sender.state()))
        def toggle_(self,_):controller.enabled=not controller.enabled
        def login_(self,sender):
            try:login_item(bool(sender.state()))
            except Exception:controller.error='设置登录启动失败；请从应用程序文件夹打开助手'
        def help_(self,_):
            alert=A.NSAlert.alloc().init();alert.setMessageText_('AI Passport 使用说明');alert.setInformativeText_('1. 首次通过 USB 配对，核对配对码后按设备确认。\n2. Codex、Claude Code 需自行安装并登录。语音输入由助手请小智识别，文字粘贴到当前光标处，无需安装输入法。\n3. 在系统设置中为 AI Passport 助手授权辅助功能。\n\n日常插线优先使用 USB；拔线后进入语音输入、AI 额度或电脑状态，助手自动改用蓝牙。小智、天气和电台使用 WiFi，没 WiFi 时可通过 USB PPP 联网。');alert.runModal()
        def statusLabels_(self,_):A.NSWorkspace.sharedWorkspace().openURL_(A.NSURL.URLWithString_('http://127.0.0.1:8768/#status'))
        def macApps_(self,_):A.NSWorkspace.sharedWorkspace().openURL_(A.NSURL.URLWithString_('http://127.0.0.1:8768/#mac'))
        def show_(self,_):self.window.makeKeyAndOrderFront_(None);A.NSApplication.sharedApplication().activateIgnoringOtherApps_(True)
        def quit_(self,_):A.NSApplication.sharedApplication().terminate_(None)
        def applicationShouldTerminateAfterLastWindowClosed_(self,_):return False
        # Opening the app from Finder or Launchpad while it runs in the background shows the window.
        def applicationShouldHandleReopen_hasVisibleWindows_(self,_,visible):self.show_(None);return True
        def applicationWillTerminate_(self,_):controller.close()
    app=A.NSApplication.sharedApplication();app.setActivationPolicy_(A.NSApplicationActivationPolicyAccessory)
    delegate=Delegate.alloc().init();app.setDelegate_(delegate);app.run()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--background',action='store_true');parser.add_argument('--self-test',type=Path);args=parser.parse_args()
    directory=user_data();os.environ['PASSPORT_DATA_DIR']=str(directory);os.environ['PASSPORT_CONFIG']=str(directory/'device.json');os.environ['SSL_CERT_FILE']=certifi.where()
    lock=(directory/'assistant.lock').open('a')
    try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:return
    if args.self_test:
        import bleak,sounddevice,psutil,websockets,cryptography,paho.mqtt.client,serial
        from passport_device import Device
        gateway=subprocess.run([str(helper('passport-ppp')),'--self-test'],capture_output=True,text=True,timeout=10,check=True)
        result={'version':VERSION,'packaged':PACKAGED,'dependencies':dependencies(),'ppp_self_test':gateway.stdout.strip(),'no_device_opened':True,'no_official_app_installed':True}
        args.self_test.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');return
    controller=Controller(directory);controller.start()
    try:gui(controller,args.background)
    finally:controller.close()


if __name__=='__main__':main()
