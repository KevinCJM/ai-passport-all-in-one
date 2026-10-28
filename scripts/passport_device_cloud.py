"""Use the real board's existing XiaoZhi WebSocket identity and account settings."""
import json,pathlib,struct,threading,time,types,urllib.request
from passport_xiaozhi import XiaoZhi
from passport_paths import config_file, user_data
ROOT=pathlib.Path(__file__).resolve().parents[1]
HELLO={'type':'hello','version':1,'transport':'websocket','features':{'mcp':False},'audio_params':{'format':'opus','sample_rate':16000,'channels':1,'frame_duration':60}}
def device_socket(config=None,open_timeout=15):
 """The board's own XiaoZhi WebSocket (its MAC and token), before the hello."""
 from websockets.sync.client import connect
 config=config or json.loads(config_file().read_text());identity=config['xiaozhi'];token=identity['token'];headers={'Protocol-Version':'1','Device-Id':config['mac'],'Client-Id':identity['client_id']}
 if token:headers['Authorization']=token if ' ' in token else 'Bearer '+token
 return connect(identity['url'],additional_headers=headers,proxy=None,compression=None,open_timeout=open_timeout,close_timeout=2,max_size=65536,max_queue=32)
OTA='https://api.tenclass.net/xiaozhi/ota/'
def transcriber(config=None):
 """A second XiaoZhi device for speech-to-text only (语音输入, 语音笔记), so dictated words stay out of the chat's
 history and memory. Its Device-Id is the board MAC with the locally administered bit set; its Client-Id is a fixed
 prefix ("ai-pas") and that MAC, so the board (native_services.cpp) computes the same one. The firmware's settings check (OTA) gives any
 device the server address and the shared token, so a Mac without the board's old identity can transcribe too."""
 config=config or json.loads(config_file().read_text());octets=config['mac'].lower().split(':');octets[0]='%02x'%(int(octets[0],16)|2);mac=':'.join(octets)
 client='61692d70-6173-4000-8000-'+mac.replace(':','');identity=config.get('xiaozhi')
 if not identity:
  request=urllib.request.Request(OTA,data=json.dumps({'mac_address':mac,'uuid':client}).encode(),method='POST',headers={'Device-Id':mac,'Client-Id':client,'User-Agent':'ai-passport/1','Content-Type':'application/json'})
  identity=json.loads(urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request,timeout=10).read())['websocket']
 return {'mac':mac,'xiaozhi':{'url':identity['url'],'token':identity.get('token',''),'client_id':client}}
class DeviceXiaoZhi(XiaoZhi):
 def __init__(self,audio,update):
  super().__init__(audio,update);self.ws=None;self.ws_lock=threading.RLock();self.reader=None;self.diagnostics=False;self.diagnostic_sent=False
 def connect(self):
  config=json.loads(config_file().read_text())
  if config.get('voice_diagnostics'):
   self.diagnostics=True;self.audio.diagnostic_path=user_data()/'voice-diagnostic.raw'
  self.ws=device_socket(config)
  self.connected.set()
  self.reader=threading.Thread(target=self.read_socket,daemon=True);self.reader.start()
  self.publish(HELLO)
  if not self.hello.wait(12):raise TimeoutError('physical XiaoZhi WebSocket hello missing')
  self.notify('云端就绪')
 def publish(self,data):
  if self.session:data=dict(data,session_id=self.session)
  with self.ws_lock:
   if self.ws:self.ws.send(json.dumps(data))
 def send_opus(self,packet):
  if not self.listening or not self.ws:return
  try:
   with self.ws_lock:self.ws.send(packet)
   self.stats['uplink_opus_bytes']+=len(packet)
  except Exception as error:
   self.connected.clear();self.hello.clear();self.stats['connection_error']=type(error).__name__
 def read_socket(self):
  try:
   for data in self.ws:
    if self.cancel.is_set():break
    if isinstance(data,bytes):
     if not self.active or self.listening or self.drop_tts:continue
     if self.diagnostics and not self.diagnostic_sent:
      self.audio.voice_captured.clear();self.audio.send(8,b'\x14',self.generation);self.diagnostic_sent=True
     self.stats['tts_opus_bytes']+=len(data);self.audio.send(2,struct.pack('<I',self.rate)+data,self.generation)
    else:
     item=json.loads(data)
     if item.get('type')=='hello':
      self.session=item.get('session_id','');self.rate=int(item.get('audio_params',{}).get('sample_rate',24000));self.hello.set()
     elif self.active:
      if item.get('type')=='tts' and item.get('state')=='start':self.diagnostic_sent=False
      self.message(None,None,types.SimpleNamespace(payload=data))
      if item.get('type')=='tts' and item.get('state')=='stop' and not self.listening and not self.drop_tts:
       self.audio.send(5,b'\x04',self.generation)
  except Exception as error:
   if not self.cancel.is_set():self.stats['connection_error']=type(error).__name__;self.notify('语音连接断开')
  finally:
   self.connected.clear();self.hello.clear()
   if not self.cancel.is_set() and self.active:
    self.listening=False
    try:self.audio.send(5,b'\x00',self.generation)
    except Exception:pass
    self.notify('连接已断开，确认重连')
 def close(self):
  self.audio.event_callback=None;self.audio.stop_microphone();self.listening=False;self.cancel.set()
  self.connected.clear();self.hello.clear()
  try:self.audio.send(5,b'\x00',self.generation)
  except Exception:pass
  if self.ws:
   self.ws.close()
   if self.reader and self.reader is not threading.current_thread():self.reader.join(3)
   self.ws=None
