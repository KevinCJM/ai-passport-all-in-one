#!/usr/bin/env python3
"""AI Passport computer assistant (macOS, Windows). No official application is installed or bundled.

The window is AppKit on a Mac; on Windows a tray icon opens the assistant's web page, which both share."""
import argparse
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

from passport_computer_link import start_gateway
from passport_identity import paired_port
from passport_paths import helper, user_data, codex_command, PACKAGED

VERSION='0.1.21-preview'
LABEL='local.ai-passport.assistant'
WINDOWS=sys.platform=='win32'
UI_PORT=8768
WEB=f'http://127.0.0.1:{UI_PORT}/'
WEB_PAGES=[('菜单自定义','menu'),('状态牌','status'),('倒数日','countdown'),('二维码','qr'),*([] if WINDOWS else [('AI 控制器','ai')]),('电子书','book'),('打开 Mac 程序' if not WINDOWS else '打开电脑程序','mac'),('模拟遥控','remote'),('诊断','diag')]


def validate_config(config):
    if not isinstance(config,dict) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',config.get('mac','')):
        raise ValueError('配对信息无效，请重新配对')
    if not re.fullmatch('[0-9a-fA-F]{64}',config.get('api_token','')):
        raise ValueError('配对信息无效，请重新配对')
    # Imported development settings must not dictate executable paths or ports.
    result={k:config[k] for k in ('mac','api_token','xiaozhi') if k in config}
    result.update(http_forward=18763,audio_forward=19548,ui_port=UI_PORT)
    return result


