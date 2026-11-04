"""Mac companion for real data. C3 owns navigation, timers and TOTP.

No model answers, quota figures, weather or process metrics are fabricated.
Dependencies that cannot run return an explicit unavailable state.
"""
import json
from datetime import datetime
import hashlib
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
from urllib.parse import urlencode

import psutil
from passport_audio import AudioBridge,MediaPlayer
from passport_xiaozhi import XiaoZhi
from passport_dictation import BluetoothDictation
from passport_guard import Guard
from passport_ledger import Ledger
from passport_mac_open import MacOpener
from passport_feishu import Feishu,BUNDLE as FEISHU_BUNDLE
from passport_computer_link import LinkBusy
from passport_paths import live_data, user_data, helper, codex_command, PACKAGED, RESOURCES, ffmpeg

ROOT=Path(__file__).resolve().parents[1]
DATA=live_data()
GUEST='http://127.0.0.1:18761'
HTTP=urllib.request.build_opener(urllib.request.ProxyHandler({}))
WEATHER_NAMES={0:'晴',1:'大部晴朗',2:'多云',3:'阴',45:'雾',48:'雾凇',51:'小毛毛雨',53:'毛毛雨',55:'浓毛毛雨',56:'冻毛毛雨',57:'强冻毛毛雨',61:'小雨',63:'中雨',65:'大雨',66:'冻雨',67:'强冻雨',71:'小雪',73:'中雪',75:'大雪',77:'米雪',80:'小阵雨',81:'阵雨',82:'强阵雨',85:'阵雪',86:'强阵雪',95:'雷雨',96:'雷雨伴冰雹',97:'强雷雨',99:'雷雨伴大冰雹'}


def fetch_json(url,timeout=8):
    with HTTP.open(urllib.request.Request(url,headers={'User-Agent':'AI-Passport-personal-simulator/1.0'}),timeout=timeout) as response:
        body=response.read(2_000_001)
        if len(body)>2_000_000: raise ValueError('upstream response too large')
        return json.loads(body)


def private_write(path,data):
    temporary=path.with_suffix('.new')
    fd=os.open(temporary,os.O_CREAT|os.O_WRONLY|os.O_TRUNC,0o600)
    with os.fdopen(fd,'wb') as out: out.write(data); out.flush(); os.fsync(out.fileno())
    temporary.replace(path)


class CodexReader:
    """Documented read-only JSON-RPC methods; no threads are resumed/started."""
    def __init__(self):
        self.process=None; self.sequence=0; self.pending={}; self.lock=threading.Lock();self.cached=None;self.cached_at=0

    def start(self):
        if self.process and self.process.poll() is None: return
        # A separate reader must not treat notLoaded as completed or running.
        self.process=subprocess.Popen([*codex_command(),'app-server','--stdio'],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,bufsize=1)
        def read():
            for line in self.process.stdout:
                try: item=json.loads(line)
                except ValueError: continue
                if 'id' in item:
                    with self.lock: target=self.pending.get(item['id'])
                    if target: target.put(item)
        threading.Thread(target=read,daemon=True).start()
        self.rpc('initialize',{'clientInfo':{'name':'passport_readonly','version':'1.0'},
                              'capabilities':{'experimentalApi':True,'explicitGatewayOauth':True}})
        self.process.stdin.write('{"method":"initialized"}\n'); self.process.stdin.flush()

    def rpc(self,method,params=None):
        if method not in ('initialize','account/rateLimits/read','thread/list'): raise ValueError('read-only methods only')
        with self.lock:
            self.sequence+=1; identity=self.sequence; target=queue.Queue(1); self.pending[identity]=target
        item={'id':identity,'method':method}
        if params is not None: item['params']=params
        try:
            self.process.stdin.write(json.dumps(item)+'\n'); self.process.stdin.flush()
            response=target.get(timeout=20)
            if 'error' in response: raise RuntimeError('Codex read endpoint unavailable')
            return response['result']
        finally:
            with self.lock: self.pending.pop(identity,None)

    def snapshot(self):
        if self.cached is not None and time.monotonic()-self.cached_at<10:return dict(self.cached)
        self.start()
        limits=self.rpc('account/rateLimits/read')
        recent=self.rpc('thread/list',{'limit':12,'sortKey':'updated_at','useStateDbOnly':True,
            'sourceKinds':['cli','vscode','appServer','unknown']})
        buckets=limits.get('rateLimitsByLimitId') or {'codex':limits.get('rateLimits')}
        windows=[]
        for bucket,info in buckets.items():
            if not info: continue
            for name in ('primary','secondary'):
                window=info.get(name)
                if window and window.get('usedPercent') is not None:
                    windows.append(dict(window,bucket=bucket))
        tasks=[dict({'id':t['id'],'title':(t.get('name') or t.get('preview') or '未命名任务')[:80],
                'status':t.get('status',{}).get('type','unknown'),'updated':t.get('updatedAt')},**self.event_status(t.get('path')))
               for t in recent.get('data',[])]
        self.cached={'windows':windows,'tasks':tasks,'source':'Codex app-server read-only'};self.cached_at=time.monotonic()
        return dict(self.cached)

    @staticmethod
    def event_status(value):
        # Only event metadata from app-server's own rollout paths is retained.
        if not value: return {}
        path=Path(value).resolve(); home=Path.home()/'.codex'
        if not any(path.is_relative_to(home/name) for name in ('sessions','archived_sessions')): return {}
        try:
            with path.open('rb') as source:
                source.seek(0,2); source.seek(max(0,source.tell()-300000)); lines=source.read(300000).splitlines()
            result={}
            for line in lines:
                try: item=json.loads(line)
                except ValueError: continue
                event=item.get('payload',{}); kind=event.get('type')
                if item.get('type')=='event_msg' and kind in ('task_started','task_complete','turn_aborted'):
                    result={'event':kind,'event_at':item.get('timestamp'),'event_id':event.get('turn_id') or item.get('timestamp'),
                            'observed_status':{'task_started':'running','task_complete':'completed','turn_aborted':'aborted'}[kind]}
            return result
        except (OSError,ValueError): return {}

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try: self.process.wait(3)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.process=None


