#!/usr/bin/env python3
"""Physical AI Passport companion. Prefers USB and falls back to paired BLE."""
import asyncio,collections,http.client,json,pathlib,queue,signal,struct,subprocess,threading,time,urllib.request
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from passport_services import Services
from passport_device_cloud import DeviceXiaoZhi
from passport_paths import config_file, helper, user_data, PACKAGED
from passport_identity import paired_port
from passport_computer_link import ble_request,UsbReports,RPC_SERVICE,RPC_READ,LinkBusy
ROOT=pathlib.Path(__file__).resolve().parents[1]
HTTP=urllib.request.build_opener(urllib.request.ProxyHandler({}))
SERVICE='5d67d66a-70ed-4e30-9f6c-4fbddce3b010'
KEY='5d67d66a-70ed-4e30-9f6c-4fbddce3b011'
AUDIO='5d67d66a-70ed-4e30-9f6c-4fbddce3b012'
ACK='5d67d66a-70ed-4e30-9f6c-4fbddce3b013'

def connected_passport(expected):
 from CoreBluetooth import CBUUID
 from bleak.backends.corebluetooth.CentralManagerDelegate import CentralManagerDelegate
 from bleak.backends.device import BLEDevice
 manager=CentralManagerDelegate.alloc().init()
 peers=manager.central_manager.retrieveConnectedPeripheralsWithServices_([CBUUID.UUIDWithString_(SERVICE)])
 matches=[p for p in peers if p.name()==expected]
 if len(matches)>1:raise ConnectionError('paired Bluetooth identity is ambiguous')
 if not matches:return None
 peer=matches[0]
 return BLEDevice(str(peer.identifier().UUIDString()),str(peer.name()),(peer,manager),-127)

class Radio:
 def __init__(self,mac):
  self.loop=asyncio.new_event_loop();self.client=None;self.device=None;self.events=collections.deque(maxlen=1024);self.lock=threading.Lock();self.dropped=0
  self.expected='AI-Passport-'+mac.replace(':','').upper()
  self.rpc_lock=asyncio.Lock();self.connect_lock=asyncio.Lock();self.sequence=0
  self.thread=threading.Thread(target=self.loop.run_forever,daemon=True);self.thread.start()
 def rpc(self,action,**values):
  future=asyncio.run_coroutine_threadsafe(self.call(action,values),self.loop)
  try:return future.result(40)
  except TimeoutError:future.cancel();raise
 def request(self,token,path,data=None):
  future=asyncio.run_coroutine_threadsafe(self.request_async(token,path,data),self.loop)
  try:return future.result(40)
  except TimeoutError:future.cancel();raise
 async def request_async(self,token,path,data):
  from bleak.exc import BleakError
  async with self.rpc_lock:
   await self.call('connect',{})
   self.sequence=self.sequence%0xffffffff+1
   try:return await ble_request(self.client,self.sequence,token,path,data)
   except (BleakError,TimeoutError):
    # A system connection can survive failed security; retry with a fresh client.
    self.device=None
    await self.call('disconnect',{})
    raise
 def notify(self,report,data):
  with self.lock:
   if len(self.events)==1024:self.dropped+=1
   self.events.append({'report':report,'hex':bytes(data).hex()})
 async def call(self,action,values):
  from bleak import BleakClient,BleakScanner
  connected=bool(self.client and self.client.is_connected)
  if action=='status':return {'ok':True,'result':{'connected':connected,'physical_adapter':True}}
  if action=='diagnostics':
   services=list(self.client.services) if connected else []
   return {'connected':connected,'services':[s.uuid for s in services],'characteristics':[c.uuid for s in services for c in s.characteristics]}
  if action in ('scan','connect'):
   async with self.connect_lock:
    if not (self.client and self.client.is_connected):
     if self.device is None:
      # A peripheral connected by macOS may have stopped advertising.
      self.device=connected_passport(self.expected)
      if self.device is None:self.device=await BleakScanner.find_device_by_filter(lambda device,adv:adv.local_name==self.expected and SERVICE in [s.lower() for s in (adv.service_uuids or [])],timeout=6,service_uuids=[SERVICE])
      if self.device is None:raise RuntimeError('paired Passport is not advertising')
     client=BleakClient(self.device,services=[SERVICE,RPC_SERVICE],disconnected_callback=lambda _:self.notify(0,b''),timeout=6)
     try:
      await client.connect()
      # Reading the encrypted key starts pairing; leave time to accept the macOS pairing prompt.
      async with asyncio.timeout(60):
       await client.read_gatt_char(KEY)
       await client.read_gatt_char(RPC_READ)
     except BaseException:
      self.device=None
      if client.is_connected:await client.disconnect()
      raise
     self.client=client
    return {'ok':True,'result':{'connected':self.client.is_connected}}
  if action=='rssi':
   # 离座锁屏: the connected badge's signal (CoreBluetooth readRSSI), None while the link is down.
   if not connected:return {'ok':True,'result':{'rssi':None}}
   try:
    async with asyncio.timeout(2):return {'ok':True,'result':{'rssi':int(await self.client.get_rssi())}}
   except Exception:return {'ok':True,'result':{'rssi':None}}
  if action=='pair':
   await self.client.read_gatt_char(KEY);return {'ok':True,'result':{'encrypted_characteristic_read':True}}
  if action=='voice_subscribe':
   with self.lock:self.events.clear();self.dropped=0
   for uuid in [KEY,AUDIO]:
    try:await self.client.stop_notify(uuid)
    except Exception:pass
   await self.client.start_notify(KEY,lambda _,data:self.notify(1,data));await self.client.start_notify(AUDIO,lambda _,data:self.notify(2,data));return {'ok':True,'result':{}}
  if action=='voice_events':
   with self.lock:events=list(self.events);self.events.clear();dropped=self.dropped;self.dropped=0
   return {'ok':True,'result':{'connected':connected,'events':events,'dropped':dropped}}
  if action=='voice_ack':
   data=struct.pack('<BII',values['kind'],values['generation'],values['session']).ljust(16,b'\0');await self.client.write_gatt_char(ACK,data,response=True);return {'ok':True,'result':{}}
  if action=='disconnect':
   async with self.connect_lock:
    if self.client and self.client.is_connected:await self.client.disconnect()
   return {'ok':True,'result':{}}
  raise ValueError('unsupported physical BLE action')