def save_config(path,config):
    config=validate_config(config)
    temporary=path.with_suffix('.new')
    with os.fdopen(os.open(temporary,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w') as target:
        json.dump(config,target);target.flush();os.fsync(target.fileno())
    temporary.replace(path);path.chmod(0o600)


def devices(ports):
    # Fixed USB Serial/JTAG with a MAC serial; generic USB boards are not opened. Windows may report the serial
    # without separators (or with dashes): written back as aa:bb:cc:dd:ee:ff.
    found=[]
    for p in ports:
        serial=re.sub('[:-]','',p.serial_number or '').lower()
        if p.vid==0x303a and p.pid==0x1001 and re.fullmatch('[0-9a-f]{12}',serial):
            found.append({'mac':':'.join(serial[i:i+2] for i in range(0,12,2)),'port':p.device})
    return found


def dependencies():
    try:codex_command();codex=True
    except FileNotFoundError:codex=False
    if WINDOWS:audio={'accessibility':True,'post_event_access':True};bluetooth=None  # no permission to grant
    else:
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


def features(deps):
    """The window's / page's 电脑功能 rows: (key, name, ok True/False/None while checking, detail)."""
    rows=[('voice','语音输入',bool(deps.get('voice')) if 'voice' in deps else None,
           '小智识别，文字粘贴到光标处' if deps.get('voice') else '编码组件不可用，请重新安装助手' if 'voice' in deps else '正在检查'),
          ('codex','Codex',True if deps.get('codex') else None,
           '已安装，可在胸牌「AI 控制器」看用量和对话' if deps.get('codex') else '未安装（可选，需自行安装并登录）' if 'codex' in deps else '正在检查')]
    if WINDOWS:return rows
    checked=all(isinstance(deps.get(k),bool) for k in ('accessibility','post_event_access'))
    missing=([] if not checked or (deps['accessibility'] and deps['post_event_access']) else ['辅助功能'])+(['蓝牙'] if deps.get('bluetooth_denied') else [])
    return rows+[('permission','系统权限',None if not checked else not missing,'正在检查' if not checked else '需要授权：'+'、'.join(missing) if missing else '辅助功能、蓝牙已授权')]


def open_file(path):
    if WINDOWS:os.startfile(path)
    else:subprocess.run(['/usr/bin/open',str(path)],check=True,timeout=10)


class Controller:
    def __init__(self,directory=None,device_factory=None):
        self.directory=Path(directory) if directory else user_data();self.directory.mkdir(parents=True,exist_ok=True,mode=0o700)
        self.path=self.directory/'device.json';self.factory=device_factory;self.device=None;self.http=None
        self.stop=threading.Event();self.enabled=True;self.lock=threading.Lock();self.pair_mac=None
        self.status='正在启动';self.error='';self.code='';self.candidates=[];self.deps={};self.thread=None;self.paired_mac=None
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
                    'paired':self.path.exists(),'devices':choices,'dependencies':dict(self.deps),
                    'connected':bool(self.device and time.monotonic()-self.device.last_seen<15),'web':self.http is not None}

    def import_config(self,path):
        if Path(path).stat().st_size>16384:raise ValueError('配对文件无效')
        save_config(self.path,json.loads(Path(path).read_text()));self.error=''

    def ip_location_allowed(self):
        try:return bool(json.loads((self.directory/'live/settings.json').read_text()).get('ip_location_allowed',False))
        except (OSError,ValueError):return False

    def ledger(self):
        """语音记账: the CSV passport_services keeps in sync with the badge, opened in Numbers or Excel."""
        path=self.directory/'记账.csv'
        if not path.exists():raise ValueError('还没有记账记录：在胸牌「语音记账」记一笔，胸牌连上电脑后会自动同步到这里。')
        open_file(path)

    def web_state(self):
        """The web page's 电脑助手 section."""
        state=self.snapshot()
        return dict(state,features=[list(row[1:]) for row in features(state['dependencies'])],login=login_enabled(),
                    ip_location=self.ip_location_allowed(),platform='windows' if WINDOWS else 'mac',help=HELP)

    def web_action(self,data):
        """The page's buttons. Errors are ValueError with the text the page shows."""
        action=data.get('action')
        if action=='pair':self.pair(str(data.get('mac','')))
        elif action=='toggle':self.enabled=not self.enabled
        elif action=='location':self.allow_ip_location(bool(data.get('on')))
        elif action=='import':
            try:save_config(self.path,data.get('config'))
            except (ValueError,TypeError):raise ValueError('配对文件无效')
            self.error=''
        elif action=='login':
            try:login_item(bool(data.get('on')))
            except ValueError:raise
            except Exception:raise ValueError('设置开机启动失败')
        elif action=='ledger':self.ledger()
        else:raise ValueError('未知操作')
        return {'ok':True}

    def allow_ip_location(self,allowed):
        folder=self.directory/'live';folder.mkdir(exist_ok=True,mode=0o700)
        path=folder/'settings.json';settings=json.loads(path.read_text()) if path.exists() else {}
        settings['ip_location_allowed']=bool(allowed)
        with os.fdopen(os.open(path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w') as target:json.dump(settings,target)
        if self.device:self.device.services.settings['ip_location_allowed']=bool(allowed)

    def pair(self,mac):
        if not any(d['mac']==mac for d in self.candidates):raise ValueError('没找到这台胸牌，请检查 USB 线')
        self.pair_mac=mac

    def pair_device(self,mac,ports):
        port=paired_port(mac,ports);gateway=None
        log=(self.directory/'gateway.log').open('a')
        try:
            gateway=start_gateway(port,18763,19548,log)
            nonce=secrets.token_hex(16);self.code=nonce[:6];self.status='正在配对，请在胸牌上确认'
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

    def serve(self):
        """The web page, from start to quit; badge sections answer 503 until the badge is connected."""
        import passport_device
        from http.server import ThreadingHTTPServer
        passport_device.ASSISTANT=self
        try:self.http=ThreadingHTTPServer(('127.0.0.1',UI_PORT),passport_device.Handler)
        except OSError:self.error=f'网页设置不可用：端口 {UI_PORT} 被占用';return
        threading.Thread(target=self.http.serve_forever,daemon=True).start()

    def disconnect(self):
        if self.device:
            import passport_device
            passport_device.DEVICE=None;self.device.close();self.device=None

    def step(self,ports):
        with self.lock:self.candidates=devices(ports)
        if not self.enabled:
            self.disconnect();self.status='助手已暂停';return
        if self.pair_mac:
            mac=self.pair_mac;self.pair_mac=None;self.disconnect();self.pair_device(mac,ports)
        if not self.path.exists():self.status='还没配对：用 USB 线连上胸牌，点「配对」';return
        config=validate_config(json.loads(self.path.read_text()))
        self.paired_mac=config['mac'].lower()
        if self.device:
            transport=getattr(self.device,'transport','none')
            label='USB' if transport=='usb' else '蓝牙' if transport=='ble' else '等待连接'
            self.status=('胸牌已连接 · '+label if time.monotonic()-self.device.last_seen<15 else '等待胸牌连接 · '+label)
            if transport=='ble' and self.deps.get('bluetooth_denied'):self.status='需要蓝牙权限'
            hint='Mac 弹出蓝牙配对请求时，请点「连接」或「配对」'
            if transport=='ble' and time.monotonic()-self.device.last_seen>=15:self.error=hint
            elif self.error==hint:self.error=''
            return
        self.status='正在连接胸牌 · USB 优先，蓝牙备用'
        if self.factory is None:
            from passport_device import Device
            self.factory=Device
        self.device=self.factory(config=config)
        self.device.assistant_permissions=lambda:{k:self.deps.get(k) for k in ('accessibility','post_event_access','bluetooth_allowed','bluetooth_denied')}
        import passport_device
        passport_device.DEVICE=self.device
        self.status='助手已启动，正在连接胸牌';self.error=''

    def start(self):
        self.serve()
        def run():
            from serial.tools import list_ports
            retry=1;last_deps=0
            while not self.stop.is_set():
                try:
                    if time.monotonic()-last_deps>(2 if self.permission_polling.is_set() else 15):self.deps=dependencies();last_deps=time.monotonic()
                    self.step(list(list_ports.comports()));self.refresh_after_permission();retry=1
                except Exception as error:
                    self.disconnect();self.error=str(error) if isinstance(error,(ValueError,ConnectionError,TimeoutError)) else '内部错误：'+type(error).__name__
                    self.status='连接未就绪，%d 秒后重试'%min(retry*2,15);retry=min(retry*2,15)
                self.stop.wait(retry)
            self.disconnect()
        self.thread=threading.Thread(target=run,daemon=True);self.thread.start()

    def close(self):
        self.stop.set()
        if self.thread:self.thread.join(40)
        if self.http:self.http.shutdown();self.http.server_close();self.http=None


def login_enabled():
    if WINDOWS:
        from passport_windows import autostart
        return autostart()
    return (Path.home()/'Library/LaunchAgents'/f'{LABEL}.plist').exists()


def login_item(enable):
    if not PACKAGED:raise ValueError('请在安装后的应用中设置登录启动')
    if WINDOWS:
        from passport_windows import autostart
        autostart(enable);return
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


HELP='''首次使用
用 USB 数据线连上胸牌，在「设备」里选中它，点「配对」。核对胸牌和助手上的配对码一致，在胸牌上按确认。

日常使用
插着线走 USB；拔线后进入语音输入、AI 控制器等电脑功能，助手会自动改用蓝牙（Mac 弹出蓝牙配对时点「连接」）。

语音输入
在胸牌「语音输入」页按确认开始说话，再按确认结束，文字粘贴到 Mac 当前光标处。需要授权辅助功能，不用装输入法。

网页设置
窗口里「网页设置」的七个按钮会在浏览器打开对应页面：菜单自定义、状态牌、倒数日、二维码、打开 Mac 程序的口令、模拟遥控（在电脑上按胸牌的键），以及诊断（连接状态，出问题时发给开发者）。保存后写进胸牌。

其他
Codex、Claude Code 需要自己安装并登录。小智、天气、电台用胸牌自己的 WiFi；没 WiFi 时可通过 USB 借电脑上网。'''
if WINDOWS:HELP='''首次使用
用 USB 数据线连上胸牌，在「设备」里选中它，点「配对」。核对胸牌和网页上的配对码一致，在胸牌上按确认。也可以导入在 Mac 上配对后导出的 device.json。

日常使用
助手在右下角托盘里运行，点图标打开这个网页。插着线走 USB；拔线后进入语音输入、AI 控制器等电脑功能，助手会自动改用蓝牙（Windows 提示配对时点「允许」）。

语音输入
在胸牌「语音输入」页按确认开始说话，再按确认结束，文字粘贴到当前光标处。不用授权、不用装输入法；以管理员身份运行的窗口收不到。

离座锁屏
Windows 读不到蓝牙信号强度，所以只有防锁屏和立即锁屏，不会按距离自动锁屏。

其他
Codex、Claude Code 需要自己安装并登录。小智、天气、电台用胸牌自己的 WiFi；没 WiFi 时可通过 USB 借电脑上网。'''


def gui(controller,background=False):
    import AppKit as A
    from Foundation import NSObject,NSTimer
    from objc import python_method
    WIDTH=500
    def text(value='',size=13,bold=False,color=None,wrap=False):
        item=A.NSTextField.wrappingLabelWithString_(value) if wrap else A.NSTextField.labelWithString_(value)
        item.setFont_(A.NSFont.boldSystemFontOfSize_(size) if bold else A.NSFont.systemFontOfSize_(size))
        if color:item.setTextColor_(color)
        if wrap:item.setPreferredMaxLayoutWidth_(WIDTH-80)
        return item
    def stack(views,vertical=False,spacing=8):
        item=A.NSStackView.stackViewWithViews_(views);item.setSpacing_(spacing)
        item.setOrientation_(A.NSUserInterfaceLayoutOrientationVertical if vertical else A.NSUserInterfaceLayoutOrientationHorizontal)
        item.setAlignment_(A.NSLayoutAttributeLeading if vertical else A.NSLayoutAttributeCenterY);return item
    def spacer():
        item=A.NSView.alloc().init();item.setContentHuggingPriority_forOrientation_(1,0);return item
    def symbol(name,color):
        image=A.NSImage.imageWithSystemSymbolName_accessibilityDescription_(name,None)
        view=A.NSImageView.imageViewWithImage_(image);view.setContentTintColor_(color);return view
    def heading(value):return text(value,12,True,A.NSColor.secondaryLabelColor())
    def open_url(url):A.NSWorkspace.sharedWorkspace().openURL_(A.NSURL.URLWithString_(url))
    class Delegate(NSObject):
        @python_method
        def button(self,title,selector,primary=False):
            item=A.NSButton.buttonWithTitle_target_action_(title,self,selector)
            if primary:item.setKeyEquivalent_('\r')
            return item
        @python_method
        def switch(self,title,selector,on):
            item=A.NSButton.checkboxWithTitle_target_action_(title,self,selector);item.setState_(int(on));return item
        def applicationDidFinishLaunching_(self,notification):
            self.window=A.NSWindow.alloc().initWithContentRect_styleMask_backing_defer_(((0,0),(WIDTH,560)),1|2|4,A.NSBackingStoreBuffered,False)
            self.window.setTitle_('AI Passport 助手');self.window.setReleasedWhenClosed_(False)
            # Header
            icon=A.NSImageView.imageViewWithImage_(A.NSApplication.sharedApplication().applicationIconImage())
            icon.widthAnchor().constraintEqualToConstant_(44).setActive_(True);icon.heightAnchor().constraintEqualToConstant_(44).setActive_(True)
            header=stack([icon,stack([text('AI Passport 助手',20,True),text('胸牌连接 · 语音输入 · 电脑功能　版本 '+VERSION,12,color=A.NSColor.secondaryLabelColor())],True,2)],spacing=12)
            # Status card: dot + one-line state, a detail line, and the pairing code in large digits.
            self.dot=text('●',15);self.status=text('正在检查',15,True)
            self.message=text('',12,color=A.NSColor.secondaryLabelColor(),wrap=True)
            self.code=text('',30,True);self.code.setFont_(A.NSFont.monospacedDigitSystemFontOfSize_weight_(30,A.NSFontWeightSemibold));self.code.setHidden_(True)
            self.card=A.NSBox.alloc().init();self.card.setBoxType_(A.NSBoxCustom);self.card.setCornerRadius_(10);self.card.setBorderColor_(A.NSColor.separatorColor())
            self.card.setFillColor_(A.NSColor.quaternarySystemFillColor());self.card.setContentViewMargins_((14,12));self.card.setTitlePosition_(A.NSNoTitle)
            inner=stack([stack([self.dot,self.status],spacing=6),self.message,self.code],True,6);inner.setTranslatesAutoresizingMaskIntoConstraints_(False)
            holder=self.card.contentView();holder.addSubview_(inner)
            for a,b in [(inner.topAnchor(),holder.topAnchor()),(inner.bottomAnchor(),holder.bottomAnchor()),(inner.leadingAnchor(),holder.leadingAnchor()),(inner.trailingAnchor(),holder.trailingAnchor())]:a.constraintEqualToAnchor_(b).setActive_(True)
            # Device
            self.choices=A.NSPopUpButton.alloc().initWithFrame_pullsDown_(((0,0),(300,28)),False);self.choices.setTarget_(self);self.choices.setAction_('deviceSelected:')
            self.choices.setContentHuggingPriority_forOrientation_(1,0)
            self.pairButton=self.button('配对','pair:',True)
            device=stack([self.choices,self.pairButton])
            importing=self.button('导入配对文件…','import:');importing.setBezelStyle_(A.NSBezelStyleInline);importing.setControlSize_(A.NSControlSizeSmall)
            # Computer features: icon, name, detail, optional fix button.
            self.rows={}
            features=[]
            for key,name in [('voice','语音输入'),('codex','Codex'),('permission','系统权限')]:
                image=symbol('circle',A.NSColor.tertiaryLabelColor());detail=text('正在检查',13,color=A.NSColor.secondaryLabelColor())
                label=text(name,13,True);label.widthAnchor().constraintEqualToConstant_(64).setActive_(True)
                row=[image,label,detail,spacer()]
                if key=='permission':self.fix=self.button('去授权','permissionGuide:');self.fix.setHidden_(True);row.append(self.fix)
                features.append(stack(row));self.rows[key]=(image,detail)
            # Options
            self.login=self.switch('登录 Mac 后自动启动助手','login:',login_enabled())
            ip=self.switch('允许按 IP 定位城市（天气、空气质量）','location:',controller.ip_location_allowed())
            ip_note=text('会向 ipwho.is 发送这台 Mac 的公网 IP。不开也可以在胸牌上说城市名。',11,color=A.NSColor.secondaryLabelColor(),wrap=True)
            # Actions
            # Web settings: one button per section of the assistant's page, opened in the browser.
            self.webButtons=[]
            for index,(title,_) in enumerate(WEB_PAGES):
                item=self.button(title,'webPage:');item.setTag_(index);item.setToolTip_('在浏览器打开「'+title+'」；胸牌连上后可用');self.webButtons.append(item)
            webRows=[stack(self.webButtons[:4]),stack(self.webButtons[4:])]
            for row in webRows:row.setDistribution_(A.NSStackViewDistributionFillEqually)
            self.toggle=self.button('暂停助手','toggle:')
            actions=stack([self.button('记账表格','ledger:'),self.button('使用说明','help:'),spacer(),self.toggle])
            sections=[header,self.card,heading('设备'),device,importing,heading('电脑功能'),*features,heading('网页设置（在浏览器打开）'),*webRows,heading('选项'),self.login,ip,ip_note,actions]
            root=stack(sections,True,10);root.setEdgeInsets_((20,24,20,24))
            for view in sections[1:]:
                if view not in (importing,self.login,ip):view.widthAnchor().constraintEqualToConstant_(WIDTH-48).setActive_(True)
            root.setCustomSpacing_afterView_(18,header);root.setCustomSpacing_afterView_(18,self.card);root.setCustomSpacing_afterView_(14,importing)
            root.setCustomSpacing_afterView_(14,features[-1]);root.setCustomSpacing_afterView_(14,webRows[-1]);root.setCustomSpacing_afterView_(18,ip_note)
            self.root=root;self.window.setContentView_(root);self.fit();self.window.center()
            # Menu bar item
            self.item=A.NSStatusBar.systemStatusBar().statusItemWithLength_(A.NSVariableStatusItemLength)
            image=A.NSImage.imageWithSystemSymbolName_accessibilityDescription_('person.text.rectangle','AI Passport')
            if image:image.setTemplate_(True);self.item.button().setImage_(image)
            else:self.item.button().setTitle_('Passport')
            menu=A.NSMenu.alloc().init();menu.setAutoenablesItems_(False)
            def entry(title,selector,key=''):
                item=menu.addItemWithTitle_action_keyEquivalent_(title,selector,key);item.setTarget_(self);return item
            self.menuStatus=menu.addItemWithTitle_action_keyEquivalent_('正在检查','',"");self.menuStatus.setEnabled_(False)
            menu.addItem_(A.NSMenuItem.separatorItem());entry('打开助手窗口','show:')
            self.webItems=[entry(title+'…','webPage:') for title,_ in WEB_PAGES]
            for index,item in enumerate(self.webItems):item.setTag_(index)
            entry('记账表格','ledger:');menu.addItem_(A.NSMenuItem.separatorItem())
            self.menuToggle=entry('暂停助手','toggle:');entry('退出 AI Passport 助手','quit:','q');self.item.setMenu_(menu)
            self.timer=NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(1,self,'refresh:',None,True)
            self.current_choices=[];self.refresh_(None)
            if not background:self.show_(None)
        @python_method
        def fit(self):
            # Keep the top edge still while the message line grows or shrinks.
            size=self.root.fittingSize();frame=self.window.frame()
            if abs(self.window.contentRectForFrameRect_(frame).size.height-size.height)<1:return
            top=frame.origin.y+frame.size.height;self.window.setContentSize_(size)
            frame=self.window.frame();self.window.setFrameTopLeftPoint_((frame.origin.x,top))
        @python_method
        def setRow(self,key,ok,detail):
            image,label=self.rows[key]
            name,color={True:('checkmark.circle.fill',A.NSColor.systemGreenColor()),False:('exclamationmark.circle.fill',A.NSColor.systemOrangeColor()),None:('circle',A.NSColor.tertiaryLabelColor())}[ok]
            image.setImage_(A.NSImage.imageWithSystemSymbolName_accessibilityDescription_(name,None));image.setContentTintColor_(color);label.setStringValue_(detail)
        def refresh_(self,_):
            state=controller.snapshot();deps=state['dependencies']
            message=state['error']
            if deps.get('bluetooth_denied'):message='请在系统设置中允许 AI Passport 助手使用蓝牙。'
            if state['code']:message='核对胸牌屏幕上的配对码，一致就在胸牌上按确认。'
            color=(A.NSColor.systemGrayColor() if not state['enabled'] else A.NSColor.systemGreenColor() if state['connected'] else
                   A.NSColor.systemRedColor() if state['error'] and not state['code'] else A.NSColor.systemOrangeColor())
            self.dot.setTextColor_(color);self.status.setStringValue_(state['status']);self.message.setStringValue_(message);self.message.setHidden_(not message)
            self.code.setStringValue_(' '.join(state['code']));self.code.setHidden_(not state['code'])
            self.menuStatus.setTitle_(state['status'])
            candidates=state['devices']
            if candidates!=self.current_choices:
                old=self.choices.indexOfSelectedItem();selected=self.current_choices[old]['mac'] if 0<=old<len(self.current_choices) else None
                self.current_choices=candidates;self.choices.removeAllItems()
                self.choices.addItemsWithTitles_([d['mac'].upper()+' · '+d['transport']+(' · 已配对' if d['paired'] else '') for d in candidates] or ['未发现胸牌，请用 USB 线连接'])
                self.choices.selectItemAtIndex_(next((i for i,d in enumerate(candidates) if d['mac']==selected),0))
            self.deviceSelected_(None)
            for key,_,ok,detail in features(deps):self.setRow(key,ok,detail)
            self.fix.setHidden_(features(deps)[-1][2] is not False)
            for item in self.webButtons+self.webItems:item.setEnabled_(state['web'])
            self.toggle.setTitle_('暂停助手' if state['enabled'] else '恢复助手');self.menuToggle.setTitle_(self.toggle.title())
            permission=controller.permission_event()
            if permission=='required':self.permissionGuide_(None)
            if permission=='granted':
                controller.error='授权完成，语音输入页面将自动恢复。'
                alert=getattr(self,'permissionAlert',None)
                if alert and alert.window().sheetParent():self.window.endSheet_(alert.window())
                self.permissionAlert=None
            self.fit()
        @python_method
        def selected(self):
            index=self.choices.indexOfSelectedItem();return self.current_choices[index] if 0<=index<len(self.current_choices) else None
        def pair_(self,_):
            item=self.selected()
            if item and item.get('port'):
                try:controller.pair(item['mac'])
                except ValueError as error:controller.error=str(error)
        def deviceSelected_(self,_):
            item=self.selected()
            self.pairButton.setEnabled_(bool(item and item.get('port')))
            self.pairButton.setTitle_('已配对' if item and item['paired'] and not item.get('port') else '重新配对' if item and item['paired'] else '配对')
        def import_(self,_):
            panel=A.NSOpenPanel.openPanel();panel.setCanChooseDirectories_(False);panel.setAllowsMultipleSelection_(False)
            panel.setMessage_('选择从其他电脑导出的 device.json 配对文件')
            if panel.runModal()==A.NSModalResponseOK:
                try:controller.import_config(panel.URL().path())
                except Exception:controller.error='配对文件无效'
        def permissionGuide_(self,_):
            deps=controller.deps
            if not all(isinstance(deps.get(k),bool) for k in ('accessibility','post_event_access')):return
            if deps['accessibility'] and deps['post_event_access'] and not deps.get('bluetooth_denied'):return
            if self.window.attachedSheet():return
            self.show_(None)
            alert=A.NSAlert.alloc().init();alert.setMessageText_('允许 AI Passport 助手使用电脑功能')
            alert.setInformativeText_('辅助功能：把识别出的文字粘贴到光标处。\n蓝牙：拔线后和胸牌无线连接。\n\n在打开的系统设置页里打开 AI Passport Assistant 的开关，助手会自动检测。\n\n如果开关已打开仍无效：先用「－」移除旧条目，再点「在 Finder 显示助手」把当前助手拖进去。')
            options=[]
            if deps.get('bluetooth_denied'):options.append(('打开蓝牙设置','Privacy_Bluetooth'))
            if not (deps.get('accessibility') and deps.get('post_event_access')):options.append(('打开辅助功能设置','Privacy_Accessibility'))
            for title,_ in options:alert.addButtonWithTitle_(title)
            for title in ['在 Finder 显示助手','稍后']:alert.addButtonWithTitle_(title)
            self.permissionAlert=alert
            def completed(response):
                index=response-A.NSAlertFirstButtonReturn
                if 0<=index<len(options):
                    open_url('x-apple.systempreferences:com.apple.preference.security?'+options[index][1]);self.window.orderBack_(None)
                elif index==len(options):
                    bundle=Path(sys.executable).resolve().parents[2] if PACKAGED else Path.home()/'Applications/AI Passport Assistant.app'
                    A.NSWorkspace.sharedWorkspace().selectFile_inFileViewerRootedAtPath_(str(bundle),'')
            alert.beginSheetModalForWindow_completionHandler_(self.window,completed)
        def location_(self,sender):controller.allow_ip_location(bool(sender.state()))
        def toggle_(self,_):controller.enabled=not controller.enabled
        def login_(self,sender):
            try:login_item(bool(sender.state()))
            except Exception as error:
                sender.setState_(int(not sender.state()))
                controller.error=str(error) if isinstance(error,ValueError) else '设置登录启动失败；请从应用程序文件夹打开助手'
        def help_(self,_):
            alert=A.NSAlert.alloc().init();alert.setMessageText_('AI Passport 助手使用说明');alert.setInformativeText_(HELP);alert.runModal()
        def webPage_(self,sender):open_url(WEB+'#'+WEB_PAGES[sender.tag()][1])
        def ledger_(self,_):
            try:controller.ledger()
            except ValueError as error:
                alert=A.NSAlert.alloc().init();alert.setMessageText_('还没有记账记录');alert.setInformativeText_(str(error).split('：',1)[-1]);alert.runModal()
        def show_(self,_):self.window.makeKeyAndOrderFront_(None);A.NSApplication.sharedApplication().activateIgnoringOtherApps_(True)
        def quit_(self,_):A.NSApplication.sharedApplication().terminate_(None)
        def applicationShouldTerminateAfterLastWindowClosed_(self,_):return False
        # Opening the app from Finder or Launchpad while it runs in the background shows the window.
        def applicationShouldHandleReopen_hasVisibleWindows_(self,_,visible):self.show_(None);return True
        def applicationWillTerminate_(self,_):controller.close()
    app=A.NSApplication.sharedApplication();app.setActivationPolicy_(A.NSApplicationActivationPolicyAccessory)
    delegate=Delegate.alloc().init();app.setDelegate_(delegate);app.run()


def tray(controller,background=False):
    """Windows: a tray icon; its window is the web page. The menu shows the state and opens each section."""
    import webbrowser
    import pystray
    from PIL import Image
    open_page=lambda section='assistant':webbrowser.open(WEB+'#'+section)
    def page(section):return lambda:open_page(section)
    def toggle():controller.enabled=not controller.enabled
    def login(_,item):
        try:login_item(not login_enabled())
        except Exception as error:controller.error=str(error) if isinstance(error,ValueError) else '设置开机启动失败'
    def ledger():
        try:controller.ledger()
        except ValueError:open_page()
    Item=pystray.MenuItem
    menu=pystray.Menu(Item(lambda _:controller.snapshot()['status'],None,enabled=False),pystray.Menu.SEPARATOR,
        Item('打开助手',page('assistant'),default=True),Item('网页设置',pystray.Menu(*[Item(title,page(key)) for title,key in WEB_PAGES])),
        Item('记账表格',ledger),pystray.Menu.SEPARATOR,Item(lambda _:'暂停助手' if controller.enabled else '恢复助手',toggle),
        Item('开机自动启动',login,checked=lambda _:login_enabled()),Item('退出 AI Passport 助手',lambda icon:icon.stop()))
    icon=pystray.Icon('ai-passport',Image.open(Path(getattr(sys,'_MEIPASS',Path(__file__).parent/'assets'))/'assistant_icon.png'),'AI Passport 助手',menu)
    def watch(icon):
        icon.visible=True
        if not background:open_page()
        shown=None
        while not controller.stop.wait(1):
            state=controller.snapshot()
            if (state['status'],state['code'])!=shown:
                shown=(state['status'],state['code']);icon.title='AI Passport 助手 · '+state['status'];icon.update_menu()
                # The pairing code is compared on the page; a notification brings it to the front.
                if state['code']:icon.notify('核对胸牌上的配对码：'+state['code'],'AI Passport 配对')
    icon.run(watch)


def single_instance(lock):
    if WINDOWS:
        import msvcrt
        try:msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1);return True
        except OSError:return False
    import fcntl
    try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);return True
    except BlockingIOError:return False


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--background',action='store_true');parser.add_argument('--self-test',type=Path);args=parser.parse_args()
    directory=user_data();os.environ['PASSPORT_DATA_DIR']=str(directory);os.environ['PASSPORT_CONFIG']=str(directory/'device.json');os.environ['SSL_CERT_FILE']=certifi.where()
    if WINDOWS:
        # A windowed app's helpers (PPP gateway, ffmpeg, codex.cmd) would each open a console window.
        # ponytail: one default for every Popen in this process; a helper that needs a console must pass creationflags=0.
        import functools
        subprocess.Popen.__init__=functools.partialmethod(subprocess.Popen.__init__,creationflags=subprocess.CREATE_NO_WINDOW)
    lock=(directory/'assistant.lock').open('a')
    if not single_instance(lock):
        if WINDOWS and not args.background and not args.self_test:
            import webbrowser;webbrowser.open(WEB+'#assistant')  # opened again: show the running one
        return
    if args.self_test:
        import bleak,sounddevice,psutil,websockets,cryptography,paho.mqtt.client,serial
        if WINDOWS:import pystray,PIL,passport_windows
        from passport_device import Device
        gateway=subprocess.run([str(helper('passport-ppp')),'--self-test'],capture_output=True,text=True,timeout=10,check=True)
        result={'version':VERSION,'packaged':PACKAGED,'dependencies':dependencies(),'ppp_self_test':gateway.stdout.strip(),'no_device_opened':True,'no_official_app_installed':True}
        args.self_test.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');return
    controller=Controller(directory);controller.start()
    try:(tray if WINDOWS else gui)(controller,args.background)
    finally:controller.close()


if __name__=='__main__':main()
