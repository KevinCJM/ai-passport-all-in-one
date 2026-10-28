"""MQTT/UDP gateway for the already bound VIRTUAL C3 identity.

TLS, AES-CTR, live cloud ASR/LLM/TTS. C3 handles Opus through AudioPipe.
No real-board backup or account secret is sent to the browser or log.
"""
import json
from pathlib import Path
import socket
import ssl
import struct
import sys
import threading
import time

import paho.mqtt.client as mqtt
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes

ROOT=Path(__file__).resolve().parents[1]


def virtual_credentials():
    tools=Path.home()/'.local/share/esp32-agent/esp-idf-v6.1/components/nvs_flash/nvs_partition_tool'
    sys.path.insert(0,str(tools))
    from nvs_parser import NVS_Partition
    flash=ROOT/'experiments/xiaozhi-cloud-lab/passport-device/flash.bin'
    raw=flash.read_bytes()[0x9000:0xd000]
    partition=NVS_Partition('virtual_identity',bytearray(raw)); namespaces={}; values={}
    for page in sorted(partition.pages,key=lambda p:p.header['page_index']):
        for entry in page.entries:
            if entry.is_empty or entry.state!='Written': continue
            if entry.metadata['namespace']==0 and entry.data: namespaces[entry.data['value']]=entry.key
    for page in sorted(partition.pages,key=lambda p:p.header['page_index']):
        for entry in page.entries:
            if entry.is_empty or entry.state!='Written' or namespaces.get(entry.metadata['namespace'])!='mqtt': continue
            if entry.metadata['type']=='string' and entry.data:
                payload=b''.join(bytes(child.raw) for child in entry.children)[:entry.data['size']]
                values[entry.key]=payload.rstrip(b'\0').decode()
    if not all(values.get(k) for k in ('endpoint','client_id','username','password','publish_topic')):
        raise RuntimeError('bound virtual C3 MQTT credentials unavailable')
    return values