CLAUDE_LIMITS=Path.home()/'.claude/passport-rate-limits.json'
def claude_usage():
    """Claude Code plan limits, read with its own login (Mac keychain; on Windows its credentials file); the token is
    never refreshed or kept."""
    if sys.platform=='win32':raw=(Path.home()/'.claude/.credentials.json').read_text()
    else:raw=subprocess.run(['security','find-generic-password','-s','Claude Code-credentials','-w'],capture_output=True,text=True,timeout=10,check=True).stdout
    request=urllib.request.Request('https://api.anthropic.com/api/oauth/usage',headers={
        'Authorization':'Bearer '+json.loads(raw)['claudeAiOauth']['accessToken'],'anthropic-beta':'oauth-2025-04-20'})
    # System proxy settings apply: this endpoint may need the same route Claude Code itself uses.
    with urllib.request.urlopen(request,timeout=8) as response: data=json.loads(response.read(200_000))
    windows=[]
    for name,minutes in (('five_hour',300),('seven_day',10080)):
        window=data.get(name) or {}
        if window.get('utilization') is None: continue
        resets=window.get('resets_at')
        windows.append({'usedPercent':window['utilization'],'windowDurationMins':minutes,
                        'resetsAt':int(datetime.fromisoformat(resets).timestamp()) if resets else 0})
    return {'windows':windows,'source':'Claude usage read-only'}


def iso_epoch(value):
    return int(datetime.fromisoformat(value.replace('Z','+00:00')).timestamp()) if value else 0