class Device:
 physical_device=True
 cloud_class=DeviceXiaoZhi
 def __init__(self,config=None,gateway=None,gateway_log=None):
  self.config=config if config is not None else json.loads(config_file().read_text());self.token=self.config['api_token'];self.headers={'Authorization':'Bearer '+self.token}
  self.guest='http://127.0.0.1:'+str(self.config['http_forward']);self.audio_endpoint=('127.0.0.1',self.config['audio_forward'])
  self.http_local=threading.local();self.state={};self.lock=threading.Lock();self.command_lock=threading.Lock();self.native_lock=threading.Lock();self.next_id=0;self.next_control=0;self.stop=threading.Event();self.radio=None;self.services=None;self.thread=None;self.last_seen=0;self.transport='none';self.link_changed=True;self.usb_reports=UsbReports(self.audio_endpoint,self.token)
  self.gateway_log=gateway_log or ((user_data() if PACKAGED else ROOT/'results/passport-device')/'gateway.log').open('a');self.gateway=gateway
  try:self.initialize()
  except Exception:self.close();raise
 def initialize(self):
  self.radio=Radio(self.config['mac'])
  if self.gateway is not None:self.transport='usb'
  self.state={'app':0,'generation':0,'command_id':0,'ble_stack':False,'transport':'none'}
  self.services=Services(self);self.thread=threading.Thread(target=self.poll,daemon=True);self.thread.start()
 def select_transport(self):
  from serial.tools import list_ports
  ports=list(list_ports.comports())
  own=[p for p in ports if p.vid==0x303a and (p.serial_number or '').replace(':','').replace('-','').lower()==self.config['mac'].replace(':','').lower()]
  desired='usb' if own else 'ble'
  if len(own)>1:raise ConnectionError('paired USB identity is ambiguous')
  if desired!=self.transport:
   self.services.dictation_ble.close();self.usb_reports.close()
   if self.gateway:
    self.gateway.terminate()
    try:self.gateway.wait(3)
    except subprocess.TimeoutExpired:self.gateway.kill();self.gateway.wait()
    self.gateway=None
   if desired=='usb':
    try:self.radio.rpc('disconnect')
    except Exception:pass
   self.transport=desired;self.link_changed=True;self.services.active=None
  if desired=='usb' and (self.gateway is None or self.gateway.poll() is not None):self.start_gateway();self.link_changed=True
  if desired=='ble' and not self.radio.rpc('status')['result']['connected']:
   self.radio.rpc('connect');self.link_changed=True
 def start_gateway(self):
  from serial.tools import list_ports
  self.config['port']=paired_port(self.config['mac'],list_ports.comports())
  self.gateway=subprocess.Popen([str(helper('passport-ppp')),'--serial',self.config['port'],'--forward',str(self.config['http_forward']),'--audio-forward',str(self.config['audio_forward'])],stdout=self.gateway_log,stderr=subprocess.STDOUT)
 def request(self,path,data=None):
  if self.transport=='ble':return self.radio.request(self.token,path,data)
  if self.transport!='usb':raise ConnectionError('device transport not ready')
  return self.http_request(path,data)
 def http_request(self,path,data=None):
  raw=None if data is None else json.dumps(data,ensure_ascii=False).encode();headers=dict(self.headers)
  if raw is not None:headers['Content-Type']='application/json'
  for attempt in range(2 if data is None else 1):
   connection=getattr(self.http_local,'connection',None)
   if connection is None:
    connection=http.client.HTTPConnection('127.0.0.1',self.config['http_forward'],timeout=5);self.http_local.connection=connection
   try:
    connection.request('GET' if raw is None else 'POST',path,body=raw,headers=headers);response=connection.getresponse();body=response.read()
    if response.status==503 or (response.status==500 and b'queue full' in body):raise LinkBusy('device service queue busy')
    if response.status!=200:raise RuntimeError('physical HTTP '+str(response.status))
    return json.loads(body)
   except Exception:
    connection.close();self.http_local.connection=None
    if attempt+1==(2 if data is None else 1):raise
 def snapshot(self):
  with self.lock:return dict(self.state,boot=True,physical_device=True,emulator_alive=False,transport=self.transport,status_age_seconds=round(time.monotonic()-self.last_seen,1) if self.last_seen else None,link_ready=bool(self.last_seen and time.monotonic()-self.last_seen<10 and self.state.get('transport')==self.transport))
 def command(self,name):
  with self.command_lock:return self.send_command(name)
 def status_labels(self,labels):
  # The status sign's own labels: up to five, under 40 UTF-8 bytes each; the device checks again.
  labels=[str(text).strip() for text in labels][:5]
  if any(len(text.encode())>=40 for text in labels):raise ValueError('status label too long')
  with self.command_lock:return self.send_command('status_labels',labels=[text for text in labels if text])
 def countdowns(self,events,offwork):
  # 倒数日: up to six {name, date YYYY-MM-DD, yearly, lunar} and the 下班 time; the device validates again.
  events=[{'name':str(e['name']).strip(),'date':str(e['date']),'yearly':bool(e.get('yearly')),'lunar':bool(e.get('lunar'))} for e in events][:6]
  if any(len(e['name'].encode())>=25 or not e['name'] for e in events):raise ValueError('countdown name')
  with self.command_lock:return self.send_command('countdowns',events=events,offwork=str(offwork))
 def qr_codes(self,codes):
  # One command per slot keeps each request under the device's 1 KiB limit.
  codes=(list(codes)+[{}]*4)[:4];result=None
  for index,code in enumerate(codes):
   title,text=str(code.get('title','')).strip(),str(code.get('text','')).strip()
   if len(title.encode())>=40 or len(text.encode())>=301:raise ValueError('qr entry too long')
   with self.command_lock:result=self.send_command('qr_code',index=index,title=title or ('二维码 %d'%(index+1)),text=text)
  return result
 def send_command(self,name,**extra):
  allowed={'up','down','ok','back','settings','wake','wifi_connect','wifi_disconnect','ap_on','ap_off','ble_on','ble_off','scan','ppp_probe','configure_wifi','refresh_service','status_labels','countdowns','qr_code'}
  if name not in allowed:raise ValueError('unsupported physical command')
  with self.lock:self.next_id=max(self.next_id,self.state.get('command_id',0))+1;identity=self.next_id
  self.request('/command',{'id':identity,'command':name,**extra});until=time.monotonic()+20
  while time.monotonic()<until:
   state=self.request('/status')
   with self.lock:self.state=dict(state,transport=self.transport);self.last_seen=time.monotonic()
   if state.get('command_id',0)==identity:
    navigation={k:state.get(k) for k in ('app','generation','selection','dictation_phase','command_id')}
    return {'id':identity,'result':state['last_result'],'navigation':navigation}
   time.sleep(.05)
  raise TimeoutError('physical command acknowledgement missing')
 def native(self,data):
  with self.native_lock:
   self.next_control+=1;identity=self.next_control;self.request('/service',dict(data,id=identity));until=time.monotonic()+12
   while time.monotonic()<until:
    state=self.request('/app_state')
    if state['control_id']==identity:
     if state['control_result']:raise ValueError('device rejected control '+str(state['control_result']))
     return {'id':identity,'result':0}
    time.sleep(.05)
   raise TimeoutError('physical control acknowledgement missing')
 def ble(self,action,**values):
  if self.transport!='usb':return self.radio.rpc(action,**values)
  if action=='status':result={'connected':self.usb_reports.connected,'physical_adapter':False,'transport':'USB'}
  elif action in ('connect','scan'):
   self.usb_reports.open(self.state['generation']);result={'connected':True}
  elif action=='pair':result={'usb_authenticated':True}
  elif action=='voice_subscribe':self.usb_reports.clear();result={}
  elif action=='voice_events':result=self.usb_reports.poll()
  elif action=='voice_ack':self.usb_reports.ack(**values);result={}
  elif action=='disconnect':self.usb_reports.close();result={}
  else:raise ValueError('unsupported USB report action')
  return {'ok':True,'result':result}
 def submit_event(self,event):
  if event['operation'].startswith('open:'):return self.services.submit(event)  # 打开 Mac 程序 outlives its page
  with self.lock:state=dict(self.state)
  if (state.get('app'),state.get('generation'))!=(event['app'],event['generation']):
   state=self.request('/status')
   assert state.get('simulator') is False and state.get('hardware',{}).get('display')
   with self.lock:self.state=dict(state,transport=self.transport)
   self.last_seen=time.monotonic()
  if (state.get('app'),state.get('generation'))==(event['app'],event['generation']):self.services.submit(event)
 def poll(self):
  last=0;selected=0
  while not self.stop.wait(.25):
   try:
    if time.monotonic()-selected>1:
     self.select_transport();selected=time.monotonic()
    interval=.7 if self.transport=='usb' else 3
    if self.link_changed or time.monotonic()-last>interval:
     state=self.request('/status')
     assert state.get('simulator') is False and state.get('hardware',{}).get('display')
     state['transport']=self.transport
     with self.lock:self.state=state;self.next_id=max(self.next_id,state['command_id'])
     last=time.monotonic();self.last_seen=last
     if self.link_changed:
      self.link_changed=False
      self.next_control=self.request('/app_state')['control_id']
      self.command('refresh_service')
    if self.services.ledger.due(self.state):
     try:self.services.ledger.sync(self.request);self.services.details.pop('ledger_error',None)
     except Exception as error:self.services.details['ledger_error']=type(error).__name__
    event=self.request('/events')
    if event:self.submit_event(event)
   except Exception as error:
    with self.lock:
     self.state['companion_error']=type(error).__name__
     self.state['missing_characteristic']=str(getattr(error,'char_specifier',''))
     from bleak.exc import BleakError
     if isinstance(error,BleakError):self.state['bluetooth_error']=str(error)[:240]
 def close(self):
  self.stop.set()
  self.usb_reports.close()
  if self.services:self.services.close()
  if self.thread:self.thread.join(6)
  if self.radio:
   try:self.radio.rpc('disconnect')
   except Exception:pass
   self.radio.loop.call_soon_threadsafe(self.radio.loop.stop)
   self.radio.thread.join(2)
  if self.gateway and self.gateway.poll() is None:
   self.gateway.terminate()
   try:self.gateway.wait(4)
   except subprocess.TimeoutExpired:self.gateway.kill();self.gateway.wait()
  self.gateway_log.close()