class XiaoZhi:
    def __init__(self,audio,update):
        self.audio=audio; self.update=update; self.client=None; self.udp=None
        self.connected=threading.Event(); self.hello=threading.Event(); self.cancel=threading.Event()
        self.session=''; self.sequence=0; self.remote_sequence=0; self.key=None; self.nonce=None; self.listening=False
        self.stats={'stt':'','answer':'','tts_opus_bytes':0,'uplink_opus_bytes':0,'emotion':'neutral','tts_done':False}
        self.generation=audio.generation; self.state_lock=threading.RLock(); self.drop_tts=False; self.auto_recording=False
        self.audio.event_callback=self.native_event
        self.active=True
        self.stats.update(interruptions=0,auto_stops=0,abort_sent=0,late_tts_dropped=0)

    def notify(self,state):
        if self.active:self.update({'state':state,'value':'小智语音','lines':[self.stats['stt'][:80],self.stats['answer'][:160],'确认：开始 / 结束说话'],'emotion':self.stats['emotion'],'speaking':state=='正在回答'})

    def suspend(self):
        if self.active and (self.listening or self.stats['answer'] and not self.stats['tts_done']):
            try:self.publish({'type':'abort','reason':'user_interruption'})
            except Exception:pass
        self.active=False;self.listening=False;self.drop_tts=True;self.audio.event_callback=None

    def resume(self,generation,update):
        self.generation=generation;self.update=update;self.active=True
        self.audio.callback=self.send_opus;self.audio.event_callback=self.native_event

    def connect(self):
        credentials=virtual_credentials(); self.topic=credentials['publish_topic']
        endpoint=credentials['endpoint']; host,separator,port=endpoint.partition(':'); port=int(port) if separator else 8883
        self.client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id=credentials['client_id'])
        self.client.username_pw_set(credentials['username'],credentials['password'])
        if port==8883: self.client.tls_set_context(ssl.create_default_context())
        def connected(client,userdata,flags,reason,properties):
            if reason==0:
                if credentials.get('subscribe_topic'): client.subscribe(credentials['subscribe_topic'])
                self.connected.set()
        self.client.on_connect=connected; self.client.on_message=self.message
        self.client.connect_async(host,port,keepalive=60); self.client.loop_start()
        if not self.connected.wait(15): raise TimeoutError('MQTT connection failed')
        self.publish({'type':'hello','version':3,'transport':'udp','features':{'mcp':False},
            'audio_params':{'format':'opus','sample_rate':16000,'channels':1,'frame_duration':60}})
        if not self.hello.wait(12): raise TimeoutError('cloud audio channel failed')
        self.notify('云端就绪')

    def publish(self,data):
        if self.session: data=dict(data,session_id=self.session)
        self.client.publish(self.topic,json.dumps(data),qos=0)

    def message(self,client,userdata,message):
        try:
            data=json.loads(message.payload); kind=data.get('type')
            if not self.active and kind!='hello':return
            if kind=='hello':
                self.session=data['session_id']; udp=data['udp']; self.key=bytes.fromhex(udp['key']); self.nonce=bytes.fromhex(udp['nonce'])
                if len(self.key)!=16 or len(self.nonce)!=16: raise ValueError('invalid cloud key size')
                self.rate=int(data.get('audio_params',{}).get('sample_rate',24000))
                self.udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); self.udp.connect((udp['server'],int(udp['port']))); self.udp.settimeout(.2)
                threading.Thread(target=self.receive,daemon=True).start(); self.hello.set()
            elif kind=='stt':
                self.stats['stt']=data.get('text',''); self.stats['stt_at']=time.monotonic(); self.drop_tts=False; self.notify('正在思考')
            elif kind=='tts':
                if self.drop_tts or self.listening:
                    self.stats['late_tts_dropped']+=1; return
                if data.get('state')=='sentence_start' and data.get('text'):
                    self.stats['answer']+=(data['text'] if not self.stats['answer'] else ' '+data['text'])
                if data.get('state')=='start':
                    self.stats['tts_started_at']=time.monotonic(); self.audio.send(5,b'\x02',self.generation); self.notify('正在回答')
                elif data.get('state')=='sentence_start': self.notify('正在回答')
                elif data.get('state')=='stop': self.stats['tts_done']=True; self.stats['tts_finished_at']=time.monotonic(); self.notify('云端就绪')
            elif kind=='llm': self.stats['emotion']=data.get('emotion','neutral')
        except Exception: self.stats['protocol_error']=True

    def send_opus(self,packet):
        if not self.listening or not self.udp: return
        nonce=bytearray(self.nonce); self.sequence+=1
        struct.pack_into('>H',nonce,2,len(packet)); struct.pack_into('>I',nonce,8,0); struct.pack_into('>I',nonce,12,self.sequence)
        cipher=Cipher(algorithms.AES(self.key),modes.CTR(bytes(nonce))).encryptor()
        self.udp.send(bytes(nonce)+cipher.update(packet)+cipher.finalize()); self.stats['uplink_opus_bytes']+=len(packet)

    def receive(self):
        while not self.cancel.is_set():
            try: packet=self.udp.recv(4096)
            except socket.timeout: continue
            except OSError: break
            if len(packet)<16 or struct.unpack_from('>H',packet,2)[0]!=len(packet)-16: continue
            if not self.active or self.listening or self.drop_tts: continue
            nonce=packet[:16]; sequence=struct.unpack_from('>I',nonce,12)[0]
            if sequence<=self.remote_sequence: continue
            self.remote_sequence=sequence
            cipher=Cipher(algorithms.AES(self.key),modes.CTR(nonce)).decryptor()
            opus=cipher.update(packet[16:])+cipher.finalize(); self.stats['tts_opus_bytes']+=len(opus)
            if not self.audio.send(2,struct.pack('<I',self.rate)+opus,self.generation): break

    def native_event(self,kind):
        # Native C3 VAD owns the decision; the companion transports abort/listen.
        with self.state_lock:
            if self.cancel.is_set(): return
            if kind==6 and not self.listening:
                self.stats['interruptions']+=1
                self.drop_tts=True; self.auto_recording=True
                self.publish({'type':'abort','reason':'user_interruption'}); self.stats['abort_sent']+=1
                self.audio.drain()
                self.stats['stt']=''; self.stats['answer']=''; self.stats['tts_done']=False; self.listening=True
                self.publish({'type':'listen','state':'start','mode':'manual'})
                self.audio.send(5,b'\x03',self.generation); self.notify('正在听')
            elif kind==7 and self.auto_recording and self.listening:
                self.stats['auto_stops']+=1; self.auto_recording=False; self.end_listen()

    def listen(self,microphone=False):
        if self.stats['answer'] and not self.stats['tts_done']: self.publish({'type':'abort','reason':'user_interruption'})
        self.auto_recording=False; self.drop_tts=False
        self.audio.drain(); self.audio.send(5,b'\x01',self.generation)
        self.stats['stt']=''; self.stats['answer']=''; self.stats['tts_done']=False; self.listening=True
        self.publish({'type':'listen','state':'start','mode':'manual'}); self.notify('正在听')
        if microphone: self.audio.start_microphone(1)

    def end_listen(self):
        self.audio.stop_microphone(); self.audio.send(5,b'\x00',self.generation)
        self.listening=False; self.publish({'type':'listen','state':'stop'}); self.notify('正在思考')

    def close(self):
        self.audio.event_callback=None
        self.audio.stop_microphone(); self.listening=False; self.cancel.set()
        if self.client:
            if self.session: self.publish({'type':'goodbye'})
            self.client.disconnect(); self.client.loop_stop(); self.client=None
        if self.udp: self.udp.close(); self.udp=None