def cursor_usage():
    """Cursor's included usage for the current period, read with the editor's own login (its state.vscdb) from the
    dashboard endpoint the usage page uses; the token is never refreshed or kept. Two rows: the whole plan, then the
    API-priced models. usage-summary still answers, but it keeps the old monthly pool at 0% used."""
    import base64,sqlite3
    base=Path(os.environ.get('APPDATA',''))/'Cursor' if sys.platform=='win32' else Path.home()/'Library/Application Support/Cursor'
    with sqlite3.connect(f"file:{base/'User/globalStorage/state.vscdb'}?mode=ro",uri=True,timeout=2) as db:
        row=db.execute("select value from ItemTable where key='cursorAuth/accessToken'").fetchone()
    if not row:raise FileNotFoundError('Cursor not signed in')
    token=row[0];claims=token.split('.')[1];claims=json.loads(base64.urlsafe_b64decode(claims+'='*(-len(claims)%4)))
    user=claims['sub'].split('|')[-1]
    request=urllib.request.Request('https://cursor.com/api/dashboard/get-current-period-usage',data=b'{}',headers={'Cookie':f'WorkosCursorSessionToken={user}%3A%3A{token}',
        'Content-Type':'application/json','User-Agent':'Mozilla/5.0','Origin':'https://cursor.com','Referer':'https://cursor.com/dashboard'})
    with urllib.request.urlopen(request,timeout=8) as response:data=json.loads(response.read(200_000))
    plan=data.get('planUsage') or {}
    def seconds(value):
        if isinstance(value,str) and value.isdigit():value=int(value)
        if isinstance(value,(int,float)):return int(value//1000) if value>10**11 else int(value)
        return iso_epoch(value) if isinstance(value,str) else 0
    start,end=seconds(data.get('billingCycleStart')),seconds(data.get('billingCycleEnd'))
    minutes=max(1,(end-start)//60) if end>start else 10080
    return {'windows':[{'usedPercent':plan[key],'windowDurationMins':minutes,'resetsAt':end} for key in ('totalPercentUsed','apiPercentUsed') if isinstance(plan.get(key),(int,float))],
            'source':'Cursor current-period usage read-only'}


def opencode_usage():
    """OpenCode Go's limits (5 hours, week, month) with the key OpenCode saved; two rows: the 5 hours, then whichever
    of the week and the month is further used."""
    key=json.loads((Path.home()/'.local/share/opencode/auth.json').read_text())['opencode-go']['key']
    request=urllib.request.Request('https://opencode.ai/zen/go/v1/usage',headers={'Authorization':'Bearer '+key,'User-Agent':'AI-Passport/1.0'})
    with urllib.request.urlopen(request,timeout=8) as response:usage=json.loads(response.read(200_000))['usage']
    row=lambda name,minutes:{'usedPercent':usage[name]['percent'],'windowDurationMins':minutes,'resetsAt':iso_epoch(usage[name].get('resetsAt'))}
    longer=max((row('weekly',10080),row('monthly',43200)),key=lambda w:w['usedPercent'])
    return {'windows':[row('rolling',300),longer],'source':'OpenCode Go usage read-only'}


def gemini_usage():
    """Gemini's limits in Antigravity (the agy CLI's login, Mac keychain). The token lasts an hour and only agy refreshes
    it, so an expired one is renewed by running `agy models`, which writes the new token back. Two
    rows: Gemini, then third-party (Claude/GPT), each at its most used model. Antigravity gives a reset time per model
    but no window length, so the rows carry the 5-hour cadence it reports."""
    import base64
    if sys.platform=='win32':raise FileNotFoundError('Gemini usage is read on the Mac only')
    def read():
        raw=subprocess.run(['security','find-generic-password','-s','gemini','-a','antigravity','-w'],capture_output=True,text=True,timeout=10,check=True).stdout.strip()
        return json.loads(base64.b64decode(raw.split(':',1)[1]))['token']
    token=read()
    expiry=token.get('expiry')
    if expiry and datetime.fromisoformat(expiry).timestamp()<time.time()+60:
        agy=shutil.which('agy') or str(Path.home()/'.local/bin/agy')  # launchd's PATH lacks ~/.local/bin
        subprocess.run([agy,'models'],stdin=subprocess.DEVNULL,capture_output=True,timeout=30,check=True);token=read()
    token=token['access_token']
    # The API answers only the Antigravity client: a plain Python request is refused with 403.
    request=urllib.request.Request('https://daily-cloudcode-pa.googleapis.com/v1internal:fetchAvailableModels',data=b'{}',headers={
        'Authorization':'Bearer '+token,'Content-Type':'application/json','User-Agent':'antigravity/1.4.10'})
    with urllib.request.urlopen(request,timeout=8) as response:models=json.loads(response.read(2_000_000))['models']
    families={'Gemini':[],'third_party':[]}
    for name,model in models.items():
        info=model.get('quotaInfo') or {}; fraction=info.get('remainingFraction')
        if 'image' in name or not isinstance(fraction,(int,float)):continue
        if name.startswith('gemini-'):families['Gemini'].append((fraction,iso_epoch(info.get('resetTime'))))
        elif name.startswith(('claude-','gpt-')):families['third_party'].append((fraction,iso_epoch(info.get('resetTime'))))
    windows=[]
    for family in ('Gemini','third_party'):
        if not families[family]:continue
        fraction,resets=min(families[family])
        windows.append({'usedPercent':round((1-fraction)*100,1),'windowDurationMins':300,'resetsAt':resets})
    return {'windows':windows,'source':'Antigravity Gemini quota read-only'}

class Services:
    def __init__(self,simulator):
        self.sim=simulator; self.stop=threading.Event(); self.jobs=queue.Queue(4)
        self.metrics=[]; self.details={}; self.codex=CodexReader(); self.active=None
        self.previous_net=(time.monotonic(),psutil.net_io_counters()); self.previous_tasks={}; self.seen_completions=set(); self.claude=(0,None,0);self.usage_cache={}
        self.audio=AudioBridge(); self.cloud=None; self.media=MediaPlayer(self.audio,HTTP); self.current_request=None
        self.requested_context=None
        self.dictation_ble=BluetoothDictation(self)
        self.guard=None;self.guard_context=None
        self.ledger=Ledger(user_data()/'记账.csv')
        self.mac=MacOpener(lambda:self.settings.get('mac_app_aliases',{}))
        if getattr(simulator,'physical_device',False):
            self.audio.endpoint=simulator.audio_endpoint;self.audio.token=simulator.token;self.audio.force_reconnect=True;self.audio.playback=False
            self.dictation_ble.stats.update(transport="BLE GATT (physical Mac adapter)",physical_adapter=True)
        self.sources=json.loads((RESOURCES/'source-hashes.json').read_text()) if PACKAGED else {name:hashlib.sha256((ROOT/'scripts'/name).read_bytes()).hexdigest()
                      for name in ('passport_services.py','passport_audio.py','passport_xiaozhi.py','passport_dictation.py','passport_suite_simulator.py','mac_dictation_bridge.cpp','passport_ppp.py','passport_ppp_gateway.cpp') if (ROOT/'scripts'/name).exists()}
        DATA.mkdir(parents=True,exist_ok=True); DATA.chmod(0o700)
        self.settings={'ip_location_allowed':False}
        if (DATA/'settings.json').exists(): self.settings.update(json.loads((DATA/'settings.json').read_text()))
        self.feishu=Feishu(self.settings,self.save_settings)
        threading.Thread(target=self.run,daemon=True).start()
        if sys.platform=='darwin':threading.Thread(target=self.feishu_loop,daemon=True).start()
        threading.Thread(target=self.heartbeat,daemon=True).start()

    def heartbeat(self):
        while not self.stop.wait(2):
            try:self.post({'control':'heartbeat'})
            except Exception:pass

    def submit(self,item):
        if item['operation'].startswith('open:'):
            threading.Thread(target=self.open_on_mac,args=(item,),daemon=True).start();return
        context=(item['app'],item['generation'])
        if context!=self.requested_context:
            self.requested_context=context
            self.audio.invalidate(item['generation'])
        try: self.jobs.put_nowait(item)
        except queue.Full:
            # Prefer the newest foreground request over already stale navigation.
            try:self.jobs.get_nowait()
            except queue.Empty:pass
            self.jobs.put_nowait(item); self.details['queue']='oldest request evicted'

    def mac_config(self):
        """The assistant page's 打开 Mac 程序: the user's words, every program, and what the badge asked for lately."""
        return {'aliases':self.settings.get('mac_app_aliases',{}),'apps':self.mac.catalog(),'history':self.mac.history}

    def set_mac_aliases(self,aliases):
        """Words to programs; each program must be one the page listed."""
        if not isinstance(aliases,dict) or len(aliases)>40:raise ValueError('aliases')
        known={a['path'] for a in self.mac.catalog()};clean={}
        for word,path in aliases.items():
            word=str(word).strip()
            if not 0<len(word)<=12 or path not in known:raise ValueError('alias')
            clean[word]=path
        self.settings['mac_app_aliases']=clean;self.save_settings()
        return {'saved':len(clean)}

    def open_on_mac(self,item):
        """打开 Mac 程序: whatever page the badge shows by now, it gets the note for its request."""
        try:note=self.mac.open(item['operation'][5:])
        except Exception as error:note='Mac 打开失败';self.details['mac_open_error']=type(error).__name__
        self.details['mac_open']=self.mac.history
        try:self.post({'mac':note,'request':item['request']})
        except Exception:pass

    def post(self,data):
        if getattr(self.sim,'physical_device',False):
            for attempt in range(6):
                try:return self.sim.request('/service',data)
                except LinkBusy:
                    if attempt==5:raise
                    if self.stop.wait(.05*2**attempt):raise ConnectionError('services stopped')
        raw=json.dumps(data,ensure_ascii=False).encode()
        if len(raw)>=2048: raise ValueError('C3 message too large')
        guest=getattr(self.sim,'guest',None) or ('http://127.0.0.1:18762' if self.sim.snapshot().get('ppp',{}).get('up') else GUEST)
        req=urllib.request.Request(guest+'/service',data=raw,headers=dict({'Content-Type':'application/json'},**getattr(self.sim,'headers',{})))
        with HTTP.open(req,timeout=5) as response: return json.load(response)

    def feishu_loop(self):
        """飞书提醒: every 2 s, new Feishu banners go to the badge on any page (it shows the alert in its footer); the
        飞书 page reloads itself when told."""
        while not self.stop.wait(2):
            alert,new=self.feishu.poll()
            if not new:continue
            shown=self.sim.snapshot().get('app')==49
            if not alert and not shown:continue
            note={'alert':bool(alert),'today':self.feishu.today()}
            if alert:note.update({'from':alert['from'],'text':alert['text'],'vip':alert['vip']})
            try:self.post({'feishu':note})
            except Exception:pass

    def feishu_test(self):
        """The settings page's 发一条测试提醒: an alert on the badge, and the message in its 飞书提醒 list."""
        item=self.feishu.test()
        self.post({'feishu':{'alert':True,'from':item['from'],'text':item['text'],'vip':False,'today':self.feishu.today()}})
        return {'sent':True}

    def feishu_page(self,op):
        """The 飞书提醒 page: a long Down steps the alert mode, a long Up brings the Feishu app forward on the computer."""
        note=''
        if self.feishu.last is None:self.feishu.poll()  # before the loop's first read (and on Windows: the reason)
        if op=='mode':
            mode=(self.feishu.config()['mode']+1)%3;self.feishu.configure(mode=mode);note=('全部提醒','只提醒特别关注','已关闭提醒')[mode]
        elif op=='open':
            opened=sys.platform=='darwin' and subprocess.run(['open','-b',FEISHU_BUNDLE],capture_output=True,timeout=8).returncode==0
            note='已在电脑上打开飞书' if opened else '电脑上没有找到飞书'
        page=self.feishu.page(note)
        if op=='enter':self.feishu.opened()
        return {'state':'飞书提醒','value':'','lines':[],'feishu':page}

    def monitor(self):
        cpu=psutil.cpu_percent(interval=.15); memory=psutil.virtual_memory(); net=psutil.net_io_counters()
        previous,counters=self.previous_net; now=time.monotonic(); elapsed=max(now-previous,.001)
        up=(net.bytes_sent-counters.bytes_sent)/elapsed; down=(net.bytes_recv-counters.bytes_recv)/elapsed
        self.previous_net=(now,net)
        swap=psutil.swap_memory();used=memory.total-memory.available
        # The writable APFS data volume; "/" is the sealed system snapshot.
        disk=psutil.disk_usage('/System/Volumes/Data' if os.path.isdir('/System/Volumes/Data') else '/')
        battery=psutil.sensors_battery();uptime=int(time.time()-psutil.boot_time());load=os.getloadavg()[0]
        self.details['monitor']={'cpu_percent':cpu,'memory_percent':memory.percent,'memory_total':memory.total,
                                 'memory_used':used,'swap_used':swap.used,'network_up_bytes_s':up,'network_down_bytes_s':down,
                                 'disk_percent':disk.percent,'disk_free':disk.free,'battery_percent':battery and battery.percent,
                                 'charging':bool(battery and battery.power_plugged),'load_1m':load,'uptime_s':uptime,'source':'macOS psutil'}
        return {'state':'实时数据','value':f'CPU {cpu:.0f}%',
                'metrics':{'cpu':round(cpu),'ram':round(memory.percent),'memory_used':used,'memory_total':memory.total,'net_up':round(up),'net_down':round(down),
                           'disk':round(disk.percent),'disk_free':disk.free,'battery':round(battery.percent) if battery else -1,
                           'charging':bool(battery and battery.power_plugged),'load':round(load,2),'cores':psutil.cpu_count(),'uptime':uptime},
                'lines':[f'内存 {memory.percent:.0f}%',f'上行 {up/1024:.0f} 下行 {down/1024:.0f} KB/s']}

    def weather(self):
        if not self.settings.get('ip_location_allowed'):raise PermissionError('IP location consent required')
        cached=self.details.get('weather')
        if cached and time.time()-cached['retrieved']<600:
            current=cached['weather']; quality=cached['air'];daily=cached['daily'];location=cached['location']
        else:
            located=fetch_json('https://ipwho.is/?fields=success,message,country,region,city,latitude,longitude&lang=zh-CN')
            if located.get('success') is not True:raise RuntimeError('IP geolocation unavailable')
            latitude=located.get('latitude');longitude=located.get('longitude')
            if not isinstance(latitude,(int,float)) or not -90<=latitude<=90 or not isinstance(longitude,(int,float)) or not -180<=longitude<=180:raise ValueError('invalid IP coordinates')
            location={k:located.get(k,'') for k in ('country','region','city','latitude','longitude')}
            location['source']='ipwho.is / public egress IP'
            parameters={'latitude':latitude,'longitude':longitude,
                'current':'temperature_2m,relative_humidity_2m,weather_code',
                'daily':'temperature_2m_max,temperature_2m_min','forecast_days':1,'timezone':'auto'}
            forecast=fetch_json('https://api.open-meteo.com/v1/forecast?'+urlencode(parameters))
            try:
                air=fetch_json('https://air-quality-api.open-meteo.com/v1/air-quality?'+urlencode({
                    'latitude':latitude,'longitude':longitude,'current':'us_aqi,pm2_5','timezone':'auto'}))
            except Exception:air={'current':{'us_aqi':None,'pm2_5':None}}
            current=forecast['current']; quality=air['current'];daily=forecast['daily']
            self.details['weather']={'city':location['city'] or location['region'] or location['country'],'location':location,'weather':current,'air':quality,'daily':daily,
                                      'source':'Open-Meteo / CAMS','retrieved':int(time.time())}
        condition=WEATHER_NAMES.get(current['weather_code'],'天气未知')
        minimum=daily['temperature_2m_min'][0];maximum=daily['temperature_2m_max'][0]
        if not isinstance(minimum,(int,float)) or not isinstance(maximum,(int,float)) or minimum>maximum:raise ValueError('daily temperature unavailable')
        city=location['city'] or location['region'] or location['country']
        city=city[:6] if any(ord(c)>127 for c in city) else city[:12]
        air=quality.get('us_aqi');aqi=f'{air:.0f}' if isinstance(air,(int,float)) else '--'
        return {'state':'IP 定位天气','value':'','weather':{'city':city,'condition':condition,'temperature':current['temperature_2m'],'minimum':minimum,'maximum':maximum,'humidity':current['relative_humidity_2m'],'code':current['weather_code'],'aqi':air},'lines':['',city+' '+condition,
                f"{current['temperature_2m']:.1f}°C 低{minimum:.0f}° 高{maximum:.0f}°",f"湿度 {current['relative_humidity_2m']}% · AQI {aqi}"]}

    def claude_snapshot(self):
        # One read a minute: the page polls every 10 seconds and these limits move slowly. A 429 (rate_limit_error)
        # comes often while Claude Code runs, probably from the same account limit: keep the last good windows for 30 minutes and retry
        # in 5, instead of blanking the card.
        # While Claude Code runs, /api/oauth/usage answers 429 but its status line gets these limits from every API reply.
        # A status line script can save them:  jq -c 'select(.rate_limits)|.rate_limits+{saved:now}' > ~/.claude/passport-rate-limits.json
        # A copy under 10 minutes old is used and the endpoint is not called.
        try:
            saved=json.loads(CLAUDE_LIMITS.read_text())
            if time.time()-saved['saved']<600:
                data={'windows':[{'usedPercent':w['used_percentage'],'windowDurationMins':minutes,'resetsAt':int(w['resets_at'])}
                                 for name,minutes in (('five_hour',300),('seven_day',10080)) if (w:=saved.get(name))],
                      'source':'Claude Code status line'}
                self.details['claude']=data; return data['windows']
        except (OSError,ValueError,KeyError,TypeError): pass
        now=time.monotonic(); next_at,data,good_at=self.claude
        if now>=next_at:
            try: data=claude_usage(); good_at=now; next_at=now+60
            except Exception as error:
                next_at=now+300
                if data is None or now-good_at>1800: data={'windows':[],'error':type(error).__name__}
            self.claude=(next_at,data,good_at); self.details['claude']=data
        return data['windows']

    def usage_snapshot(self,name,read):
        """Cursor and OpenCode Go like Claude Code: one read a minute, failures as no rows."""
        at,data=self.usage_cache.get(name,(0,None))
        if time.monotonic()-at>=60 or data is None:
            try:data=read()
            except Exception as error:data={'windows':[],'error':type(error).__name__}
            self.usage_cache[name]=(time.monotonic(),data);self.details[name]=data
        return data['windows']

    def codex_snapshot(self):
        # Codex and Claude Code fail independently; the page errors only when neither can be read.
        try: data=self.codex.snapshot(); failure=None
        except Exception as error: data={'windows':[],'tasks':[]}; failure=error
        self.details['codex']=data; claude=self.claude_snapshot()
        gemini=self.usage_snapshot('gemini',gemini_usage);cursor=self.usage_snapshot('cursor',cursor_usage);opencode=self.usage_snapshot('opencode',opencode_usage)
        if failure and not (claude or gemini or cursor or opencode): raise failure
        windows=sorted(data['windows'],key=lambda w:w.get('windowDurationMins') or 10**9)
        lines=[]
        completed=[t for t in data['tasks'] if t.get('event')=='task_complete' and (t['id'],t['event_id']) not in self.seen_completions]
        if completed:
            task=max(completed,key=lambda t:t.get('event_at') or '')
            notice='任务已完成'
            self.seen_completions.update((t['id'],t['event_id']) for t in completed)
            data['notice_source']={'thread_id':task['id'],'event':'task_complete','at':task['event_at']}
        else: notice=''
        # Ten fixed slots, two a tool: Codex short/long window, Claude Code 5 hours/7 days, Gemini Pro/Flash, Cursor
        # plan/API, OpenCode Go 5 hours/week or month; missing ones read unknown.
        slots=lambda found:[{'used':round(w['usedPercent']),'minutes':w.get('windowDurationMins') or 0,'resets':w.get('resetsAt') or 0} for w in found[:2]]+[{'used':-1,'minutes':0,'resets':0}]*(2-len(found[:2]))
        quotas=slots(windows)+slots(claude)+slots(gemini)+slots(cursor)+slots(opencode)
        return {'state':'额度已更新','value':'','windows':quotas,'lines':lines,'notice':notice}

    def push(self,payload,context=None):
        request=self.current_request
        if request is None: return
        if self.requested_context!=(request['app'],request['generation']): return
        if context is not None and context!=(request['app'],request['generation']): return
        result=dict(payload,stream=True,epoch=int(time.time()))
        result.setdefault('phase','error' if '断开' in payload.get('state','') else 'ready')
        result.update({k:request[k] for k in ('request','app','generation')})
        try: self.post(result)
        except Exception: pass

    def stop_media(self): self.media.close()

    def save_settings(self):
        private_write(DATA/'settings.json',json.dumps(self.settings,ensure_ascii=False).encode())

    def guard_rssi(self):
        """离座锁屏's reading: dBm of the Bluetooth link, None while it is down, 'usb' on the cable."""
        if not getattr(self.sim,'physical_device',False):return None
        # Windows cannot read a connected device's signal: treat it as the cable (keep awake, lock on request), since
        # no reading at all would look like leaving and lock the screen.
        if self.sim.transport=='usb' or sys.platform=='win32':return 'usb'
        try:return self.sim.radio.rpc('rssi')['result'].get('rssi')
        except Exception:return None

    def guard_loop(self,context):
        """Once a second while the badge shows 离座锁屏 (a badge out of range still reports it): read, decide, and
        tell the badge when something it shows changed."""
        shown=None;sent=0
        try:
            while not self.stop.wait(1) and self.guard_context==context:
                current=self.sim.snapshot()
                if (current.get('app'),current.get('generation'))!=context:break
                state=self.guard.tick();g=state['guard']
                self.details['guard']=dict(g,events=self.guard.events[-6:])
                key=(g['phase'],g['left'],g['rssi']//3,g['line'],g['base'])
                if key!=shown or time.monotonic()-sent>5:
                    linked=self.sim.transport=='usb' or g['rssi']!=0
                    if linked:self.push(state,context);shown=key;sent=time.monotonic()
        finally:
            if self.guard_context==context:self.guard_context=None
            self.guard.stop()

    def response(self,request):
        app=request['app']; op=request['operation']; self.current_request=request
        if self.active!=(app,request['generation']):
            self.dictation_ble.close()
            if app!=9: self.stop_media()
            if self.cloud:self.cloud.suspend()
            self.audio.stop_microphone(); self.audio.drain()
            if app in (1,2,9) and not(app==2 and getattr(self.sim,'physical_device',False)) and op!='audio_unavailable': self.audio.activate(app,request['generation'])
            self.active=(app,request['generation'])
        if op=='audio_unavailable': return {'state':'音频不可用','value':'--','lines':['C3 音频初始化或内存准入失败']}
        if app==0: return self.weather()
        if app==5: return self.monitor()
        if app==3: return self.codex_snapshot()
        if app==4: return {'state':'本地计时','value':'','lines':[]}
        if app==49: return self.feishu_page(op)
        if app==41:
            # 离座锁屏: the badge's settings come with every request; the loop runs while the page is open.
            if self.guard is None:self.guard=Guard(self.guard_rssi,self.settings,self.save_settings)
            self.guard.configure(op);context=(app,request['generation'])
            if self.guard_context!=context:
                self.guard_context=context;threading.Thread(target=self.guard_loop,args=(context,),daemon=True).start()
            return self.guard.state()
        if app==2:
            if op=='enter': self.dictation_ble.open((app,request['generation']))
            return self.dictation_ble.status()
        if app==1:
            if self.cloud is not None and not self.hello_ready():
                self.cloud.close();self.cloud=None
            if self.cloud is None:
                context=(app,request['generation'])
                self.cloud=getattr(self.sim,"cloud_class",XiaoZhi)(self.audio,lambda payload:self.push(payload,context)); self.audio.callback=self.cloud.send_opus
                try: self.cloud.connect()
                except Exception: self.cloud.close(); self.cloud=None; raise
            self.cloud.resume(request['generation'],lambda payload:self.push(payload,(app,request['generation'])))
            if op=='toggle':
                if self.cloud.listening: self.cloud.end_listen()
                else: self.cloud.listen()
            return {'state':'正在听' if self.cloud.listening else '云端就绪','value':'小智语音',
                    'lines':[self.cloud.stats['stt'][:80],self.cloud.stats['answer'][:160],'确认：开始 / 结束说话']}
        if app==9:
            context=(app,request['generation'])
            self.media.update=lambda payload:self.push(payload,context)
            if op=='toggle':
                if self.media.active: self.media.close()
                else: self.media.start()
            return {'state':('正在播放' if self.media.details.get('compressed_bytes') else '缓冲中') if self.media.active else '已停止',
                    'value':'Radio','media':{'index':0,'count':1},
                    'lines':[self.media.details.get('name','SomaFM')[:80],
                             '确认：播放 / 停止','','C3 原生 MP3 解码']}
        return {'state':'系统','value':'','lines':[]}

    def run(self):
        while not self.stop.is_set():
            try: request=self.jobs.get(timeout=.2)
            except queue.Empty: continue
            current=self.sim.snapshot()
            if current.get('app')!=request['app'] or current.get('generation')!=request['generation']: continue
            started=time.monotonic()
            try:
                result=self.response(request); ok=True
            except Exception as error:
                reason='连接超时，请重试' if isinstance(error,TimeoutError) else '连接已断开，请重试'
                if isinstance(error,PermissionError):reason='Mac 开启助手的辅助功能权限' if request['app']==2 else '请先允许公网 IP 定位'
                if isinstance(error,FileNotFoundError):reason='请在电脑登录 Codex 或 Claude Code' if request['app']==3 else '电脑适配组件不可用'
                result={'state':'连接失败','value':'--','lines':[reason,'请检查电脑伴随服务','确认：重新连接']}; ok=False
                if isinstance(error,PermissionError) and request['app']==2:
                    result={'state':'需要电脑授权','value':'辅助功能权限','lines':[reason,'授权后退出并重新进入本页']}
                self.details['last_error']={'app':request['app'],'type':type(error).__name__}
                import traceback
                self.details['last_error']['frames']=[{'file':Path(frame.filename).name,'line':frame.lineno,'function':frame.name} for frame in traceback.extract_tb(error.__traceback__)]
            result.update({k:request[k] for k in ('request','app','generation')}); result['epoch']=int(time.time());result['phase']='ready' if ok else 'error'
            try: self.post(result)
            except Exception: ok=False
            self.metrics.append({'request':request['request'],'app':request['app'],'operation':request['operation'],'ok':ok,
                                 'ms':round((time.monotonic()-started)*1000,2),'generation':request['generation'],'finished_at':time.time()})
            self.metrics=self.metrics[-256:]

    def chat_test(self):
        if not self.cloud or not self.hello_ready(): raise RuntimeError('open XiaoZhi and wait for cloud readiness first')
        previous_playback=self.audio.playback; self.audio.playback=False; self.audio.captured.clear()
        before_pcm=self.audio.stats['pcm_bytes']; before_tts=self.cloud.stats['tts_opus_bytes']
        started=time.monotonic(); self.cloud.listen(microphone=False)
        try:
            injected=self.audio.inject(ROOT/'results/passport-test-utterance.wav')
            time.sleep(.5); self.cloud.end_listen()
            end=time.monotonic()+45
            while time.monotonic()<end:
                if self.cloud.stats['tts_done'] and self.audio.stats['pcm_bytes']>before_pcm: break
                time.sleep(.1)
            time.sleep(.3)
        finally:
            if self.cloud.listening: self.cloud.end_listen()
            self.audio.playback=previous_playback
        result=dict(injected,asr=self.cloud.stats['stt'],answer=self.cloud.stats['answer'],
            tts_opus_bytes=self.cloud.stats['tts_opus_bytes']-before_tts,
            c3_decoded_pcm_bytes=self.audio.stats['pcm_bytes']-before_pcm,seconds=round(time.monotonic()-started,3))
        result['passed']=bool(result['asr'] and result['answer'] and result['tts_opus_bytes'] and result['c3_decoded_pcm_bytes'])
        import numpy as np
        signal=np.frombuffer(self.audio.captured,dtype='<i2').astype(float)
        result['pcm_rms']=round(float(np.sqrt(np.mean(signal**2))),2) if signal.size else 0
        result['tts_done']=self.cloud.stats['tts_done']
        result['asr_after_start_ms']=round((self.cloud.stats.get('stt_at',started)-started)*1000,1)
        result['tts_after_start_ms']=round((self.cloud.stats.get('tts_started_at',started)-started)*1000,1)
        return result

    def barge_test(self):
        """Known PCM enters the microphone HAL; only native VAD triggers abort."""
        import numpy as np
        if not self.hello_ready(): raise RuntimeError('open XiaoZhi first')
        cloud=self.cloud; audio=self.audio; generation=audio.generation
        playback=audio.playback; audio.playback=False; audio.captured.clear()
        baseline=dict(cloud.stats); encoded=self.sim.snapshot()['audio_encoded']
        def wait_for(predicate,seconds=45):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                if predicate(): return
                time.sleep(.05)
            raise TimeoutError('voice interruption stage timed out')
        raw=subprocess.run(['/opt/usr/bin/ffmpeg','-v','error','-i',str(ROOT/'results/passport-test-utterance.wav'),
            '-f','s16le','-ar','16000','-ac','1','pipe:1'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,timeout=20).stdout
        utterance=np.frombuffer(raw,dtype='<i2').astype(float)
        try:
            cloud.listen(microphone=False); audio.inject(ROOT/'results/passport-test-utterance.wav'); cloud.end_listen()
            wait_for(lambda:cloud.stats.get('tts_started_at',0)>baseline.get('tts_started_at',0) and len(audio.captured)>=cloud.rate)
            reference=np.frombuffer(bytes(audio.captured),dtype='<i2').astype(float)
            reference=np.interp(np.arange(0,len(reference),cloud.rate/16000),np.arange(len(reference)),reference)
            reference=reference*(1800/max(1,float(np.max(np.abs(reference)))))
            ref=np.resize(reference,16000*8); echo=np.concatenate((np.zeros(128),ref[:-128]))*.55
            def feed(mic,reference):
                started=time.monotonic()
                for offset in range(0,len(mic),960):
                    m=np.clip(mic[offset:offset+960],-32768,32767).astype('<i2').tobytes().ljust(1920,b'\0')
                    r=np.clip(reference[offset:offset+960],-32768,32767).astype('<i2').tobytes().ljust(1920,b'\0')
                    if not audio.send(6,m+r,generation): raise RuntimeError('audio generation changed')
                    time.sleep(max(0,started+(offset+960)/16000-time.monotonic()))
            before=cloud.stats['interruptions']
            feed(echo[:19200],ref[:19200]); time.sleep(.15)
            if cloud.stats['interruptions']!=before: raise AssertionError('speaker echo triggered interruption')
            during_tts=not cloud.stats['tts_done']
            near=utterance*(15000/max(1,float(np.max(np.abs(utterance)))))
            size=len(near); microphone=np.concatenate((near+echo[:size],np.zeros(19200)))
            reference=np.concatenate((ref[:size],np.zeros(19200)))
            trigger=time.monotonic(); feed(microphone,reference)
            wait_for(lambda:cloud.stats['auto_stops']>baseline['auto_stops'],8)
            wait_for(lambda:cloud.stats['stt'] and cloud.stats['tts_done'] and cloud.stats['answer'])
            result={'echo_only_frames':20,'echo_false_interruptions':0,'during_tts':during_tts,
                'asr':cloud.stats['stt'],'answer':cloud.stats['answer'],
                'interruptions':cloud.stats['interruptions']-baseline['interruptions'],
                'abort_sent':cloud.stats['abort_sent']-baseline['abort_sent'],
                'auto_stops':cloud.stats['auto_stops']-baseline['auto_stops'],
                'native_encoded_frames':self.sim.snapshot()['audio_encoded']-encoded,
                'first_word_retained':'你好' in cloud.stats['stt'],'native':self.sim.snapshot(),
                'seconds':round(time.monotonic()-trigger,3),'microphone_source':'fixed fixture through C3 microphone HAL',
                'echo_source':'actual C3 decoded cloud TTS, delayed 8ms, gain 0.55'}
            result['passed']=bool(result['during_tts'] and result['first_word_retained'] and result['interruptions']==1 and result['abort_sent']==1 and result['auto_stops']==1 and not result['native']['voice_overflows'] and not result['native']['audio_errors'])
            return result
        finally:
            if cloud.listening: cloud.end_listen()
            audio.playback=playback

    def hello_ready(self): return self.cloud is not None and self.cloud.hello.is_set() and self.cloud.connected.is_set()

    def helper(self,*arguments):
        binary=helper('passport-dictation')
        result=subprocess.run([str(binary),*map(str,arguments)],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=8)
        data=json.loads(result.stdout or b'{}')
        if result.returncode: raise RuntimeError('Mac helper failed')
        return data

    def require_dictation_permissions(self):
        # Windows needs no permission to paste (SendInput); only elevated windows refuse it.
        status={'accessibility':True,'post_event_access':True} if sys.platform=='win32' else self.helper('status');self.details['dictation_permissions']=status
        if not (status.get('accessibility') and status.get('post_event_access')):
            raise PermissionError('Mac keyboard event permission required')

    def snapshot(self):
        details=dict(self.details,audio=dict(self.audio.stats),media=dict(self.media.details))
        details['bluetooth_dictation']=dict(self.dictation_ble.stats)
        details['ledger']=dict(self.ledger.stats,rows=len(self.ledger.rows),path=str(self.ledger.path))
        if self.cloud: details['chat']=dict(self.cloud.stats,ready=self.hello_ready(),listening=self.cloud.listening)
        return {'active':self.active,'details':details,'metrics':self.metrics,'source_sha256':self.sources}

    def close(self):
        self.stop.set()
        for cleanup in (self.dictation_ble.close,self.stop_media,self.codex.close,
                        lambda:self.cloud.close() if self.cloud else None,self.audio.close):
            try: cleanup()
            except Exception: pass