PAGE='''<!doctype html><html lang=zh-CN><meta charset=utf-8><title>AI Passport 实机</title><style>body{font:16px system-ui;background:#10191f;color:#e4ecee;margin:32px}button{padding:12px;margin:6px}pre{white-space:pre-wrap;max-width:1000px}</style><h1>AI Passport 实机</h1><p>USB / 蓝牙自动切换；设备麦克风、扬声器、屏幕与按键。确认进入功能，长按确认返回。</p><button onclick="act('up')">上</button><button onclick="act('down')">下</button><button onclick="act('ok')">确认</button><button onclick="act('back')">返回</button><button onclick="act('settings')">设置</button><h2>豆包输入结果</h2><p>在设备上按确认开始说话，再按确认结束。</p><textarea rows=5 cols=60 aria-label=豆包输入结果></textarea><h2 id=status>设备内容</h2><style>.row input,.row textarea{font-size:16px;padding:6px;margin:3px}.row{margin:4px 0}section{border-top:1px solid #2a3a44;padding:8px 0}</style>
<section><h3>状态牌文字</h3><p>显示在「状态牌」页面，排在预设标签前面。最多 5 条，每条不超过 8 个字，留空表示不用。在胸牌状态牌页按确认说话也能添加，发送会替换胸牌上的全部自定义文字。</p><div id=labels class=row></div><button onclick="saveLabels()">发送到设备</button> <span id=labelResult></span></section>
<section><h3>倒数日</h3><p>下班时间（留空表示不显示下班倒计时）：<input id=offwork type=time value="18:00"></p><p>自己的日子，最多 6 个；名称不超过 8 个字。勾「每年」按周年倒数，勾「农历」按农历月日。没勾「每年」且已过去的，显示「已经 N 天」。</p><div id=events></div><button onclick="saveEvents()">发送到设备</button> <span id=eventResult></span></section>
<section><h3>二维码</h3><p>最多 4 个，内容可以是网址、微信 / 支付宝收款码里的链接、任意文字，不超过 300 个字节。WiFi 二维码由设备按已连接的 WiFi 自动生成。</p><div id=codes></div><button onclick="saveCodes()">发送到设备</button> <span id=codeResult></span></section>
<section id=mac><h3>打开 Mac 程序</h3><p>对胸牌说「你好小智」，等它开始听，再说「打开 Mac 的 xxx」。下面自己设的口令优先，没设的按程序名自动找。胸牌上已有的功能名（计算器、日历、天气…）仍打开胸牌的，要开电脑上的请说「打开 Mac 的计算器」。最多 40 条，口令不超过 12 个字。</p><div id=macRows></div><datalist id=macApps></datalist><button onclick="macRow()">添加一条</button> <button onclick="saveMac()">保存</button> <span id=macResult></span><h4>胸牌最近听到的</h4><p>没打开对的，可以把听到的词设为口令。</p><div id=macHeard></div></section>
<details><summary>连接诊断</summary><pre id=state></pre></details><script>
function load(key){try{return JSON.parse(localStorage[key]||'null')}catch(e){return null}}
function keep(key,value){try{localStorage[key]=JSON.stringify(value)}catch(e){}}
async function send(path,body,out){out.textContent='发送中…';try{const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});if(!r.ok)throw new Error((await r.json()).error);out.textContent='已发送';return true}catch(e){out.textContent='发送失败：'+e.message+'（请确认设备已连接）';return false}}
const box=document.querySelector('#labels');const savedLabels=load('labels')||[];
for(let i=0;i<5;i++){const input=document.createElement('input');input.maxLength=8;input.placeholder='例如：蹲坑中';input.value=savedLabels[i]||'';input.style.width='140px';box.append(input)}
async function saveLabels(){const labels=[...box.querySelectorAll('input')].map(i=>i.value.trim());if(await send('/api/status-labels',{labels},document.querySelector('#labelResult')))keep('labels',labels)}
const events=document.querySelector('#events');const savedEvents=load('events')||[];const off=load('offwork');if(off!==null)document.querySelector('#offwork').value=off;
for(let i=0;i<6;i++){const e=savedEvents[i]||{};const row=document.createElement('div');row.className='row';row.innerHTML='<input class=name maxlength=8 placeholder="例如：发工资" style="width:130px"><input class=date type=date><label><input class=yearly type=checkbox>每年</label><label><input class=lunar type=checkbox>农历</label>';row.querySelector('.name').value=e.name||'';row.querySelector('.date').value=e.date||'';row.querySelector('.yearly').checked=!!e.yearly;row.querySelector('.lunar').checked=!!e.lunar;events.append(row)}
async function saveEvents(){const list=[...events.children].map(r=>({name:r.querySelector('.name').value.trim(),date:r.querySelector('.date').value,yearly:r.querySelector('.yearly').checked,lunar:r.querySelector('.lunar').checked}));const offwork=document.querySelector('#offwork').value;const out=document.querySelector('#eventResult');if(list.some(e=>e.name&&!e.date)){out.textContent='请为每个日子选择日期';return}if(await send('/api/countdowns',{events:list.filter(e=>e.name),offwork},out)){keep('events',list);keep('offwork',offwork)}}
const codes=document.querySelector('#codes');const savedCodes=load('codes')||[];
for(let i=0;i<4;i++){const c=savedCodes[i]||{};const row=document.createElement('div');row.className='row';row.innerHTML='<input class=title maxlength=12 placeholder="标题，例如：我的名片" style="width:170px"><br><textarea class=text rows=2 cols=48 placeholder="内容，例如：https://..."></textarea>';row.querySelector('.title').value=c.title||'';row.querySelector('.text').value=c.text||'';codes.append(row)}
async function saveCodes(){const list=[...codes.children].map(r=>({title:r.querySelector('.title').value.trim(),text:r.querySelector('.text').value.trim()}));const out=document.querySelector('#codeResult');if(list.some(c=>new TextEncoder().encode(c.text).length>300)){out.textContent='有内容超过 300 个字节';return}if(await send('/api/qr-codes',{codes:list},out))keep('codes',list)}
let macApps=[];const macRows=document.querySelector('#macRows'),macOut=document.querySelector('#macResult');
async function post(path,body){const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const j=await r.json();if(!r.ok)throw new Error(j.error);return j}
function macRow(word='',path=''){const row=document.createElement('div');row.className='row';row.innerHTML='<input class=word maxlength=12 placeholder="说的词，例如：浏览器" style="width:170px"> → <input class=app list=macApps placeholder="输入或选择程序" style="width:280px"> <button class=try>试一下</button> <button class=del>删除</button> <span class=note></span>';
 row.querySelector('.word').value=word;const a=macApps.find(x=>x.path==path);row.querySelector('.app').value=a?a.label:'';
 row.querySelector('.del').onclick=()=>row.remove();
 row.querySelector('.try').onclick=async()=>{const note=row.querySelector('.note'),p=macPath(row);if(!p){note.textContent='请从列表里选程序';return}try{note.textContent=(await post('/api/mac-open',{path:p})).note}catch(e){note.textContent='失败：'+e.message}};
 macRows.append(row);return row}
function macPath(row){const v=row.querySelector('.app').value.trim();const a=macApps.find(x=>x.label==v);return a?a.path:''}
async function macLoad(){try{const r=await (await fetch('/api/mac-apps')).json();macApps=r.apps;const list=document.querySelector('#macApps');list.replaceChildren(...macApps.map(a=>{const o=document.createElement('option');o.value=a.label;return o}));
 macRows.replaceChildren();for(const [w,p] of Object.entries(r.aliases||{}))macRow(w,p);if(!macRows.children.length)macRow();
 const heard=document.querySelector('#macHeard');heard.replaceChildren(...(r.history||[]).slice().reverse().map(h=>{const d=document.createElement('div');d.className='row';const s=document.createElement('span');s.textContent='「'+h.heard+'」 → '+(h.app?(h.ok?h.app:h.app+'（打不开）'):'没找到')+' ';const b=document.createElement('button');b.textContent='设为口令';b.onclick=()=>macRow(h.heard).querySelector('.app').focus();d.append(s,b);return d}));
 if(!(r.history||[]).length)heard.textContent='还没有'}catch(e){macOut.textContent='读取失败：'+e.message}}
async function saveMac(){const aliases={};const rows=[...macRows.children];for(let i=0;i<rows.length;i++){const w=rows[i].querySelector('.word').value.trim(),p=macPath(rows[i]);if(!w&&!rows[i].querySelector('.app').value.trim())continue;if(!w){macOut.textContent='第 '+(i+1)+' 条没填口令';return}if(!p){macOut.textContent='第 '+(i+1)+' 条请从列表里选程序';return}if(w in aliases){macOut.textContent='口令「'+w+'」重复了';return}aliases[w]=p}
 try{await post('/api/mac-apps',{aliases});macOut.textContent='已保存，马上生效'}catch(e){macOut.textContent='保存失败：'+e.message}}
macLoad();
async function act(command){await fetch('/api/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command})})}setInterval(async()=>{let r=await fetch('/api/state');document.querySelector('#state').textContent=JSON.stringify(await r.json(),null,2)},1500)</script></html>'''
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def send(self,code,data,kind='application/json'):
  raw=data.encode() if isinstance(data,str) else json.dumps(data,ensure_ascii=False).encode();self.send_response(code);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(raw)));self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(raw)
 def do_GET(self):
  try:
   if self.headers.get('Host')!='127.0.0.1:'+str(DEVICE.config['ui_port']):return self.send(403,{'error':'host'})
   if self.path=='/':return self.send(200,PAGE,'text/html;charset=utf-8')
   if self.path=='/api/state':return self.send(200,DEVICE.snapshot())
   if self.path=='/api/services':return self.send(200,DEVICE.services.snapshot())
   if self.path=='/api/assistant-permissions':
    reader=getattr(DEVICE,'assistant_permissions',None)
    return self.send(200,reader() if reader else {})
   if self.path=='/api/link-state':return self.send(200,DEVICE.radio.rpc('diagnostics'))
   if self.path=='/api/worker-state':
    import sys,traceback
    return self.send(200,[[{'file':pathlib.Path(f.filename).name,'line':f.lineno,'function':f.name} for f in traceback.extract_stack(frame)] for frame in sys._current_frames().values()])
   if self.path=='/api/app-state':return self.send(200,DEVICE.request('/app_state'))
   if self.path=='/api/device-info':return self.send(200,DEVICE.request('/wifi/status'))
   if self.path=='/api/mac-apps':return self.send(200,DEVICE.services.mac_config())
   return self.send(404,{'error':'not found'})
  except Exception as error:return self.send(500,{'error':type(error).__name__})
 def do_POST(self):
  try:
   if self.headers.get('Host')!='127.0.0.1:'+str(DEVICE.config['ui_port']):return self.send(403,{'error':'host'})
   origin=self.headers.get('Origin');allowed='http://127.0.0.1:'+str(DEVICE.config['ui_port'])
   if origin and origin!=allowed: return self.send(403,{'error':'origin'})
   if self.headers.get('Content-Type','').split(';')[0]!='application/json':return self.send(415,{'error':'content type'})
   size=int(self.headers.get('Content-Length','0'))
   if size<=0 or size>16384:raise ValueError('invalid size')
   data=json.loads(self.rfile.read(size))
   if self.path=='/api/action':result=DEVICE.command(data['command'])
   elif self.path=='/api/native':result=DEVICE.native(data)
   elif self.path=='/api/status-labels':result=DEVICE.status_labels(data['labels'])
   elif self.path=='/api/countdowns':result=DEVICE.countdowns(data['events'],data['offwork'])
   elif self.path=='/api/qr-codes':result=DEVICE.qr_codes(data['codes'])
   elif self.path=='/api/mac-apps':result=DEVICE.services.set_mac_aliases(data['aliases'])
   elif self.path=='/api/mac-open':result={'note':DEVICE.services.mac.open_path(data['path'])}
   else:return self.send(404,{'error':'not found'})
   return self.send(200,result)
  except Exception as error:return self.send(500,{'error':type(error).__name__})
if __name__=='__main__':
 DEVICE=Device();SERVER=ThreadingHTTPServer(('127.0.0.1',DEVICE.config['ui_port']),Handler)
 print('PHYSICAL_PASSPORT_READY http://127.0.0.1:'+str(DEVICE.config['ui_port']),flush=True)
 def stop(*_):threading.Thread(target=SERVER.shutdown,daemon=True).start()
 signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
 try:SERVER.serve_forever()
 finally:DEVICE.close();SERVER.server_close()
